#include "cloud_translation.h"

#include "config/ime_config.h"
#include "ai_translation.h"
#include "aliyun_mt.h"
#include "bilibili_translation.h"
#include "custom_translation.h"
#include "niutrans_translation.h"
#include "tencent_tmt.h"
#include "translation_gloss.h"
#include "engine/english/english_dictionary.h"
#include <curl/curl.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace
{
// Wait until the candidate window has been idle for 500ms before a TMT request.
constexpr auto kIdleDelay = std::chrono::milliseconds(500);
constexpr size_t kMaxSourceChars = 40;
constexpr size_t kMaxCacheEntries = 4096;

std::mutex g_mutex;
std::condition_variable g_cv;
std::thread g_worker;
std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_job{0};
std::vector<EnglishIme::TranslationQuery> g_latest_queries;
uint64_t g_latest_generation = 0;
std::chrono::steady_clock::time_point g_last_request;
std::string g_db_path;
CloudTranslation::ApplyCallback g_callback;

std::unordered_map<std::string, std::string> g_cache;

// 提供方对超时、限流、凭据错误和「没有译文」都只回空串，分不清哪次值得马上重试，所以失败项
// 按次数退避：网络恢复后很快能补上，持续失败的词也不会每次刷新候选都被重发、挤占批次预算。
struct FailureBackoff
{
    unsigned failures = 0;
    std::chrono::steady_clock::time_point retry_at;
};
std::unordered_map<std::string, FailureBackoff> g_failures;

// 英文候选与中文候选各有自己的目标语种。
std::string TargetLanguage(const TranslationConfig &config, EnglishIme::TranslationDirection direction)
{
    if (direction == EnglishIme::TranslationDirection::EnglishToChinese)
        return config.english_target_language.empty() ? "zh" : config.english_target_language;
    const auto &language = config.tencent.target_language;
    return language.empty() ? "en" : language;
}

enum class Provider
{
    Tencent,
    Aliyun,
    Custom,
    NiuTrans,
    Ai,
    Bilibili,
};

Provider ProviderNamed(const std::string &name)
{
    if (name == "bilibili")
        return Provider::Bilibili;
    if (name == "ai")
        return Provider::Ai;
    if (name == "niutrans")
        return Provider::NiuTrans;
    if (name == "aliyun")
        return Provider::Aliyun;
    if (name == "custom")
        return Provider::Custom;
    return Provider::Tencent;
}

// 中文候选的提供方由 [translation].provider 决定，英文候选的由 [translation].english_provider 决定；
// 服务端与页面不再各写一份优先级。凭据（各家的段）两边共用。
Provider ActiveProvider(const TranslationConfig &config, EnglishIme::TranslationDirection direction)
{
    return ProviderNamed(direction == EnglishIme::TranslationDirection::EnglishToChinese ? config.english_provider
                                                                                         : config.provider);
}

std::string ProviderScope(const TranslationConfig &config, EnglishIme::TranslationDirection direction)
{
    switch (ActiveProvider(config, direction))
    {
    case Provider::Bilibili:
        // 不同模型的译文不同，按模型分缓存域，切换模型后能马上看到新模型的结果。
        return "bilibili:" + BilibiliTranslation::NormalizeModel(config.bilibili.model);
    case Provider::Ai:
        return "ai:" + config.ai.provider + ":" + config.ai.endpoint + ":" + config.ai.model;
    case Provider::Aliyun:
        // 阿里云没有应用 id，缓存域按 AccessKey ID 区分。
        return "aliyun:" + config.aliyun.secret_id;
    case Provider::NiuTrans:
        return "niutrans:" + config.niutrans.app_id;
    case Provider::Custom:
        return "custom:" + config.custom.endpoint;
    case Provider::Tencent:
    default:
        return "tencent";
    }
}

std::string Identity(const EnglishIme::TranslationQuery &query, const std::string &target_language,
                     const std::string &provider_scope)
{
    return provider_scope + ":" + target_language + ":" +
           (query.direction == EnglishIme::TranslationDirection::EnglishToChinese ? "e:" : "z:") + query.key;
}

bool TooLong(const std::string &key)
{
    return CloudTranslation::Utf8Length(key) > kMaxSourceChars;
}

void RememberPositive(const std::string &identity, std::string gloss)
{
    std::lock_guard lock(g_mutex);
    if (g_cache.size() >= kMaxCacheEntries)
        g_cache.clear();
    g_cache[identity] = std::move(gloss);
    g_failures.erase(identity);
}

bool IsBackingOff(const std::string &identity)
{
    const auto found = g_failures.find(identity);
    return found != g_failures.end() && std::chrono::steady_clock::now() < found->second.retry_at;
}

void RememberFailure(const std::string &identity)
{
    std::lock_guard lock(g_mutex);
    if (g_failures.size() >= kMaxCacheEntries && g_failures.find(identity) == g_failures.end())
        g_failures.clear();
    auto &entry = g_failures[identity];
    entry.retry_at = std::chrono::steady_clock::now() + CloudTranslation::FailureRetryDelay(++entry.failures);
}

TencentTmt::Credentials ResolveCredentials(const TencentTmtConfig &config, bool chosen_explicitly)
{
    TencentTmt::Credentials credentials;
    // provider 由设置页明确选成腾讯云时，加载配置会把 enabled 置为 true；只是从旧布尔推导出
    // 腾讯云的配置仍要求 enabled，和旧版一样，不会因为升级就开始向腾讯云发送候选词。英文候选的
    // english_provider 明确写成腾讯云时同样算选过。
    if (!config.enabled && !chosen_explicitly)
        return credentials;
    credentials.region = config.region.empty() ? "ap-guangzhou" : config.region;
    credentials.secret_id = CloudTranslation::TrimSecret(config.secret_id);
    credentials.secret_key = CloudTranslation::TrimSecret(config.secret_key);
    if (!CloudTranslation::IsUsableSecret(credentials.secret_id) ||
        !CloudTranslation::IsUsableSecret(credentials.secret_key))
        return {};
    return credentials;
}

// 一个任务只需要当前生效提供方的配置，其余提供方保持空。
struct ResolvedProvider
{
    Provider provider = Provider::Tencent;
    TencentTmt::Credentials tencent;
    AliyunMt::Credentials aliyun;
    CustomTranslation::Config custom;
    NiuTransTranslation::Config niutrans;
    AiTranslation::Config ai;
    BilibiliTranslation::Config bilibili;
};

std::optional<ResolvedProvider> ResolveActiveProvider(const TranslationConfig &config,
                                                      EnglishIme::TranslationDirection direction)
{
    ResolvedProvider resolved;
    resolved.provider = ActiveProvider(config, direction);
    switch (resolved.provider)
    {
    case Provider::Bilibili:
        // 免费接口不需要凭据，选中即可用。
        resolved.bilibili.model = BilibiliTranslation::NormalizeModel(config.bilibili.model);
        break;
    case Provider::Ai:
        // 复用 AI 辅助当前服务商的接口地址、Token 与模型，与 ai_assistant.enabled 解耦：
        // 用户可能只想用翻译、不开 AI 联想。
        resolved.ai = AiTranslation::FromConfiguredTranslation(config);
        if (!AiTranslation::IsUsableConfig(resolved.ai))
            return std::nullopt;
        break;
    case Provider::Aliyun:
        resolved.aliyun = {CloudTranslation::TrimSecret(config.aliyun.secret_id),
                           CloudTranslation::TrimSecret(config.aliyun.secret_key)};
        if (!AliyunMt::IsUsableCredentials(resolved.aliyun))
            return std::nullopt;
        break;
    case Provider::NiuTrans:
        resolved.niutrans = {CloudTranslation::TrimSecret(config.niutrans.app_id),
                             CloudTranslation::TrimSecret(config.niutrans.apikey)};
        if (!NiuTransTranslation::IsUsableConfig(resolved.niutrans))
            return std::nullopt;
        break;
    case Provider::Custom:
        resolved.custom = {CloudTranslation::TrimSecret(config.custom.endpoint),
                           CloudTranslation::TrimSecret(config.custom.api_key)};
        if (!CustomTranslation::IsSupportedEndpoint(resolved.custom.endpoint))
            return std::nullopt;
        break;
    case Provider::Tencent:
    default:
        resolved.tencent =
            ResolveCredentials(config.tencent, direction == EnglishIme::TranslationDirection::EnglishToChinese &&
                                                   config.english_provider_explicit);
        if (!CloudTranslation::IsUsableSecret(resolved.tencent.secret_id))
            return std::nullopt;
        break;
    }
    return resolved;
}

void PersistGloss(const EnglishIme::TranslationQuery &query, const std::string &gloss,
                  const std::string &target_language)
{
    // english.db only holds English→Chinese and Chinese→English glosses. Other
    // target languages remain in the in-memory cloud cache only.
    const bool chinese_to_english = query.direction == EnglishIme::TranslationDirection::ChineseToEnglish;
    if (target_language != (chinese_to_english ? "en" : "zh") || g_db_path.empty() ||
        !CloudTranslation::ShouldPersistGloss(query.key, gloss))
        return;
    EnglishDictionary::upsert_gloss(g_db_path, chinese_to_english, query.key, gloss);
}

void ApplyTranslatedGroup(const std::vector<EnglishIme::TranslationQuery> &group,
                          const std::vector<std::string> &translated, const std::string &target_language,
                          const std::string &provider_scope, std::vector<EnglishIme::TranslationResult> &out)
{
    for (size_t i = 0; i < group.size(); ++i)
    {
        // 结果数组比请求短，说明提供方的批次预算先用完了，后面的词根本没有发出，不算失败。
        if (i >= translated.size())
            break;
        const std::string identity = Identity(group[i], target_language, provider_scope);
        const std::string gloss = CloudTranslation::FormatGloss(translated[i]);
        if (gloss.empty())
        {
            RememberFailure(identity);
            continue;
        }
        RememberPositive(identity, gloss);
        PersistGloss(group[i], gloss, target_language);
        out.push_back({group[i].key, group[i].direction, gloss});
    }
}

std::vector<std::string> TranslateWithProvider(const ResolvedProvider &resolved, const std::vector<std::string> &texts,
                                               const std::string &source, const std::string &target)
{
    switch (resolved.provider)
    {
    case Provider::Bilibili:
        return BilibiliTranslation::TextTranslateBatch(resolved.bilibili, texts, source, target);
    case Provider::Ai:
        return AiTranslation::TextTranslateBatch(resolved.ai, texts, source, target);
    case Provider::Aliyun:
        return AliyunMt::TextTranslateBatch(resolved.aliyun, texts, source, target);
    case Provider::NiuTrans:
        return NiuTransTranslation::TextTranslateBatch(resolved.niutrans, texts, source, target);
    case Provider::Custom:
        return CustomTranslation::TextTranslateBatch(resolved.custom, texts, source, target);
    case Provider::Tencent:
    default:
        return TencentTmt::TextTranslateBatch(resolved.tencent, texts, source, target);
    }
}

void TranslateGroup(const ResolvedProvider &resolved, const std::vector<EnglishIme::TranslationQuery> &group,
                    const std::string &source, const std::string &target, const std::string &target_language,
                    const std::string &provider_scope, std::vector<EnglishIme::TranslationResult> &out)
{
    std::vector<std::string> texts;
    texts.reserve(group.size());
    for (const auto &query : group)
        texts.push_back(query.key);
    const auto translated = TranslateWithProvider(resolved, texts, source, target);
    ApplyTranslatedGroup(group, translated, target_language, provider_scope, out);
}

void WorkerLoop()
{
    uint64_t observed_job = g_job.load();
    while (g_running)
    {
        std::unique_lock lock(g_mutex);
        g_cv.wait(lock, [&] { return !g_running || g_job.load() != observed_job; });
        if (!g_running)
            break;
        observed_job = g_job.load();
        auto queries = g_latest_queries;
        auto generation = g_latest_generation;
        auto target = g_last_request + kIdleDelay;
        while (g_running)
        {
            if (g_cv.wait_until(lock, target, [&] { return !g_running || g_job.load() != observed_job; }))
            {
                if (!g_running)
                    return;
                observed_job = g_job.load();
                queries = g_latest_queries;
                generation = g_latest_generation;
                target = g_last_request + kIdleDelay;
                continue;
            }
            break;
        }
        lock.unlock();
        if (!g_running || queries.empty() || !EnglishIme::IsTranslationCurrent(generation))
            continue;

        const auto config = GetConfiguredTranslation();
        constexpr auto kEnglish = EnglishIme::TranslationDirection::EnglishToChinese;
        constexpr auto kChinese = EnglishIme::TranslationDirection::ChineseToEnglish;
        // 英文候选与中文候选各用自己选的服务；哪边的服务没配好，只是那一边不发请求。
        const auto english_resolved = ResolveActiveProvider(*config, kEnglish);
        const auto chinese_resolved = ResolveActiveProvider(*config, kChinese);
        if (!english_resolved && !chinese_resolved)
            continue;

        const std::string english_target = TargetLanguage(*config, kEnglish);
        const std::string chinese_target = TargetLanguage(*config, kChinese);
        const std::string english_scope = ProviderScope(*config, kEnglish);
        const std::string chinese_scope = ProviderScope(*config, kChinese);
        std::vector<EnglishIme::TranslationQuery> en_target;
        std::vector<EnglishIme::TranslationQuery> zh_target;
        std::vector<EnglishIme::TranslationResult> results;
        {
            std::lock_guard cache_lock(g_mutex);
            for (const auto &query : queries)
            {
                if (TooLong(query.key))
                    continue;
                const bool english = query.direction == kEnglish;
                const std::string identity =
                    Identity(query, english ? english_target : chinese_target, english ? english_scope : chinese_scope);
                const auto cached = g_cache.find(identity);
                if (cached != g_cache.end())
                {
                    // A previous request may have finished after its candidate
                    // generation became stale. Its result is still valid for
                    // this language, so publish it to the current generation
                    // instead of silently skipping it.
                    results.push_back({query.key, query.direction, cached->second});
                    continue;
                }
                if (IsBackingOff(identity))
                    continue;
                if (english && english_resolved && CloudTranslation::IsCloudTranslatableEnglish(query.key))
                    en_target.push_back(query);
                else if (!english && chinese_resolved && CloudTranslation::IsCloudTranslatableChinese(query.key))
                    zh_target.push_back(query);
            }
        }

        if (!en_target.empty())
            TranslateGroup(*english_resolved, en_target, "en", english_target, english_target, english_scope, results);
        if (!zh_target.empty())
            TranslateGroup(*chinese_resolved, zh_target, "zh", chinese_target, chinese_target, chinese_scope, results);
        if (results.empty() || !g_running || g_job.load() != observed_job ||
            !EnglishIme::IsTranslationCurrent(generation) || !g_callback)
            continue;
        g_callback(std::move(results), generation);
    }
}
} // namespace

namespace CloudTranslation
{
void Start(const std::string &db_path, ApplyCallback apply_callback)
{
    if (g_running)
        return;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_db_path = db_path;
    g_callback = std::move(apply_callback);
    g_running = true;
    g_worker = std::thread(WorkerLoop);
}

void Stop()
{
    if (!g_running)
        return;
    g_running = false;
    g_cv.notify_all();
    if (g_worker.joinable())
        g_worker.join();
    g_callback = {};
}

void RequestMisses(std::vector<EnglishIme::TranslationQuery> queries, uint64_t generation)
{
    {
        std::lock_guard lock(g_mutex);
        g_latest_queries = std::move(queries);
        g_latest_generation = generation;
        g_last_request = std::chrono::steady_clock::now();
        g_job.fetch_add(1);
    }
    g_cv.notify_one();
}

void Clear()
{
    RequestMisses({}, 0);
}

std::string ActiveProviderScope()
{
    const auto config = GetConfiguredTranslation();
    return ProviderScope(*config, EnglishIme::TranslationDirection::ChineseToEnglish) + "|" +
           ProviderScope(*config, EnglishIme::TranslationDirection::EnglishToChinese);
}

std::string LookupCache(const std::string &key, EnglishIme::TranslationDirection direction)
{
    const auto config = GetConfiguredTranslation();
    const std::string identity =
        Identity({key, direction}, TargetLanguage(*config, direction), ProviderScope(*config, direction));
    std::lock_guard lock(g_mutex);
    const auto found = g_cache.find(identity);
    return found == g_cache.end() ? std::string{} : found->second;
}
} // namespace CloudTranslation
