#pragma once

#include "engine/core/word_item.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace EnglishIme
{
// 名字沿用中英互译时代：实际表示源语言。EnglishToChinese 是英文候选，译成
// translation.english_target_language（默认中文）；ChineseToEnglish 是中文候选，译成
// tencent_tmt.target_language（默认英语）。
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
// english.db 只有英→中、中→英两张释义表：英文候选的目标语种不是中文、中文候选的目标语种不是英语时，
// 对应那一边不查本地词库，回空释义让云端补。
void RequestTranslations(std::vector<TranslationQuery> queries, bool local_english_source = true,
                         bool local_chinese_source = true);
void ClearTranslations();
bool IsTranslationCurrent(uint64_t generation);

// 候选旁显示的音标（english.db 的 en_ipa）：英文候选标它自己，中文候选译成英语时标英文译文。
// term 是小写的词或短语；查完在后台线程上回调，查不到的 ipa 为空串。连着请求只查最后一批。
struct Phonetic
{
    std::string term;
    std::string ipa;
};
using PhoneticsCallback = std::function<void(std::vector<Phonetic> phonetics)>;
void SetPhoneticsCallback(PhoneticsCallback callback);
void RequestPhonetics(std::vector<std::string> terms);
} // namespace EnglishIme
