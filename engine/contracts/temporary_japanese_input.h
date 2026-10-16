#pragma once

#include <cstddef>
#include <cstdint>

// Shared TSF/Server classification of the display-only R prefix. The engine
// receives the body without R. Policy is supplied by the Server after negotiation.
namespace FanyImeTemporaryJapanese
{
using Policy = std::uint8_t;
inline constexpr Policy Allowed = 1;
inline constexpr Policy EnglishCandidates = 2;
constexpr bool IsValidPayload(wchar_t ch)
{
    return ch >= L'0' && ch <= L'3';
}
constexpr Policy FromPayload(wchar_t ch)
{
    return IsValidPayload(ch) ? Policy(ch - L'0') : 0;
}
constexpr wchar_t ToPayload(Policy policy)
{
    return wchar_t(L'0' + (policy & 3));
}
constexpr Policy ToggleEnglish(Policy policy)
{
    return policy ^ EnglishCandidates;
}
constexpr Policy LeaveEnglish(Policy policy)
{
    return policy & ~EnglishCandidates;
}
template <typename Char> constexpr bool IsMode(bool japanese, Policy policy, const Char *raw, std::size_t length)
{
    return japanese || ((policy & Allowed) != 0 && (policy & EnglishCandidates) == 0 && raw != nullptr && length > 0 &&
                        raw[0] == Char('R'));
}
// R is a display-only mode prefix. Caret positions in the romaji body start at 1.
template <typename Char>
constexpr std::size_t MinimumCaret(bool japanese, Policy policy, const Char *raw, std::size_t length)
{
    return !japanese && IsMode(false, policy, raw, length) ? 1 : 0;
}

enum class KeyAction
{
    None,
    LongVowel,
    Punctuation
};

// Virtual-key and modifier values are the existing Windows pipe contract values.
// Decide before candidate paging, word-to-character conversion and punctuation commit.
template <typename Char>
constexpr KeyAction ClassifyKey(std::uint32_t key, std::uint32_t character, std::uint32_t modifiers, bool japanese,
                                Policy policy, const Char *raw, std::size_t length, bool composition_active)
{
    if ((modifiers & 6u) != 0 || (key != 0xBD && key != 0xBB) || !IsMode(japanese, policy, raw, length))
        return KeyAction::None;
    if (key == 0xBD && character == '-')
        return KeyAction::LongVowel;
    return length > 0 && composition_active ? KeyAction::Punctuation : KeyAction::None;
}

template <typename Char>
constexpr std::size_t ClampCaret(bool japanese, Policy policy, const Char *raw, std::size_t length, std::size_t caret)
{
    const auto minimum = MinimumCaret(japanese, policy, raw, length);
    return caret < minimum ? minimum : (caret > length ? length : caret);
}

template <typename Char>
constexpr bool CanBackspace(bool japanese, Policy policy, const Char *raw, std::size_t length, std::size_t caret)
{
    const auto minimum = MinimumCaret(japanese, policy, raw, length);
    return caret > 0 && caret <= length && !(caret == minimum && length > minimum);
}

template <typename Char>
constexpr bool CanMoveCaret(bool japanese, Policy policy, const Char *raw, std::size_t length, std::int64_t next)
{
    return next >= static_cast<std::int64_t>(MinimumCaret(japanese, policy, raw, length)) &&
           next <= static_cast<std::int64_t>(length);
}

template <typename Char> constexpr bool IsValidPolicyFrame(const Char *data, std::size_t capacity)
{
    return data != nullptr && capacity >= 2 && IsValidPayload(data[0]) && data[1] == Char(0);
}
} // namespace FanyImeTemporaryJapanese
