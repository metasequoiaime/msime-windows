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

// OR'd into modifiers_down of the Main-pipe KeyEvent that requests a cycle.
// Only a client that negotiated FanyImeProtocol::TrilingualCycle sets them, and
// only on the VK_SHIFT it normalizes from an already matched language hotkey.
// The binary language toggle still sends a plain VK_SHIFT, so the Server never
// infers a cycle from the key alone. The two state bits carry the client's own
// OPENCLOSE and native language: with ime_mode_scope=app every host has its own
// state, and the Server must cycle from the requester's state, not its global one.
// The Server strips these bits before any key-modifier policy runs.
constexpr std::uint32_t CycleRequestFlag = 0x10000000u;
constexpr std::uint32_t CycleFromImeOpenFlag = 0x08000000u;
constexpr std::uint32_t CycleFromJapaneseFlag = 0x04000000u;
constexpr std::uint32_t CycleRequestMask = CycleRequestFlag | CycleFromImeOpenFlag | CycleFromJapaneseFlag;

constexpr std::uint32_t EncodeCycleRequest(bool ime_enabled, bool japanese)
{
    return CycleRequestFlag | (ime_enabled ? CycleFromImeOpenFlag : 0u) | (japanese ? CycleFromJapaneseFlag : 0u);
}

constexpr bool IsCycleRequest(std::uint32_t key, std::uint32_t modifiers)
{
    return key == 0x10u && (modifiers & CycleRequestFlag) != 0 && (modifiers & 7u) == 0;
}

constexpr bool CycleRequestImeEnabled(std::uint32_t modifiers)
{
    return (modifiers & CycleFromImeOpenFlag) != 0;
}

constexpr bool CycleRequestJapanese(std::uint32_t modifiers)
{
    return (modifiers & CycleFromJapaneseFlag) != 0;
}

// Destination the Server answers for a cycle request. When the switch was turned
// off after the client queued the request, the request still gets its reply, but
// as the ordinary binary toggle: English, or back to the configured language.
constexpr Mode Destination(bool ime_enabled, bool japanese, bool cycle_enabled, bool configured_japanese)
{
    if (cycle_enabled)
    {
        return Next(ime_enabled, japanese);
    }
    return ime_enabled ? Mode::English : configured_japanese ? Mode::Japanese : Mode::Chinese;
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
