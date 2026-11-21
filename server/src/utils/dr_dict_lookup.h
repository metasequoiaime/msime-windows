#pragma once

#include <windows.h>

#include <optional>
#include <string>

// DR Dict 词典联动：在 DR Dict 的查词弹窗里查询一个词（协议见 dr_dict_lookup_policy.h）。
namespace DrDictLookup
{
// 在后台线程里查询 word，立即返回：DR Dict 卡住时 SendMessageTimeout 最多等 3 秒，启动 DR Dict
// 也要时间，都不能占着候选窗的界面线程或按键工作线程。word 整理后为空时什么也不做。

// 鼠标唤起（候选右键菜单）：弹窗开在屏幕点 at（物理像素）旁边，按鼠标位置自动关闭。
void LookupAtPointer(std::wstring word, POINT at);
// 键盘唤起（查词快捷键）：有光标行矩形 caret 就开在它下方（放不下时上方），没有就退回鼠标位置；
// 两种都不按鼠标位置自动关闭。
void LookupFromKeyboard(std::wstring word, std::optional<RECT> caret);
} // namespace DrDictLookup
