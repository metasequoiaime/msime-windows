#pragma once
#include "key_chord.h"

namespace FanyImeCandidateActionKey
{
// Default expand key: bare Backquote.
constexpr std::string_view Default = "Backquote";

// Parse a configured candidate translation expand key.
// Empty disables. Bare keys are restricted to Backquote / GraveAccent; chords must be
// Ctrl[+Shift]+<virtual key>. Invalid values return false and leave result empty.
inline constexpr bool Parse(std::string_view value, FanyImeKeyChord::Binding &result)
{
    result = {};
    if (value.empty())
        return true;
    if (value == "GraveAccent")
        value = "Backquote";
    if (value.substr(0, 5) != "Ctrl+")
    {
        const unsigned key = FanyImeKeyChord::KeyCode(value);
        if (key != 0xc0) // VK_OEM_3 / Backquote
            return false;
        result = {key, 0};
        return true;
    }
    return FanyImeKeyChord::Parse(value, result);
}

// Constant default binding used by the TSF config cache and the Server config global.
inline constexpr FanyImeKeyChord::Binding DefaultBinding()
{
    FanyImeKeyChord::Binding binding;
    FanyImeCandidateActionKey::Parse(Default, binding);
    return binding;
}
} // namespace FanyImeCandidateActionKey
