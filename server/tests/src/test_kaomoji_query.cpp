#include "tests/includes/test_framework.h"
#include "engine/local_modes/kaomoji_query.h"
#include "engine/shuangpin/shuangpin_profile.h"
#include "config/ime_config.h"
#include "utils/common_utils.h"

#include <filesystem>
#include <string>
#include <vector>

namespace
{
// 精确命中 ku（哭）的 kaomoji；旧排序下会被更长的键 kunnao 的 "( -'`-)" 顶掉。
const std::string kCrying = "( ;`д´; )";
const std::string kLongerKey = "( -'`-)";

bool KaomojiDatabaseAvailable()
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

TEST_CASE(kaomoji_query_prefix_matches_full_pinyin)
{
    if (!KaomojiDatabaseAvailable())
        return;
    const auto results = metasequoia::local_modes::query_kaomoji("haixiu", SchemeType::Quanpin, 10,
                                                                 GetShuangpinProfile(GetConfiguredShuangpinSchema()))
                             .candidates;
    REQUIRE(Contains(results, "(*/ω＼*)"));
}

TEST_CASE(kaomoji_query_prefix_matches_jianpin)
{
    if (!KaomojiDatabaseAvailable())
        return;
    const auto results = metasequoia::local_modes::query_kaomoji("hx", SchemeType::Quanpin, 10,
                                                                 GetShuangpinProfile(GetConfiguredShuangpinSchema()))
                             .candidates;
    REQUIRE(Contains(results, "(*/ω＼*)"));
}

TEST_CASE(kaomoji_query_prefix_matches_english_word)
{
    if (!KaomojiDatabaseAvailable())
        return;
    const auto results = metasequoia::local_modes::query_kaomoji("kiss", SchemeType::Quanpin, 10,
                                                                 GetShuangpinProfile(GetConfiguredShuangpinSchema()))
                             .candidates;
    REQUIRE(!results.empty());
    REQUIRE(results[0].source == CandidateSource::Kaomoji);
}

TEST_CASE(kaomoji_query_shuangpin_expands_to_quanpin)
{
    if (!KaomojiDatabaseAvailable())
        return;
    // Xiaohe hx -> haixiu: must reach the haixiu kaomoji via Chinese pinyin.
    const auto results = metasequoia::local_modes::query_kaomoji("hx", SchemeType::Shuangpin, 10,
                                                                 GetShuangpinProfile(GetConfiguredShuangpinSchema()))
                             .candidates;
    REQUIRE(Contains(results, "(*/ω＼*)"));
}

TEST_CASE(kaomoji_query_exact_key_outranks_longer_prefix)
{
    if (!KaomojiDatabaseAvailable())
        return;
    // 打 ku（哭）时，精确命中的 kaomoji 要排在更长键 kunnao 的 "( -'`-)" 之前。
    const auto results = metasequoia::local_modes::query_kaomoji("ku", SchemeType::Quanpin, 10,
                                                                 GetShuangpinProfile(GetConfiguredShuangpinSchema()))
                             .candidates;
    // ku 精确命中的条目不止一条，先后由目录顺序决定；只断言更长键那条没有排到首位，且泪颜在结果里。
    REQUIRE(!results.empty());
    REQUIRE(results[0].word != kLongerKey);
    REQUIRE(Contains(results, kCrying));
}

TEST_CASE(kaomoji_query_single_char_pinyin_prefix)
{
    if (!KaomojiDatabaseAvailable())
        return;
    // "Mk": one char after the trigger must return pinyin-"k"-prefixed kaomoji.
    const auto quanpin = metasequoia::local_modes::query_kaomoji("k", SchemeType::Quanpin, 10,
                                                                 GetShuangpinProfile(GetConfiguredShuangpinSchema()))
                             .candidates;
    REQUIRE(!quanpin.empty());
    const auto shuangpin = metasequoia::local_modes::query_kaomoji("k", SchemeType::Shuangpin, 10,
                                                                   GetShuangpinProfile(GetConfiguredShuangpinSchema()))
                               .candidates;
    REQUIRE(!shuangpin.empty());
}
