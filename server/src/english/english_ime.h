#pragma once

#include "engine/core/word_item.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace EnglishIme
{
enum class TranslationDirection
{
    EnglishToChinese,
    ChineseToEnglish,
};

struct TranslationQuery
{
    std::string key;
    TranslationDirection direction = TranslationDirection::ChineseToEnglish;
};

struct TranslationResult
{
    std::string key;
    TranslationDirection direction = TranslationDirection::ChineseToEnglish;
    std::string gloss;
};

using ApplyCallback =
    std::function<void(std::vector<WordItem> candidates, const std::string &input, uint64_t generation)>;
using TranslationCallback = std::function<void(std::vector<TranslationResult> results, uint64_t generation)>;

void Start(const std::string &db_path, ApplyCallback apply_callback, TranslationCallback translation_callback = {});
void Stop();
// While running, every non-empty dedicated-mode input is answered through the
// apply callback, even when the dictionary has no match.
bool IsRunning();
void OnInputChanged(const std::string &input, bool dedicated_mode = false, size_t mixed_min_prefix = 2);
void Clear();
bool IsCurrent(const std::string &input, uint64_t generation, bool dedicated_mode = false);
// Non-English target languages have no local dictionary and must go directly
// to cloud translation.
void RequestTranslations(std::vector<TranslationQuery> queries, bool use_local_dictionary = true);
void ClearTranslations();
bool IsTranslationCurrent(uint64_t generation);
// 一次性同步译文探测：只给候选展开键的开面板路径用——面板要在打开前就知道高亮词有没有译文，
// 异步管线那一刻还没跑完。读路径与 WorkerLoop 一致（同一个 english.db、同一对 gloss 查询），
// 但不排队、不回调、不推进 generation。每键路径不要用，那是 RequestTranslations 的活。
std::string LookupTranslationSync(const TranslationQuery &query);
} // namespace EnglishIme
