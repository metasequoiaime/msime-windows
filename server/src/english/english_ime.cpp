#include "english_ime.h"
#include "japanese_dictionary.h"

#include "engine/core/data_path.h"
#include "engine/english/english_dictionary.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace
{
// Mixed Chinese/English input starts offering completions once the prefix
// reaches the configured length. Dedicated English mode still starts from one
// character.
constexpr size_t kDefaultMixedMinPrefix = 2;
constexpr size_t kDedicatedCandidateLimit = 1000;

std::mutex g_mutex;
std::condition_variable g_cv;
std::thread g_worker;
std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_generation{0};
std::atomic<uint64_t> g_translation_generation{0};
std::string g_latest_input;
std::vector<EnglishIme::TranslationQuery> g_latest_translation_queries;
std::string g_translation_target_language = "en";
bool g_dedicated_mode = false;
size_t g_mixed_min_prefix = kDefaultMixedMinPrefix;
std::string g_db_path;
EnglishIme::ApplyCallback g_apply_callback;
EnglishIme::TranslationCallback g_translation_callback;

std::string NormalizeInput(const std::string &input)
{
    std::string normalized;
    normalized.reserve(input.size());
    for (const unsigned char ch : input)
    {
        if (ch >= 'a' && ch <= 'z')
        {
            normalized.push_back(static_cast<char>(ch));
        }
        else if (ch >= 'A' && ch <= 'Z')
        {
            normalized.push_back(static_cast<char>(ch + ('a' - 'A')));
        }
        else
        {
            return {};
        }
    }
    return normalized;
}

void WorkerLoop()
{
    EnglishDictionary dictionary(g_db_path);
    uint64_t observed_generation = g_generation.load();
    uint64_t observed_translation_generation = g_translation_generation.load();
    const auto japanese_path = std::filesystem::u8path(g_db_path).parent_path() / JapaneseDictionary::kFileName;
    const auto pending_path = std::filesystem::u8path(g_db_path).parent_path() / "japanese.db.pending";
    std::error_code install_error;
    if (std::filesystem::exists(pending_path, install_error))
        std::filesystem::rename(pending_path, japanese_path, install_error);
    JapaneseDictionary japanese(metasequoia::path_to_utf8(japanese_path));

    while (g_running)
    {
        std::unique_lock lock(g_mutex);
        g_cv.wait(lock, [&] {
            return !g_running || g_generation.load() != observed_generation ||
                   g_translation_generation.load() != observed_translation_generation;
        });
        if (!g_running)
        {
            break;
        }

        const uint64_t next_generation = g_generation.load();
        const uint64_t next_translation_generation = g_translation_generation.load();
        const bool run_candidates = next_generation != observed_generation;
        const bool run_translations = next_translation_generation != observed_translation_generation;
        observed_generation = next_generation;
        observed_translation_generation = next_translation_generation;
        const std::string input = g_latest_input;
        const auto translation_queries = g_latest_translation_queries;
        const std::string target_language = g_translation_target_language;
        const bool dedicated_mode = g_dedicated_mode;
        const size_t mixed_min_prefix = g_mixed_min_prefix;
        lock.unlock();

        if (run_translations)
        {
            std::vector<EnglishIme::TranslationResult> results;
            results.reserve(translation_queries.size());
            for (const auto &query : translation_queries)
            {
                if (!g_running || g_translation_generation.load() != observed_translation_generation)
                    break;
                std::string gloss;
                std::string reading;
                if (query.direction == EnglishIme::TranslationDirection::EnglishToChinese)
                    gloss = dictionary.query_chinese_gloss(query.key);
                else if (target_language == "en")
                    gloss = dictionary.query_english_gloss(query.key);
                else if (target_language == "ja")
                {
                    auto entry = japanese.lookup(query.key);
                    gloss = std::move(entry.gloss);
                    reading = std::move(entry.reading);
                }
                results.push_back({query.key, query.direction, std::move(gloss), std::move(reading)});
            }
            if (g_running && g_translation_generation.load() == observed_translation_generation &&
                g_translation_callback)
            {
                g_translation_callback(std::move(results), observed_translation_generation);
            }
        }

        if (!run_candidates)
            continue;

        const std::string prefix = NormalizeInput(input);
        const size_t min_prefix = dedicated_mode ? 1 : (std::max)(size_t{1}, mixed_min_prefix);
        if (prefix.size() < min_prefix)
        {
            if (dedicated_mode && !input.empty() && g_running && g_generation.load() == observed_generation &&
                g_apply_callback)
            {
                g_apply_callback({}, input, observed_generation);
            }
            continue;
        }

        auto candidates = dedicated_mode ? dictionary.query_prefix(prefix, kDedicatedCandidateLimit)
                                         : dictionary.query_mixed_prefix(prefix);
        if (!g_running || g_generation.load() != observed_generation)
        {
            continue;
        }

        if (g_apply_callback)
        {
            g_apply_callback(std::move(candidates), input, observed_generation);
        }
    }
}
} // namespace

namespace EnglishIme
{
void Start(const std::string &db_path, ApplyCallback apply_callback, TranslationCallback translation_callback)
{
    if (g_running)
    {
        return;
    }
    g_db_path = db_path;
    g_apply_callback = std::move(apply_callback);
    g_translation_callback = std::move(translation_callback);
    g_running = true;
    g_worker = std::thread(WorkerLoop);
}

void Stop()
{
    if (!g_running)
    {
        return;
    }
    g_running = false;
    g_cv.notify_all();
    if (g_worker.joinable())
    {
        g_worker.join();
    }
    g_apply_callback = {};
    g_translation_callback = {};
}

bool IsRunning()
{
    return g_running;
}

void OnInputChanged(const std::string &input, bool dedicated_mode, size_t mixed_min_prefix)
{
    {
        std::lock_guard lock(g_mutex);
        g_latest_input = input;
        g_dedicated_mode = dedicated_mode;
        g_mixed_min_prefix = mixed_min_prefix == 0 ? kDefaultMixedMinPrefix : mixed_min_prefix;
        g_generation.fetch_add(1);
    }
    g_cv.notify_one();
}

void Clear()
{
    OnInputChanged("");
}

bool IsCurrent(const std::string &input, uint64_t generation, bool dedicated_mode)
{
    std::lock_guard lock(g_mutex);
    return g_running && g_generation.load() == generation && g_latest_input == input &&
           g_dedicated_mode == dedicated_mode;
}

void RequestTranslations(std::vector<TranslationQuery> queries, std::string target_language)
{
    {
        std::lock_guard lock(g_mutex);
        g_latest_translation_queries = std::move(queries);
        g_translation_target_language = std::move(target_language);
        g_translation_generation.fetch_add(1);
    }
    g_cv.notify_one();
}

void ClearTranslations()
{
    RequestTranslations({});
}

bool IsTranslationCurrent(uint64_t generation)
{
    return g_running && g_translation_generation.load() == generation;
}
} // namespace EnglishIme
