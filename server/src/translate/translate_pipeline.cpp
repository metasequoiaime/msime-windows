#include "translate/translate_pipeline.h"

#include "cloud/translation_gloss.h"
#include "config/ime_config.h"
#include "translate/translate_cache.h"
#include "translate/translate_persist.h"
#include "translate/translate_provider.h"
#include "translate/translate_review.h"
#include "translate/translate_state.h"
#include <chrono>

namespace Translate
{
namespace
{
// 输入停顿多久才发翻译请求。120ms 能把连打的中间状态合并掉，体感上接近瞬时。
constexpr auto kIdleDelay = std::chrono::milliseconds(120);
// 一轮最多交多少条去校对：与翻译侧一次请求的上限对齐。
constexpr size_t kMaxPendingPerRound = 12;

std::string FormatOrEmpty(const std::string &raw)
{
    return CloudTranslation::FormatGloss(raw);
}
} // namespace

void ApplyTranslatedGroup(const RuntimeConfig &config, const std::vector<Query> &group, const Outcome &outcome,
                          const std::string &target_language, const std::string &vendor_scope,
                          std::vector<Result> &results, bool sentence, bool review_enabled,
                          std::vector<PendingReview> &pending)
{
    (void)config; // 提供方与凭据只在 TranslateGroup 里用，这里只需要结果本身
    std::vector<GlossWrite> writes;
    writes.reserve(group.size());
    for (size_t i = 0; i < group.size(); ++i)
    {
        // 结果数组比请求短，说明提供方的批次预算先用完了，后面的词根本没有发出，不算失败。
        if (!outcome.attempted(i))
            break;
        const std::string identity = Identity(group[i], target_language, vendor_scope);
        const std::string gloss = FormatOrEmpty(outcome.translations[i]);
        if (gloss.empty())
        {
            RememberFailure(identity);
            continue;
        }
        RememberPositive(identity, gloss);
        writes.push_back({group[i], gloss});
        results.push_back({group[i].key, group[i].direction, gloss});
        // 译完先上屏，再交给校对；校对结果只回写词库与缓存，不阻塞这一帧。
        if (review_enabled && pending.size() < kMaxPendingPerRound)
        {
            const std::string source = group[i].direction == Direction::EnglishToChinese ? "en" : "zh";
            pending.push_back({group[i], gloss, source, sentence});
        }
    }
    // 一整批写完再落库：一次 open、一个事务，不再每条 fsync。
    PersistGlosses(writes, target_language);
}

void TranslateGroup(const RuntimeConfig &config, const std::vector<Query> &group, const std::string &source,
                    const std::string &target, const std::string &target_language, const std::string &vendor_scope,
                    std::vector<Result> &results, std::vector<PendingReview> &pending, bool sentence)
{
    if (group.empty())
        return;
    Batch batch;
    batch.texts.reserve(group.size());
    for (const auto &query : group)
        batch.texts.push_back(query.key);
    batch.source = source;
    batch.target = target;
    batch.sentence = sentence;
    const Outcome outcome = TranslateWithProvider(config, batch);
    const bool review_enabled = config.vendor == Vendor::Custom && config.custom.review;
    ApplyTranslatedGroup(config, group, outcome, target_language, vendor_scope, results, sentence, review_enabled,
                         pending);
}

void WorkerLoop()
{
    auto &running = Internal::Running();
    auto &job_counter = Internal::Job();
    uint64_t observed_job = job_counter.load();
    while (running.load())
    {
        std::unique_lock lock(Internal::Mutex());
        Internal::Cv().wait(lock, [&] { return !running.load() || job_counter.load() != observed_job; });
        if (!running.load())
            break;
        observed_job = job_counter.load();
        auto queries = Internal::LatestQueries();
        auto generation = Internal::LatestGeneration();
        auto target = Internal::LastRequest() + kIdleDelay;
        while (running.load())
        {
            if (Internal::Cv().wait_until(lock, target,
                                          [&] { return !running.load() || job_counter.load() != observed_job; }))
            {
                if (!running.load())
                    return;
                observed_job = job_counter.load();
                queries = Internal::LatestQueries();
                generation = Internal::LatestGeneration();
                target = Internal::LastRequest() + kIdleDelay;
                continue;
            }
            break;
        }
        lock.unlock();
        if (!running.load() || queries.empty() || !EnglishIme::IsTranslationCurrent(generation))
            continue;

        const auto config = GetConfiguredTranslation();
        const auto resolved = Resolve(*config);
        if (!resolved)
            continue;

        const std::string target_language = resolved->target_language;
        const std::string vendor_scope = VendorScope(*config);
        std::vector<Query> en_zh;
        std::vector<Query> zh_target;
        std::vector<Query> zh_sentence;
        std::vector<Result> results;
        {
            std::lock_guard cache_lock(Internal::Mutex());
            for (const auto &query : queries)
            {
                if (TooLong(query.key))
                    continue;
                const std::string identity = Identity(query, target_language, vendor_scope);
                const auto cached = Internal::Cache().find(identity);
                if (cached != Internal::Cache().end())
                {
                    // 上一轮请求可能在这一代候选失效后才回来：结果仍然有效，直接补给当前这一代。
                    results.push_back({query.key, query.direction, cached->second});
                    continue;
                }
                if (IsBackingOff(identity))
                    continue;
                if (query.direction == Direction::EnglishToChinese &&
                    CloudTranslation::IsCloudTranslatableEnglish(query.key))
                    en_zh.push_back(query);
                else if (query.direction == Direction::ChineseToEnglish &&
                         CloudTranslation::IsCloudTranslatableChinese(query.key))
                    (IsSentenceLike(query.key) ? zh_sentence : zh_target).push_back(query);
            }
        }

        // 每批译完就立即上屏一次：词条不该等整句（句子一路要几秒）才一起显出来。
        const auto publish = [&]() -> bool {
            if (results.empty())
                return true;
            if (!running.load() || job_counter.load() != observed_job ||
                !EnglishIme::IsTranslationCurrent(generation) || !Internal::Callback())
                return false;
            Internal::Callback()(std::move(results), generation);
            results.clear();
            return true;
        };
        bool alive = true;
        std::vector<PendingReview> pending;
        if (!en_zh.empty())
        {
            TranslateGroup(*resolved, en_zh, "en", "zh", target_language, vendor_scope, results, pending);
            alive = publish();
        }
        if (alive && !zh_target.empty())
        {
            TranslateGroup(*resolved, zh_target, "zh", target_language, target_language, vendor_scope, results,
                           pending);
            alive = publish();
        }
        // 整句单独一批：超时和输出上限都跟词条不一样，混在一个批次里会互相拖。
        if (alive && !zh_sentence.empty())
        {
            TranslateGroup(*resolved, zh_sentence, "zh", target_language, target_language, vendor_scope, results,
                           pending, /*sentence=*/true);
            alive = publish();
        }
        if (!alive || pending.empty())
            continue;
        // 校对丢给独立线程，工作循环立刻回去等下一次输入，上屏延迟不受影响。
        SubmitReview(*resolved, std::move(pending), target_language, vendor_scope);
    }
}
} // namespace Translate
