#include "Key/CapsLockPunctuationPolicy.h"
#include <cstdio>

int main()
{
    int failures = 0;

    // Bits are IME open, Caps Lock. Only Chinese mode without Caps Lock follows into
    // Chinese punctuation; Caps Lock makes the followed punctuation English.
    constexpr bool expectedFollowed[4] = {false, false, true, false};
    for (unsigned state = 0; state < 4; ++state)
    {
        const bool imeOpen = (state & 2) != 0;
        const bool capsLock = (state & 1) != 0;
        const bool actual = FollowedPunctuationOpen(imeOpen, capsLock);
        if (actual != expectedFollowed[state])
        {
            std::fprintf(stderr, "FAIL followed ime=%d caps=%d: expected=%d actual=%d\n", imeOpen, capsLock,
                         expectedFollowed[state], actual);
            ++failures;
        }
    }

    // Bits are known, last applied, Caps Lock. The first report always syncs; afterwards
    // only a changed Caps Lock state does, so a repeated report keeps a manual toggle.
    constexpr bool expectedResync[8] = {true, true, true, true, false, true, true, false};
    for (unsigned state = 0; state < 8; ++state)
    {
        const bool known = (state & 4) != 0;
        const bool lastApplied = (state & 2) != 0;
        const bool capsLock = (state & 1) != 0;
        const bool actual = ShouldResyncPunctuationForCapsLock(known, lastApplied, capsLock);
        if (actual != expectedResync[state])
        {
            std::fprintf(stderr, "FAIL resync known=%d last=%d caps=%d: expected=%d actual=%d\n", known, lastApplied,
                         capsLock, expectedResync[state], actual);
            ++failures;
        }
    }

    if (failures != 0)
        return 1;
    std::puts("PASS: Caps Lock punctuation follow and resync policy");
    return 0;
}
