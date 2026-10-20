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
    // Display annotation only; translation commit continues to consume gloss.
    std::string reading;
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
// The target is captured with the job. English candidates always use en->zh;
// Chinese candidates select the local English or Japanese dictionary.
void RequestTranslations(std::vector<TranslationQuery> queries, std::string target_language = "en");
void ClearTranslations();
bool IsTranslationCurrent(uint64_t generation);
} // namespace EnglishIme
