#pragma once

// Shared, immutable screen anchor for the voice overlays. The monitor is captured once when a
// recording starts, on the control thread, and the same value is handed to both the live overlay
// and the result card. Neither window may re-resolve the foreground monitor on every paint: doing
// that would make the card drift to another display when focus changes while ASR is running.
//
// The struct is deliberately just data (no HWND, COM or owning host). It can be copied across
// threads and is also usable by small independent fixtures, which may leave it invalid and let the
// overlay fall back to capturing the foreground monitor itself.

#include "mvi_utils.h"

#include <windows.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>

inline constexpr int kVoiceOverlayBottomMarginDip = 24;
inline constexpr int kVoiceOverlayPreferredWidthDip = 420;
inline constexpr UINT kVoiceOverlayDefaultDpi = 96;

struct VoiceOverlayAnchor
{
    RECT work{};                        // rcWork in physical pixels; already excludes taskbars/app bars on any edge.
    RECT full{};                        // rcMonitor in physical pixels; preserves the recording bar's centering.
    UINT dpi = kVoiceOverlayDefaultDpi; // Effective DPI of the captured monitor.
    bool valid = false;

    float scale() const noexcept
    {
        const UINT effective = dpi == 0 ? kVoiceOverlayDefaultDpi : dpi;
        return static_cast<float>(effective) / static_cast<float>(kVoiceOverlayDefaultDpi);
    }

    // Distance from the work-area bottom to the overlay bottom, in physical pixels.
    int bottom_margin_px() const noexcept
    {
        const UINT effective = dpi == 0 ? kVoiceOverlayDefaultDpi : dpi;
        return MulDiv(kVoiceOverlayBottomMarginDip, static_cast<int>(effective),
                      static_cast<int>(kVoiceOverlayDefaultDpi));
    }
};

// Safe from a background/control thread. GetForegroundWindow/GetMonitorInfo/GetDpiForMonitor do not
// require a message pump. The foreground monitor is sampled once per call so rcWork and DPI always
// describe the same display, including displays with negative virtual-screen coordinates.
inline VoiceOverlayAnchor CaptureVoiceOverlayAnchor()
{
    VoiceOverlayAnchor anchor;
    const HMONITOR monitor = mvi_utils::GetForegroundMonitor();
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info))
        return anchor;

    UINT dpi_x = kVoiceOverlayDefaultDpi;
    UINT dpi_y = kVoiceOverlayDefaultDpi;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) || dpi_x == 0)
    {
        dpi_x = GetDpiForSystem();
        if (dpi_x == 0)
            dpi_x = kVoiceOverlayDefaultDpi;
    }

    anchor.work = info.rcWork;
    anchor.full = info.rcMonitor;
    anchor.dpi = dpi_x;
    anchor.valid = info.rcWork.right > info.rcWork.left && info.rcWork.bottom > info.rcWork.top;
    return anchor;
}

// Refresh a previously captured anchor after the system reports a display/DPI layout change. The
// monitor is chosen from the anchor's own saved full rectangle with MONITOR_DEFAULTTONEAREST, never
// from GetForegroundWindow: ordinary focus changes during recognition must not move the overlays.
// Callers hold their anchor mutex across read-refresh-write so a newer recording anchor cannot be
// overwritten by a stale refresh.
inline VoiceOverlayAnchor RefreshVoiceOverlayAnchorForDisplayChange(const VoiceOverlayAnchor &anchor)
{
    if (!anchor.valid)
        return anchor;

    RECT reference = anchor.full;
    if (reference.right <= reference.left || reference.bottom <= reference.top)
        reference = anchor.work;
    const HMONITOR monitor = MonitorFromRect(&reference, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info))
        return anchor;

    VoiceOverlayAnchor refreshed = anchor;
    refreshed.work = info.rcWork;
    refreshed.full = info.rcMonitor;

    UINT dpi_x = anchor.dpi;
    UINT dpi_y = anchor.dpi;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) || dpi_x == 0)
    {
        dpi_x = GetDpiForSystem();
        if (dpi_x == 0)
            dpi_x = kVoiceOverlayDefaultDpi;
    }
    refreshed.dpi = dpi_x;
    refreshed.valid = info.rcWork.right > info.rcWork.left && info.rcWork.bottom > info.rcWork.top;
    return refreshed;
}

inline int VoiceOverlayDipToPx(float dip, UINT dpi) noexcept
{
    const UINT effective = dpi == 0 ? kVoiceOverlayDefaultDpi : dpi;
    return static_cast<int>(
        std::lround(static_cast<double>(dip) * static_cast<double>(effective) / kVoiceOverlayDefaultDpi));
}

// Place a physical-pixel sized overlay horizontally centered in the captured work area and with its
// bottom edge kVoiceOverlayBottomMarginDip above the work-area bottom. Callers grow the height when
// the card expands, so the bottom stays fixed and the window grows upward. Width/height are clamped
// to the work area for small displays; when even the margin cannot fit, the bottom is clamped to the
// work area instead of extending past it. Negative monitor origins are preserved.
inline RECT VoiceOverlayBottomAnchoredRect(const VoiceOverlayAnchor &anchor, int width_px, int height_px) noexcept
{
    RECT rect{};
    const int work_width = static_cast<int>(anchor.work.right - anchor.work.left);
    const int work_height = static_cast<int>(anchor.work.bottom - anchor.work.top);
    if (!anchor.valid || work_width <= 0 || work_height <= 0)
        return rect;

    const int width = (std::max)(1, (std::min)(width_px, work_width));
    const int height = (std::max)(1, (std::min)(height_px, work_height));
    const int left = anchor.work.left + (work_width - width) / 2;
    int top = anchor.work.bottom - anchor.bottom_margin_px() - height;
    top = (std::max)(static_cast<int>(anchor.work.top), (std::min)(top, static_cast<int>(anchor.work.bottom) - height));

    rect.left = left;
    rect.top = top;
    rect.right = left + width;
    rect.bottom = top + height;
    return rect;
}
