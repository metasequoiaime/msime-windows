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

// 英文候选模式里高亮候选的音标与本地释义。word 是小写词；查完在后台线程上回调，
// 同一个词连着请求只查最后一次。
struct EnglishDetail
{
    std::string word;
    std::string ipa;
    std::string gloss;
};
using DetailCallback = std::function<void(EnglishDetail detail)>;
void SetDetailCallback(DetailCallback callback);
void RequestDetail(const std::string &word);
} // namespace EnglishIme
