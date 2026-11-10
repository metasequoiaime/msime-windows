#include "tests/includes/test_framework.h"
#include "engine/local_modes/emoji_query.h"
#include "engine/shuangpin/shuangpin_profile.h"
#include "config/ime_config.h"
#include "utils/common_utils.h"

#include <filesystem>
#include <string>
#include <vector>

namespace
{
const std::string kSmiley = "\xF0\x9F\x98\x80";          // 😀
const std::string kCrying = "\xF0\x9F\x98\xA2";          // 😢
const std::string kLoudlyCrying = "\xF0\x9F\x98\xAD";    // 😭
const std::string kSweat = "\xF0\x9F\x98\x85";           // 😅
const std::string kSunWithFace = "\xF0\x9F\x8C\x9E";     // 🌞
const std::string kWordSun = "\xE5\xA4\xAA\xE9\x98\xB3"; // 太阳
// 「右」的整词命中（➡️）与只在长关键词里带「右」的子串命中（👉：向右指、右边…）。
const std::string kWordRight = "\xE5\x8F\xB3";              // 右
const std::string kRightArrow = "\xE2\x9E\xA1\xEF\xB8\x8F"; // ➡️
const std::string kPointRight = "\xF0\x9F\x91\x89";         // 👉

bool EmojiDatabaseAvailable()
{
    return std::filesystem::exists(CommonUtils::get_ime_data_path() + "\\others.db");
}

bool Contains(const std::vector<WordItem> &items, const std::string &word)
{
    for (const auto &item : items)
        if (item.word == word)
            return true;
    return false;
}

int IndexOf(const std::vector<WordItem> &items, const std::string &word)
{
    for (std::size_t index = 0; index < items.size(); ++index)
        if (items[index].word == word)
            return static_cast<int>(index);
    return -1;
}
} // namespace

TEST_CASE(emoji_query_prefix_matches_full_pinyin)
{
    if (!EmojiDatabaseAvailable())
        return;
    const auto results = metasequoia::local_modes::query_emoji("xiaolian", SchemeType::Quanpin, 10,
                                                               GetShuangpinProfile(GetConfiguredShuangpinSchema()))
                             .candidates;
    REQUIRE(!results.empty());
    REQUIRE_EQ(results[0].word, kSmiley);
    REQUIRE(results[0].source == CandidateSource::Emoji);
}

TEST_CASE(emoji_query_prefix_matches_jianpin)
{
    if (!EmojiDatabaseAvailable())
        return;
    const auto results = metasequoia::local_modes::query_emoji("xl", SchemeType::Quanpin, 200,
                                                               GetShuangpinProfile(GetConfiguredShuangpinSchema()))
                             .candidates;
    REQUIRE(Contains(results, kSmiley));
}

TEST_CASE(emoji_query_prefix_matches_english_word)
{
    if (!EmojiDatabaseAvailable())
        return;
    const auto results = metasequoia::local_modes::query_emoji("laugh", SchemeType::Quanpin, 50,
                                                               GetShuangpinProfile(GetConfiguredShuangpinSchema()))
                             .candidates;
    REQUIRE(Contains(results, kSmiley));
}

TEST_CASE(emoji_query_exact_key_outranks_longer_prefix)
{
    if (!EmojiDatabaseAvailable())
        return;
    // 用户反馈：打 ku（哭）第一个出的是 kuxiao（苦笑）的 😅。精确命中要排在更长前缀之前。
    const auto results = metasequoia::local_modes::query_emoji("ku", SchemeType::Quanpin, 10,
                                                               GetShuangpinProfile(GetConfiguredShuangpinSchema()))
                             .candidates;
    // ku 精确命中的几条之间的先后由目录顺序决定，这里只断言首条是 哭 的表情，而不是 kuxiao 的 😅。
    REQUIRE(!results.empty());
    REQUIRE(results[0].word == kCrying || results[0].word == kLoudlyCrying);
    REQUIRE(results[0].word != kSweat);
}

TEST_CASE(emoji_query_xiaohe_shuangpin_expands_to_quanpin)
{
    if (!EmojiDatabaseAvailable())
        return;
    // Xiaohe xnlm -> xiaolian, which must reach the smiley via Chinese pinyin.
    const auto results = metasequoia::local_modes::query_emoji("xnlm", SchemeType::Shuangpin, 10,
                                                               GetShuangpinProfile(GetConfiguredShuangpinSchema()))
                             .candidates;
    REQUIRE(Contains(results, kSmiley));
}

TEST_CASE(emoji_query_by_keyword_matches_chinese_token)
{
    if (!EmojiDatabaseAvailable())
        return;
    // 展开面板按高亮词查 emoji：「太阳」是若干 emoji 的关键词，应命中且来源标为 Emoji。
    const auto results = metasequoia::local_modes::query_emoji_by_keyword(kWordSun, 10).candidates;
    REQUIRE(!results.empty());
    for (const auto &item : results)
        REQUIRE(item.source == CandidateSource::Emoji);
    REQUIRE(Contains(results, kSunWithFace));
}

TEST_CASE(emoji_query_by_keyword_matches_substring_ranks_exact_first)
{
    if (!EmojiDatabaseAvailable())
        return;
    // 用户反馈：「右」要同时命中 ➡️（把「右」当独立关键词）和 👉（只在「向右指」「右边」这类长关键词里带
    // 「右」）。子串匹配取到两者；整词命中必须排在子串命中之前，否则 ➡️ 会被 LIMIT 截掉。空词与不存在的词仍返回空。
    const auto results = metasequoia::local_modes::query_emoji_by_keyword(kWordRight, 9).candidates;
    const int arrow = IndexOf(results, kRightArrow);
    const int point = IndexOf(results, kPointRight);
    REQUIRE(arrow >= 0);
    REQUIRE(point >= 0);
    REQUIRE(arrow < point);
    REQUIRE(metasequoia::local_modes::query_emoji_by_keyword("", 10).candidates.empty());
    REQUIRE(metasequoia::local_modes::query_emoji_by_keyword("zzqqxx", 10).candidates.empty());
}
