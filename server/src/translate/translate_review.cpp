#include "translate/translate_review.h"

#include "cloud/custom_translation.h"
#include "cloud/translation_gloss.h"
#include "translate/translate_cache.h"
#include "translate/translate_persist.h"
#include "translate/translate_state.h"
#include <condition_variable>
#include <mutex>
#include <thread>

namespace Translate
{
namespace
{
// 一审一批最多多少条：与翻译侧一次请求的上限对齐。再多模型容易漏行，
// 反而一条修正都拿不回来。
constexpr size_t kMaxReviewItems = 12;
// 校对线程的积压上限：超过就丢旧的一批，宁可漏校也不排长队。
constexpr size_t kMaxReviewBacklog = 48;

struct ReviewJob
{
    std::vector<PendingReview> items;
    RuntimeConfig config;
    std::string target_language;
    std::string vendor_scope;
};

std::mutex g_review_mutex;
std::condition_variable g_review_cv;
ReviewJob g_review_job;
bool g_review_pending = false;
std::thread g_review_worker;

std::vector<Result> ReviewPending(const RuntimeConfig &config, const std::vector<PendingReview> &pending,
                                  const std::string &target_language, const std::string &vendor_scope)
{
    std::vector<Result> corrections;
    // deeplx 没有聊天接口；腾讯/阿里云/小牛也没有能跑校对提示词的通道。
    if (config.vendor != Vendor::Custom || pending.empty() || config.custom.format == "deeplx")
        return corrections;
    for (const std::string source : {"zh", "en"})
    {
        // 词条与整句分开发：整句的校对要重写一整段，超时和词条不是一个量级。
        for (const bool sentence : {false, true})
        {
            std::vector<const PendingReview *> group;
            for (const auto &item : pending)
            {
                if (item.source == source && item.sentence == sentence)
                    group.push_back(&item);
            }
            if (group.empty())
                continue;
            std::vector<std::string> texts;
            std::vector<std::string> translations;
            texts.reserve(group.size());
            translations.reserve(group.size());
            for (const auto *item : group)
            {
                texts.push_back(item->query.key);
                translations.push_back(item->gloss);
            }
            const std::string target = source == "en" ? std::string("zh") : target_language;
            const auto fixes =
                CustomTranslation::ReviewTranslations(config.custom, texts, translations, source, target, sentence);
            std::vector<GlossWrite> writes;
            for (size_t i = 0; i < group.size() && i < fixes.size(); ++i)
            {
                if (fixes[i].empty())
                    continue;
                const std::string gloss = CloudTranslation::FormatGloss(fixes[i]);
                if (gloss.empty() || gloss == group[i]->gloss)
                    continue;
                // 先过长度门再写缓存：否则内存里是校对版、词库里还是旧版，重开输入法就回退。
                if (!ShouldPersist(group[i]->query, gloss))
                    continue;
                RememberPositive(Identity(group[i]->query, target_language, vendor_scope), gloss);
                writes.push_back({group[i]->query, gloss});
                corrections.push_back({group[i]->query.key, group[i]->query.direction, gloss});
            }
            // 一组最多十几条，一次写完。
            PersistGlosses(writes, target_language);
        }
    }
    return corrections;
}

void ReviewLoop()
{
    auto &running = Internal::Running();
    while (running.load())
    {
        ReviewJob job;
        {
            std::unique_lock lock(g_review_mutex);
            g_review_cv.wait(lock, [&running] { return !running.load() || g_review_pending; });
            if (!running.load())
                break;
            job = std::move(g_review_job);
            g_review_job = ReviewJob{};
            g_review_pending = false;
        }
        if (job.items.empty())
            continue;
        auto corrections = ReviewPending(job.config, job.items, job.target_language, job.vendor_scope);
        if (corrections.empty() || !running.load() || !Internal::Callback())
            continue;
        uint64_t generation = 0;
        {
            std::lock_guard lock(Internal::Mutex());
            generation = Internal::LatestGeneration();
        }
        if (!EnglishIme::IsTranslationCurrent(generation))
            continue;
        Internal::Callback()(std::move(corrections), generation);
    }
}
} // namespace

void StartReviewWorker()
{
    g_review_worker = std::thread(ReviewLoop);
}

void StopReviewWorker()
{
    g_review_cv.notify_all();
    if (g_review_worker.joinable())
        g_review_worker.join();
}

void SubmitReview(const RuntimeConfig &config, std::vector<PendingReview> items, const std::string &target_language,
                  const std::string &vendor_scope)
{
    if (items.empty())
        return;
    {
        std::lock_guard lock(g_review_mutex);
        if (g_review_job.items.size() + items.size() > kMaxReviewBacklog)
            g_review_job.items.clear(); // 积压过多说明用户一直在连打，丢掉旧的一批
        g_review_job.items.insert(g_review_job.items.end(), items.begin(), items.end());
        g_review_job.config = config;
        g_review_job.target_language = target_language;
        g_review_job.vendor_scope = vendor_scope;
        g_review_pending = true;
    }
    g_review_cv.notify_one();
}
} // namespace Translate
