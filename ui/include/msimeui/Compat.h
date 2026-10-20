#pragma once

#include <windows.h>

namespace msimeui
{
namespace compat
{
// True when running on Windows 8 or newer.
bool IsWindows8OrGreater();
// True when running on Windows 8.1 or newer.
bool IsWindows81OrGreater();
// True when running on Windows 10 or newer.
bool IsWindows10OrGreater();

// DPI helpers. The per-monitor user32/shcore entry points are resolved with
// GetProcAddress so a binary built for Windows 10 still loads on Windows 7,
// where they do not exist; every helper falls back to the legacy API set.
UINT GetWindowDpi(HWND hwnd);
UINT GetSystemDpi();
bool SetProcessDpiAwarenessPerMonitorV2();
bool AdjustWindowRectExForDpi(LPRECT rect, DWORD style, BOOL menu, DWORD exStyle, UINT dpi);
int GetSystemMetricsForDpi(int index, UINT dpi);
// Effective DPI of a monitor (MDT_EFFECTIVE_DPI semantics).
HRESULT GetMonitorDpi(HMONITOR monitor, UINT *dpi);

// Cloak. Returns false on platforms without DWM cloaking (Windows 7); callers
// must fall back to ShowWindow. IsWindowCloaked reports the DWM cloaked state
// and returns false when the attribute is unavailable.
bool SetWindowCloaked(HWND hwnd, bool cloaked);
bool IsWindowCloaked(HWND hwnd, bool *cloaked);
} // namespace compat
} // namespace msimeui
