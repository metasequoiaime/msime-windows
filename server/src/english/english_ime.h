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
// 每个目标语种都先查本地词库：英语读 zh_en_glosses，其余语种读 zh_glosses，
// 本地没有才交给云端。
void RequestTranslations(std::vector<TranslationQuery> queries, bool use_local_dictionary = true,
                         std::string target_language = {});
void ClearTranslations();
bool IsTranslationCurrent(uint64_t generation);
} // namespace EnglishIme
