#include "settings/settings_mouse_input.h"

#include "tests/includes/test_framework.h"

namespace
{
struct Button
{
    UINT down;
    UINT up;
    UINT double_click;
    WORD key;
    WORD x_button;
    COREWEBVIEW2_MOUSE_EVENT_KIND up_kind;
};

const Button kButtons[] = {
    {WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK, MK_LBUTTON, 0, COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP},
    {WM_RBUTTONDOWN, WM_RBUTTONUP, WM_RBUTTONDBLCLK, MK_RBUTTON, 0, COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP},
    {WM_MBUTTONDOWN, WM_MBUTTONUP, WM_MBUTTONDBLCLK, MK_MBUTTON, 0, COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP},
    {WM_XBUTTONDOWN, WM_XBUTTONUP, WM_XBUTTONDBLCLK, MK_XBUTTON1, XBUTTON1, COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP},
    {WM_XBUTTONDOWN, WM_XBUTTONUP, WM_XBUTTONDBLCLK, MK_XBUTTON2, XBUTTON2, COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP},
};

WPARAM ButtonParam(WORD keys, WORD x_button)
{
    return MAKEWPARAM(keys, x_button);
}
} // namespace

TEST_CASE(settings_mouse_press_captures_before_forwarding)
{
    for (const Button &button : kButtons)
    {
        for (const UINT message : {button.down, button.double_click})
        {
            SettingsMouseInput input;
            const SettingsMouseCommands commands =
                input.OnMessage(message, ButtonParam(button.key, button.x_button), MAKELPARAM(10, 20));
            REQUIRE(commands.handled);
            REQUIRE(commands.capture);
            REQUIRE(!commands.release_capture);
            REQUIRE_EQ(commands.focus, message == WM_LBUTTONDOWN);
            REQUIRE_EQ(commands.events.size(), size_t{1});
            REQUIRE_EQ(commands.events[0].data, UINT32{button.x_button});
            REQUIRE(input.Capturing());
        }
    }
}

TEST_CASE(settings_mouse_drag_outside_window_keeps_capture_and_defers_leave)
{
    SettingsMouseInput input;
    input.OnMessage(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 20));

    SettingsMouseCommands move = input.OnMessage(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(-20, -30));
    REQUIRE_EQ(move.events.size(), size_t{1});
    REQUIRE_EQ(move.events[0].kind, COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE);
    REQUIRE_EQ(move.events[0].keys, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON);
    REQUIRE_EQ(move.events[0].point.x, -20L);
    REQUIRE_EQ(move.events[0].point.y, -30L);
    REQUIRE(!move.track_leave);

    // 拖到窗口外时的离开消息被吞掉，不能让页面以为拖动结束了。
    SettingsMouseCommands leave = input.OnMessage(WM_MOUSELEAVE, 0, 0);
    REQUIRE(leave.handled);
    REQUIRE(leave.events.empty());

    SettingsMouseCommands up = input.OnMessage(WM_LBUTTONUP, 0, MAKELPARAM(-20, -30));
    REQUIRE_EQ(up.events.size(), size_t{1});
    REQUIRE_EQ(up.events[0].kind, COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP);
    REQUIRE(up.release_capture);
    REQUIRE(up.track_leave);
    REQUIRE(!input.Capturing());

    // 抬起后系统补发的离开消息照常转发。
    SettingsMouseCommands late_leave = input.OnMessage(WM_MOUSELEAVE, 0, 0);
    REQUIRE_EQ(late_leave.events.size(), size_t{1});
    REQUIRE_EQ(late_leave.events[0].kind, COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE);
}

TEST_CASE(settings_mouse_hover_tracks_leave)
{
    SettingsMouseInput input;
    REQUIRE(input.OnMessage(WM_MOUSEMOVE, 0, MAKELPARAM(1, 2)).track_leave);
    SettingsMouseCommands leave = input.OnMessage(WM_MOUSELEAVE, 0, 0);
    REQUIRE_EQ(leave.events.size(), size_t{1});
    REQUIRE_EQ(leave.events[0].kind, COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE);
}

TEST_CASE(settings_mouse_last_button_up_releases_capture)
{
    for (const Button &button : kButtons)
    {
        SettingsMouseInput input;
        input.OnMessage(button.down, ButtonParam(button.key, button.x_button), 0);
        const SettingsMouseCommands up =
            input.OnMessage(button.up, ButtonParam(MK_SHIFT | MK_CONTROL, button.x_button), 0);
        REQUIRE_EQ(up.events.size(), size_t{1});
        REQUIRE_EQ(up.events[0].kind, button.up_kind);
        REQUIRE_EQ(up.events[0].data, UINT32{button.x_button});
        REQUIRE(up.release_capture);
        REQUIRE(!input.Capturing());
    }
}

TEST_CASE(settings_mouse_keeps_capture_while_another_button_is_held)
{
    for (const Button &released : kButtons)
    {
        for (const Button &held : kButtons)
        {
            if (released.key == held.key)
                continue;
            SettingsMouseInput input;
            input.OnMessage(held.down, ButtonParam(held.key, held.x_button), 0);
            input.OnMessage(released.down, ButtonParam(held.key | released.key, released.x_button), 0);
            REQUIRE(!input.OnMessage(released.up, ButtonParam(held.key, released.x_button), 0).release_capture);
            REQUIRE(input.Capturing());
            REQUIRE(input.OnMessage(held.up, ButtonParam(0, held.x_button), 0).release_capture);
            REQUIRE(!input.Capturing());
        }
    }
}

TEST_CASE(settings_mouse_up_without_press_does_not_release_capture)
{
    SettingsMouseInput input;
    const SettingsMouseCommands up = input.OnMessage(WM_LBUTTONUP, 0, 0);
    REQUIRE(up.handled);
    REQUIRE_EQ(up.events.size(), size_t{1});
    REQUIRE(!up.release_capture);
}

TEST_CASE(settings_mouse_capture_lost_releases_held_buttons_in_page)
{
    SettingsMouseInput input;
    input.OnMessage(WM_LBUTTONDOWN, MK_LBUTTON, 0);
    input.OnMessage(WM_XBUTTONDOWN, ButtonParam(MK_LBUTTON | MK_XBUTTON2, XBUTTON2), 0);

    const SettingsMouseCommands lost = input.OnCaptureLost({-5, 7});
    REQUIRE(lost.handled);
    REQUIRE(!lost.capture);
    REQUIRE(!lost.release_capture);
    REQUIRE(lost.track_leave);
    REQUIRE_EQ(lost.events.size(), size_t{2});
    REQUIRE_EQ(lost.events[0].kind, COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP);
    REQUIRE_EQ(lost.events[0].keys, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_X_BUTTON2);
    REQUIRE_EQ(lost.events[0].point.x, -5L);
    REQUIRE_EQ(lost.events[0].point.y, 7L);
    REQUIRE_EQ(lost.events[1].kind, COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP);
    REQUIRE_EQ(lost.events[1].keys, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE);
    REQUIRE_EQ(lost.events[1].data, UINT32{XBUTTON2});
    REQUIRE(!input.Capturing());

    // 真正的抬起随后才到（比如移动循环结束后），不能再释放一次别人的抓取。
    REQUIRE(!input.OnMessage(WM_LBUTTONUP, 0, 0).release_capture);
}

TEST_CASE(settings_mouse_capture_lost_after_release_is_noop)
{
    SettingsMouseInput input;
    input.OnMessage(WM_LBUTTONDOWN, MK_LBUTTON, 0);
    input.OnMessage(WM_LBUTTONUP, 0, 0);
    const SettingsMouseCommands lost = input.OnCaptureLost({0, 0});
    REQUIRE(!lost.handled);
    REQUIRE(lost.events.empty());
}

TEST_CASE(settings_mouse_wheel_uses_screen_point)
{
    SettingsMouseInput input;
    const SettingsMouseCommands wheel =
        input.OnMessage(WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(300, 400));
    REQUIRE_EQ(wheel.events.size(), size_t{1});
    REQUIRE(wheel.events[0].screen_point);
    REQUIRE_EQ(wheel.events[0].data, UINT32{WHEEL_DELTA});
    REQUIRE(!wheel.capture);
}

TEST_CASE(settings_mouse_ignores_unrelated_messages)
{
    SettingsMouseInput input;
    REQUIRE(!input.OnMessage(WM_CAPTURECHANGED, 0, 0).handled);
    REQUIRE(!input.OnMessage(WM_KEYDOWN, VK_ESCAPE, 0).handled);
}
