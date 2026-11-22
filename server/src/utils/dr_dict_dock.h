#pragma once

#include <windows.h>

#include <string>

// 英文候选模式里让 DR Dict 的查词弹窗停靠在候选窗旁边，显示高亮候选的音标与释义（协议见
// dr_dict_dock_policy.h）。请求在自己的线程上发：DR Dict 正忙时不会卡住候选窗。只发最新的状态，
// 和上次发出的一样就不发。DR Dict 没在运行时什么都不做，也不去启动它。
namespace DrDictDock
{
void Start();
void Stop();
// 界面线程在候选窗显示或移动之后调用：word 是高亮的英文候选，card 是候选卡片的屏幕物理像素矩形。
// word 为空等同于 Hide。
void Show(const std::wstring &word, const RECT &card);
// 候选窗隐藏，或高亮的不再是英文候选。
void Hide();
} // namespace DrDictDock
