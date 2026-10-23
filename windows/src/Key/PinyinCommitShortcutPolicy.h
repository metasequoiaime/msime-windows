#pragma once

// VK_RETURN, spelled out so this header stays free of <Windows.h> like the other key policies.
constexpr unsigned kVirtualKeyReturn = 0x0D;

// Shift+Enter commits the Server's converted spelling (#642). Typing uppercase ASCII with Shift
// held and pressing Enter before letting go then commits a conversion the user never asked for
// (#1102), so keybindings.commit_pinyin_shift_enter can turn the chord off. With the toggle off
// the key must fall through to the plain Enter classifier, which commits the raw input, instead
// of being eaten here.
inline bool ShouldCommitPinyinOnShiftEnter(bool enabled, unsigned virtualKey, unsigned modifiers, bool windowsKeyDown)
{
    return enabled && !windowsKeyDown && virtualKey == kVirtualKeyReturn && (modifiers & 0b00000111u) == 0b00000001u;
}
