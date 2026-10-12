#include "cloud_translation.h"

#include "config/ime_config.h"
#include "ai_translation.h"
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
constexpr auto kNegativeTtl = std::chrono::minutes(8);
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
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_negative;

std::string TargetLanguage(const TranslationConfig &config)
{
    const auto &language = config.tencent.target_language;
    return language.empty() ? "en" : language;
}

enum class Provider
{
    Tencent,
    Custom,
    NiuTrans,
    Ai,
};

// 当前生效的提供方由 [translation].provider 单一决定，服务端与页面不再各写一份优先级。
Provider ActiveProvider(const TranslationConfig &config)
{
    if (config.provider == "ai")
        return Provider::Ai;
    if (config.provider == "niutrans")
        return Provider::NiuTrans;
    if (config.provider == "custom")
        return Provider::Custom;
    return Provider::Tencent;
}

std::string ProviderScope(const TranslationConfig &config)
{
    switch (ActiveProvider(config))
    {
    case Provider::Ai:
        return "ai:" + config.ai.provider + ":" + config.ai.endpoint + ":" + config.ai.model;
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
    g_negative.erase(identity);
}

bool IsNegative(const std::string &identity)
{
    const auto found = g_negative.find(identity);
    if (found == g_negative.end())
        return false;
    if (found->second <= std::chrono::steady_clock::now())
    {
        g_negative.erase(found);
        return false;
    }
    return true;
}

void RememberNegative(const std::string &identity)
{
    std::lock_guard lock(g_mutex);
    if (g_negative.size() >= kMaxCacheEntries)
        g_negative.clear();
    g_negative[identity] = std::chrono::steady_clock::now() + kNegativeTtl;
}

TencentTmt::Credentials ResolveCredentials(const TencentTmtConfig &config)
{
    TencentTmt::Credentials credentials;
    // provider 由设置页明确选成腾讯云时，加载配置会把 enabled 置为 true；只是从旧布尔推导出
    // 腾讯云的配置仍要求 enabled，和旧版一样，不会因为升级就开始向腾讯云发送候选词。
    if (!config.enabled)
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
    CustomTranslation::Config custom;
    NiuTransTranslation::Config niutrans;
    AiTranslation::Config ai;
};

std::optional<ResolvedProvider> ResolveActiveProvider(const TranslationConfig &config)
{
    ResolvedProvider resolved;
    resolved.provider = ActiveProvider(config);
    switch (resolved.provider)
    {
    case Provider::Ai:
        // 复用 AI 辅助当前服务商的接口地址、Token 与模型，与 ai_assistant.enabled 解耦：
        // 用户可能只想用翻译、不开 AI 联想。
        resolved.ai = AiTranslation::FromConfiguredTranslation(config);
        if (!AiTranslation::IsUsableConfig(resolved.ai))
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
        resolved.tencent = ResolveCredentials(config.tencent);
        if (!CloudTranslation::IsUsableSecret(resolved.tencent.secret_id))
            return std::nullopt;
        break;
    }
    return resolved;
}

void PersistGloss(const EnglishIme::TranslationQuery &query, const std::string &gloss,
                  const std::string &target_language)
{
    // english.db is language-specific. Other target languages remain in the
    // in-memory cloud cache only.
    if (target_language != "en" || g_db_path.empty() || !CloudTranslation::ShouldPersistGloss(query.key, gloss))
        return;
    const bool chinese_to_english = query.direction == EnglishIme::TranslationDirection::ChineseToEnglish;
    EnglishDictionary::upsert_gloss(g_db_path, chinese_to_english, query.key, gloss);
}

void ApplyTranslatedGroup(const std::vector<EnglishIme::TranslationQuery> &group,
                          const std::vector<std::string> &translated, const std::string &target_language,
                          const std::string &provider_scope, std::vector<EnglishIme::TranslationResult> &out)
{
    for (size_t i = 0; i < group.size(); ++i)
    {
        const std::string identity = Identity(group[i], target_language, provider_scope);
        const std::string raw = i < translated.size() ? translated[i] : std::string{};
        const std::string gloss = CloudTranslation::FormatGloss(raw);
        if (gloss.empty())
        {
            RememberNegative(identity);
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
    case Provider::Ai:
        return AiTranslation::TextTranslateBatch(resolved.ai, texts, source, target);
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
        const auto resolved = ResolveActiveProvider(*config);
        if (!resolved)
            continue;

        const std::string target_language = TargetLanguage(*config);
        const std::string provider_scope = ProviderScope(*config);
        std::vector<EnglishIme::TranslationQuery> en_zh;
        std::vector<EnglishIme::TranslationQuery> zh_target;
        std::vector<EnglishIme::TranslationResult> results;
        {
            std::lock_guard cache_lock(g_mutex);
            for (const auto &query : queries)
            {
                if (TooLong(query.key))
                    continue;
                const std::string identity = Identity(query, target_language, provider_scope);
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
                if (IsNegative(identity))
                    continue;
                if (query.direction == EnglishIme::TranslationDirection::EnglishToChinese &&
                    CloudTranslation::IsCloudTranslatableEnglish(query.key))
                    en_zh.push_back(query);
                else if (query.direction == EnglishIme::TranslationDirection::ChineseToEnglish &&
                         CloudTranslation::IsCloudTranslatableChinese(query.key))
                    zh_target.push_back(query);
            }
        }

        if (!en_zh.empty())
            TranslateGroup(*resolved, en_zh, "en", "zh", target_language, provider_scope, results);
        if (!zh_target.empty())
            TranslateGroup(*resolved, zh_target, "zh", target_language, target_language, provider_scope, results);
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
    return ProviderScope(*GetConfiguredTranslation());
}

std::string LookupCache(const std::string &key, EnglishIme::TranslationDirection direction)
{
    const auto config = GetConfiguredTranslation();
    const std::string identity = Identity({key, direction}, TargetLanguage(*config), ProviderScope(*config));
    std::lock_guard lock(g_mutex);
    const auto found = g_cache.find(identity);
    return found == g_cache.end() ? std::string{} : found->second;
}
} // namespace CloudTranslation
