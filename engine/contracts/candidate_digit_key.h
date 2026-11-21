#pragma once

#include <cstdint>

// 哪些键算候选数字键（按数字选词）。TSF 在吃键预判里按它把键归到选词，Server 按它进选词分支，
// 两边必须一致：曾经 Server 把 0 当选词键、TSF 却把 0 当成结束组合，按 0 就把预编辑清空了。
//
// 主键盘和小键盘的 0~9 都算。0 落在本页第一个候选之前，Server 回 OutofRange，既不上屏也不取消组合。
// 键码直接写数值，不引用 VK_NUMPAD0/9，这样不依赖 Windows SDK 的独立测试也能包含它。
namespace FanyImeCandidateDigitKey
{
inline constexpr uint32_t kNumpad0 = 0x60;
inline constexpr uint32_t kNumpad9 = 0x69;

constexpr bool IsCandidateDigitKey(uint32_t keycode)
{
    return (keycode >= static_cast<uint32_t>('0') && keycode <= static_cast<uint32_t>('9')) ||
           (keycode >= kNumpad0 && keycode <= kNumpad9);
}
} // namespace FanyImeCandidateDigitKey
