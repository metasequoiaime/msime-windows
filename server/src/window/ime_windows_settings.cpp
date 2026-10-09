// 设置窗口宿主：前台激活与有限次重试、自绘标题栏的非客户区与最大化按钮、
// 向 composition WebView 转发鼠标输入，以及 WndProcSettingsWindow。
#include "window/ime_windows_internal.h"
#include "config/ime_config.h"
#include "ipc/ipc.h"
#include "ime_windows.h"
#include "defines/defines.h"
#include "defines/globals.h"
#include <cmath>
#include <string>
#include "webview2/windows_webview2.h"
#include "utils/common_utils.h"
#include "utils/window_utils.h"
#include "ipc/event_listener.h"
#include <windowsx.h>

using namespace ime_windows_detail;

namespace
{
int g_settings_activation_retries_remaining = 0;

void ScheduleSettingsWindowActivation(HWND hwnd)
{
    // Mouse activation and Alt+Tab complete asynchronously. A foreground
    // change can therefore overwrite a single SetForegroundWindow call made
    // while handling the click. Retry only for a short bounded interval and
    // stop immediately once Windows confirms this HWND as foreground.
    g_settings_activation_retries_remaining = 6;
    PostMessage(hwnd, WM_ACTIVATE_SETTINGS_WINDOW, 0, 0);
    SetTimer(hwnd, TIMER_ID_SETTINGS_ACTIVATION_RETRY, 50, nullptr);
}

void CancelSettingsWindowActivation(HWND hwnd)
{
    g_settings_activation_retries_remaining = 0;
    KillTimer(hwnd, TIMER_ID_SETTINGS_ACTIVATION_RETRY);
}
} // namespace

bool ActivateSettingsWindow(HWND hwnd)
{
    if (!IsWindow(hwnd))
    {
        return false;
    }

    if (IsIconic(hwnd))
    {
        ShowWindow(hwnd, SW_RESTORE);
    }
    else
    {
        ShowWindow(hwnd, SW_SHOW);
    }

    const HWND foreground = GetForegroundWindow();
    const DWORD current_thread = GetCurrentThreadId();
    const DWORD foreground_thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const bool should_attach_input = foreground_thread != 0 && foreground_thread != current_thread;
    bool input_attached = false;

    if (should_attach_input)
    {
        input_attached = AttachThreadInput(current_thread, foreground_thread, TRUE) != FALSE;
    }

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);

    if (input_attached)
    {
        AttachThreadInput(current_thread, foreground_thread, FALSE);
    }

    // Detaching can restore some thread-local state, so verify foreground and
    // reassert this thread's active/focus state after the shared queue has been
    // separated again.
    if (GetForegroundWindow() != hwnd)
    {
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
    }
    SetActiveWindow(hwnd);
    SetFocus(hwnd);

    return GetForegroundWindow() == hwnd;
}

void RequestSettingsWindowActivation(HWND hwnd)
{
    if (!IsWindow(hwnd))
    {
        return;
    }

    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
    ScheduleSettingsWindowActivation(hwnd);
}

int GetTopNcInsetForWindow(HWND hwnd)
{
    const UINT dpi = GetDpiForWindow(hwnd);
    const DWORD style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_EXSTYLE));

    RECT withCaption{0, 0, 0, 0};
    RECT withoutCaption{0, 0, 0, 0};

    AdjustWindowRectExForDpi(&withCaption, style, FALSE, exStyle, dpi);
    AdjustWindowRectExForDpi(&withoutCaption, style & ~WS_CAPTION, FALSE, exStyle, dpi);

    const int captionInset = withoutCaption.top - withCaption.top;
    const int frameInset = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);

    return captionInset + frameInset;
}

bool IsPointInMaximizeButtonSettingsWnd(HWND hwnd, POINT screenPoint)
{
    if (!hasMaximizeButtonRectSettingsWnd)
    {
        return false;
    }

    POINT clientPoint = screenPoint;
    ScreenToClient(hwnd, &clientPoint);
    return PtInRect(&maximizeButtonRectSettingsWnd, clientPoint) != FALSE;
}

void PostMaximizeButtonEventSettingsWnd(const char *eventName)
{
    if (!::webviewSettingsWnd || !eventName)
    {
        return;
    }

    std::string ev(eventName);
    std::string payload = R"({"type":"maxButtonEvent","data":{"event":")" + ev + R"("}})";
    const std::wstring message = string_to_wstring(payload);
    ::webviewSettingsWnd->PostWebMessageAsJson(message.c_str());
}

UINT32 GetMouseVirtualKeysSettingsWnd(WPARAM wParam)
{
    UINT32 keys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE;
    if (wParam & MK_LBUTTON)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON;
    if (wParam & MK_RBUTTON)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_RIGHT_BUTTON;
    if (wParam & MK_MBUTTON)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_MIDDLE_BUTTON;
    if (wParam & MK_SHIFT)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_SHIFT;
    if (wParam & MK_CONTROL)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_CONTROL;
    if (wParam & MK_XBUTTON1)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_X_BUTTON1;
    if (wParam & MK_XBUTTON2)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_X_BUTTON2;
    return keys;
}

bool ForwardMouseMessageToWebViewSettingsWnd(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (!::webviewCompositionControllerSettingsWnd)
        return false;

    COREWEBVIEW2_MOUSE_EVENT_KIND kind{};
    UINT32 mouseData = 0;
    bool handled = true;

    switch (message)
    {
    case WM_MOUSEMOVE:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
        break;
    case WM_LBUTTONDOWN:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN;
        break;
    case WM_LBUTTONUP:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
        break;
    case WM_LBUTTONDBLCLK:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOUBLE_CLICK;
        break;
    case WM_RBUTTONDOWN:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN;
        break;
    case WM_RBUTTONUP:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
        break;
    case WM_RBUTTONDBLCLK:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOUBLE_CLICK;
        break;
    case WM_MBUTTONDOWN:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN;
        break;
    case WM_MBUTTONUP:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP;
        break;
    case WM_MBUTTONDBLCLK:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOUBLE_CLICK;
        break;
    case WM_XBUTTONDOWN:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOWN;
        mouseData = GET_XBUTTON_WPARAM(wParam);
        break;
    case WM_XBUTTONUP:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP;
        mouseData = GET_XBUTTON_WPARAM(wParam);
        break;
    case WM_XBUTTONDBLCLK:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOUBLE_CLICK;
        mouseData = GET_XBUTTON_WPARAM(wParam);
        break;
    case WM_MOUSEWHEEL:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL;
        mouseData = static_cast<UINT32>(GET_WHEEL_DELTA_WPARAM(wParam));
        break;
    case WM_MOUSEHWHEEL:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_HORIZONTAL_WHEEL;
        mouseData = static_cast<UINT32>(GET_WHEEL_DELTA_WPARAM(wParam));
        break;
    case WM_MOUSELEAVE:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE;
        break;
    default:
        handled = false;
        break;
    }

    if (!handled)
        return false;

    POINT point{};
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
    {
        point.x = GET_X_LPARAM(lParam);
        point.y = GET_Y_LPARAM(lParam);
        ScreenToClient(hwnd, &point);
    }
    else
    {
        point.x = GET_X_LPARAM(lParam);
        point.y = GET_Y_LPARAM(lParam);
    }

    if (message == WM_MOUSEMOVE)
    {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
    }

    ::webviewCompositionControllerSettingsWnd->SendMouseInput(
        kind, static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(GetMouseVirtualKeysSettingsWnd(wParam)), mouseData,
        point);
    return true;
}

LRESULT CALLBACK WndProcSettingsWindow(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_ACTIVATE_SETTINGS_WINDOW:
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
        {
            CancelSettingsWindowActivation(hwnd);
            return 0;
        }
        ActivateSettingsWindow(hwnd);
        return 0;
    case WM_MOUSEACTIVATE:
        // The click itself gives Windows permission to activate this window.
        // Request foreground synchronously, before button-down, but do not use
        // ActivateSettingsWindow here: temporarily attaching the two input
        // queues interferes with subsequent mouse activations. Deferring this
        // work can also split the down/up pair forwarded to WebView.
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        return MA_ACTIVATE;
    case WM_ACTIVATE: {
        const LRESULT result = DefWindowProc(hwnd, message, wParam, lParam);
        if (LOWORD(wParam) != WA_INACTIVE)
        {
            // Restoring from the taskbar or switching back from another app
            // activates the top-level HWND without necessarily producing a new
            // WM_SETFOCUS. Re-establish both the host and Composition WebView
            // focus only on that real activation transition.
            if (GetFocus() != hwnd)
            {
                SetFocus(hwnd);
            }
            else if (::webviewControllerSettingsWnd)
            {
                ::webviewControllerSettingsWnd->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            }
        }
        return result;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE)
        {
            CancelSettingsWindowActivation(hwnd);
        }
        if ((wParam & 0xFFF0) == SC_RESTORE)
        {
            const LRESULT result = DefWindowProc(hwnd, message, wParam, lParam);
            ScheduleSettingsWindowActivation(hwnd);
            return result;
        }
        break;
    case WM_TIMER: {
        if (wParam == TIMER_ID_SETTINGS_ACTIVATION_RETRY)
        {
            if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || GetForegroundWindow() == hwnd ||
                g_settings_activation_retries_remaining <= 0)
            {
                CancelSettingsWindowActivation(hwnd);
            }
            else
            {
                --g_settings_activation_retries_remaining;
                PostMessage(hwnd, WM_ACTIVATE_SETTINGS_WINDOW, 0, 0);
            }
        }
        else if (wParam == TIMER_ID_CONFIG_SYNC)
        {
            const SchemeType previous_input_scheme = GetConfiguredActiveInputScheme();
            const bool previous_trilingual_cycle = GetConfiguredTrilingualCycleEnabled();
            const std::string previous_shuangpin_schema = GetConfiguredShuangpinSchema();
            const std::string previous_character_set = GetConfiguredCharacterSet();
            const std::string previous_layout = GetConfiguredCandidateWindowLayout();
            const std::string previous_candidate_skin = GetConfiguredCandidateSkin();
            const bool previous_floating_toolbar = GetConfiguredFloatingToolbarEnabled();
            const bool previous_caret_state_indicator = GetConfiguredCaretStateIndicatorEnabled();
            const FloatingToolbarItemsConfig previous_floating_toolbar_items = GetConfiguredFloatingToolbarItems();
            const double previous_floating_toolbar_scale = GetConfiguredFloatingToolbarScale();
            const int previous_floating_toolbar_font_size = GetConfiguredFloatingToolbarFontSize();
            const bool previous_floating_toolbar_auto_hide = GetConfiguredFloatingToolbarAutoHide();
            const int previous_floating_toolbar_auto_hide_delay = GetConfiguredFloatingToolbarAutoHideDelay();
            const bool previous_cloud_candidates = GetConfiguredCloudCandidatesEnabled();
            const bool previous_comma_period = GetConfiguredPagingCommaPeriodEnabled();
            const bool previous_smart_punctuation = GetConfiguredSmartPunctuationEnabled();
            const bool previous_smart_punctuation_space_convert = GetConfiguredSmartPunctuationSpaceConvertEnabled();
            const bool previous_smart_punctuation_direct_digit = GetConfiguredSmartPunctuationDirectDigitEnabled();
            const bool previous_smart_punctuation_direct_letter = GetConfiguredSmartPunctuationDirectLetterEnabled();
            const bool previous_smart_punctuation_repeat_to_chinese =
                GetConfiguredSmartPunctuationRepeatToChineseEnabled();
            const bool previous_paired_punctuation = GetConfiguredPairedPunctuationEnabled();
            const std::string previous_punctuation_lock = GetConfiguredPunctuationLock();
            const bool previous_tsf_diagnostic_log = GetConfiguredTsfDiagnosticLogEnabled();
            const bool previous_statistics_enabled = GetConfiguredStatisticsEnabled();
            const std::wstring previous_mid_sentence_helpcode = FormatMidSentenceHelpcodeWorkerPayload();
            const std::wstring previous_mid_sentence_helpcode_semicolon =
                FormatMidSentenceHelpcodeSemicolonWorkerPayload();
            const std::wstring previous_direct_helpcode = FormatDirectHelpcodeWorkerPayload();
            const std::wstring previous_v_mode = FormatVModeWorkerPayload();
            const std::wstring previous_mid_sentence_helpcode_uppercase =
                FormatMidSentenceHelpcodeUppercaseWorkerPayload();
            const std::wstring previous_paging_worker_payload = FormatPagingCommaPeriodWorkerPayload();
            const std::string previous_theme_mode = GetConfiguredThemeMode();
            const std::string previous_theme_cand = GetConfiguredThemeCand();
            const std::string previous_theme_ftb = GetConfiguredThemeFtb();
            const std::string previous_theme_menu = GetConfiguredThemeMenu();
            const std::string previous_font = GetConfiguredCandidateFont();
            const std::string previous_english_font = GetConfiguredCandidateEnglishFont();
            const std::string previous_default_font = GetConfiguredCandidateDefaultFont();
            const int previous_font_size = GetConfiguredCandidateFontSize();
            const int previous_preedit_font_size = GetConfiguredCandidateWindowPreeditFontSize();
            const std::string previous_cand_text_color = GetConfiguredCandidateTextColor();
            if (ReloadImeConfigIfChanged())
            {
                FanyNamedPipe::EnqueueApplyCandidatePageSizeTask();
                if (previous_input_scheme != GetConfiguredActiveInputScheme())
                {
                    ApplyConfiguredInputScheme();
                }
                else if (previous_shuangpin_schema != GetConfiguredShuangpinSchema())
                {
                    ApplyConfiguredShuangpinSchema();
                }
                if (previous_input_scheme == GetConfiguredActiveInputScheme() &&
                    previous_trilingual_cycle != GetConfiguredTrilingualCycleEnabled())
                    BroadcastTrilingualCycleState();
                if (previous_character_set != GetConfiguredCharacterSet())
                {
                    UpdateFtbCharacterSetState(::webviewFtbWnd);
                    FanyNamedPipe::EnqueueRefreshCandidatePageTask();
                }
                if (previous_layout != GetConfiguredCandidateWindowLayout())
                {
                    ApplyConfiguredCandidateWindowLayout();
                }
                if (previous_candidate_skin != GetConfiguredCandidateSkin() ||
                    previous_theme_mode != GetConfiguredThemeMode() ||
                    previous_theme_cand != GetConfiguredThemeCand() || previous_theme_ftb != GetConfiguredThemeFtb() ||
                    previous_theme_menu != GetConfiguredThemeMenu())
                {
                    ApplyConfiguredUiThemes();
                }
                else if (previous_font != GetConfiguredCandidateFont() ||
                         previous_english_font != GetConfiguredCandidateEnglishFont() ||
                         previous_default_font != GetConfiguredCandidateDefaultFont() ||
                         previous_font_size != GetConfiguredCandidateFontSize() ||
                         previous_preedit_font_size != GetConfiguredCandidateWindowPreeditFontSize() ||
                         previous_cand_text_color != GetConfiguredCandidateTextColor())
                {
                    ApplyConfiguredCandidateAppearance();
                }
                if (previous_floating_toolbar != GetConfiguredFloatingToolbarEnabled())
                {
                    RestartFloatingToolbarAutoHide(L"config-sync");
                    SyncMenuFloatingToolbarToggle();
                }
                else if (previous_floating_toolbar_auto_hide != GetConfiguredFloatingToolbarAutoHide() ||
                         previous_floating_toolbar_auto_hide_delay != GetConfiguredFloatingToolbarAutoHideDelay())
                {
                    RestartFloatingToolbarAutoHide(L"config-sync-auto-hide");
                }
                if (previous_caret_state_indicator != GetConfiguredCaretStateIndicatorEnabled() &&
                    !GetConfiguredCaretStateIndicatorEnabled() && ::global_hwnd_caret_state)
                {
                    PostMessage(::global_hwnd_caret_state, WM_HIDE_CARET_STATE, 0, 0);
                }
                if (!FloatingToolbarItemsEqual(previous_floating_toolbar_items, GetConfiguredFloatingToolbarItems()))
                {
                    ApplyConfiguredFloatingToolbarItems();
                }
                else if (std::fabs(previous_floating_toolbar_scale - GetConfiguredFloatingToolbarScale()) > 0.001 ||
                         previous_floating_toolbar_font_size != GetConfiguredFloatingToolbarFontSize())
                {
                    ApplyConfiguredFloatingToolbarSize();
                }
                if (previous_cloud_candidates && !GetConfiguredCloudCandidatesEnabled())
                {
                    FanyNamedPipe::CancelCloudCandidateRequest();
                }
                if (previous_comma_period != GetConfiguredPagingCommaPeriodEnabled() ||
                    previous_paging_worker_payload != FormatPagingCommaPeriodWorkerPayload())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::PagingCommaPeriodChanged,
                        FormatPagingCommaPeriodWorkerPayload());
                }
                if (previous_smart_punctuation != GetConfiguredSmartPunctuationEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationChanged,
                        GetConfiguredSmartPunctuationEnabled() ? L"1" : L"0");
                }
                if (previous_smart_punctuation_space_convert != GetConfiguredSmartPunctuationSpaceConvertEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationSpaceConvertChanged,
                        GetConfiguredSmartPunctuationSpaceConvertEnabled() ? L"1" : L"0");
                }
                if (previous_smart_punctuation_direct_digit != GetConfiguredSmartPunctuationDirectDigitEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationDirectDigitChanged,
                        GetConfiguredSmartPunctuationDirectDigitEnabled() ? L"1" : L"0");
                }
                if (previous_smart_punctuation_direct_letter != GetConfiguredSmartPunctuationDirectLetterEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationDirectLetterChanged,
                        GetConfiguredSmartPunctuationDirectLetterEnabled() ? L"1" : L"0");
                }
                if (previous_smart_punctuation_repeat_to_chinese !=
                    GetConfiguredSmartPunctuationRepeatToChineseEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationRepeatToChineseChanged,
                        GetConfiguredSmartPunctuationRepeatToChineseEnabled() ? L"1" : L"0");
                }
                if (previous_paired_punctuation != GetConfiguredPairedPunctuationEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::PairedPunctuationChanged,
                        GetConfiguredPairedPunctuationEnabled() ? L"1" : L"0");
                }
                if (previous_punctuation_lock != GetConfiguredPunctuationLock())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::PunctuationLockChanged,
                        FormatPunctuationLockWorkerPayload());
                    const std::string &punctuation_lock = GetConfiguredPunctuationLock();
                    if (punctuation_lock == "chinese")
                    {
                        UpdateFtbPuncState(::webviewFtbWnd, 1);
                    }
                    else if (punctuation_lock == "english")
                    {
                        UpdateFtbPuncState(::webviewFtbWnd, 0);
                    }
                }
                if (previous_tsf_diagnostic_log != GetConfiguredTsfDiagnosticLogEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::TsfDiagnosticLogChanged,
                        GetConfiguredTsfDiagnosticLogEnabled() ? L"1" : L"0");
                }
                if (previous_statistics_enabled != GetConfiguredStatisticsEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::StatisticsEnabledChanged,
                        GetConfiguredStatisticsEnabled() ? L"1" : L"0");
                }
                if (previous_mid_sentence_helpcode != FormatMidSentenceHelpcodeWorkerPayload())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::MidSentenceHelpcodeChanged,
                        FormatMidSentenceHelpcodeWorkerPayload());
                }
                if (previous_mid_sentence_helpcode_semicolon != FormatMidSentenceHelpcodeSemicolonWorkerPayload())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::MidSentenceHelpcodeSemicolonChanged,
                        FormatMidSentenceHelpcodeSemicolonWorkerPayload());
                }
                if (previous_direct_helpcode != FormatDirectHelpcodeWorkerPayload())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::DirectHelpcodeChanged,
                        FormatDirectHelpcodeWorkerPayload());
                }
                if (previous_v_mode != FormatVModeWorkerPayload())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(Global::DataFromServerMsgTypeToTsfWorkerThread::VModeChanged,
                                                           FormatVModeWorkerPayload());
                }
                if (previous_mid_sentence_helpcode_uppercase != FormatMidSentenceHelpcodeUppercaseWorkerPayload())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::MidSentenceHelpcodeUppercaseChanged,
                        FormatMidSentenceHelpcodeUppercaseWorkerPayload());
                }
                PostSettingsConfig();
            }
            FanyNamedPipe::EnqueueEnsureInputSessionMatchesConfigTask();
            ApplyConfiguredCandidateSkinIfChanged();
        }
        else if (wParam == TIMER_ID_MOVE_WEBVIEW_SETTINGS)
        {
            KillTimer(hwnd, TIMER_ID_MOVE_WEBVIEW_SETTINGS);
            if (::webviewSettingsWnd)
            {
                // 放在屏幕右下角
                // 获取主屏幕尺寸
                MonitorCoordinates coordinates = GetMainMonitorCoordinates();
                // 获取窗口尺寸
                RECT rect;
                GetWindowRect(hwnd, &rect);
                // 获取任务栏高度
                int taskbarHeight = GetTaskbarHeight();
                // 移动窗口
                SetWindowPos(                                                              //
                    hwnd,                                                                  //
                    0,                                                                     //
                    coordinates.right / 2 - (rect.right - rect.left) / 2,                  //
                    coordinates.bottom / 2 - (rect.bottom - rect.top) / 2 - taskbarHeight, //
                    0,                                                                     //
                    0,                                                                     //
                    SWP_NOSIZE | SWP_HIDEWINDOW);
                break;
            }
            else
            {
                // 如果 webview 还没准备好，再等一会
                SetTimer(hwnd, TIMER_ID_MOVE_WEBVIEW_SETTINGS, 100, nullptr);
            }
        }
        break;
    }
    case WM_NCCALCSIZE: {
        if (wParam)
        {
            NCCALCSIZE_PARAMS *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(lParam);
            const LRESULT defResult = DefWindowProc(hwnd, message, wParam, lParam);

            if (!IsZoomed(hwnd))
            {
                params->rgrc[0].top -= GetTopNcInsetForWindow(hwnd);
            }
            else
            {
                params->rgrc[0].top -= GetSystemMetrics(SM_CYCAPTION);
            }

            return defResult;
        }
        break;
    }
    case WM_NCHITTEST: {
        const LRESULT result = DefWindowProcW(hwnd, message, wParam, lParam);
        if (result == HTCLIENT && hasMaximizeButtonRectSettingsWnd)
        {
            POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (IsPointInMaximizeButtonSettingsWnd(hwnd, screenPoint))
            {
                return HTMAXBUTTON;
            }
        }
        return result;
    }
    case WM_MOVE:
    case WM_MOVING:
        if (::webviewControllerSettingsWnd)
        {
            ::webviewControllerSettingsWnd->NotifyParentWindowPositionChanged();
        }
        break;
    case WM_SETFOCUS:
        if (::webviewControllerSettingsWnd)
        {
            ::webviewControllerSettingsWnd->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
        break;
    case WM_MOUSEMOVE:
    case WM_MOUSELEAVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        if (ForwardMouseMessageToWebViewSettingsWnd(hwnd, message, wParam, lParam))
        {
            // Start the bounded foreground retry only after WebView has
            // received the complete click. Scheduling from WM_MOUSEACTIVATE
            // can split the down/up pair; scheduling here also handles a real
            // foreground application (such as Chrome) reclaiming activation
            // shortly after the click. Hidden windows cancel the retry above.
            if (message == WM_LBUTTONUP)
            {
                ScheduleSettingsWindowActivation(hwnd);
            }
            if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP || message == WM_XBUTTONDBLCLK)
            {
                return TRUE;
            }
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (::webviewCompositionControllerSettingsWnd && LOWORD(lParam) == HTCLIENT)
        {
            UINT32 cursorId = 0;
            if (SUCCEEDED(::webviewCompositionControllerSettingsWnd->get_SystemCursorId(&cursorId)) && cursorId != 0)
            {
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(cursorId)));
                return TRUE;
            }
        }
        break;
    case WM_NCMOUSEMOVE: {
        POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (IsPointInMaximizeButtonSettingsWnd(hwnd, screenPoint))
        {
            if (!isMaximizeButtonHoverSettingsWnd)
            {
                isMaximizeButtonHoverSettingsWnd = true;
                PostMaximizeButtonEventSettingsWnd("enter");
            }

            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE | TME_NONCLIENT;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            return 0;
        }
        break;
    }
    case WM_NCMOUSELEAVE:
        if (isMaximizeButtonHoverSettingsWnd)
        {
            isMaximizeButtonHoverSettingsWnd = false;
            PostMaximizeButtonEventSettingsWnd("leave");
        }
        break;
    case WM_NCLBUTTONDOWN: {
        POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (IsPointInMaximizeButtonSettingsWnd(hwnd, screenPoint))
        {
            PostMaximizeButtonEventSettingsWnd("down");
            return 0;
        }
        break;
    }
    case WM_NCLBUTTONUP: {
        POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (IsPointInMaximizeButtonSettingsWnd(hwnd, screenPoint))
        {
            PostMaximizeButtonEventSettingsWnd("up");
            return 0;
        }
        break;
    }
    case WM_CLOSE: {
        // 不销毁窗口，只隐藏
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HBRUSH darkBrush = CreateSolidBrush(RGB(32, 32, 32));
        FillRect(hdc, &rc, darkBrush);
        DeleteObject(darkBrush);
        return 1;
    }
    case WM_GETMINMAXINFO: {
        auto *mmi = reinterpret_cast<MINMAXINFO *>(lParam);
        // 设置 settings 窗口最小可拖拽尺寸
        mmi->ptMinTrackSize.x = 1200;
        mmi->ptMinTrackSize.y = 800;
        return 0;
    }
    case WM_SIZE: {
        if (wParam == SIZE_MINIMIZED)
        {
            CancelSettingsWindowActivation(hwnd);
        }
        if (::webviewControllerSettingsWnd)
        {
            RECT rect;
            GetClientRect(hwnd, &rect);
            webviewControllerSettingsWnd->put_Bounds(rect);
            PostSettingsWindowState(hwnd);
        }
        break;
    }
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}
