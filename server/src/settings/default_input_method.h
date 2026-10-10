#pragma once

// 把水杉设为当前用户的默认输入法（系统设置「高级键盘设置 → 替代默认输入法」）。
//
// 只在设置页用户主动点击时调用，绝不在启动或升级时静默改写。写入的是 HKCU 下的
// 每用户设置，设置进程以当前用户身份运行即可，不需要提权。
//
// 写入走 input.dll 的公开函数 InstallLayoutOrTip / SetDefaultLayoutOrTip。读取不用同属
// input.dll 的 EnumEnabledLayoutOrTip：它在 Windows 10/11 上对 TSF 输入法返回空列表，
// 判断不了默认项，因此直接读系统设置页自己维护的 International\User Profile。

#include <Windows.h>

#include <string>

namespace default_input_method
{

struct Status
{
    bool enabled = false;    // 水杉在当前用户的键盘列表里
    bool is_default = false; // 水杉是当前用户的默认输入法
};

Status QueryStatus();

// 先确保水杉在键盘列表里，再设为默认；完成后按注册表复核。失败时 error 给出人类可读原因。
bool SetAsDefault(std::string &error);

// 打开系统的「高级键盘设置」，供 API 失败或用户想手动调整时使用。
void OpenSystemSettings(HWND owner);

} // namespace default_input_method
