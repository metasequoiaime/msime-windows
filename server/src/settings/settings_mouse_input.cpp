#include "settings/settings_mouse_input.h"

#include <windowsx.h>

namespace
{
constexpr WPARAM kButtonKeys = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2;

struct ButtonInfo
{
    WPARAM key;
    UINT32 data;
    COREWEBVIEW2_MOUSE_EVENT_KIND up;
};

constexpr ButtonInfo kButtons[] = {
    {MK_LBUTTON, 0, COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP},
    {MK_RBUTTON, 0, COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP},
    {MK_MBUTTON, 0, COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP},
    {MK_XBUTTON1, XBUTTON1, COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP},
    {MK_XBUTTON2, XBUTTON2, COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP},
};

COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS MouseKeys(WPARAM w_param)
{
    UINT32 keys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE;
    if (w_param & MK_LBUTTON)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON;
    if (w_param & MK_RBUTTON)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_RIGHT_BUTTON;
    if (w_param & MK_MBUTTON)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_MIDDLE_BUTTON;
    if (w_param & MK_SHIFT)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_SHIFT;
    if (w_param & MK_CONTROL)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_CONTROL;
    if (w_param & MK_XBUTTON1)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_X_BUTTON1;
    if (w_param & MK_XBUTTON2)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_X_BUTTON2;
    return static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(keys);
}
} // namespace

SettingsMouseCommands SettingsMouseInput::OnMessage(UINT message, WPARAM w_param, LPARAM l_param)
{
    SettingsMouseCommands commands;
    SettingsMouseEvent event;
    bool press = false;
    bool release = false;
    switch (message)
    {
    case WM_MOUSEMOVE:
        event.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
        // 抓取期间光标在窗口外，登记离开跟踪只会让系统在每次移动后都补一个 WM_MOUSELEAVE。
        commands.track_leave = !Capturing();
        break;
    case WM_MOUSELEAVE:
        // 拖到窗口外时不能告诉页面鼠标离开了，Chromium 会据此结束滚动条拖动；
        // 等最后一个键抬起后由 track_leave 重新判断。
        if (Capturing())
        {
            commands.handled = true;
            return commands;
        }
        event.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE;
        break;
    case WM_LBUTTONDOWN:
        event.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN;
        commands.focus = true;
        press = true;
        break;
    case WM_LBUTTONUP:
        event.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
        release = true;
        break;
    case WM_LBUTTONDBLCLK:
        event.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOUBLE_CLICK;
        press = true;
        break;
    case WM_RBUTTONDOWN:
        event.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN;
        press = true;
        break;
    case WM_RBUTTONUP:
        event.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
        release = true;
        break;
    case WM_RBUTTONDBLCLK:
        event.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOUBLE_CLICK;
        press = true;
        break;
    case WM_MBUTTONDOWN:
        event.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN;
        press = true;
        break;
    case WM_MBUTTONUP:
        event.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP;
        release = true;
        break;
    case WM_MBUTTONDBLCLK:
        event.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOUBLE_CLICK;
        press = true;
        break;
    case WM_XBUTTONDOWN:
        event.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOWN;
        event.data = GET_XBUTTON_WPARAM(w_param);
        press = true;
        break;
    case WM_XBUTTONUP:
        event.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP;
        event.data = GET_XBUTTON_WPARAM(w_param);
        release = true;
        break;
    case WM_XBUTTONDBLCLK:
        event.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOUBLE_CLICK;
        event.data = GET_XBUTTON_WPARAM(w_param);
        press = true;
        break;
    case WM_MOUSEWHEEL:
        event.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL;
        event.data = static_cast<UINT32>(GET_WHEEL_DELTA_WPARAM(w_param));
        event.screen_point = true;
        break;
    case WM_MOUSEHWHEEL:
        event.kind = COREWEBVIEW2_MOUSE_EVENT_KIND_HORIZONTAL_WHEEL;
        event.data = static_cast<UINT32>(GET_WHEEL_DELTA_WPARAM(w_param));
        event.screen_point = true;
        break;
    default:
        return commands;
    }
    event.keys = MouseKeys(w_param);
    event.point = {GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
    commands.handled = true;
    commands.events.push_back(event);

    // w_param 带的是消息发生后的按键状态：按下时包含刚按的键，抬起时已经去掉刚放开的键。
    const WPARAM buttons = w_param & kButtonKeys;
    if (press)
    {
        commands.capture = true;
        held_ = buttons;
    }
    else if (release && Capturing())
    {
        held_ = buttons;
        if (!Capturing())
        {
            commands.release_capture = true;
            commands.track_leave = true;
        }
    }
    return commands;
}

SettingsMouseCommands SettingsMouseInput::OnCaptureLost(POINT client_point)
{
    SettingsMouseCommands commands;
    if (!Capturing())
        return commands;
    commands.handled = true;
    for (const ButtonInfo &button : kButtons)
    {
        if (!(held_ & button.key))
            continue;
        held_ &= ~button.key;
        commands.events.push_back({button.up, MouseKeys(held_), button.data, client_point, false});
    }
    commands.track_leave = true;
    return commands;
}
