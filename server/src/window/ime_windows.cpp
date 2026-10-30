#include "global/globals.h"
#include "config/ime_config.h"
#include "ipc/ipc.h"
#include "ime_windows.h"
#include "window/candidate_presenter.h"
#include "window/floating_toolbar_presenter.h"
#include "window/tray_menu_presenter.h"
#include "defines/defines.h"
#include "defines/globals.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <fmt/xchar.h>
#include "utils/window_utils.h"
#include <dwmapi.h>
#include "ipc/event_listener.h"
#include "window_hook.h"
#include "window/caret_state_indicator.h"
#include "window/caret_state_indicator_policy.h"
#include "log/candidate_diag_log.h"
#include "resource/resource.h"
#include "window/ime_windows_internal.h"

#pragma comment(lib, "dwmapi.lib")

using namespace ime_windows_detail;

namespace ime_windows_detail
{
bool g_is_ime_active = false;
int g_last_placed_caret_x = Global::INVALID_Y;
int g_last_placed_caret_y = Global::INVALID_Y;
bool g_candidate_session_anchor_valid = false;
POINT g_candidate_session_anchor{};
// SetWindowPos on the candidate host can synchronously deliver WM_DPICHANGED on
// the same call stack (hide, cross-monitor moves). Applying the suggested rect
// from that handler races the hide and can recurse until the UI thread stalls —
// seen when switching apps.
int g_candidate_dpi_change_suppress_count = 0;

// WM_SETTINGCHANGE lParam points to "ImmersiveColorSet" when the user flips
// the Windows app light/dark theme. The presenters own their colors, so they
// have to be told.
bool IsSystemLightDarkToggle(UINT message, LPARAM lParam)
{
    return message == WM_SETTINGCHANGE && lParam != 0 &&
           lstrcmpiW(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0;
}

// The small-window hosts start shown but DWM-cloaked, and every hide of the
// candidate window is a cloak as well: each presenter uncloaks its host when it
// actually has something to show.
void SetHostWindowCloaked(HWND hwnd, bool cloaked)
{
    if (!hwnd)
    {
        return;
    }
    BOOL value = cloaked ? TRUE : FALSE;
    const HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &value, sizeof(value));
    if (hwnd == ::global_hwnd)
    {
        CAND_DIAG_LOGF(L"candidate-frame cloak requested={} hr={:#x} actual={} tick={}", cloaked,
                       static_cast<unsigned>(hr), IsHostWindowCloaked(hwnd), GetTickCount64());
    }
}

// A cloaked host is still "visible" to IsWindowVisible and still reports its
// rect to window enumeration, so the trace has to distinguish the two.
bool IsHostWindowCloaked(HWND hwnd)
{
    DWORD cloaked = 0;
    if (!hwnd || FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
    {
        return false;
    }
    return cloaked != 0;
}
} // namespace ime_windows_detail

namespace
{
// Who cloaked it matters: the app value is ours, while shell/inherited means
// something outside this process (virtual desktop switch, shell policy) took
// the toolbar off screen and no amount of ShowWindow will bring it back.
std::wstring DescribeCloak(DWORD cloaked)
{
    if (cloaked == 0)
    {
        return L"no";
    }
    std::wstring description;
    if (cloaked & DWM_CLOAKED_APP)
    {
        description += L"app,";
    }
    if (cloaked & DWM_CLOAKED_SHELL)
    {
        description += L"shell,";
    }
    if (cloaked & DWM_CLOAKED_INHERITED)
    {
        description += L"inherited,";
    }
    if (description.empty())
    {
        return L"yes(" + std::to_wstring(cloaked) + L")";
    }
    description.back() = L')';
    return L"yes(" + description;
}

// A host can be fully transparent while every ordinary check says it is fine.
// Only a handful of things produce that, and none of them are visible from
// IsWindowVisible: a host still DWM-cloaked is excluded from composition while
// input routing continues normally, a host parked off every monitor has nowhere
// to paint, and a host that became layered without its alpha paints nothing at
// all. The small-window hosts are not layered, so layered attributes only show
// up here when something set the style behind our back. Record all of them
// together so a blank window can be attributed instead of guessed at.
std::wstring DescribeHostWindowState(HWND hwnd)
{
    if (!hwnd)
    {
        return L"host=none";
    }

    RECT window_rect{};
    GetWindowRect(hwnd, &window_rect);
    RECT client_rect{};
    GetClientRect(hwnd, &client_rect);

    DWORD cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));

    COLORREF color_key = 0;
    BYTE alpha = 0;
    DWORD layered_flags = 0;
    const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const bool layered = (ex_style & WS_EX_LAYERED) != 0;
    const bool layered_ok = layered && GetLayeredWindowAttributes(hwnd, &color_key, &alpha, &layered_flags) != FALSE;

    return fmt::format(L"rect=({},{},{}x{}) client={}x{} on_monitor={} visible={} cloaked={} "
                       L"layered_attrs={} alpha={} lwa_flags={:#x} ex_layered={} ex_topmost={}",
                       window_rect.left, window_rect.top, window_rect.right - window_rect.left,
                       window_rect.bottom - window_rect.top, client_rect.right, client_rect.bottom,
                       MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) != nullptr, IsWindowVisible(hwnd) != FALSE,
                       DescribeCloak(cloaked), !layered ? L"n/a" : (layered_ok ? L"ok" : L"MISSING"),
                       static_cast<unsigned>(alpha), layered_flags, layered, (ex_style & WS_EX_TOPMOST) != 0);
}

} // namespace

namespace ime_windows_detail
{
std::wstring DescribeFloatingToolbarHostState()
{
    return DescribeHostWindowState(::global_hwnd_ftb);
}
} // namespace ime_windows_detail

void SetCandidateHostCloaked(bool cloaked)
{
    SetHostWindowCloaked(::global_hwnd, cloaked);
}

std::wstring DescribeCandidateHostState()
{
    return fmt::format(L"logical_shown={} {}", ::is_global_wnd_cand_shown, DescribeHostWindowState(::global_hwnd));
}

std::wstring DescribeTrayMenuHostState()
{
    return DescribeHostWindowState(::global_hwnd_menu);
}

namespace
{

// Shown on a real monitor but DWM-cloaked: the user sees nothing until the
// presenter uncloaks the host for its first real show.
void ShowHostWindowCloaked(HWND hwnd)
{
    if (!hwnd)
    {
        return;
    }
    SetHostWindowCloaked(hwnd, true);
    ShowWindow(hwnd, SW_SHOWNA);
    UpdateWindow(hwnd);
}
} // namespace

void ApplyConfiguredInputScheme()
{
    FanyNamedPipe::EnqueueReloadInputSessionTask();
    UpdateFtbInputModeState(GetActiveInputMode() == "japanese" ? 1 : 0);
    BroadcastConfiguredInputModeState();
}

void ApplyConfiguredShuangpinSchema()
{
    FanyNamedPipe::EnqueueReloadInputSessionTask();
    BroadcastToTsfWorkerThreadViaNamedpipe(Global::DataFromServerMsgTypeToTsfWorkerThread::MicrosoftShuangpinChanged,
                                           IsConfiguredShuangpinSemicolonFinal() ? L"1" : L"0");
}

LRESULT RegisterCandidateWindowMessage()
{

    WM_SHOW_MAIN_WINDOW = RegisterWindowMessage(L"WM_SHOW_MAIN_WINDOW");
    WM_HIDE_MAIN_WINDOW = RegisterWindowMessage(L"WM_HIDE_MAIN_WINDOW");
    WM_MOVE_CANDIDATE_WINDOW = RegisterWindowMessage(L"WM_MOVE_CANDIDATE_WINDOW");
    return 0;
}

LRESULT RegisterIMEWindowsClass(WNDCLASSEX &wcex, HINSTANCE hInstance)
{
    //
    // 注册窗口类
    //
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_IME_ICON));
    wcex.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_IME_ICON));
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    /* We do not need background color, otherwise it will flash when rendering */
    wcex.hbrBackground = NULL;
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, IDI_APPLICATION);

    if (!RegisterClassEx(&wcex))
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return 1;
    }
    return 0;
}

int CreateCandidateWindow(HINSTANCE hInstance)
{
    //
    // 候选框窗口
    // Create on a real monitor and show while DWM-cloaked; the presenter sizes
    // and uncloaks it on the first show.
    //
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif
    DWORD dwExStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    FLOAT scale = GetForegroundWindowScale();
    const int candWidth = (std::max)(8, static_cast<int>(32 * scale));
    const int candHeight = (std::max)(8, static_cast<int>(32 * scale));

    HWND hwnd_cand = CreateWindowEx( //
        dwExStyle,                   //
        szWindowClass,               //
        lpWindowNameCand,            //
        WS_POPUP,                    //
        100,                         //
        100,                         //
        candWidth,                   //
        candHeight,                  //
        nullptr,                     //
        nullptr,                     //
        hInstance,                   //
        nullptr                      //
    );                               //

    if (!hwnd_cand)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return 1;
    }
    else
    {
        BOOL disableTransitions = TRUE;
        DwmSetWindowAttribute(hwnd_cand, DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions,
                              sizeof(disableTransitions));
    }

    ::global_hwnd = hwnd_cand;
    CandidatePresenter::Instance().Bind(hwnd_cand);
    CAND_DIAG_LOGF(L"candidate host created {}", DescribeCandidateHostState());

    // A client can activate while the pipe server is already listening but this
    // window does not exist yet, which is the race that leaves the toolbar
    // hidden after a server restart. The queued message waits for the loop that
    // starts once the remaining windows are up.
    FanyNamedPipe::ReplayDeferredClientActivation();

    //
    // 任务栏托盘区的菜单窗口
    //
    // Like the candidate window, the presenter's DirectComposition visual is
    // the only content. A layered host also kept a GDI redirection surface that
    // nothing paints: after a resize it could hold stale video memory, which
    // showed through the transparent shadow margin.
    dwExStyle = WS_EX_NOREDIRECTIONBITMAP |                   //
                WS_EX_TOOLWINDOW |                            //
                WS_EX_NOACTIVATE;                             //
                                                              // WS_EX_TOPMOST;              //
    HWND hwnd_menu = CreateWindowEx(                          //
        dwExStyle,                                            //
        szWindowClass,                                        //
        lpWindowNameMenu,                                     //
        WS_POPUP,                                             //
        200,                                                  //
        200,                                                  //
        static_cast<int>((::MENU_WINDOW_WIDTH)*scale),        //
        static_cast<int>((::MENU_WINDOW_HEIGHT * 2) * scale), //
        nullptr,                                              //
        nullptr,                                              //
        hInstance,                                            //
        nullptr                                               //
    );                                                        //
    if (!hwnd_menu)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return 1;
    }
    ::global_hwnd_menu = hwnd_menu;
    TrayMenuPresenter::Instance().Bind(hwnd_menu);

    //
    // floating toolbar 窗口
    //
    ::FTB_WND_WIDTH = ConfiguredFloatingToolbarWidth();
    ::FTB_WND_HEIGHT = ConfiguredFloatingToolbarHeight();
    // Same as the tray menu: no redirection surface under the composition
    // visual. The auto-hide fade goes through the presenter's opacity.
    dwExStyle = WS_EX_NOREDIRECTIONBITMAP | //
                WS_EX_TOOLWINDOW |          //
                WS_EX_NOACTIVATE;           //
                                            // WS_EX_TOPMOST;                               //
    MonitorCoordinates ftbMonitor = GetMainMonitorCoordinates();
    const int ftbShadowWidth = GetConfiguredFloatingToolbarShadow() ? ::FTB_WND_SHADOW_WIDTH : 0;
    const int ftbWidth = static_cast<int>((::FTB_WND_WIDTH + ftbShadowWidth) * scale);
    const int ftbHeight = static_cast<int>((::FTB_WND_HEIGHT + ftbShadowWidth) * scale);
    const int ftbTaskbarHeight = GetTaskbarHeight();
    const int ftbCornerInset = static_cast<int>(std::lround(10.0 * static_cast<double>(scale > 0 ? scale : 1.0f)));
    const int ftbX = ftbMonitor.right - ftbWidth - ftbCornerInset;
    const int ftbY = ftbMonitor.bottom - ftbHeight - ftbTaskbarHeight - ftbCornerInset;
    // The caret badge is optional: without its window every post is a no-op,
    // so a creation failure must not take the input windows down with it.
    HWND hwnd_caret_state =
        CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, szWindowClass,
                        lpWindowNameCaretState, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hInstance, nullptr);
    if (hwnd_caret_state)
    {
        ::global_hwnd_caret_state = hwnd_caret_state;
        SetLayeredWindowAttributes(hwnd_caret_state, 0, 245, LWA_ALPHA);
        ShowWindow(hwnd_caret_state, SW_HIDE);
    }

    HWND hwnd_ftb = CreateWindowEx( //
        dwExStyle,                  //
        szWindowClass,              //
        lpWindowNameFtb,            //
        WS_POPUP,                   //
        ftbX,                       //
        ftbY,                       //
        ftbWidth,                   //
        ftbHeight,                  //
        nullptr,                    //
        nullptr,                    //
        hInstance,                  //
        nullptr                     //
    );                              //
    if (!hwnd_ftb)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return 1;
    }
    ::global_hwnd_ftb = hwnd_ftb;
    FanyNamedPipe::RegisterStatusSnapshotWindow(hwnd_ftb);
    FloatingToolbarPresenter::Instance().Bind(hwnd_ftb);

    // Cloaked show: the hosts are on a real monitor but invisible to the user
    // until their presenters uncloak them.
    ShowHostWindowCloaked(hwnd_cand);
    ShowHostWindowCloaked(hwnd_menu);
    ShowHostWindowCloaked(hwnd_ftb);
    ApplyConfiguredFloatingToolbarVisibility(L"startup");
    UpdateWindow(hwnd_ftb);

    /* 监听文本配置文件变化，并同步到运行中的候选框。Settings 已是独立进程。 */
    SetTimer(hwnd_cand, TIMER_ID_CONFIG_SYNC, 300, nullptr);

    /* floating toolbar：再确认一次落在主屏右下角 */
    SetTimer(hwnd_ftb, TIMER_ID_PLACE_FTB, 200, nullptr);

    //
    // 注册一下全局钩子
    //
    InitServerCapsLockState();
    g_hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
    if (!g_hHook)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return 1;
    }
#ifdef FANY_DEBUG
    (void)0;
#endif

    // The hook lives for the whole process; nothing ever unhooks it, so the handle is not kept.
    SetWinEventHook(                 //
        EVENT_SYSTEM_FOREGROUND,     //
        EVENT_OBJECT_LOCATIONCHANGE, //
        nullptr,                     //
        WinEventProc,                //
        0,                           //
        0,                           //
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // UIPI filters this hook for windows of higher integrity, so a focused Task
    // Manager or an elevated console never reports a foreground change. Poll as
    // the fallback; the event above stays the fast path for everything else.
    if (::global_hwnd_ftb && IsWindow(::global_hwnd_ftb))
    {
        SetTimer(::global_hwnd_ftb, TIMER_ID_FTB_FOREGROUND_POLL, kFloatingToolbarForegroundPollMs, nullptr);
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    /* 卸载钩子 */
    UnhookWindowsHookEx(g_hHook);

    return (int)msg.wParam;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_SHOWWINDOW && hwnd == ::global_hwnd)
    {
        CAND_DIAG_LOGF(L"host WM_SHOWWINDOW showing={} reason={} {}", wParam != FALSE,
                       static_cast<unsigned long long>(lParam), DescribeCandidateHostState());
    }

    if (message == WM_APPLY_IME_CONFIG || message == WM_APPLY_IME_INPUT_SCHEME)
    {
        if (hwnd != ::global_hwnd && ::global_hwnd && IsWindow(::global_hwnd))
        {
            PostMessageW(::global_hwnd, message, wParam, lParam);
            return 0;
        }
    }

    /* 候选窗口 */
    if (hwnd == ::global_hwnd)
    {
        return WndProcCandWindow(hwnd, message, wParam, lParam);
    }

    /* tray icon 菜单窗口 */
    if (hwnd == ::global_hwnd_menu)
    {
        return WndProcMenuWindow(hwnd, message, wParam, lParam);
    }

    /* floating toolbar 窗口 */
    if (hwnd == ::global_hwnd_ftb)
    {
        return WndProcFtbWindow(hwnd, message, wParam, lParam);
    }
    if (hwnd == ::global_hwnd_caret_state)
    {
        return WndProcCaretStateWindow(hwnd, message, wParam, lParam);
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}

namespace
{
void DiscardPendingCaretStateRequests(HWND hwnd)
{
    MSG pending{};
    while (PeekMessageW(&pending, hwnd, WM_SHOW_CARET_STATE, WM_SHOW_CARET_STATE, PM_REMOVE))
        delete reinterpret_cast<CaretStateIndicator::ShowRequest *>(pending.lParam);
    while (PeekMessageW(&pending, hwnd, WM_MOVE_CARET_STATE, WM_MOVE_CARET_STATE, PM_REMOVE))
        delete reinterpret_cast<POINT *>(pending.lParam);
}
} // namespace

LRESULT CALLBACK WndProcCaretStateWindow(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (IsSystemLightDarkToggle(message, lParam))
    {
        // The palette follows the next Show; a stale badge simply goes away.
        CaretStateIndicator::Hide(hwnd);
        ::is_global_wnd_caret_state_shown = false;
        return 0;
    }

    switch (message)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        CaretStateIndicator::Paint(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (CaretStateIndicator::HandleTimer(hwnd, wParam))
        {
            ::is_global_wnd_caret_state_shown = false;
            return 0;
        }
        break;
    case WM_SHOW_CARET_STATE: {
        std::unique_ptr<CaretStateIndicator::ShowRequest> request(
            reinterpret_cast<CaretStateIndicator::ShowRequest *>(lParam));
        if (!request ||
            !FanyImeUi::ShouldShowCaretStateIndicator(GetConfiguredCaretStateIndicatorEnabled(), g_is_ime_active,
                                                      request->uiLess, request->caret.x, request->caret.y) ||
            IsForegroundD3DExclusiveFullscreen())
        {
            CaretStateIndicator::Hide(hwnd);
            ::is_global_wnd_caret_state_shown = false;
            return 0;
        }
        const bool topmost = EnsureSmallWindowsTopmost(L"show-caret-state");
        ::is_global_wnd_caret_state_shown = CaretStateIndicator::Show(hwnd, request->badge, request->caret, topmost);
        return 0;
    }
    case WM_MOVE_CARET_STATE: {
        std::unique_ptr<POINT> caret(reinterpret_cast<POINT *>(lParam));
        if (::is_global_wnd_caret_state_shown &&
            (!caret || !FanyImeUi::IsUsableCaretAnchor(caret->x, caret->y) ||
             !CaretStateIndicator::Reposition(hwnd, *caret, EnsureSmallWindowsTopmost(L"move-caret-state"))))
        {
            CaretStateIndicator::Hide(hwnd);
            ::is_global_wnd_caret_state_shown = false;
        }
        return 0;
    }
    case WM_HIDE_CARET_STATE:
        CaretStateIndicator::Hide(hwnd);
        // Posted messages are FIFO. Do not discard shows queued after this
        // hide: they belong to a newer state switch on the same UI thread.
        ::is_global_wnd_caret_state_shown = false;
        return 0;
    case WM_NCDESTROY:
        ::global_hwnd_caret_state = nullptr;
        DiscardPendingCaretStateRequests(hwnd);
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}
