#include "Composition/CandidateDigitPolicy.h"
#include <cstdio>

int main()
{
    int failures = 0;
    auto check = [&](unsigned virtualKey, bool expected) {
        const bool actual = IsCandidateDigitKey(virtualKey);
        if (actual != expected)
        {
            std::fprintf(stderr, "FAIL vk=0x%02X: expected=%d actual=%d\n", virtualKey, expected, actual);
            ++failures;
        }
    };

    // Main-row digits 1..9 select a candidate. Zero belongs to the same family:
    // the Server answers it as out of range, so nothing is selected and the
    // composition is left alone instead of being cancelled.
    for (unsigned key = L'0'; key <= L'9'; ++key)
    {
        check(key, true);
    }
    check(0x60, true); // VK_NUMPAD0
    check(0x61, true); // VK_NUMPAD1
    check(0x68, true); // VK_NUMPAD8
    check(0x69, true); // VK_NUMPAD9

    // Letters, punctuation and neighbouring key codes stay on their own path.
    // (Virtual keys never carry lowercase ASCII: 0x61 is VK_NUMPAD1.)
    check(L'A', false);
    check(L'Z', false);
    check(L';', false);
    check(L'/', false);
    check(L' ', false);
    check(0x6A, false); // VK_MULTIPLY, just past the numpad digits
    check(0x6F, false); // VK_DIVIDE
    check(0x08, false); // VK_BACK

    if (failures != 0)
        return 1;
    std::puts("PASS: zero is a candidate digit key on the main row and the numpad");
    return 0;
}
