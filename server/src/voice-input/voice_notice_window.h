#pragma once

#include <string>

// 语音输入的提示框，替代系统 MessageBox：后者的字体跟系统界面语言走，英文系统上中文渲染很差，
// 外观也和输入法其它界面对不上。这里用 Direct2D 自绘，配色和字体与语音结果卡片同一套。
enum class VoiceNoticeKind
{
    // 出错了，只给「知道了」。
    Error,
    // 缺配置（没填 Token、本地模型没下载），多一个「打开设置」直达语音输入页。
    SettingsNeeded,
};

// 任意线程可调，立即返回：提示框在自己的线程上显示和泵消息，绝不阻塞调用方（语音控制线程，
// 弹窗挂着时下一次语音的指令队列不能被卡住）。它不抢焦点，用户手里的输入框保持激活；同一时刻
// 只留一个，新的提示顶替屏幕上的旧提示。
void ShowVoiceNoticeWindow(VoiceNoticeKind kind, std::wstring message);
