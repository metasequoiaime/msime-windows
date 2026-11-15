#pragma once

#include <cstdint>

// DR Dict 词典联动的两个快捷键。
//
// - Ctrl+Shift+Enter：查询高亮候选（有候选时）。TSF 在吃键预判里按它决定这颗键要不要交给 Server，
//   Server 按它进查词分支，两边必须是同一份判定：TSF 吃了而 Server 不认，键就落进普通回车分支把预编辑上屏了。
// - Ctrl+Alt+Enter：查询当前预编辑（候选窗显示时）。只由 Server 的低级键盘钩子识别，TSF 不认它：
//   Edge / Chrome 不把按着 Alt 的键交给 TSF，单按 Alt 时 Windows 又把后续键当 WM_SYSKEYDOWN，TSF
//   同样收不到。钩子在候选窗不显示时放行，应用自己的 Ctrl+Alt+Enter 照常可用。
//
// 也别选会被别的程序注册成全局热键的组合（RegisterHotKey 抢在应用和输入法之前）。
//
// 修饰位与管道里的 ModifiersDown 低三位一致：Shift = 1、Ctrl = 2、Alt = 4。键码直接写数值，
// 不依赖 Windows SDK。
namespace FanyImeDictionaryLookupKeys
{
inline constexpr uint32_t kVirtualKeyReturn = 0x0D;
inline constexpr uint32_t kModifierMask = 0b111u;
inline constexpr uint32_t kCtrlShift = 0b011u;
inline constexpr uint32_t kCtrlAlt = 0b110u;

constexpr bool IsCandidateLookupKey(uint32_t keycode, uint32_t modifiers)
{
    return keycode == kVirtualKeyReturn && (modifiers & kModifierMask) == kCtrlShift;
}

constexpr bool IsPreeditLookupKey(uint32_t keycode, uint32_t modifiers)
{
    return keycode == kVirtualKeyReturn && (modifiers & kModifierMask) == kCtrlAlt;
}
} // namespace FanyImeDictionaryLookupKeys
