#include "translate/translate_persist.h"

#include "cloud/translation_gloss.h"
#include "engine/english/english_dictionary.h"
#include "translate/translate_state.h"

namespace Translate
{
namespace
{
// 整句联想的候选是一整句话，不是词条。超过这个长度或带句读就按句子走：
// 单独一批、单独的超时与输出上限；写回词库时用更宽的长度门。
constexpr size_t kSentenceThreshold = 12;
constexpr size_t kMaxSentenceChars = 200;
constexpr size_t kMaxSourceChars = 40;
// 写回词库的长度上限：词条 32，整句放宽到 128（中文一句常见 15–40 字，译成目标语种还会变长）。
constexpr size_t kMaxPersistChars = 32;
constexpr size_t kMaxPersistSentenceChars = 128;
} // namespace

bool HasSentencePunctuation(const std::string &text)
{
    for (const char *mark : {"，", "。", "！", "？", "、", "；", "：", "…", "—", "“", "”", "《", "》"})
    {
        if (text.find(mark) != std::string::npos)
            return true;
    }
    for (const char mark : {',', '.', '!', '?', ';'})
    {
        if (text.find(mark) != std::string::npos)
            return true;
    }
    return false;
}

bool IsSentenceLike(const std::string &key)
{
    return CloudTranslation::Utf8Length(key) > kSentenceThreshold || HasSentencePunctuation(key);
}

bool TooLong(const std::string &key)
{
    // 原来一律卡 40 字：整句候选正好死在这条线上，一整个长句连请求都发不出去。
    const size_t length = CloudTranslation::Utf8Length(key);
    return length > (IsSentenceLike(key) ? kMaxSentenceChars : kMaxSourceChars);
}

bool ShouldPersist(const Query &query, const std::string &gloss)
{
    const bool sentence = query.direction == Direction::ChineseToEnglish && IsSentenceLike(query.key);
    return CloudTranslation::ShouldPersistGloss(query.key, gloss,
                                                sentence ? kMaxPersistSentenceChars : kMaxPersistChars);
}

void PersistGloss(const Query &query, const std::string &gloss, const std::string &target_language)
{
    const std::string &db_path = Internal::DbPath();
    if (db_path.empty())
        return;
    const bool chinese_to_english = query.direction == Direction::ChineseToEnglish;
    if (!ShouldPersist(query, gloss))
        return;
    // 和英语一样：英语落 zh_en_glosses，其余语种落 zh_glosses(chinese, lang, gloss)。
    EnglishDictionary::upsert_gloss(db_path, chinese_to_english, query.key, gloss,
                                    chinese_to_english ? target_language : std::string{});
}

void PersistGlosses(const std::vector<GlossWrite> &writes, const std::string &target_language)
{
    const std::string &db_path = Internal::DbPath();
    if (db_path.empty() || writes.empty())
        return;
    std::vector<EnglishDictionary::GlossEntry> entries;
    entries.reserve(writes.size());
    for (const auto &write : writes)
    {
        if (!ShouldPersist(write.query, write.gloss))
            continue;
        const bool chinese_to_english = write.query.direction == Direction::ChineseToEnglish;
        entries.push_back(
            {chinese_to_english, write.query.key, write.gloss, chinese_to_english ? target_language : std::string{}});
    }
    if (!entries.empty())
        EnglishDictionary::upsert_glosses(db_path, entries);
}
} // namespace Translate
