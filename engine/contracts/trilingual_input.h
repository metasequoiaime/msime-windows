#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace FanyImeTrilingualInput
{
enum class Mode
{
    Chinese = 0,
    Japanese = 1,
    English = 2
};

constexpr Mode Next(bool ime_enabled, bool japanese)
{
    return !ime_enabled ? Mode::Chinese : japanese ? Mode::English : Mode::Japanese;
}

constexpr bool IsCycleKey(std::uint32_t key, std::uint32_t modifiers, bool enabled)
{
    // VK_SHIFT is normalized from an already matched configured language hotkey,
    // never from a raw Shift key-down event.
    return enabled && key == 0x10u && (modifiers & 7u) == 0;
}

inline std::wstring BuildPayload(Mode mode, std::wstring_view text)
{
    std::wstring payload(1, static_cast<wchar_t>(L'0' + static_cast<int>(mode)));
    payload.push_back(L'\t');
    payload.append(text);
    return payload;
}

inline bool ParsePayload(std::wstring_view payload, Mode &mode, std::wstring &text)
{
    if (payload.size() < 2 || payload[0] < L'0' || payload[0] > L'2' || payload[1] != L'\t' ||
        payload.find(L'\0') != std::wstring_view::npos)
    {
        return false;
    }
    mode = static_cast<Mode>(payload[0] - L'0');
    text.assign(payload.substr(2));
    return true;
}
} // namespace FanyImeTrilingualInput
