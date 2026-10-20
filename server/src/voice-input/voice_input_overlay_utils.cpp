#include "mvi_utils.h"

#include <msimeui/Compat.h>

HMONITOR mvi_utils::GetForegroundMonitor()
{
    const HWND foreground = GetForegroundWindow();
    return MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
}

mvi_utils::MonitorMetrics mvi_utils::GetForegroundMonitorMetrics()
{
    MonitorMetrics metrics{};
    const HMONITOR monitor = GetForegroundMonitor();
    MONITORINFO info{sizeof(info)};
    if (monitor && GetMonitorInfoW(monitor, &info))
    {
        metrics.full = info.rcMonitor;
        metrics.work = info.rcWork;
    }
    return metrics;
}

float mvi_utils::GetForegroundMonitorScale()
{
    const HMONITOR monitor = GetForegroundMonitor();
    if (monitor)
    {
        UINT dpi = 0;
        // MDT_EFFECTIVE_DPI semantics; shcore resolved dynamically, DC fallback
        // on systems without it.
        if (SUCCEEDED(msimeui::compat::GetMonitorDpi(monitor, &dpi)) && dpi != 0)
            return static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
    }

    // 取不到显示器 DPI 时退回系统 DPI，最后兜底 1.0，避免把窗口缩放成 0。
    UINT systemDpi = msimeui::compat::GetSystemDpi();
    if (systemDpi == 0)
        systemDpi = USER_DEFAULT_SCREEN_DPI;
    return static_cast<float>(systemDpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
}
