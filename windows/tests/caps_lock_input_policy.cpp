#include "Key/CapsLockInputPolicy.h"
#include <cstdio>

int main()
{
    int failures = 0;
    auto check = [&](bool capsLockOn, bool inputInProgress, unsigned virtualKey, wchar_t character, bool expected) {
        const bool actual = ShouldPassCapsLockAlphabet(capsLockOn, inputInProgress, virtualKey, character);
        if (actual != expected)
        {
            std::fprintf(stderr, "FAIL caps=%d composing=%d vk=%u char=%u: expected=%d actual=%d\n", capsLockOn,
                         inputInProgress, virtualKey, static_cast<unsigned>(character), expected, actual);
            ++failures;
        }
    };

    // CapsLock + Shift produces lowercase characters with the same alphabet virtual keys
    for (unsigned key = L'A'; key <= L'Z'; ++key)
    {
        for (unsigned state = 0; state < 4; ++state)
        {
            const bool capsLockOn = (state & 1) != 0;
            const bool inputInProgress = (state & 2) != 0;
            const bool expected = state == 1;
            check(capsLockOn, inputInProgress, key, static_cast<wchar_t>(key), expected);
            check(capsLockOn, inputInProgress, key, static_cast<wchar_t>(key + L'a' - L'A'), expected);
        }
    }

    // Keep non-alphabet keys and characters on their existing classification path
    check(true, false, L'1', L'1', false);
    check(true, false, L'1', L'a', false);
    check(true, false, L'A', L'@', false);
    check(true, false, L'A', L'[', false);
    check(true, false, L'A', L'`', false);
    check(true, false, L'A', L'{', false);
    check(true, false, L'A', L'\0', false);

    if (failures != 0)
        return 1;
    std::puts("PASS: CapsLock alphabet passthrough with and without Shift");
    return 0;
}
