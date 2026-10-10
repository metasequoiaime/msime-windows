#include "windows_webview2.h"
#include "config/ime_config.h"
#include "defines/globals.h"
#include "utils/common_utils.h"
#include "window/candidate_presenter.h"
#include "window/floating_toolbar_presenter.h"
#include "window/tray_menu_presenter.h"
#include <nlohmann/json.hpp>
#include <string>
#include <windows.h>
#include <dwmapi.h>
#include <winuser.h>
#include "defines/defines.h"
#include "global/globals.h"
#include "fmt/xchar.h"
#include "ipc/event_listener.h"
#include "utils/window_utils.h"
#include <WebView2EnvironmentOptions.h>
#include <algorithm>
#include <cmath>
#include <thread>
#include "webview2/windows_webview2_internal.h"

#pragma comment(lib, "dcomp.lib")

using namespace windows_webview2_detail;

std::wstring bodyRes = L"";
std::string loadedCandidateSkin;
std::string loadedFloatingToolbarSkin;
std::string preparedCandidateSkin;
uint64_t candidateSkinReloadRevision = 0;

namespace
{
ComPtr<ICoreWebView2Environment> smallWindowWebviewEnvironment;

HWND smallWindowCandHwnd = nullptr;
HWND smallWindowMenuHwnd = nullptr;
HWND smallWindowFtbHwnd = nullptr;

enum class SmallWindowInitState
{
    Idle,
    InProgress,
    Ready,
    Failed
};

SmallWindowInitState smallWindowInitState = SmallWindowInitState::Idle;
int smallWindowInitAttempts = 0;
bool pendingTrayMenuShow = false;
bool pendingCandidateShow = false;
} // namespace

namespace windows_webview2_detail
{
bool floatingToolbarNavigationReady = false;
// After NavigationCompleted, keep the host shown briefly so a cold WebView2
// user-data folder can finish first paint; then reconcile hide/show for real.
bool floatingToolbarPaintGraceActive = false;
bool floatingToolbarNavigationRetryUsed = false;
} // namespace windows_webview2_detail

namespace
{
// WebView2 rejects a CreateCoreWebView2Controller request with E_INVALIDARG when
// another controller creation on the same environment is still in flight. The
// three small windows must therefore be brought up strictly one at a time.
bool smallWindowControllerRequestInFlight = false;
ULONGLONG smallWindowControllerRequestStartTick = 0;
constexpr ULONGLONG kSmallWindowControllerRequestTimeoutMs = 15000;
constexpr int kMaxSmallWindowInitAttempts = 12;
constexpr UINT_PTR kRetrySmallWindowWebviewTimerId = 9001;
// Avoid remasure storms when HTML keeps reporting contentTruncated after DPI.
constexpr ULONGLONG kContentTruncationCooldownMs = 400;
} // namespace

namespace windows_webview2_detail
{
ULONGLONG g_last_content_truncation_ftb_ms = 0;
ULONGLONG g_last_content_truncation_menu_ms = 0;
ULONGLONG g_last_content_truncation_cand_ms = 0;
} // namespace windows_webview2_detail

namespace
{
bool AllowContentTruncationRemeasure(ULONGLONG &last_ms)
{
    const ULONGLONG now = GetTickCount64();
    if (last_ms != 0 && now - last_ms < kContentTruncationCooldownMs)
    {
        return false;
    }
    last_ms = now;
    return true;
}
} // namespace

namespace windows_webview2_detail
{
double JsonNumberAsDouble(const json::value &value)
{
    if (value.is_double())
    {
        return value.as_double();
    }
    if (value.is_int64())
    {
        return static_cast<double>(value.as_int64());
    }
    if (value.is_uint64())
    {
        return static_cast<double>(value.as_uint64());
    }
    return 0.0;
}
} // namespace windows_webview2_detail

namespace
{
ICoreWebView2Controller *ControllerForHost(HWND hwnd)
{
    if (hwnd == ::global_hwnd)
        return webviewControllerCandWnd.Get();
    if (hwnd == ::global_hwnd_menu)
        return webviewControllerMenuWnd.Get();
    if (hwnd == ::global_hwnd_ftb)
        return webviewControllerFtbWnd.Get();
    return nullptr;
}

HalfScreenDipLimits ApplyRasterizationScale(HalfScreenDipLimits limits, FLOAT scale)
{
    if (scale <= 0.0f)
        return limits;
    const double monitorWidthPx = static_cast<double>((std::max)(1, limits.monitor.right - limits.monitor.left));
    const double monitorHeightPx = static_cast<double>((std::max)(1, limits.monitor.bottom - limits.monitor.top));
    limits.scale = scale;
    limits.maxWidthDip = (monitorWidthPx * 0.5) / static_cast<double>(scale);
    limits.maxHeightDip = (monitorHeightPx * 0.5) / static_cast<double>(scale);
    return limits;
}

} // namespace

FLOAT GetWebViewRasterizationScale(HWND hwnd)
{
    ICoreWebView2Controller *controller = ControllerForHost(hwnd);
    if (controller)
    {
        ComPtr<ICoreWebView2Controller3> controller3;
        double scale = 0.0;
        if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&controller3))) &&
            SUCCEEDED(controller3->get_RasterizationScale(&scale)) && std::isfinite(scale) && scale > 0.0)
        {
            return static_cast<FLOAT>(scale);
        }
    }
    return GetWindowScale(hwnd);
}

HalfScreenDipLimits QueryWebViewHalfScreenDipLimitsForHwnd(HWND hwnd)
{
    return ApplyRasterizationScale(QueryHalfScreenDipLimitsForHwnd(hwnd), GetWebViewRasterizationScale(hwnd));
}

HalfScreenDipLimits QueryCandidateHalfScreenDipLimitsForPoint(HWND hwnd, POINT pt)
{
    HalfScreenDipLimits limits = QueryHalfScreenDipLimitsForPoint(pt);
    const FLOAT hostNativeScale = GetWindowScale(hwnd);
    const FLOAT webViewScale = GetWebViewRasterizationScale(hwnd);
    // RasterizationScale = monitor DPI scale * user text scale. Preserve the
    // text-scale component when the caret is on another monitor.
    const FLOAT textScale = hostNativeScale > 0.0f ? webViewScale / hostNativeScale : 1.0f;
    const FLOAT targetScale = limits.scale * (textScale > 0.0f ? textScale : 1.0f);
    return ApplyRasterizationScale(limits, targetScale);
}

namespace
{

void InjectSurfaceViewportLimitsImpl(ICoreWebView2 *webview, HWND hwnd)
{
    if (!webview || !hwnd)
    {
        return;
    }
    const HalfScreenDipLimits limits = QueryWebViewHalfScreenDipLimitsForHwnd(hwnd);
    nlohmann::json cfg = {
        {"maxWidthDip", limits.maxWidthDip}, {"maxHeightDip", limits.maxHeightDip}, {"scale", limits.scale}};
    const std::wstring script = L"(function(c){"
                                L"const root=document.documentElement;"
                                L"if(!root)return;"
                                L"root.style.setProperty('--msime-max-width-dip',String(c.maxWidthDip||0)+'px');"
                                L"root.style.setProperty('--msime-max-height-dip',String(c.maxHeightDip||0)+'px');"
                                L"root.style.setProperty('--msime-dpi-scale',String(c.scale||1));"
                                L"if(window.CheckContentTruncation)window.CheckContentTruncation();"
                                L"})(" +
                                string_to_wstring(cfg.dump()) + L");";
    const HRESULT hr = webview->ExecuteScript(script.c_str(), nullptr);
    DIAG_LOGF(L"ui-viewport-limits hwnd={:#x} monitor=({},{})-({},{}) max_dip=({:.2f},{:.2f}) "
              L"scale={:.3f} submit_hr={:#x}",
              reinterpret_cast<UINT_PTR>(hwnd), limits.monitor.left, limits.monitor.top, limits.monitor.right,
              limits.monitor.bottom, limits.maxWidthDip, limits.maxHeightDip, static_cast<double>(limits.scale),
              static_cast<unsigned>(hr));
}

// Fallback only: grow the host to at least 1.2x HTML content (DIP), capped at
// half the monitor. Never shrink — truncation means the viewport is too small.
// Candidate hosts are already quarter-screen; shrinking them to the card and
// clearing CSS margins was parking the window away from the caret.
bool ApplyContentTruncationResize(       //
    HWND hwnd,                           //
    ICoreWebView2 *webview,              //
    ICoreWebView2Controller *controller, //
    double contentWidthDip,              //
    double contentHeightDip,             //
    int extraShadowDip                   //
)
{
    if (!hwnd || contentWidthDip < 1.0 || contentHeightDip < 1.0)
    {
        return false;
    }
    const HalfScreenDipLimits limits = QueryWebViewHalfScreenDipLimitsForHwnd(hwnd);
    const double cappedContentW = ClampWidthDipToHalfScreen(contentWidthDip, limits);
    const double cappedContentH = ClampHeightDipToHalfScreen(contentHeightDip, limits);
    double hostWidthDip = ClampWidthDipToHalfScreen(cappedContentW * kTruncationSizeFactor, limits) +
                          static_cast<double>((std::max)(0, extraShadowDip));
    double hostHeightDip = ClampHeightDipToHalfScreen(cappedContentH * kTruncationSizeFactor, limits) +
                           static_cast<double>((std::max)(0, extraShadowDip));

    int physWidth = static_cast<int>(std::ceil(hostWidthDip * static_cast<double>(limits.scale)));
    int physHeight = static_cast<int>(std::ceil(hostHeightDip * static_cast<double>(limits.scale)));
    const int monitorWidth = (std::max)(1, limits.monitor.right - limits.monitor.left);
    const int monitorHeight = (std::max)(1, limits.monitor.bottom - limits.monitor.top);
    physWidth = (std::min)(physWidth, monitorWidth);
    physHeight = (std::min)(physHeight, monitorHeight);

    RECT current{};
    GetWindowRect(hwnd, &current);
    const int curW = current.right - current.left;
    const int curH = current.bottom - current.top;
    // Grow-only: a larger current host (e.g. quarter-screen candidate) must stay.
    physWidth = (std::max)(physWidth, curW);
    physHeight = (std::max)(physHeight, curH);
    int posX = current.left;
    int posY = current.top;
    if (posX + physWidth > limits.monitor.right)
    {
        posX = limits.monitor.right - physWidth;
    }
    if (posY + physHeight > limits.monitor.bottom)
    {
        posY = limits.monitor.bottom - physHeight;
    }
    if (posX < limits.monitor.left)
    {
        posX = limits.monitor.left;
    }
    if (posY < limits.monitor.top)
    {
        posY = limits.monitor.top;
    }

    const bool sizeUnchanged = std::abs(curW - physWidth) <= 1 && std::abs(curH - physHeight) <= 1;
    if (!sizeUnchanged)
    {
        SetWindowPos(hwnd, nullptr, posX, posY, physWidth, physHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (controller)
    {
        RECT bounds{};
        GetClientRect(hwnd, &bounds);
        controller->put_Bounds(bounds);
        controller->NotifyParentWindowPositionChanged();
    }
    InjectSurfaceViewportLimitsImpl(webview, hwnd);
    FTB_DIAG_LOGF(
        L"trunc-fallback dip=({:.1f},{:.1f})→host=({:.1f},{:.1f}) px=({},{}) half=({:.1f},{:.1f}) scale={:.3f}",
        contentWidthDip, contentHeightDip, hostWidthDip, hostHeightDip, physWidth, physHeight, limits.maxWidthDip,
        limits.maxHeightDip, static_cast<double>(limits.scale));
    return !sizeUnchanged;
}
} // namespace

namespace windows_webview2_detail
{
bool HandleContentTruncatedMessage(      //
    HWND hwnd,                           //
    ICoreWebView2 *webview,              //
    ICoreWebView2Controller *controller, //
    const json::value &val,              //
    ULONGLONG &cooldownSlot,             //
    int extraShadowDip                   //
)
{
    double widthDip = 0.0;
    double heightDip = 0.0;
    double viewportWidthDip = 0.0;
    double viewportHeightDip = 0.0;
    std::string surface = "unknown";
    if (const auto *data = val.as_object().if_contains("data"))
    {
        if (data->is_object())
        {
            const auto &obj = data->as_object();
            if (const auto *w = obj.if_contains("width"))
            {
                widthDip = JsonNumberAsDouble(*w);
            }
            if (const auto *h = obj.if_contains("height"))
            {
                heightDip = JsonNumberAsDouble(*h);
            }
            if (const auto *w = obj.if_contains("viewportWidth"))
            {
                viewportWidthDip = JsonNumberAsDouble(*w);
            }
            if (const auto *h = obj.if_contains("viewportHeight"))
            {
                viewportHeightDip = JsonNumberAsDouble(*h);
            }
            if (const auto *s = obj.if_contains("surface"); s && s->is_string())
            {
                surface = s->as_string().c_str();
            }
        }
    }
    const bool cooldownAllowed = AllowContentTruncationRemeasure(cooldownSlot);
    DIAG_LOGF(L"ui-truncated surface={} content_dip=({:.2f},{:.2f}) viewport_dip=({:.2f},{:.2f}) "
              L"cooldown_allowed={} {}",
              string_to_wstring(surface), widthDip, heightDip, viewportWidthDip, viewportHeightDip, cooldownAllowed,
              hwnd == ::global_hwnd ? DescribeCandidateHostState() : L"");
    if (!cooldownAllowed)
    {
        return false;
    }
    if (widthDip < 1.0 || heightDip < 1.0)
    {
        return false;
    }
    return ApplyContentTruncationResize(hwnd, webview, controller, widthDip, heightDip, extraShadowDip);
}

bool candidateNavigationReady = false;
bool menuNavigationReady = false;
} // namespace windows_webview2_detail

namespace
{
bool smallWindowTopmostRequested = false;
bool smallWindowTopmostApplied = false;
// Guards against scheduling the staggered pin more than once.
bool smallWindowTopmostScheduled = false;

// The three small-window hosts enter the topmost band one at a time, on their
// own timers, rather than together. Under uiAccess a TOPMOST transition is
// exactly what breaks a WebView2 that is still bringing up its first frames, so
// the first step waits well past navigation-ready; the gaps after it only need
// to keep two hosts from changing bands in the same frame. The tray menu goes
// last because later HWND_TOPMOST wins within the band, which is the order an
// open menu needs. Steps must stay listed in firing order: the timer id is the
// enum value, and the delays are indexed by it.
enum class SmallWindowTopmostStep
{
    Candidate,
    FloatingToolbar,
    TrayMenu,
};
constexpr UINT_PTR kTopmostStepTimerIdBase = 9100;
constexpr UINT kTopmostStepCount = 3;
constexpr UINT kCandidateTopmostDelayMs = 1000;
constexpr UINT kFloatingToolbarTopmostDelayMs = 1200;
constexpr UINT kTrayMenuTopmostDelayMs = 1400;
// Remembered so the pending steps can be cancelled. Leaving them armed across a
// controller rebuild would let a stale step pin a host TOPMOST while WebView2 is
// creating a controller for it, which fails with E_INVALIDARG under uiAccess.
HWND smallWindowTopmostTimerHost = nullptr;
// Counted down rather than finalizing in whichever case happens to be last, so
// reordering the steps cannot silently leave the gate open or strand a timer.
UINT smallWindowTopmostStepsPending = 0;

// Retained as a no-op sink so the call sites stay in place; the parameter is named only in the
// comment because nothing consumes it while webview logging is compiled out.
void WebviewDebugLog(const std::wstring & /*message*/)
{
}

void ScheduleSmallWindowWebviewRetry(DWORD delay_ms);
void BeginSmallWindowWebviewEnvironmentCreate();
void RequestNextSmallWindowController();
void OnSmallWindowWebviewInitFailed(HRESULT hr);
void MaybeFlushPendingTrayMenuShow();
void ResetSmallWindowTopmostGate();

void CALLBACK SmallWindowWebviewRetryTimerProc(HWND hwnd, UINT /*msg*/, UINT_PTR id, DWORD /*time*/)
{
    KillTimer(hwnd, id);
    // An existing environment must never be rebuilt: that would replace the
    // candidate / floating-toolbar controllers that are already working and make
    // the toolbar disappear. Only fill in the controllers that are missing.
    if (smallWindowWebviewEnvironment)
    {
        RequestNextSmallWindowController();
        return;
    }
    BeginSmallWindowWebviewEnvironmentCreate();
}

void ScheduleSmallWindowWebviewRetry(DWORD delay_ms)
{
    HWND timer_hwnd = smallWindowCandHwnd ? smallWindowCandHwnd : ::global_hwnd;
    if (!timer_hwnd)
    {
        return;
    }
    KillTimer(timer_hwnd, kRetrySmallWindowWebviewTimerId);
    SetTimer(timer_hwnd, kRetrySmallWindowWebviewTimerId, delay_ms, SmallWindowWebviewRetryTimerProc);
}

// The WebView trace is compiled out in this translation unit (see
// windows_webview2_internal.h) because it drowns the input-latency trace. A failed start-up is
// rare and terminal, and it is the one thing the user cannot see at all, so it goes to the unified
// diagnostic log regardless of that.
void TraceSmallWindowWebview(const std::wstring &line)
{
    if (::DiagnosticLog::IsEnabled())
    {
        ::DiagnosticLog::Write(line);
    }
}

enum class SmallWindowWebviewFailure
{
    // CreateCoreWebView2EnvironmentWithOptions failed: the Runtime is missing, broken or cannot start.
    Environment,
    // The environment is up but CreateCoreWebView2Controller failed for one host, e.g. the uiAccess
    // E_INVALIDARG below. Reinstalling the Runtime does not help there.
    Controller,
};

// Re-armed on forward progress, so a session that recovers and later fails again still says so,
// while a host that keeps failing does not raise a box on every exhausted budget.
bool smallWindowUnavailableReported = false;

// The three small windows are shown DWM-cloaked and only uncloaked once the WebView2 controller has
// painted, so a start-up that never completes looks exactly like a broken engine: no candidate
// window, no error, nothing on screen. Once the retry budget is spent nothing retries on its own
// until the user right-clicks the tray icon, so stop being silent.
// The host windows are parked off-screen, and a box owned by one of them is centred on an invisible
// window — use an ownerless box. It is modal and this runs on the Server message thread, so it goes
// out on its own thread.
void ReportSmallWindowWebviewUnavailable(HRESULT last_hr, SmallWindowWebviewFailure failure)
{
    if (smallWindowUnavailableReported)
    {
        return;
    }
    smallWindowUnavailableReported = true;

    // Only the hosts that are still without a controller are affected; the others already work.
    std::wstring hosts_zh;
    std::wstring hosts_en;
    std::wstring hosts_trace;
    const auto add_host = [&](bool missing, const wchar_t *zh, const wchar_t *en, const wchar_t *trace) {
        if (!missing)
        {
            return;
        }
        hosts_zh += hosts_zh.empty() ? zh : std::wstring(L"、") + zh;
        hosts_en += hosts_en.empty() ? en : std::wstring(L", ") + en;
        hosts_trace += hosts_trace.empty() ? trace : std::wstring(L",") + trace;
    };
    add_host(webviewControllerCandWnd == nullptr, L"候选窗", L"candidate window", L"cand");
    add_host(webviewControllerFtbWnd == nullptr, L"悬浮工具栏", L"floating toolbar", L"ftb");
    add_host(webviewControllerMenuWnd == nullptr, L"托盘菜单", L"tray menu", L"menu");
    if (hosts_trace.empty())
    {
        return;
    }

    const bool environment = failure == SmallWindowWebviewFailure::Environment;
    TraceSmallWindowWebview(fmt::format(L"edge webview unavailable: {} failed {}/{} times, last hr={:#x}; "
                                        L"hosts still cloaked: {}",
                                        environment ? L"environment" : L"controller", smallWindowInitAttempts,
                                        kMaxSmallWindowInitAttempts, static_cast<unsigned>(last_hr), hosts_trace));

    const std::wstring message =
        environment
            ? fmt::format(L"水杉输入法没能启动 WebView2 运行时，{}暂时无法显示。\r\n"
                          L"Metasequoia IME could not start the WebView2 Runtime, so the following cannot be "
                          L"shown for now: {}.\r\n\r\n"
                          L"请安装或修复 Microsoft Edge WebView2 Runtime 后重新登录：\r\n"
                          L"Install or repair the Microsoft Edge WebView2 Runtime, then sign in again:\r\n"
                          L"https://developer.microsoft.com/microsoft-edge/webview2/\r\n\r\n"
                          L"错误码 / error: {:#x}",
                          hosts_zh, hosts_en, static_cast<unsigned>(last_hr))
            : fmt::format(L"WebView2 运行时已启动，但{}没能创建，暂时无法显示。\r\n"
                          L"The WebView2 Runtime started, but the following could not be created and cannot be "
                          L"shown for now: {}.\r\n\r\n"
                          L"右键托盘图标会再试一次；仍然失败请重新登录，或附上诊断日志反馈。\r\n"
                          L"Right-click the tray icon to retry; if it keeps failing, sign in again or report it "
                          L"with the diagnostic log.\r\n\r\n"
                          L"错误码 / error: {:#x}",
                          hosts_zh, hosts_en, static_cast<unsigned>(last_hr));
    std::thread([message]() {
        MessageBoxW(nullptr, message.c_str(), L"水杉输入法 / Metasequoia IME",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    }).detach();
}

void ScheduleSmallWindowRetryWithBackoff(HRESULT hr, SmallWindowWebviewFailure failure)
{
    if (smallWindowInitAttempts >= kMaxSmallWindowInitAttempts)
    {
        ReportSmallWindowWebviewUnavailable(hr, failure);
        return;
    }
    // 1s, 2s, 4s, 8s capped at 10s: covers user-data-folder locks left by
    // orphaned WebView2 processes and slow bring-up right after logon.
    const DWORD delay_ms =
        (std::min)(DWORD{1000} << (std::min)((std::max)(smallWindowInitAttempts - 1, 0), 3), DWORD{10000});
    ScheduleSmallWindowWebviewRetry(delay_ms);
}

// The retry path is the same for every failure code, so the HRESULT is only useful for the trace —
// but until now nothing wrote it anywhere.
void OnSmallWindowWebviewInitFailed(HRESULT hr)
{
    smallWindowInitState = SmallWindowInitState::Failed;
    smallWindowWebviewEnvironment.Reset();
    TraceSmallWindowWebview(fmt::format(L"edge webview environment create failed hr={:#x} attempt={}/{}",
                                        static_cast<unsigned>(hr), smallWindowInitAttempts,
                                        kMaxSmallWindowInitAttempts));
    ScheduleSmallWindowRetryWithBackoff(hr, SmallWindowWebviewFailure::Environment);
}

void MaybeFlushPendingTrayMenuShow()
{
    if (!pendingTrayMenuShow || !::global_hwnd_menu)
    {
        return;
    }
    if (TrayMenuPresenter::Instance().IsBound())
    {
        pendingTrayMenuShow = false;
        PostMessage(::global_hwnd_menu, WM_LANGBAR_RIGHTCLICK, 0, 0);
        return;
    }
    if (!pendingTrayMenuShow || !webviewControllerMenuWnd || !menuNavigationReady || !::global_hwnd_menu)
    {
        return;
    }
    pendingTrayMenuShow = false;
    FTB_DIAG_LOGF(L"menu replaying show that was deferred until the webview was ready");
    // lParam 0: the menu host reuses the icon rect it saved from the right-click
    // that arrived while the menu WebView was not ready.
    PostMessage(::global_hwnd_menu, WM_LANGBAR_RIGHTCLICK, 0, 0);
}

int currentSmallWindowHostIndex = -1;
int lastFailedSmallWindowHostIndex = -1;

// Request a controller for exactly one host at a time, and only for hosts that
// do not have one yet, so neither concurrency nor a retry can disturb siblings
// that already came up. The scan starts after the host that failed last so a
// persistently failing host cannot starve its siblings.
void RequestNextSmallWindowController()
{
    ICoreWebView2Environment *env = smallWindowWebviewEnvironment.Get();
    if (!env)
    {
        return;
    }
    if (smallWindowControllerRequestInFlight)
    {
        // A completion handler that never runs would otherwise wedge the queue.
        if (GetTickCount64() - smallWindowControllerRequestStartTick < kSmallWindowControllerRequestTimeoutMs)
        {
            return;
        }
        smallWindowControllerRequestInFlight = false;
    }

    struct Host
    {
        const wchar_t *name;
        HWND hwnd;
        bool hasController;
        HRESULT (*request)(HWND, HRESULT, ICoreWebView2Environment *);
    };
    const Host hosts[] = {
        {L"cand", smallWindowCandHwnd, webviewControllerCandWnd != nullptr, &OnEnvironmentCreated},
        {L"menu", smallWindowMenuHwnd, webviewControllerMenuWnd != nullptr, &OnMenuWindowEnvironmentCreated},
        {L"ftb", smallWindowFtbHwnd, webviewControllerFtbWnd != nullptr, &OnFtbWindowEnvironmentCreated},
    };
    constexpr int kHostCount = 3;

    int chosen = -1;
    for (int step = 1; step <= kHostCount; ++step)
    {
        const int i = (lastFailedSmallWindowHostIndex + step) % kHostCount;
        if (!hosts[i].hasController && hosts[i].hwnd)
        {
            chosen = i;
            break;
        }
    }
    if (chosen < 0)
    {
        MaybeFlushPendingTrayMenuShow();
        MaybeFlushPendingCandidateShow();
        return;
    }
    const Host &host = hosts[chosen];

    // In a uiAccess=true process a TOPMOST parent makes WebView2's internal
    // cross-process SetParent fail with E_INVALIDARG (WebView2Feedback #486),
    // and the failure persists as long as WS_EX_TOPMOST stays on the window.
    // Demote before creating; the lazy-topmost gate re-pins once all three
    // WebViews are ready.
    if (GetWindowLongPtrW(host.hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)
    {
        SetWindowPos(host.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    smallWindowControllerRequestInFlight = true;
    smallWindowControllerRequestStartTick = GetTickCount64();
    currentSmallWindowHostIndex = chosen;
    const HRESULT hr = host.request(host.hwnd, S_OK, env);
    if (FAILED(hr))
    {
        smallWindowControllerRequestInFlight = false;
        lastFailedSmallWindowHostIndex = chosen;
        ++smallWindowInitAttempts;
        ScheduleSmallWindowRetryWithBackoff(hr, SmallWindowWebviewFailure::Controller);
    }
}
} // namespace

namespace windows_webview2_detail
{
// Called from every controller-created handler so the next host is only started
// once the previous creation has fully settled.
void OnSmallWindowControllerSettled(HRESULT hr)
{
    smallWindowControllerRequestInFlight = false;
    if (FAILED(hr))
    {
        lastFailedSmallWindowHostIndex = currentSmallWindowHostIndex;
        ++smallWindowInitAttempts;
        ScheduleSmallWindowRetryWithBackoff(hr, SmallWindowWebviewFailure::Controller);
        return;
    }
    // Forward progress: give the remaining hosts a full attempt budget.
    smallWindowInitAttempts = 0;
    smallWindowUnavailableReported = false;
    ScheduleSmallWindowWebviewRetry(1);
}
} // namespace windows_webview2_detail

namespace
{
void BeginSmallWindowWebviewEnvironmentCreate()
{
    if (!smallWindowCandHwnd || !smallWindowMenuHwnd || !smallWindowFtbHwnd)
    {
        return;
    }
    if (smallWindowInitState == SmallWindowInitState::InProgress)
    {
        return;
    }
    if (smallWindowWebviewEnvironment)
    {
        RequestNextSmallWindowController();
        return;
    }

    smallWindowInitState = SmallWindowInitState::InProgress;
    ++smallWindowInitAttempts;

    ResetSmallWindowTopmostGate();
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments( //
        L"--disable-features=TranslateUI "
        L"--disable-background-networking "
        L"--disable-default-apps "
        L"--disable-sync "
        L"--disable-prompt-on-repost "
        L"--no-first-run");

    const std::wstring appDataPath = GetAppdataPath();
    if (appDataPath.empty() || appDataPath[0] == L'\\')
    {
        OnSmallWindowWebviewInitFailed(E_INVALIDARG);
        return;
    }

    const HRESULT createHr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, appDataPath.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT result,
                                                                                ICoreWebView2Environment *env)
                                                                                 -> HRESULT {
            if (FAILED(result) || !env)
            {
                OnSmallWindowWebviewInitFailed(FAILED(result) ? result : E_FAIL);
                return FAILED(result) ? result : E_FAIL;
            }

            smallWindowWebviewEnvironment = env;
            smallWindowInitState = SmallWindowInitState::Ready;
            smallWindowInitAttempts = 0;
            smallWindowUnavailableReported = false;
            RequestNextSmallWindowController();
            return S_OK;
        }).Get());

    if (FAILED(createHr))
    {
        OnSmallWindowWebviewInitFailed(createHr);
    }
}

bool AreSmallWindowWebviewsReadyUnlocked()
{
    const bool candReady =
        CandidatePresenter::Instance().IsBound() ||
        (candidateNavigationReady && webviewCandWnd != nullptr && webviewControllerCandWnd != nullptr);
    const bool menuReady = TrayMenuPresenter::Instance().IsBound() ||
                           (menuNavigationReady && webviewMenuWnd != nullptr && webviewControllerMenuWnd != nullptr);
    const bool ftbReady =
        FloatingToolbarPresenter::Instance().IsBound() ||
        (floatingToolbarNavigationReady && webviewFtbWnd != nullptr && webviewControllerFtbWnd != nullptr);
    return candReady && menuReady && ftbReady;
}

void CancelStaggeredTopmost()
{
    smallWindowTopmostStepsPending = 0;
    if (!smallWindowTopmostTimerHost)
    {
        return;
    }
    for (UINT_PTR step = 0; step < kTopmostStepCount; ++step)
    {
        KillTimer(smallWindowTopmostTimerHost, kTopmostStepTimerIdBase + step);
    }
    smallWindowTopmostTimerHost = nullptr;
}

void ResetSmallWindowTopmostGate()
{
    CancelStaggeredTopmost();
    candidateNavigationReady = false;
    menuNavigationReady = false;
    ClearFloatingToolbarNavigationState();
    smallWindowTopmostRequested = false;
    smallWindowTopmostApplied = false;
    smallWindowTopmostScheduled = false;
    (void)0;
}

// Same as WebviewDebugLog: a no-op sink kept for its call sites while the gate logging is off.
void LogSmallWindowReadyGateUnlocked(const wchar_t * /*context*/)
{
}

void PinHostTopmost(HWND hwnd)
{
    if (!hwnd)
    {
        return;
    }
    constexpr UINT flag = SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE;
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flag);
}

// HWND_TOPMOST moves the host between z-bands. In a uiAccess process a WebView2
// that is not told about it keeps compositing against the old parent state: the
// host stays visible with correct bounds while nothing is ever painted into it.
void RenotifyControllerAfterPin(ICoreWebView2Controller *controller, HWND hwnd)
{
    if (!controller || !hwnd || !IsWindowVisible(hwnd))
    {
        return;
    }
    UpdateSmallWindowWebviewVisibility(hwnd, true);
    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    controller->put_Bounds(bounds);
    controller->NotifyParentWindowPositionChanged();
}

// The menu host is shown DWM-cloaked for WebView2 warmup, so IsWindowVisible()
// reports true from startup onwards even though the user sees nothing and the
// first frame may not exist yet. Callers that mean "the menu is open in front of
// the user" must exclude that state: treating warmup as open is what pins the
// host into the topmost band mid-initialisation and leaves it permanently blank.
bool TrayMenuIsOpenToUser()
{
    if (TrayMenuPresenter::Instance().IsBound())
    {
        return TrayMenuPresenter::Instance().IsOpenToUser();
    }
    if (!::global_hwnd_menu || !webviewControllerMenuWnd || !menuNavigationReady)
    {
        return false;
    }
    if (!IsWindowVisible(::global_hwnd_menu))
    {
        return false;
    }
    DWORD cloaked = 0;
    DwmGetWindowAttribute(::global_hwnd_menu, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return cloaked == 0;
}

void ApplySmallWindowTopmostStep(SmallWindowTopmostStep step)
{
    FTB_DIAG_LOGF(L"topmost step {} applying", step == SmallWindowTopmostStep::Candidate         ? L"candidate"
                                               : step == SmallWindowTopmostStep::FloatingToolbar ? L"floating-toolbar"
                                                                                                 : L"tray-menu");
    switch (step)
    {
    case SmallWindowTopmostStep::Candidate:
        PinHostTopmost(::global_hwnd);
        if (CandidatePresenter::Instance().IsBound())
        {
            if (::is_global_wnd_cand_shown && ::global_hwnd)
            {
                CandidatePresenter::Instance().Present();
            }
        }
        else
        {
            RenotifyControllerAfterPin(webviewControllerCandWnd.Get(), ::global_hwnd);
            if (::is_global_wnd_cand_shown && ::global_hwnd)
            {
                FineTuneWindow(::global_hwnd);
            }
        }
        break;

    case SmallWindowTopmostStep::FloatingToolbar:
        PinHostTopmost(::global_hwnd_ftb);
        if (FloatingToolbarPresenter::Instance().IsBound())
        {
            FloatingToolbarPresenter::Instance().Present();
        }
        else
        {
            RenotifyControllerAfterPin(webviewControllerFtbWnd.Get(), ::global_hwnd_ftb);
        }
        // The menu step lands a moment later and would fix the order anyway;
        // raising now keeps an already-open menu from being covered in between.
        if (TrayMenuIsOpenToUser())
        {
            RaiseTrayMenuAboveSmallWindows(L"after-staggered-topmost");
        }
        break;

    case SmallWindowTopmostStep::TrayMenu:
        PinHostTopmost(::global_hwnd_menu);
        if (TrayMenuPresenter::Instance().IsBound())
        {
            if (TrayMenuPresenter::Instance().IsOpenToUser())
            {
                TrayMenuPresenter::Instance().Present();
            }
        }
        else
        {
            RenotifyControllerAfterPin(webviewControllerMenuWnd.Get(), ::global_hwnd_menu);
        }
        break;
    }

    if (smallWindowTopmostStepsPending > 0 && --smallWindowTopmostStepsPending == 0)
    {
        // Only now is the whole band in effect, and no timer is left to cancel.
        smallWindowTopmostApplied = true;
        smallWindowTopmostTimerHost = nullptr;
        LogSmallWindowReadyGateUnlocked(L"after-topmost-applied");
    }
}

void CALLBACK SmallWindowTopmostTimerProc(HWND hwnd, UINT, UINT_PTR timerId, DWORD)
{
    KillTimer(hwnd, timerId);
    ApplySmallWindowTopmostStep(static_cast<SmallWindowTopmostStep>(timerId - kTopmostStepTimerIdBase));
}

// Returns false when there is no host window to hang the timers on, leaving the
// caller to pin inline rather than never.
bool ScheduleStaggeredTopmost()
{
    const HWND timer_host = ::global_hwnd_ftb ? ::global_hwnd_ftb : ::global_hwnd;
    if (!timer_host)
    {
        return false;
    }
    const UINT delays[kTopmostStepCount] = {kCandidateTopmostDelayMs, kFloatingToolbarTopmostDelayMs,
                                            kTrayMenuTopmostDelayMs};
    UINT scheduled = 0;
    for (UINT_PTR step = 0; step < kTopmostStepCount; ++step)
    {
        if (SetTimer(timer_host, kTopmostStepTimerIdBase + step, delays[step], SmallWindowTopmostTimerProc) != 0)
        {
            ++scheduled;
        }
    }
    // Counting what was armed rather than kTopmostStepCount keeps the countdown
    // able to reach zero if SetTimer fails for one of the steps.
    smallWindowTopmostStepsPending = scheduled;
    smallWindowTopmostTimerHost = scheduled > 0 ? timer_host : nullptr;
    return scheduled > 0;
}

// Pinning z-order is what breaks WebView2 rendering under uiAccess, and the
// gate normally opens inside the toolbar's own navigation-completed handler --
// before it has painted a single frame. Spread the three transitions out in
// time so each WebView2 is well settled before its host moves, and so that no
// two hosts change bands close enough together to interact.
void TryApplyPendingLazyTopmost(const wchar_t *reason)
{
    if (!smallWindowTopmostRequested || smallWindowTopmostApplied)
    {
        return;
    }
    if (!AreSmallWindowWebviewsReadyUnlocked())
    {
        LogSmallWindowReadyGateUnlocked(L"topmost-still-waiting-webviews");
        return;
    }
    // Already scheduled. Falling through here would pin z-order from whatever
    // is running right now, which is the navigation-completed handler this
    // deferral exists to stay out of: the toolbar reports ready last, so its
    // own apply asks for topmost again a few lines later.
    if (smallWindowTopmostScheduled)
    {
        return;
    }
    smallWindowTopmostScheduled = true;
    if (ScheduleStaggeredTopmost())
    {
        FTB_DIAG_LOGF(L"topmost staggered from reason={}: candidate +{}ms, floating-toolbar +{}ms, "
                      L"tray-menu +{}ms",
                      reason, kCandidateTopmostDelayMs, kFloatingToolbarTopmostDelayMs, kTrayMenuTopmostDelayMs);
        return;
    }
    FTB_DIAG_LOGF(L"topmost timers unavailable for reason={}, pinning inline", reason);
    smallWindowTopmostStepsPending = kTopmostStepCount;
    ApplySmallWindowTopmostStep(SmallWindowTopmostStep::Candidate);
    ApplySmallWindowTopmostStep(SmallWindowTopmostStep::FloatingToolbar);
    ApplySmallWindowTopmostStep(SmallWindowTopmostStep::TrayMenu);
}
} // namespace

namespace windows_webview2_detail
{
// `which` names the window for diagnostics only; the gate logic is identical for every caller.
void NotifySmallWindowNavigationReady(bool &readyFlag, const wchar_t * /*which*/)
{
    if (readyFlag)
    {
        return;
    }
    readyFlag = true;
    LogSmallWindowReadyGateUnlocked(L"after-nav-ready");
    TryApplyPendingLazyTopmost(L"pending-after-nav-ready");
    MaybeFlushPendingTrayMenuShow();
    MaybeFlushPendingCandidateShow();
}
} // namespace windows_webview2_detail

namespace
{
void SetWebviewMemoryUsageTarget(ComPtr<ICoreWebView2> webview, COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL level)
{
    if (!webview)
        return;

    ComPtr<ICoreWebView2_23> webview23;
    if (SUCCEEDED(webview.As(&webview23)))
    {
        webview23->put_MemoryUsageTargetLevel(level);
    }
}
} // namespace

void InjectSurfaceViewportLimits(ICoreWebView2 *webview, HWND hwnd)
{
    InjectSurfaceViewportLimitsImpl(webview, hwnd);
}

bool EnsureSmallWindowsTopmost(const wchar_t *reason)
{
    smallWindowTopmostRequested = true;
    if (smallWindowTopmostApplied)
    {
        return true;
    }
    if (!AreSmallWindowWebviewsReadyUnlocked())
    {
        (void)0;
        LogSmallWindowReadyGateUnlocked(L"topmost-deferred");
        return false;
    }

    // Shares the staggered path so the pin never lands on the stack of a
    // navigation-completed handler and every host gets renotified afterwards.
    // This used to pin all three inline and skip the renotification entirely.
    TryApplyPendingLazyTopmost(reason);
    return smallWindowTopmostApplied;
}

void RaiseTrayMenuAboveSmallWindows(const wchar_t *reason)
{
    if (!::global_hwnd_menu)
    {
        return;
    }
    if (TrayMenuPresenter::Instance().IsBound())
    {
        constexpr UINT flag = SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE;
        SetWindowPos(::global_hwnd_menu, HWND_TOPMOST, 0, 0, 0, 0, flag);
        if (TrayMenuPresenter::Instance().IsOpenToUser())
        {
            TrayMenuPresenter::Instance().Present();
        }
        return;
    }
    // Backstop for the callers above: making the host TOPMOST before a
    // controller exists is fatal in a uiAccess=true process, because UIPI then
    // blocks WebView2's cross-process SetParent and every
    // CreateCoreWebView2Controller for this window fails with E_INVALIDARG
    // (WebView2Feedback #486). Nothing needs raising before content exists.
    if (!webviewControllerMenuWnd)
    {
        FTB_DIAG_LOGF(L"menu raise reason={} skipped: no controller yet", reason);
        return;
    }
    // Re-assert TOPMOST after FTB (or a peer) was pinned last. A second
    // HWND_TOPMOST is what actually lifts the menu within the topmost band;
    // HWND_TOP alone is unreliable here with WS_EX_NOACTIVATE layered hosts.
    FTB_DIAG_LOGF(L"menu raise reason={} nav_ready={} {}", reason, menuNavigationReady, DescribeTrayMenuHostState());
    constexpr UINT flag = SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE;
    SetLastError(0);
    if (!SetWindowPos(::global_hwnd_menu, HWND_TOPMOST, 0, 0, 0, 0, flag))
    {
        FTB_DIAG_LOGF(L"menu raise reason={} SetWindowPos failed err={}", reason, GetLastError());
        return;
    }
    // This is the same band transition PinHostTopmost performs and so needs the
    // same repair. Skipping it is what leaves the menu interactive but blank:
    // the controller keeps compositing against the parent state it saw before
    // the move, while the host reports correct bounds throughout.
    RenotifyControllerAfterPin(webviewControllerMenuWnd.Get(), ::global_hwnd_menu);
}

void DeferCandidateShowUntilWebviewReady()
{
    pendingCandidateShow = true;
    CAND_DIAG_LOGF(L"candidate show deferred until webview ready {}", DescribeCandidateHostState());
}

void MaybeFlushPendingCandidateShow()
{
    if (!pendingCandidateShow || !::global_hwnd || !IsWindow(::global_hwnd))
    {
        return;
    }
    if (!::is_global_wnd_cand_shown)
    {
        pendingCandidateShow = false;
        return;
    }
    if (!IsCandidateWebviewReady())
    {
        return;
    }
    pendingCandidateShow = false;
    g_candidate_show_msg_pending.store(false);
    CAND_DIAG_LOGF(L"candidate replaying show that was deferred until the webview was ready");
    PostMessage(::global_hwnd, WM_SHOW_MAIN_WINDOW, 0, 0);
}

void RaiseCandidateHostForShow(const wchar_t *reason)
{
    if (!::global_hwnd)
    {
        return;
    }
    if (CandidatePresenter::Instance().IsBound())
    {
        constexpr UINT flag = SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE;
        SetWindowPos(::global_hwnd, HWND_TOPMOST, 0, 0, 0, 0, flag);
        return;
    }
    if (!webviewControllerCandWnd)
    {
        CAND_DIAG_LOGF(L"candidate raise reason={} skipped: no controller yet", reason);
        return;
    }
    CAND_DIAG_LOGF(L"candidate raise reason={} nav_ready={} {}", reason, candidateNavigationReady,
                   DescribeCandidateHostState());
    PinHostTopmost(::global_hwnd);
    RenotifyControllerAfterPin(webviewControllerCandWnd.Get(), ::global_hwnd);
}

bool AreSmallWindowsTopmostApplied()
{
    return smallWindowTopmostApplied;
}

bool AreSmallWindowWebviewsReady()
{
    return AreSmallWindowWebviewsReadyUnlocked();
}

bool IsCandidateWebviewReady()
{
    return CandidatePresenter::Instance().IsBound() ||
           (candidateNavigationReady && webviewCandWnd != nullptr && webviewControllerCandWnd != nullptr);
}

bool IsFloatingToolbarWebviewReady()
{
    return FloatingToolbarPresenter::Instance().IsBound() ||
           (floatingToolbarNavigationReady && webviewControllerFtbWnd != nullptr);
}

bool IsFloatingToolbarPaintGraceActive()
{
    return floatingToolbarPaintGraceActive;
}

void BeginFloatingToolbarPaintGrace()
{
    floatingToolbarPaintGraceActive = true;
}

void EndFloatingToolbarPaintGrace()
{
    floatingToolbarPaintGraceActive = false;
}

void NotifyFloatingToolbarPageReady()
{
    ReconcileFloatingToolbarVisibilityAfterReady(L"ftb-page-ready");
}

void ClearFloatingToolbarNavigationState()
{
    floatingToolbarNavigationReady = false;
    floatingToolbarPaintGraceActive = false;
    floatingToolbarNavigationRetryUsed = false;
}

bool IsTrayMenuOpenToUser()
{
    return TrayMenuIsOpenToUser();
}

bool GetFloatingToolbarWebviewState(bool &isVisible, RECT &bounds)
{
    isVisible = false;
    bounds = RECT{};
    if (!webviewControllerFtbWnd)
    {
        return false;
    }
    BOOL visible = FALSE;
    webviewControllerFtbWnd->get_IsVisible(&visible);
    isVisible = visible != FALSE;
    webviewControllerFtbWnd->get_Bounds(&bounds);
    return true;
}

bool GetTrayMenuWebviewState(bool &isVisible, RECT &bounds)
{
    isVisible = false;
    bounds = RECT{};
    if (!webviewControllerMenuWnd)
    {
        return false;
    }
    BOOL visible = FALSE;
    webviewControllerMenuWnd->get_IsVisible(&visible);
    isVisible = visible != FALSE;
    webviewControllerMenuWnd->get_Bounds(&bounds);
    return true;
}

void LogSmallWindowReadyGate(const wchar_t *context)
{
    LogSmallWindowReadyGateUnlocked(context);
}

void UpdateSmallWindowWebviewVisibility(HWND hwnd, bool visible)
{
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    bool lowerMemoryWhenHidden = false;

    if (hwnd == ::global_hwnd)
    {
        controller = webviewControllerCandWnd;
        webview = webviewCandWnd;
    }
    else if (hwnd == ::global_hwnd_menu)
    {
        controller = webviewControllerMenuWnd;
        webview = webviewMenuWnd;
        // The language-bar menu is reopened interactively and must paint on
        // the first frame. Returning its renderer from LOW is asynchronous
        // and can leave the host window visible with transparent content.
        lowerMemoryWhenHidden = false;
    }
    else if (hwnd == ::global_hwnd_ftb)
    {
        controller = webviewControllerFtbWnd;
        webview = webviewFtbWnd;
        // Keep the small, persistent toolbar warm when configuration or
        // fullscreen policy temporarily hides it. Switching its WebView back
        // from LOW can otherwise produce a visible white repaint.
        lowerMemoryWhenHidden = false;
    }
    else
    {
        (void)0;
        return;
    }

    if (controller)
    {
        controller->put_IsVisible(visible ? TRUE : FALSE);
    }
    else
    {
        (void)0;
    }

    if (lowerMemoryWhenHidden)
    {
        SetWebviewMemoryUsageTarget(webview, visible ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL
                                                     : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
    }
}

bool GetCandidateWebviewState(bool &isVisible, RECT &bounds)
{
    if (!webviewControllerCandWnd)
        return false;
    BOOL visible = FALSE;
    if (FAILED(webviewControllerCandWnd->get_IsVisible(&visible)) ||
        FAILED(webviewControllerCandWnd->get_Bounds(&bounds)))
        return false;
    isVisible = visible != FALSE;
    return true;
}

std::wstring GetAppdataPath()
{
    return CommonUtils::get_webview2_user_data_path(L"webview2");
}

/**
 * @brief Initialize candidate, tray menu, and floating toolbar WebViews in one
 *        environment when appearance.ui_backend is webview2.
 */
void InitSmallWindowWebviews(HWND candHwnd, HWND menuHwnd, HWND ftbHwnd)
{
    smallWindowCandHwnd = candHwnd;
    smallWindowMenuHwnd = menuHwnd;
    smallWindowFtbHwnd = ftbHwnd;
    smallWindowInitAttempts = 0;
    smallWindowInitState = SmallWindowInitState::Idle;
    smallWindowControllerRequestInFlight = false;
    currentSmallWindowHostIndex = -1;
    lastFailedSmallWindowHostIndex = -1;
    pendingTrayMenuShow = false;
    pendingCandidateShow = false;
    if (UseD2dSmallWindowUi())
    {
        FTB_DIAG_LOGF(L"skip small-window webview init: ui_backend=d2d");
        return;
    }
    BeginSmallWindowWebviewEnvironmentCreate();
}

bool PrepareTrayMenuWebviewForShow()
{
    if (TrayMenuPresenter::Instance().IsBound())
    {
        return true;
    }
    if (webviewControllerMenuWnd && menuNavigationReady)
    {
        return true;
    }

    pendingTrayMenuShow = true;

    if (webviewControllerMenuWnd)
    {
        // Controller exists but nothing ever painted. An unreadable menu asset
        // makes NavigateToString run on an empty string, which yields a blank
        // document with no title (and therefore no visible WebView2 child entry).
        if (::HTMLStringMenuWnd.empty())
        {
            PrepareHtmlForWnds();
        }
        FTB_DIAG_LOGF(L"menu prepare: controller exists, navigation not ready, html_empty={} -> renavigate",
                      ::HTMLStringMenuWnd.empty());
        if (webviewMenuWnd && !::HTMLStringMenuWnd.empty())
        {
            webviewMenuWnd->NavigateToString(::HTMLStringMenuWnd.c_str());
        }
        return false;
    }

    FTB_DIAG_LOGF(L"menu prepare: no controller yet, init_state={} attempts={}", static_cast<int>(smallWindowInitState),
                  smallWindowInitAttempts);
    if (smallWindowInitState != SmallWindowInitState::InProgress)
    {
        // An explicit right-click is a fresh user intent: reset the attempt
        // budget so a long-idle session can still recover the menu.
        smallWindowInitAttempts = 0;
        ScheduleSmallWindowWebviewRetry(200);
    }
    return false;
}

void ShutdownWebviews()
{
    // WebView2 objects are apartment-bound. Release every controller and
    // interface on the UI STA before WinMain balances CoInitializeEx.
    ResetSmallWindowTopmostGate();
    pendingTrayMenuShow = false;
    pendingCandidateShow = false;
    smallWindowInitState = SmallWindowInitState::Idle;
    smallWindowControllerRequestInFlight = false;
    if (smallWindowCandHwnd)
    {
        KillTimer(smallWindowCandHwnd, kRetrySmallWindowWebviewTimerId);
    }

    if (candidateRasterizationScaleChangedRegistered && webviewController3CandWnd)
    {
        webviewController3CandWnd->remove_RasterizationScaleChanged(candidateRasterizationScaleChangedToken);
        candidateRasterizationScaleChangedRegistered = false;
    }
    if (webviewControllerCandWnd)
    {
        webviewControllerCandWnd->Close();
    }
    if (webviewControllerMenuWnd)
    {
        webviewControllerMenuWnd->Close();
    }
    if (webviewControllerFtbWnd)
    {
        webviewControllerFtbWnd->Close();
    }

    webviewController2CandWnd.Reset();
    webviewController3CandWnd.Reset();
    webviewController2MenuWnd.Reset();
    webviewController2FtbWnd.Reset();

    webview3CandWnd.Reset();
    webview3MenuWnd.Reset();
    webview3FtbWnd.Reset();

    webviewCandWnd.Reset();
    webviewMenuWnd.Reset();
    webviewFtbWnd.Reset();

    webviewControllerCandWnd.Reset();
    webviewControllerMenuWnd.Reset();
    webviewControllerFtbWnd.Reset();

    smallWindowWebviewEnvironment.Reset();
}
