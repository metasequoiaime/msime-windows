#pragma once

inline bool ShouldPassCapsLockAlphabet(bool capsLockOn, bool inputInProgress, unsigned virtualKey, wchar_t character)
{
    return capsLockOn && !inputInProgress && virtualKey >= L'A' && virtualKey <= L'Z' &&
           ((character >= L'A' && character <= L'Z') || (character >= L'a' && character <= L'z'));
}
