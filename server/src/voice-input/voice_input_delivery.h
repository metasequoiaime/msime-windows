#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Pure delivery-result policy for the Windows SendInput adapters. Keeping the
// classification here makes the zero/partial/full outcomes testable without
// injecting real user input.
namespace VoiceInput
{
// One SendInput call delivers only when every requested event was inserted.
inline bool AllInputEventsInjected(std::uint32_t requested, std::uint32_t injected)
{
    return requested != 0 && injected == requested;
}

// Send one Unicode character as a key-down/key-up pair. `send` returns the
// number of events SendInput accepted for that character. A short count means
// the pair is incomplete, so delivery stops immediately; the returned value is
// the number of characters for which both events were accepted.
template <typename SendUnicodeCharacter>
std::size_t SendUnicodeTextWith(const std::wstring &text, SendUnicodeCharacter send)
{
    std::size_t delivered = 0;
    for (wchar_t ch : text)
    {
        if (!AllInputEventsInjected(2, send(ch)))
        {
            break;
        }
        ++delivered;
    }
    return delivered;
}

// Ctrl+V is sent as four events: Ctrl down, V down, V up, Ctrl up. Only all
// four count as a delivered paste. A partial prefix still identifies the keys
// that may need key-up cleanup; cleanup must never be reported as success.
struct CtrlVInjectionOutcome
{
    bool delivered = false;
    bool ctrl_key_down = false;
    bool v_key_down = false;
};

inline CtrlVInjectionOutcome ClassifyCtrlVInjection(std::uint32_t injected)
{
    CtrlVInjectionOutcome outcome;
    outcome.delivered = injected == 4;
    outcome.ctrl_key_down = injected >= 1 && injected < 4;
    outcome.v_key_down = injected >= 2 && injected < 3;
    return outcome;
}

} // namespace VoiceInput
