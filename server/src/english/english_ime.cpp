#include "english_ime.h"

#include "engine/english/english_dictionary.h"
#include "engine/english/english_spelling.h"
#include "english/english_candidate_text_policy.h"
#include "english/windows_spell_checker.h"
#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
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
constexpr size_t kDedicatedPhraseLimit = 1000;
// 纠错候选：同读音的词库词最多 4 个，再用系统拼写检查补到 8 个（同 hallelujahIM）。
constexpr size_t kSpellingPerSource = 4;
constexpr size_t kSpellingLimit = 8;

std::mutex g_mutex;
std::condition_variable g_cv;
std::thread g_worker;
std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_generation{0};
std::atomic<uint64_t> g_translation_generation{0};
std::string g_latest_input;
std::vector<EnglishIme::TranslationQuery> g_latest_translation_queries;
bool g_local_english_source = true;
bool g_local_chinese_source = true;
bool g_dedicated_mode = false;
size_t g_mixed_min_prefix = kDefaultMixedMinPrefix;
std::string g_db_path;
EnglishIme::ApplyCallback g_apply_callback;
EnglishIme::TranslationCallback g_translation_callback;
EnglishIme::DetailCallback g_detail_callback;
std::atomic<uint64_t> g_detail_generation{0};
std::string g_latest_detail_word;

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

// 英文候选模式里前缀补全查不到词时的纠错。索引和系统拼写检查都在第一次用到时才建，
// 只在这条后台线程上用。
class SpellingCorrector
{
  public:
    explicit SpellingCorrector(EnglishDictionary &dictionary) : dictionary_(dictionary)
    {
    }

    std::vector<WordItem> correct(const std::string &prefix, const std::string &input)
    {
        if (!index_built_)
        {
            index_built_ = true;
            for (const auto &word : dictionary_.query_distinct_words())
                index_.add(word.pinyin, word.word, word.weight);
        }
        if (!spell_checker_)
            spell_checker_ = std::make_unique<WindowsSpellChecker>();

        std::vector<EnglishIme::SpellingCandidate> phonetic;
        for (auto &suggestion : index_.suggest(prefix, kSpellingPerSource))
            phonetic.push_back({std::move(suggestion.word), std::move(suggestion.display)});
        const auto system = spell_checker_->suggest(prefix, kSpellingLimit);

        std::vector<WordItem> candidates;
        for (auto &candidate :
             EnglishIme::MergeSpellingCandidates(prefix, phonetic, system, kSpellingPerSource, kSpellingLimit))
        {
            // 纠错结果不是这串输入在词库里的行：记成 Generated，调频、删词、固定排位都不碰词库；
            // corrected_from 让候选窗挂上纠错标记。
            WordItem item("", std::move(candidate.display), 0, CandidateSource::Generated);
            item.corrected_from = input;
            candidates.push_back(std::move(item));
        }
        return candidates;
    }

  private:
    EnglishDictionary &dictionary_;
    metasequoia::english::PhonexIndex index_;
    bool index_built_ = false;
    std::unique_ptr<WindowsSpellChecker> spell_checker_;
};

// 系统拼写检查是 COM 组件。先于 SpellingCorrector 构造、后于它析构。
struct ComApartment
{
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComApartment()
    {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
};

void WorkerLoop()
{
    const ComApartment com;
    EnglishDictionary dictionary(g_db_path);
    SpellingCorrector corrector(dictionary);
    uint64_t observed_generation = g_generation.load();
    uint64_t observed_translation_generation = g_translation_generation.load();
    uint64_t observed_detail_generation = g_detail_generation.load();

    while (g_running)
    {
        std::unique_lock lock(g_mutex);
        g_cv.wait(lock, [&] {
            return !g_running || g_generation.load() != observed_generation ||
                   g_translation_generation.load() != observed_translation_generation ||
                   g_detail_generation.load() != observed_detail_generation;
        });
        if (!g_running)
        {
            break;
        }

        const uint64_t next_generation = g_generation.load();
        const uint64_t next_translation_generation = g_translation_generation.load();
        const uint64_t next_detail_generation = g_detail_generation.load();
        const bool run_candidates = next_generation != observed_generation;
        const bool run_translations = next_translation_generation != observed_translation_generation;
        const bool run_detail = next_detail_generation != observed_detail_generation;
        observed_generation = next_generation;
        observed_translation_generation = next_translation_generation;
        observed_detail_generation = next_detail_generation;
        const std::string input = g_latest_input;
        const auto translation_queries = g_latest_translation_queries;
        const bool local_english_source = g_local_english_source;
        const bool local_chinese_source = g_local_chinese_source;
        const bool dedicated_mode = g_dedicated_mode;
        const size_t mixed_min_prefix = g_mixed_min_prefix;
        const std::string detail_word = g_latest_detail_word;
        const auto detail_callback = g_detail_callback;
        lock.unlock();

        if (run_detail && !detail_word.empty() && detail_callback)
        {
            detail_callback(
                {detail_word, dictionary.query_ipa(detail_word), dictionary.query_chinese_gloss(detail_word)});
        }

        if (run_translations)
        {
            std::vector<EnglishIme::TranslationResult> results;
            results.reserve(translation_queries.size());
            for (const auto &query : translation_queries)
            {
                if (!g_running || g_translation_generation.load() != observed_translation_generation)
                    break;
                std::string gloss;
                if (query.direction == EnglishIme::TranslationDirection::EnglishToChinese)
                {
                    if (local_english_source)
                        gloss = dictionary.query_chinese_gloss(query.key);
                }
                else if (local_chinese_source)
                {
                    gloss = dictionary.query_english_gloss(query.key);
                }
                results.push_back({query.key, query.direction, std::move(gloss)});
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
        // 短语只进英文候选模式的整页列表，中文里混输的英文不带短语。
        if (dedicated_mode && g_running && g_generation.load() == observed_generation)
            candidates = EnglishIme::MergeEnglishPhrases(prefix, std::move(candidates),
                                                         dictionary.query_phrase_prefix(prefix, kDedicatedPhraseLimit));
        if (dedicated_mode && candidates.empty() && g_running && g_generation.load() == observed_generation)
            candidates = corrector.correct(prefix, input);
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
    std::lock_guard lock(g_mutex);
    g_detail_callback = {};
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

void RequestTranslations(std::vector<TranslationQuery> queries, bool local_english_source, bool local_chinese_source)
{
    {
        std::lock_guard lock(g_mutex);
        g_latest_translation_queries = std::move(queries);
        g_local_english_source = local_english_source;
        g_local_chinese_source = local_chinese_source;
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

void SetDetailCallback(DetailCallback callback)
{
    std::lock_guard lock(g_mutex);
    g_detail_callback = std::move(callback);
}

void RequestDetail(const std::string &word)
{
    if (word.empty())
        return;
    {
        std::lock_guard lock(g_mutex);
        g_latest_detail_word = word;
        g_detail_generation.fetch_add(1);
    }
    g_cv.notify_one();
}
} // namespace EnglishIme
