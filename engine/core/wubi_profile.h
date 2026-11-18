#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace metasequoia
{
// 五笔码表版本。三版的按键规则、码长和词组取码规则完全相同，只是查的码表不同：msime.db 里各有一张
// 同结构的表，表名就是版本名。配置值（input.wubi_schema）也是版本名。
enum class WubiProfile
{
    Wubi86,
    Wubi98,
    // 新世纪五笔，码表 wubixsj。
    WubiXsj,
};

inline constexpr std::array<WubiProfile, 3> kWubiProfiles{WubiProfile::Wubi86, WubiProfile::Wubi98,
                                                          WubiProfile::WubiXsj};

constexpr std::string_view wubi_profile_name(WubiProfile profile)
{
    switch (profile)
    {
    case WubiProfile::Wubi98:
        return "wubi98";
    case WubiProfile::WubiXsj:
        return "wubixsj";
    case WubiProfile::Wubi86:
        break;
    }
    return "wubi86";
}

// msime.db 里这一版的码表。
constexpr std::string_view wubi_profile_table(WubiProfile profile)
{
    return wubi_profile_name(profile);
}

constexpr std::optional<WubiProfile> wubi_profile_from_name(std::string_view name)
{
    for (const auto profile : kWubiProfiles)
    {
        if (wubi_profile_name(profile) == name)
            return profile;
    }
    return std::nullopt;
}
} // namespace metasequoia
