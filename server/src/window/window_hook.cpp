#include "window_hook.h"
#include "config/ime_config.h"
#include "ime_windows.h"
#include "settings/settings_launcher.h"
#include "utils/window_utils.h"
#include "window/floating_toolbar_presenter.h"
#include "defines/globals.h"
#include "defines/defines.h"
#include "watchdog/watchdog_protocol.h"
#include "log/ftb_diag_log.h"
#include <atomic>
#include <fmt/xchar.h>
#include <winuser.h>

void OnWinEvent(HWND hwnd, DWORD event);

namespace
{
std::atomic<int> g_caps_lock{0};
// Low-level hooks get every typematic repeat of a held key with no repeat
// flag, but Windows toggles Caps Lock only on the first press.
bool g_caps_key_held = false;

void PublishServerCapsLockState(int enabled)
{
    if (::global_hwnd_ftb && IsWindow(::global_hwnd_ftb))
    {
        PostMessage(::global_hwnd_ftb, UPDATE_FTB_CAPS_LOCK, static_cast<WPARAM>(enabled), 0);
    }
}
} // namespace

void InitServerCapsLockState()
{
    g_caps_lock.store((GetKeyState(VK_CAPITAL) & 0x0001) != 0 ? 1 : 0, std::memory_order_relaxed);
}

int GetServerCapsLockState()
{
    return g_caps_lock.load(std::memory_order_relaxed);
}

void SyncServerCapsLockState(bool enabled)
{
    const int next = enabled ? 1 : 0;
    if (g_caps_lock.exchange(next, std::memory_order_relaxed) != next)
    {
        PublishServerCapsLockState(next);
    }
}

bool IsKeyPressed(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void RestartServerProcess()
{
    if (g_hHook)
        UnhookWindowsHookEx(g_hHook);
    ExitProcess(WatchdogProtocol::kRestartExitCode);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) &&
        reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam)->vkCode == VK_CAPITAL)
    {
        g_caps_key_held = false;
    }

    // When Alt is held, Windows normally reports the following non-system key
    // as WM_SYSKEYDOWN rather than WM_KEYDOWN.  Handle both; otherwise all of
    // the Ctrl+Shift+Alt shortcuts depend on modifier press order.
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
    {
        KBDLLHOOKSTRUCT *p = (KBDLLHOOKSTRUCT *)lParam;

        if (p->vkCode == VK_CAPITAL && (p->flags & LLKHF_UP) == 0 && !g_caps_key_held)
        {
            g_caps_key_held = true;
            const int next = g_caps_lock.load(std::memory_order_relaxed) ? 0 : 1;
            g_caps_lock.store(next, std::memory_order_relaxed);
            PublishServerCapsLockState(next);
        }

        bool ctrl = IsKeyPressed(VK_CONTROL);
        bool alt = IsKeyPressed(VK_MENU);
        bool shift = IsKeyPressed(VK_SHIFT);
        bool win = IsKeyPressed(VK_LWIN) || IsKeyPressed(VK_RWIN);

        // Ctrl + Shift + Win + K opens the standalone screen keyboard. Consume the
        // shortcut so the foreground application does not receive a stray K.
        if (ctrl && shift && win && !alt && p->vkCode == 'K')
        {
            OpenKeyboardPanelApplication();
            return 1;
        }

        //
        // Ctrl + Shift + Alt + T to terminate
        //
        if (ctrl && shift && alt && p->vkCode == 'T')
        {
            CloseSettingsApplication();
            CloseEmojiPanelApplication();
            CloseKeyboardPanelApplication();
            CloseHandwritingPanelApplication();
            ExitProcess(WatchdogProtocol::kStopExitCode);
        }

        //
        // Ctrl + Shift + Alt + R to restart
        //
        if (ctrl && shift && alt && p->vkCode == 'R')
        {
            RestartServerProcess();
        }

        //
        // Ctrl + Shift + Alt + C to clear ime engine cache
        //
        if (ctrl && shift && alt && p->vkCode == 'C')
        {
            PostMessage(::global_hwnd, WM_CLEAR_IME_ENGINE_CACHE, 0, 0);
        }

        //
        // Ctrl + Alt + Shift + 1-8 to delete candidate
        //
        if (!::is_global_wnd_cand_shown)
        {
            return CallNextHookEx(g_hHook, nCode, wParam, lParam);
        }

        if (ctrl && alt && shift)
        {
            // 只处理主键盘数字
            int idx = -1;
            if (p->vkCode >= '1' && p->vkCode <= '8')
                idx = p->vkCode - '1';

            if (idx >= 0)
            {
// 执行候选项删除逻辑，PostMessage 给窗口过程去执行
#ifdef FANY_DEBUG
                (void)0;
#endif
                PostMessage(::global_hwnd, WM_DELETE_CANDIDATE, idx + 1, 0);
                // This is an IME command, not text intended for the foreground
                // application.  Consuming it also prevents an Alt+number side
                // effect in the application.
                return 1;
            }
        }
    }

    // 放行其他程序
    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0)
    {
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN)
        {
            MSLLHOOKSTRUCT *p = (MSLLHOOKSTRUCT *)lParam;
            POINT pt = p->pt;

            RECT rc;
            GetWindowRect(global_hwnd_menu, &rc);
            if (!PtInRect(&rc, pt))
            {
                FTB_DIAG_LOGF(L"menu hide on click-outside {}", DescribeTrayMenuHostState());
                ShowWindow(global_hwnd_menu, SW_HIDE);
                UnhookWindowsHookEx(g_mouseHook);
                g_mouseHook = nullptr;
            }
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD)
{
    if (idObject != OBJID_WINDOW || !hwnd)
        return;

    if (event == EVENT_OBJECT_LOCATIONCHANGE || event == EVENT_SYSTEM_FOREGROUND)
    {
        OnWinEvent(hwnd, event);
    }
}

static bool g_isHiddenDueToFullscreen = false;

void OnWinEvent(HWND hwnd, DWORD event)
{
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd))
        return;

    // Stay out of the way while the toolbar cannot paint: without a presenter
    // the host is only ever cloaked, so the check below would act on a toolbar
    // that is not on screen and leave g_isHiddenDueToFullscreen set for a
    // restore nobody asked for.
    if (!FloatingToolbarPresenter::Instance().IsBound())
        return;

    hwnd = GetForegroundWindow();
    bool nowFullscreen = CheckFullscreen(hwnd);
    // bool wasFullscreen = g_fullscreen_states[hwnd];

    if (nowFullscreen)
    {
        if (IsWindowVisible(::global_hwnd_ftb))
        {
            // This hides the toolbar for as long as the foreground window keeps
            // looking fullscreen, so a false positive here is indistinguishable
            // from the toolbar failing to appear. Name the window that caused
            // it: an overlay from a screenshot tool or a borderless app is very
            // hard to tell apart from a real fullscreen client afterwards.
            wchar_t foreground_class[64] = {};
            GetClassNameW(hwnd, foreground_class, ARRAYSIZE(foreground_class));
            RECT foreground_rect{};
            GetWindowRect(hwnd, &foreground_rect);
            FTB_DIAG_LOGF(L"fullscreen detected: class={} rect=({},{},{}x{}) -> hiding toolbar", foreground_class,
                          foreground_rect.left, foreground_rect.top, foreground_rect.right - foreground_rect.left,
                          foreground_rect.bottom - foreground_rect.top);
            HideFloatingToolbarHost();
            g_isHiddenDueToFullscreen = true;
        }
    }
    else
    {
        if (g_isHiddenDueToFullscreen)
        {
            ApplyConfiguredFloatingToolbarVisibility(L"fullscreen-exit");
            g_isHiddenDueToFullscreen = false;
        }
    }

    // Ownership belongs to the focused window, so only a real foreground change
    // can answer it. Location changes fire per caret move and stay cheap.
    if (event == EVENT_SYSTEM_FOREGROUND)
    {
        ScheduleFloatingToolbarForegroundReconcile();
    }

    // g_fullscreen_states[hwnd] = nowFullscreen;
}
