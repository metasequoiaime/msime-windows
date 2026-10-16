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
const std::string kSmiley = "\xF0\x9F\x98\x80";       // 😀
const std::string kCrying = "\xF0\x9F\x98\xA2";       // 😢
const std::string kLoudlyCrying = "\xF0\x9F\x98\xAD"; // 😭
const std::string kSweat = "\xF0\x9F\x98\x85";        // 😅

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
