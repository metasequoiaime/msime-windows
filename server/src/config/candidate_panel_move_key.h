#pragma once
#include "engine/contracts/key_chord.h"

#include <string_view>

namespace CandidatePanelMoveKey
{
// 默认移动键：裸 ↑ / ↓。
constexpr std::string_view DefaultPrev = "Up";
constexpr std::string_view DefaultNext = "Down";

// 解析面板移动键绑定。空串 = 未绑定（该方向不移动）。允许裸键（方向键、字母、数字、
// F1-F12、标点、Space）或 Ctrl + <键>；Shift 不参与判键，也不出现在绑定里。
// 非法值返回 false 并把 result 置空。
inline bool Parse(std::string_view value, FanyImeKeyChord::Binding &result)
{
    result = {};
    if (value.empty())
    {
        return true;
    }
    unsigned modifiers = 0;
    if (value.substr(0, 5) == "Ctrl+")
    {
        modifiers = FanyImeKeyChord::Control;
        value.remove_prefix(5);
    }
    if (value == "Up")
    {
        result = {0x26, modifiers}; // VK_UP
        return true;
    }
    if (value == "Down")
    {
        result = {0x28, modifiers}; // VK_DOWN
        return true;
    }
    if (value == "Left")
    {
        result = {0x27, modifiers}; // VK_LEFT
        return true;
    }
    if (value == "Right")
    {
        result = {0x29, modifiers}; // VK_RIGHT
        return true;
    }
    const unsigned key = FanyImeKeyChord::KeyCode(value);
    if (!key)
    {
        return false;
    }
    result = {key, modifiers};
    return true;
}

// 与 Binding::Matches 的区别：Shift 只是大小写修饰，不参与移动键判定。
inline bool Matches(const FanyImeKeyChord::Binding &binding, unsigned code, unsigned pressed)
{
    constexpr unsigned kIgnoredModifiers = FanyImeKeyChord::Shift;
    return binding.key != 0 && binding.key == code &&
           (binding.modifiers & ~kIgnoredModifiers) == (pressed & ~kIgnoredModifiers);
}
} // namespace CandidatePanelMoveKey
