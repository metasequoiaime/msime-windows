#pragma once

// 一次按键打出的字符与当时 Shift 按没按着。不依赖 TSF 与 Windows 头文件，单独测试见
// windows/tests/custom_punctuation.cpp。
struct KeyCharacter
{
    wchar_t character = 0;
    bool shiftDown = false;
};

namespace KeyboardStateCodes
{
// Win32 的 VK_SHIFT，KeyEventSink.cpp 里 static_assert 它与 Windows 头文件相同。
inline constexpr unsigned kShift = 0x10;
// GetKeyboardState 的约定：一个键的那一字节高位为 1 表示按着，低位是切换状态。
inline constexpr unsigned char kDownBit = 0x80;
} // namespace KeyboardStateCodes

// 从同一份键盘状态（GetKeyboardState 取到的 256 字节）得出字符与 Shift：translate 用这份状态换算字符，
// Shift 读这份状态里的 VK_SHIFT。
template <typename Translate>
KeyCharacter ReadKeyCharacter(const unsigned char (&keyboardState)[256], Translate &&translate)
{
    KeyCharacter key;
    key.character = translate(keyboardState);
    key.shiftDown = (keyboardState[KeyboardStateCodes::kShift] & KeyboardStateCodes::kDownBit) != 0;
    return key;
}
