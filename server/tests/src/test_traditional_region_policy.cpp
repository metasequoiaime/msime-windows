#include "ipc/traditional_region_policy.h"
#include "tests/includes/test_framework.h"

// 入参依据的 Windows 返回格式：
//   GetUserDefaultGeoName 返回 ISO 3166-1 两字母码，大写（"TW"、"HK"、"MO"、"US"）；
//   没有两字母码的地区返回 UN M.49 数字码（"001"）；读不到时调用方传空串。
//   GetUserDefaultLocaleName 返回 BCP 47 形式的区域名，语言小写、地区大写、文字首字母大写
//   （"zh-TW"、"zh-HK"、"zh-Hant-TW"）。

using FanyImeIpc::ResolveTraditionalRegion;
using FanyImeIpc::TraditionalRegion;

TEST_CASE(traditional_region_location_wins_over_the_regional_format)
{
    REQUIRE(ResolveTraditionalRegion("TW", "zh-HK") == TraditionalRegion::Taiwan);
    REQUIRE(ResolveTraditionalRegion("TW", "zh-Hant-MO") == TraditionalRegion::Taiwan);
    REQUIRE(ResolveTraditionalRegion("HK", "zh-TW") == TraditionalRegion::HongKong);
    REQUIRE(ResolveTraditionalRegion("MO", "zh-Hant-TW") == TraditionalRegion::HongKong);
}

TEST_CASE(traditional_region_falls_back_to_the_regional_format_when_location_is_empty_or_elsewhere)
{
    for (const char *location : {"", "US", "CN", "JP", "001"})
    {
        REQUIRE(ResolveTraditionalRegion(location, "zh-TW") == TraditionalRegion::Taiwan);
        REQUIRE(ResolveTraditionalRegion(location, "zh-Hant-TW") == TraditionalRegion::Taiwan);
        REQUIRE(ResolveTraditionalRegion(location, "zh-HK") == TraditionalRegion::HongKong);
        REQUIRE(ResolveTraditionalRegion(location, "zh-MO") == TraditionalRegion::HongKong);
        REQUIRE(ResolveTraditionalRegion(location, "zh-Hant-HK") == TraditionalRegion::HongKong);
        REQUIRE(ResolveTraditionalRegion(location, "zh-Hant-MO") == TraditionalRegion::HongKong);
    }
}

TEST_CASE(traditional_region_is_hong_kong_when_neither_source_names_taiwan_or_hong_kong)
{
    REQUIRE(ResolveTraditionalRegion("", "") == TraditionalRegion::HongKong);
    REQUIRE(ResolveTraditionalRegion("US", "en-US") == TraditionalRegion::HongKong);
    REQUIRE(ResolveTraditionalRegion("CN", "zh-CN") == TraditionalRegion::HongKong);
    REQUIRE(ResolveTraditionalRegion("SG", "zh-SG") == TraditionalRegion::HongKong);
    REQUIRE(ResolveTraditionalRegion("", "zh-Hant") == TraditionalRegion::HongKong);
    REQUIRE(ResolveTraditionalRegion("", "en-TW") == TraditionalRegion::HongKong);
}

// 区域名可以带排序后缀（zh-TW_radstr、zh-HK_stroke）：截掉第一个下划线及其后的部分再判定。
TEST_CASE(traditional_region_ignores_the_sort_suffix_of_the_regional_format)
{
    REQUIRE(ResolveTraditionalRegion("", "zh-TW_radstr") == TraditionalRegion::Taiwan);
    REQUIRE(ResolveTraditionalRegion("US", "zh-TW_pronun") == TraditionalRegion::Taiwan);
    REQUIRE(ResolveTraditionalRegion("", "zh-Hant-TW_radstr") == TraditionalRegion::Taiwan);
    REQUIRE(ResolveTraditionalRegion("", "zh-TW_") == TraditionalRegion::Taiwan);
    REQUIRE(ResolveTraditionalRegion("", "zh-TW_radstr_x") == TraditionalRegion::Taiwan);
    REQUIRE(ResolveTraditionalRegion("", "zh-HK_radstr") == TraditionalRegion::HongKong);
    REQUIRE(ResolveTraditionalRegion("", "_zh-TW") == TraditionalRegion::HongKong);
    REQUIRE(ResolveTraditionalRegion("", "zh-CN_stroke") == TraditionalRegion::HongKong);
    // 后缀只从区域格式里截，所在地不截。
    REQUIRE(ResolveTraditionalRegion("TW_x", "") == TraditionalRegion::HongKong);
    // 所在地仍优先。
    REQUIRE(ResolveTraditionalRegion("HK", "zh-TW_radstr") == TraditionalRegion::HongKong);
}
