#pragma once

#include <windows.h>
#include <WebView2.h>

#include <vector>

// 设置窗口把鼠标消息转发给 WebView2 composition controller 时的抓取状态机。
// 只做决策不调 Win32：调用方按返回的指令设置焦点、抓取或释放鼠标，再把事件交给 controller。
struct SettingsMouseEvent
{
    COREWEBVIEW2_MOUSE_EVENT_KIND kind{};
    COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS keys{};
    UINT32 data = 0;
    POINT point{};
    // 滚轮消息带的是屏幕坐标，转发前要换算成客户区坐标。
    bool screen_point = false;
};

// 执行顺序：focus、capture、转发 events、release_capture、track_leave。
struct SettingsMouseCommands
{
    bool handled = false;
    bool focus = false;
    bool capture = false;
    std::vector<SettingsMouseEvent> events;
    // 只在窗口仍持有抓取时释放，转发途中抓取被别人接走就不要再抢回来。
    bool release_capture = false;
    // 重新登记 TME_LEAVE；光标已在窗口外时系统会立刻补一个 WM_MOUSELEAVE。
    bool track_leave = false;
};

class SettingsMouseInput
{
  public:
    SettingsMouseCommands OnMessage(UINT message, WPARAM w_param, LPARAM l_param);
    // 抓取被系统或别的窗口拿走（WM_CAPTURECHANGED）时，页面收不到真正的抬起，
    // 这里给仍按着的键补发抬起，否则滚动条拖动、文字选择会卡在按下状态。
    SettingsMouseCommands OnCaptureLost(POINT client_point);
    bool Capturing() const
    {
        return held_ != 0;
    }

  private:
    // 在页面里按下、还没转发抬起的 MK_* 键。
    WPARAM held_ = 0;
};
