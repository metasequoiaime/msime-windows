#include "window_utils.h"
#include "config/ime_config.h"
#include "defines/globals.h"
#include "ipc/ipc.h"
#include "webview_utils.h"
#include <algorithm>
#include <cmath>
#include <utility>
#include <winuser.h>
#include <dwmapi.h>
#include <shellscalingapi.h>

#pragma comment(lib, "Shcore.lib")

namespace
{
double g_max_vertical_container_height_dip = ::DEFAULT_WINDOW_HEIGHT_DIP;

MonitorCoordinates MonitorCoordinatesFromHandle(HMONITOR hMonitor)
{
    MonitorCoordinates coordinates{};
    if (!hMonitor)
    {
        return coordinates;
    }

    MONITORINFO monitorInfo = {sizeof(monitorInfo)};
    if (!GetMonitorInfo(hMonitor, &monitorInfo))
    {
        return coordinates;
    }

    coordinates.left = monitorInfo.rcMonitor.left;
    coordinates.top = monitorInfo.rcMonitor.top;
    coordinates.right = monitorInfo.rcMonitor.right;
    coordinates.bottom = monitorInfo.rcMonitor.bottom;
    return coordinates;
}

MonitorCoordinates MonitorWorkAreaCoordinatesFromHandle(HMONITOR hMonitor)
{
    MonitorCoordinates coordinates{};
    if (!hMonitor)
    {
        return coordinates;
    }

    MONITORINFO monitorInfo = {sizeof(monitorInfo)};
    if (!GetMonitorInfo(hMonitor, &monitorInfo))
    {
        return coordinates;
    }

    // Candidate windows must avoid the taskbar and other app bars. Using the
    // full monitor rectangle makes a candidate near the bottom look as though
    // it still fits, so it remains below the caret and overlaps the taskbar.
    coordinates.left = monitorInfo.rcWork.left;
    coordinates.top = monitorInfo.rcWork.top;
    coordinates.right = monitorInfo.rcWork.right;
    coordinates.bottom = monitorInfo.rcWork.bottom;
    return coordinates;
}

FLOAT ScaleFromMonitor(HMONITOR hMonitor)
{
    if (!hMonitor)
    {
        return GetForegroundWindowScale();
    }

    UINT dpiX = USER_DEFAULT_SCREEN_DPI;
    UINT dpiY = USER_DEFAULT_SCREEN_DPI;
    if (FAILED(GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || dpiX == 0)
    {
        return GetForegroundWindowScale();
    }
    return static_cast<FLOAT>(dpiX) / static_cast<FLOAT>(USER_DEFAULT_SCREEN_DPI);
}
} // namespace

namespace
{
const RECT &ScreenAreaRect(const MONITORINFO &info, ScreenArea area)
{
    return area == ScreenArea::WorkArea ? info.rcWork : info.rcMonitor;
}

struct VisibleRegionBuilder
{
    HRGN region = nullptr;
    ScreenArea area = ScreenArea::WorkArea;
};

BOOL CALLBACK AddMonitorAreaToRegion(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    auto *builder = reinterpret_cast<VisibleRegionBuilder *>(data);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfo(monitor, &info))
    {
        return TRUE;
    }

    HRGN areaRegion = CreateRectRgnIndirect(&ScreenAreaRect(info, builder->area));
    if (areaRegion)
    {
        CombineRgn(builder->region, builder->region, areaRegion, RGN_OR);
        DeleteObject(areaRegion);
    }
    return TRUE;
}

bool IsRectInsideVisibleScreens(const RECT &rect, ScreenArea area)
{
    HRGN visibleRegion = CreateRectRgn(0, 0, 0, 0);
    HRGN windowRegion = CreateRectRgnIndirect(&rect);
    HRGN outsideRegion = CreateRectRgn(0, 0, 0, 0);
    bool inside = true;
    if (visibleRegion && windowRegion && outsideRegion)
    {
        VisibleRegionBuilder builder{visibleRegion, area};
        EnumDisplayMonitors(nullptr, nullptr, AddMonitorAreaToRegion, reinterpret_cast<LPARAM>(&builder));
        inside = CombineRgn(outsideRegion, windowRegion, visibleRegion, RGN_DIFF) == NULLREGION;
    }
    if (visibleRegion)
        DeleteObject(visibleRegion);
    if (windowRegion)
        DeleteObject(windowRegion);
    if (outsideRegion)
        DeleteObject(outsideRegion);
    return inside;
}
} // namespace

bool KeepRectOnVisibleScreens(RECT &rect, ScreenArea area, HMONITOR *monitor, const RECT &offscreenInsets)
{
    RECT content{rect.left + offscreenInsets.left, rect.top + offscreenInsets.top, rect.right - offscreenInsets.right,
                 rect.bottom - offscreenInsets.bottom};
    if (content.right <= content.left || content.bottom <= content.top)
    {
        content = rect;
    }
    if (IsRectInsideVisibleScreens(content, area))
    {
        return false;
    }

    // Nearest monitor of the rect's center instead of MonitorFromRect: a host
    // straddling a seam must keep the side the user dropped it on, and a host
    // beyond every screen must come back towards where it was pushed.
    const POINT center{content.left + (content.right - content.left) / 2,
                       content.top + (content.bottom - content.top) / 2};
    const HMONITOR target = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!target || !GetMonitorInfo(target, &info))
    {
        return false;
    }
    if (monitor)
    {
        *monitor = target;
    }

    const RECT &bounds = ScreenAreaRect(info, area);
    const int width = content.right - content.left;
    const int height = content.bottom - content.top;
    const int maxX = (std::max)(static_cast<int>(bounds.left), static_cast<int>(bounds.right) - width);
    const int maxY = (std::max)(static_cast<int>(bounds.top), static_cast<int>(bounds.bottom) - height);
    const int x = (std::max)(static_cast<int>(bounds.left), (std::min)(static_cast<int>(content.left), maxX));
    const int y = (std::max)(static_cast<int>(bounds.top), (std::min)(static_cast<int>(content.top), maxY));
    if (x == content.left && y == content.top)
    {
        return false;
    }
    OffsetRect(&rect, x - content.left, y - content.top);
    return true;
}

void ClipWindowToVisibleScreens(HWND hwnd, ScreenArea area)
{
    RECT rect{};
    if (!hwnd || !GetWindowRect(hwnd, &rect))
    {
        return;
    }
    HRGN visible = CreateRectRgn(0, 0, 0, 0);
    HRGN clipped = CreateRectRgnIndirect(&rect);
    HRGN whole = CreateRectRgnIndirect(&rect);
    HRGN current = CreateRectRgn(0, 0, 0, 0);
    if (visible && clipped && whole && current)
    {
        VisibleRegionBuilder builder{visible, area};
        EnumDisplayMonitors(nullptr, nullptr, AddMonitorAreaToRegion, reinterpret_cast<LPARAM>(&builder));
        const int kind = CombineRgn(clipped, clipped, visible, RGN_AND);
        const bool hasRegion = GetWindowRgn(hwnd, current) != ERROR;
        // Nothing visible means the host is mid-drag between screens; the
        // clamp at the end of the drag brings it back, so leave it alone.
        if (kind == NULLREGION || kind == ERROR || EqualRgn(clipped, whole))
        {
            if (hasRegion && kind != NULLREGION)
            {
                SetWindowRgn(hwnd, nullptr, TRUE);
            }
        }
        else
        {
            OffsetRgn(clipped, -rect.left, -rect.top);
            // SetWindowRgn repaints, and it runs on every move during a drag.
            if (!hasRegion || !EqualRgn(current, clipped))
            {
                // The system owns the region once it is set.
                if (SetWindowRgn(hwnd, clipped, TRUE))
                {
                    clipped = nullptr;
                }
            }
        }
    }
    for (HRGN region : {visible, clipped, whole, current})
    {
        if (region)
        {
            DeleteObject(region);
        }
    }
}

bool IsWindowInMoveSizeLoop(HWND hwnd)
{
    // Ask USER32 instead of mirroring WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE in a
    // flag: a mirrored flag stays set forever if the HWND is torn down while
    // the loop runs and never sees WM_EXITSIZEMOVE.
    const DWORD thread = hwnd ? GetWindowThreadProcessId(hwnd, nullptr) : 0;
    GUITHREADINFO info{sizeof(info)};
    return thread != 0 && GetGUIThreadInfo(thread, &info) && (info.flags & GUI_INMOVESIZE) != 0 &&
           info.hwndMoveSize == hwnd;
}

POINT PlaceResizedHost(HWND hwnd, int width, int height, const RECT *suggestedRect, const RECT &offscreenInsets,
                       const RECT *previousInsets)
{
    RECT target{};
    if (suggestedRect)
    {
        target = *suggestedRect;
    }
    else
    {
        GetWindowRect(hwnd, &target);
    }
    // Keep the visible part still when the margin around it changes size;
    // anchoring the host's own top-left would shift it by the difference.
    const bool insetsChanged = previousInsets && !suggestedRect && !EqualRect(previousInsets, &offscreenInsets);
    if (insetsChanged)
    {
        target.left += previousInsets->left - offscreenInsets.left;
        target.top += previousInsets->top - offscreenInsets.top;
    }
    target.right = target.left + width;
    target.bottom = target.top + height;
    // A caption drag owns the position until it ends (WM_EXITSIZEMOVE clamps
    // then). Clamping mid-drag fights the move loop and snaps the host back to
    // the screen it came from.
    if (!IsWindowInMoveSizeLoop(hwnd))
    {
        // A changed margin re-checks the visible part against the work area,
        // as the end of a drag does, so rounding cannot tuck it under the taskbar.
        KeepRectOnVisibleScreens(target, insetsChanged ? ScreenArea::WorkArea : ScreenArea::Monitor, nullptr,
                                 offscreenInsets);
    }
    return {target.left, target.top};
}

FLOAT GetWindowScale(HWND hwnd)
{
    // GetDpiForWindow returns 0 for an invalid HWND. A 0 scale silently collapses
    // every host window to 0x0, which breaks WebView2 bring-up and hides the
    // floating toolbar, so fall back to the system DPI instead.
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
    if (dpi == 0)
    {
        dpi = GetDpiForSystem();
    }
    if (dpi == 0)
    {
        dpi = USER_DEFAULT_SCREEN_DPI;
    }
    FLOAT scale = dpi / 96.0f;
    return scale;
}

FLOAT GetForegroundWindowScale()
{
    // There is often no foreground window while the IME is being switched or
    // right after logon, which is exactly when the server gets relaunched.
    HWND hwnd = GetForegroundWindow();
    FLOAT scale = GetWindowScale(hwnd);
    return scale;
}

FLOAT GetScaleForPoint(POINT pt)
{
    return ScaleFromMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
}

ResolvedCandidateScale ResolveCandidateScaleForCaret(POINT caret)
{
    // SM_REMOTESESSION is only set inside a real remote session; console and
    // local logons never see it, so the local path is byte-for-byte unchanged.
    // GetDpiForWindow returns 0 for an invalid/absent foreground HWND, which
    // drops to the monitor path instead of trusting a bogus 0 scale.
    if (GetSystemMetrics(SM_REMOTESESSION))
    {
        const UINT dpi = GetDpiForWindow(GetForegroundWindow());
        if (dpi != 0)
        {
            return {static_cast<FLOAT>(dpi) / static_cast<FLOAT>(USER_DEFAULT_SCREEN_DPI),
                    CandidateScaleSource::RdpForeground};
        }
    }
    return {GetScaleForPoint(caret), CandidateScaleSource::Monitor};
}

HalfScreenDipLimits QueryHalfScreenDipLimitsForPoint(POINT pt)
{
    HalfScreenDipLimits limits{};
    limits.scale = GetScaleForPoint(pt);
    if (limits.scale <= 0.0f)
    {
        limits.scale = 1.0f;
    }
    limits.monitor = GetMonitorCoordinatesFromPoint(pt);
    const double monitorWidthPx = static_cast<double>((std::max)(1, limits.monitor.right - limits.monitor.left));
    const double monitorHeightPx = static_cast<double>((std::max)(1, limits.monitor.bottom - limits.monitor.top));
    limits.maxWidthDip = (monitorWidthPx * 0.5) / static_cast<double>(limits.scale);
    limits.maxHeightDip = (monitorHeightPx * 0.5) / static_cast<double>(limits.scale);
    return limits;
}

HalfScreenDipLimits QueryHalfScreenDipLimitsForHwnd(HWND hwnd)
{
    RECT windowRect{};
    if (hwnd && GetWindowRect(hwnd, &windowRect))
    {
        POINT anchor{windowRect.left + (windowRect.right - windowRect.left) / 2,
                     windowRect.top + (windowRect.bottom - windowRect.top) / 2};
        HalfScreenDipLimits limits = QueryHalfScreenDipLimitsForPoint(anchor);
        // Prefer the HWND's own DPI once it has landed on a monitor.
        const FLOAT hwndScale = GetWindowScale(hwnd);
        if (hwndScale > 0.0f)
        {
            const double monitorWidthPx =
                static_cast<double>((std::max)(1, limits.monitor.right - limits.monitor.left));
            const double monitorHeightPx =
                static_cast<double>((std::max)(1, limits.monitor.bottom - limits.monitor.top));
            limits.scale = hwndScale;
            limits.maxWidthDip = (monitorWidthPx * 0.5) / static_cast<double>(limits.scale);
            limits.maxHeightDip = (monitorHeightPx * 0.5) / static_cast<double>(limits.scale);
        }
        return limits;
    }
    POINT origin{0, 0};
    return QueryHalfScreenDipLimitsForPoint(origin);
}

double ClampWidthDipToHalfScreen(double widthDip, const HalfScreenDipLimits &limits)
{
    if (limits.maxWidthDip > 0.0)
    {
        return (std::min)(widthDip, limits.maxWidthDip);
    }
    return widthDip;
}

double ClampHeightDipToHalfScreen(double heightDip, const HalfScreenDipLimits &limits)
{
    if (limits.maxHeightDip > 0.0)
    {
        return (std::min)(heightDip, limits.maxHeightDip);
    }
    return heightDip;
}

MonitorCoordinates GetMonitorCoordinatesFromPoint(POINT pt)
{
    return MonitorWorkAreaCoordinatesFromHandle(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
}

//+---------------------------------------------------------------------------
//
// GetMonitorCoordinates
//
//----------------------------------------------------------------------------

MonitorCoordinates GetMonitorCoordinates()
{
    HWND hwnd = GetForegroundWindow();
    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hMonitor)
    {
        return MonitorCoordinates{};
    }
    return MonitorCoordinatesFromHandle(hMonitor);
}

/**
 * @brief Get the Main Monitor Coordinates
 *
 * @return MonitorCoordinates
 */
MonitorCoordinates GetMainMonitorCoordinates()
{
    MonitorCoordinates coordinates{};

    HMONITOR hPrimary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);

    if (GetMonitorInfo(hPrimary, &mi))
    {
        coordinates.left = mi.rcMonitor.left;
        coordinates.top = mi.rcMonitor.top;
        coordinates.right = mi.rcMonitor.right;
        coordinates.bottom = mi.rcMonitor.bottom;
    }

    return coordinates;
}

/**
 * @brief Get the Taskbar Height
 *
 * @return int
 */
int GetTaskbarHeight()
{
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);

    if (!SHAppBarMessage(ABM_GETTASKBARPOS, &abd))
        return 0;

    RECT &r = abd.rc;

    switch (abd.uEdge)
    {
    case ABE_BOTTOM:
    case ABE_TOP:
        return r.bottom - r.top; // 高度
    case ABE_LEFT:
    case ABE_RIGHT:
        return r.right - r.left; // 宽度（竖向任务栏）
    }

    return 0;
}

int AdjustCandidateWindowPosition(                  //
    const POINT *point,                             //
    const std::pair<double, double> &containerSize, //
    std::shared_ptr<std::pair<int, int>> properPos, //
    FLOAT layoutScale,                              //
    double minWidthDip                              //
)
{
    Global::MarginTop = 0;

    const bool isVertical = GetConfiguredCandidateWindowLayout() == "vertical";
    if (isVertical && containerSize.second > g_max_vertical_container_height_dip)
    {
        g_max_vertical_container_height_dip = containerSize.second;
    }

    // Clamp against the caret's monitor, not GetForegroundWindow()'s. Word/Excel
    // focus transitions on an extended display can otherwise pull the card onto
    // the wrong screen and clip its left edge at the virtual-desktop seam.
    MonitorCoordinates coordinates = GetMonitorCoordinatesFromPoint(*point);
    FLOAT scale = layoutScale > 0.0f ? layoutScale : GetScaleForPoint(*point);
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }
    // Design offsets are DIPs so gaps stay visually stable under 150%/200% DPI.
    const int caretGapPx = static_cast<int>(std::lround(3.0 * scale));
    const int edgePadPx = static_cast<int>(std::lround(2.0 * scale));

    properPos->first = point->x;
    properPos->second = point->y + caretGapPx;
    // minWidthDip: first-pass DOM measure can be far too narrow near a screen
    // edge. Flip using at least that floor so the card is not parked at the
    // caret with its real right half hanging off the monitor.
    const double widthDip = (std::max)(containerSize.first, minWidthDip);
    int width = static_cast<int>(std::ceil(widthDip * scale));
    // Decide whether to flip using the tallest vertical list seen so far. This
    // keeps a short list above the caret when a full list would not fit below,
    // avoiding a later below-to-above jump as more candidates appear. Position
    // the flipped window using its *current* height, however, so its lower edge
    // stays next to the current input line instead of preserving empty packing.
    const double decisionHeightDip = isVertical ? g_max_vertical_container_height_dip : containerSize.second;
    const int decisionHeightPx = static_cast<int>(std::ceil(decisionHeightDip * scale));
    const int currentHeightPx = static_cast<int>(std::ceil(containerSize.second * scale));
    if (properPos->first + width > coordinates.right - edgePadPx)
    {
        properPos->first = coordinates.right - width - edgePadPx;
    }
    if (properPos->first < coordinates.left + edgePadPx)
    {
        properPos->first = coordinates.left + edgePadPx;
    }
    if (properPos->second < coordinates.top + edgePadPx)
    {
        properPos->second = coordinates.top + edgePadPx;
    }

    // Decide against the same lower limit every downstream clamp uses. Testing
    // against the bare work-area bottom declared a card "fits below" that the
    // edge pad then pushed back up — straight onto the caret's own text line.
    if (properPos->second + decisionHeightPx > coordinates.bottom - edgePadPx)
    {
        // Point[1] is GetTextExt.bottom (the line's bottom). Sit the opaque card
        // so its bottom meets the line's top — same for short and fully expanded
        // lists. A fixed flipGap left a large hole under short above-caret cards.
        const int lineHeightPx = static_cast<int>(std::lround(24.0 * scale));
        properPos->second = point->y - currentHeightPx - lineHeightPx;
        if (properPos->second < coordinates.top + edgePadPx)
        {
            properPos->second = coordinates.top + edgePadPx;
        }
    }
    return 0;
}

void ResetCandidatePlacementMemory()
{
    g_max_vertical_container_height_dip = ::DEFAULT_WINDOW_HEIGHT_DIP;
}

// hwnd and height are part of the call contract but this placement only clamps the anchor point
// against the monitor rect, which needs the width alone.
int AdjustWndPosition( //
    HWND /*hwnd*/,     //
    int crateX,        //
    int crateY,        //
    int width,         //
    int /*height*/,    //
    int properPos[2]   //
)
{
    properPos[0] = crateX;
    properPos[1] = crateY + 3;
    MonitorCoordinates coordinates = GetMonitorCoordinates();
    if (properPos[0] < coordinates.left)
    {
        properPos[0] = coordinates.left + 2;
    }
    if (properPos[1] < coordinates.top)
    {
        properPos[1] = coordinates.top + 2;
    }
    if (properPos[0] + width > coordinates.right)
    {
        properPos[0] = coordinates.right - width - 2;
    }
    if (properPos[1] + ::DEFAULT_WINDOW_HEIGHT > coordinates.bottom)
    {
        properPos[1] = properPos[1] - ::DEFAULT_WINDOW_HEIGHT - 30 - 2;
    }
    return 0;
}

bool CoversMonitor(HWND hwnd)
{
    if (!IsWindow(hwnd) || IsIconic(hwnd) || !IsWindowVisible(hwnd))
        return false;

    if (hwnd == GetDesktopWindow() || hwnd == GetShellWindow())
        return false;

    RECT rcWin;
    GetWindowRect(hwnd, &rcWin);

    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfo(hMon, &mi);

    RECT rcMon = mi.rcMonitor;

    constexpr int tolerance = 2;                          // 容忍 1~2px
    return abs(rcWin.left - rcMon.left) <= tolerance      //
           && abs(rcWin.top - rcMon.top) <= tolerance     //
           && abs(rcWin.right - rcMon.right) <= tolerance //
           && abs(rcWin.bottom - rcMon.bottom) <= tolerance;
}

bool IsActuallyFullscreen(HWND hwnd)
{
    // 1. 基础合法性检查
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd))
        return false;

    // 2. 过滤掉桌面、任务栏等系统窗口
    //
    // 后两个是 shell 自己的覆盖层，不是真正的全屏客户端：Alt-Tab / 任务视图 /
    // 贴靠辅助用的 XAML 岛，以及前台切换过程中的中转窗口。它们都铺满整个显示器
    // 且没有 WS_CAPTION，能干净地通过下面每一项检查，于是每按一次 Alt-Tab 工具栏
    // 就会被藏一次——误判和"工具栏根本没弹出来"在现象上无法区分。
    char className[256];
    if (GetClassNameA(hwnd, className, sizeof(className)))
    {
        if (strcmp(className, "Progman") == 0 || strcmp(className, "WorkerW") == 0 ||
            strcmp(className, "Shell_TrayWnd") == 0 || strcmp(className, "XamlExplorerHostIslandWindow") == 0 ||
            strcmp(className, "ForegroundStaging") == 0)
        {
            return false;
        }
    }

    // 3. 获取窗口真实的物理位置 (DWM 坐标)
    // 使用 DWM 才能排除掉 Win10/11 窗口四周那种看不见的“隐形边框”
    RECT rcWin;
    HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rcWin, sizeof(rcWin));
    if (FAILED(hr))
    {
        // 如果 DWM 获取失败（比如句柄失效），回退到普通 Rect
        if (!GetWindowRect(hwnd, &rcWin))
            return false;
    }

    // 4. 获取窗口所在的显示器信息
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    if (!GetMonitorInfo(hMon, &mi))
        return false;

    // 5. 核心逻辑判断
    // 全屏窗口必须完全覆盖（甚至超出）屏幕的每一个边缘
    // 注意：全屏窗口的坐标往往会比 rcMonitor 稍微大一点点（例如 -1, -1），所以用 <= 和 >=
    bool coversMonitor = (rcWin.left <= mi.rcMonitor.left && rcWin.top <= mi.rcMonitor.top &&
                          rcWin.right >= mi.rcMonitor.right && rcWin.bottom >= mi.rcMonitor.bottom);

    if (!coversMonitor)
        return false;

    // 6. 排除“最大化”但不是“全屏”的情况
    // 最大化窗口会被限制在 mi.rcWork (即避开任务栏后的区域)
    // 真正的全屏会盖住任务栏
    bool touchesWorkArea = (rcWin.left == mi.rcWork.left && rcWin.top == mi.rcWork.top &&
                            rcWin.right == mi.rcWork.right && rcWin.bottom == mi.rcWork.bottom);

    // 如果窗口和工作区完全重合，且工作区小于屏幕（有任务栏存在），那它只是最大化
    if (touchesWorkArea)
    {
        if (mi.rcWork.bottom != mi.rcMonitor.bottom || mi.rcWork.right != mi.rcMonitor.right)
        {
            return false;
        }
    }

    // 7. 样式检查：全屏窗口通常没有标题栏
    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    if (style & WS_CAPTION)
    {
        // 特殊情况：有些窗口即使全屏也保留了样式位，但在屏幕外。
        // 所以如果已经盖住了屏幕，样式只是辅助参考。
        return false;
    }

    return true;
}

bool CheckFullscreen(HWND hwnd)
{
    return IsActuallyFullscreen(hwnd);
}
