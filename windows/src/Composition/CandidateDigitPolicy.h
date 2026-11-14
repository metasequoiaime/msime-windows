#pragma once

// Written out instead of VK_NUMPAD0/9 so the standalone policy test target does
// not need the Windows SDK.
inline constexpr unsigned kNumpadDigitFirstKey = 0x60;
inline constexpr unsigned kNumpadDigitLastKey = 0x69;

// Zero is a candidate key too. The Server answers it as out of range, so it
// selects nothing and leaves the composition alone instead of cancelling it.
inline constexpr bool IsCandidateDigitKey(unsigned virtualKey)
{
    return (virtualKey >= L'0' && virtualKey <= L'9') ||
           (virtualKey >= kNumpadDigitFirstKey && virtualKey <= kNumpadDigitLastKey);
}
