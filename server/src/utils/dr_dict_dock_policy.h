#pragma once

// 英文候选模式里把 DR Dict 的查词弹窗停靠在候选窗旁边：纯函数部分（请求负载、要不要重发）。
//
// 协议来自 DR Dict（app/src/popup.h 的 dock_request、docs/architecture.md「供其他程序调用」）：
//   - 向窗口类 DrDictPopup 发 WM_COPYDATA，dwData = 0x4B434F44（"DOCK"），数据是 UTF-8 JSON；
//   - {"word": "...", "rect": [left, top, right, bottom]}：查这个词，弹窗贴着 rect（候选窗可见卡片的
//     屏幕物理像素矩形）的右边，右侧放不下时贴左边，并按工作区退避；同一个词只换 rect 时只移动；
//   - {"hide": true}：候选窗关了，收起停靠中的弹窗（用户固定住的不收）。
// 停靠的弹窗不抢焦点、不记查询历史。旧版 DR Dict 不认这种请求，回 FALSE，什么也不发生。

#include "utils/dr_dict_lookup_policy.h"

#include <cstdint>
#include <string>

namespace DrDictDock
{
inline constexpr std::uintptr_t kDockRequest = 0x4B434F44; // "DOCK"

// 想让弹窗处于的状态：visible 为假时收起，word 与 rect 不用。
struct State
{
    bool visible = false;
    std::wstring word;
    DrDictLookup::CaretRect rect;
};

inline bool SameState(const State &a, const State &b)
{
    if (a.visible != b.visible)
        return false;
    if (!a.visible)
        return true;
    return a.word == b.word && a.rect.left == b.rect.left && a.rect.top == b.rect.top && a.rect.right == b.rect.right &&
           a.rect.bottom == b.rect.bottom;
}

// 词沿用查词联动的整理与 UTF-8 转义（只是少了 x/y），负载里不会出现裸控制字符或孤立代理项。
inline std::string BuildPayload(const State &state)
{
    if (!state.visible)
        return "{\"hide\":true}";
    std::string escaped;
    for (const char c : DrDictLookup::ToUtf8(DrDictLookup::NormalizeWord(state.word)))
    {
        if (c == '"' || c == '\\')
        {
            escaped.push_back('\\');
            escaped.push_back(c);
        }
        else if (static_cast<unsigned char>(c) < 0x20)
        {
            escaped.push_back(' ');
        }
        else
        {
            escaped.push_back(c);
        }
    }
    return "{\"word\":\"" + escaped + "\",\"rect\":[" + std::to_string(state.rect.left) + "," +
           std::to_string(state.rect.top) + "," + std::to_string(state.rect.right) + "," +
           std::to_string(state.rect.bottom) + "]}";
}
} // namespace DrDictDock
