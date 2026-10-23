#include "Key/PinyinCommitShortcutPolicy.h"
#include <cstdio>

int main()
{
    int failures = 0;
    auto check = [&](bool enabled, unsigned virtualKey, unsigned modifiers, bool windowsKeyDown, bool expected) {
        const bool actual = ShouldCommitPinyinOnShiftEnter(enabled, virtualKey, modifiers, windowsKeyDown);
        if (actual != expected)
        {
            std::fprintf(stderr, "FAIL enabled=%d vk=%u mods=%u win=%d: expected=%d actual=%d\n", enabled, virtualKey,
                         modifiers, windowsKeyDown, expected, actual);
            ++failures;
        }
    };

    constexpr unsigned kShift = 0b001;
    constexpr unsigned kCtrl = 0b010;
    constexpr unsigned kAlt = 0b100;

    // On by default: Shift+Enter commits the Server's converted spelling.
    check(true, kVirtualKeyReturn, kShift, false, true);

    // Turning it off is the point: the chord must reach the plain Enter path instead.
    check(false, kVirtualKeyReturn, kShift, false, false);

    // Plain Enter and the other chords keep their own classification.
    check(true, kVirtualKeyReturn, 0, false, false);
    check(true, kVirtualKeyReturn, kCtrl, false, false);
    check(true, kVirtualKeyReturn, kCtrl | kShift, false, false);
    check(true, kVirtualKeyReturn, kAlt, false, false);
    check(true, kVirtualKeyReturn, kShift | kAlt, false, false);

    // Win+Shift+Enter belongs to the shell.
    check(true, kVirtualKeyReturn, kShift, true, false);

    // No other key becomes this chord.
    check(true, L'K', kShift, false, false);
    check(true, L'A', kShift, false, false);

    if (failures != 0)
        return 1;
    std::puts("PASS: Shift+Enter pinyin commit can be turned off");
    return 0;
}
