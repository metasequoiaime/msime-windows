#include <Windows.h>
#include <windowsx.h>
#include <WebView2.h>

#include "tests/includes/test_framework.h"

namespace
{
enum class Action
{
    Capture,
    Send,
    Release,
};

struct MouseInput
{
    COREWEBVIEW2_MOUSE_EVENT_KIND kind{};
    COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS keys{};
    UINT32 data{};
    POINT point{};
};

struct State
{
    HWND capture{};
    HWND capture_after_send{};
    std::vector<Action> actions;
    MouseInput input;
} state;

// Local API fakes keep the production function from interacting with the user's desktop
HWND FakeSetCapture(HWND hwnd)
{
    const HWND previous = state.capture;
    state.capture = hwnd;
    state.actions.push_back(Action::Capture);
    return previous;
}

HWND FakeGetCapture()
{
    return state.capture;
}

BOOL FakeReleaseCapture()
{
    state.capture = nullptr;
    state.actions.push_back(Action::Release);
    return TRUE;
}

HWND FakeSetFocus(HWND)
{
    return nullptr;
}

BOOL FakeScreenToClient(HWND, POINT *)
{
    return TRUE;
}

BOOL FakeTrackMouseEvent(TRACKMOUSEEVENT *)
{
    return TRUE;
}

struct Controller
{
    HRESULT SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND kind, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS keys, UINT32 data,
                           POINT point)
    {
        state.actions.push_back(Action::Send);
        state.input = {kind, keys, data, point};
        if (state.capture_after_send)
            state.capture = state.capture_after_send;
        return S_OK;
    }
} controller;

Controller *g_composition_controller = &controller;

#define SetCapture FakeSetCapture
#define GetCapture FakeGetCapture
#define ReleaseCapture FakeReleaseCapture
#define SetFocus FakeSetFocus
#define ScreenToClient FakeScreenToClient
#define TrackMouseEvent FakeTrackMouseEvent
#include "settings_mouse_input.inc"
#undef TrackMouseEvent
#undef ScreenToClient
#undef SetFocus
#undef ReleaseCapture
#undef GetCapture
#undef SetCapture

const HWND window = reinterpret_cast<HWND>(1);
const HWND other_window = reinterpret_cast<HWND>(2);

struct Button
{
    UINT down;
    UINT up;
    UINT double_click;
    WORD key;
    WORD x_button;
};

const Button buttons[] = {
    {WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK, MK_LBUTTON, 0},
    {WM_RBUTTONDOWN, WM_RBUTTONUP, WM_RBUTTONDBLCLK, MK_RBUTTON, 0},
    {WM_MBUTTONDOWN, WM_MBUTTONUP, WM_MBUTTONDBLCLK, MK_MBUTTON, 0},
    {WM_XBUTTONDOWN, WM_XBUTTONUP, WM_XBUTTONDBLCLK, MK_XBUTTON1, XBUTTON1},
    {WM_XBUTTONDOWN, WM_XBUTTONUP, WM_XBUTTONDBLCLK, MK_XBUTTON2, XBUTTON2},
};
} // namespace

TEST_CASE(settings_mouse_down_and_double_click_capture_before_forwarding)
{
    for (const auto &button : buttons)
    {
        for (const UINT message : {button.down, button.double_click})
        {
            state = {};
            REQUIRE(ForwardMouse(window, message, MAKEWPARAM(button.key, button.x_button), MAKELPARAM(10, 20)));
            REQUIRE_EQ(state.capture, window);
            REQUIRE_EQ(state.actions, (std::vector<Action>{Action::Capture, Action::Send}));
            REQUIRE_EQ(state.input.data, button.x_button);
        }
    }
}

TEST_CASE(settings_mouse_drag_keeps_capture_outside_window)
{
    state = {};
    REQUIRE(ForwardMouse(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 20)));
    state.actions.clear();
    REQUIRE(ForwardMouse(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(-20, -30)));
    REQUIRE_EQ(state.capture, window);
    REQUIRE_EQ(state.input.kind, COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE);
    REQUIRE_EQ(state.input.keys, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON);
    REQUIRE_EQ(state.input.point.x, -20);
    REQUIRE_EQ(state.input.point.y, -30);
    REQUIRE(ForwardMouse(window, WM_MOUSELEAVE, 0, 0));
    REQUIRE_EQ(state.capture, window);
    REQUIRE_EQ(state.actions, (std::vector<Action>{Action::Send, Action::Send}));
    REQUIRE(ForwardMouse(window, WM_LBUTTONUP, 0, MAKELPARAM(-20, -30)));
    REQUIRE_EQ(state.capture, nullptr);
}

TEST_CASE(settings_mouse_last_button_up_forwards_before_releasing)
{
    for (const auto &button : buttons)
    {
        state = {};
        state.capture = window;
        REQUIRE(ForwardMouse(window, button.up, MAKEWPARAM(MK_SHIFT | MK_CONTROL, button.x_button), 0));
        REQUIRE_EQ(state.capture, nullptr);
        REQUIRE_EQ(state.actions, (std::vector<Action>{Action::Send, Action::Release}));
        REQUIRE_EQ(state.input.data, button.x_button);
    }
}

TEST_CASE(settings_mouse_keeps_capture_while_another_button_is_held)
{
    for (const auto &released : buttons)
    {
        for (const auto &held : buttons)
        {
            if (released.key == held.key)
                continue;
            state = {};
            state.capture = window;
            REQUIRE(ForwardMouse(window, released.up, MAKEWPARAM(held.key, released.x_button), 0));
            REQUIRE_EQ(state.capture, window);
            REQUIRE_EQ(state.actions, (std::vector<Action>{Action::Send}));
            state.actions.clear();
            REQUIRE(ForwardMouse(window, held.up, MAKEWPARAM(0, held.x_button), 0));
            REQUIRE_EQ(state.capture, nullptr);
            REQUIRE_EQ(state.actions, (std::vector<Action>{Action::Send, Action::Release}));
        }
    }
}

TEST_CASE(settings_mouse_up_does_not_release_another_windows_capture)
{
    for (const HWND capture : {static_cast<HWND>(nullptr), other_window})
    {
        state = {};
        state.capture = capture;
        REQUIRE(ForwardMouse(window, WM_LBUTTONUP, 0, 0));
        REQUIRE_EQ(state.capture, capture);
        REQUIRE_EQ(state.actions, (std::vector<Action>{Action::Send}));
    }
}

TEST_CASE(settings_mouse_up_respects_capture_transferred_while_forwarding)
{
    state = {};
    state.capture = window;
    state.capture_after_send = other_window;
    REQUIRE(ForwardMouse(window, WM_LBUTTONUP, 0, 0));
    REQUIRE_EQ(state.capture, other_window);
    REQUIRE_EQ(state.actions, (std::vector<Action>{Action::Send}));
}

TEST_CASE(settings_mouse_ignores_unhandled_messages_and_missing_controller)
{
    state = {};
    REQUIRE(!ForwardMouse(window, WM_CAPTURECHANGED, 0, reinterpret_cast<LPARAM>(other_window)));
    REQUIRE(state.actions.empty());

    g_composition_controller = nullptr;
    const bool handled = ForwardMouse(window, WM_LBUTTONDOWN, MK_LBUTTON, 0);
    g_composition_controller = &controller;
    REQUIRE(!handled);
    REQUIRE(state.actions.empty());
}
