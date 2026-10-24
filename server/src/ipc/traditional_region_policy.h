#pragma once

#include <algorithm>
#include <initializer_list>
#include <string_view>

namespace FanyImeIpc
{
/** 繁体写法按哪个地区的用字排序。 */
enum class TraditionalRegion
{
    Taiwan,
    HongKong,
};

/** 所在地与区域格式都不是台湾或港澳时用的地区。 */
constexpr TraditionalRegion kFallbackTraditionalRegion = TraditionalRegion::HongKong;

namespace traditional_region_detail
{
inline bool EqualsIgnoringAsciiCase(std::string_view left, std::string_view right)
{
    const auto lower = [](char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; };
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [&](char a, char b) { return lower(a) == lower(b); });
}

inline bool MatchesAnyCode(std::string_view value, std::initializer_list<std::string_view> codes)
{
    return std::any_of(codes.begin(), codes.end(),
                       [&](std::string_view code) { return EqualsIgnoringAsciiCase(value, code); });
}
} // namespace traditional_region_detail

/**
 * 由系统「所在地」与「区域格式」判定繁体写法的地区。
 * location 是 GetUserDefaultGeoName 返回的两字母地区码，locale 是 GetUserDefaultLocaleName 返回的区域名；
 * 读不到的传空串。所在地是台湾或港澳时以所在地为准，否则看区域格式，两者都不是时为 kFallbackTraditionalRegion。
 * 区域格式先去掉第一个 `_` 起的排序后缀（`zh-TW_radstr` 按 `zh-TW`）；两者都整串比较，不分大小写。
 */
inline TraditionalRegion ResolveTraditionalRegion(std::string_view location, std::string_view locale)
{
    using traditional_region_detail::MatchesAnyCode;
    locale = locale.substr(0, locale.find('_'));
    if (MatchesAnyCode(location, {"TW"}))
        return TraditionalRegion::Taiwan;
    if (MatchesAnyCode(location, {"HK", "MO"}))
        return TraditionalRegion::HongKong;
    if (MatchesAnyCode(locale, {"zh-TW", "zh-Hant-TW"}))
        return TraditionalRegion::Taiwan;
    if (MatchesAnyCode(locale, {"zh-HK", "zh-MO", "zh-Hant-HK", "zh-Hant-MO"}))
        return TraditionalRegion::HongKong;
    return kFallbackTraditionalRegion;
}
} // namespace FanyImeIpc
