#include "msimeui/Compat.h"

#include <dwmapi.h>

// DWMWA_CLOAK / DWMWA_CLOAKED arrive with the Windows 8 SDK headers and are
// hidden behind WINVER guards; the dwmapi.dll export itself is Vista+ and the
// attribute simply fails below Windows 8.
#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif
#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

namespace msimeui
{
namespace
{
// The layout of OSVERSIONINFOW, declared locally so this file does not depend
// on where the RTL_OSVERSIONINFOW typedef sits in a given SDK.
struct OsVersionInfo
{
    unsigned long size;
    unsigned long major;
    unsigned long minor;
    unsigned long build;
    unsigned long platform;
    wchar_t servicePack[128];
};

// Windows version from ntdll. VerifyVersionInfo is shaped by the executable's
// manifest, which lies under compatibility settings; the ntdll export reports
// the version the system really runs. Resolved once per process.
using RtlGetVersionFn = long(__stdcall *)(OsVersionInfo *);

const OsVersionInfo &OsVersion()
{
    static const OsVersionInfo info = [] {
        OsVersionInfo version = {};
        version.size = sizeof(version);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
            ntdll ? reinterpret_cast<void *>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr);
        if (!rtlGetVersion || rtlGetVersion(&version) != 0)
        {
            version = {};
            version.size = sizeof(version);
        }
        return version;
    }();
    return info;
}

bool IsWindowsOrGreater(unsigned long major, unsigned long minor)
{
    const OsVersionInfo info = OsVersion();
    return info.major > major || (info.major == major && info.minor >= minor);
}

template <typename Fn> Fn Resolve(const wchar_t *module, const char *name)
{
    // GetModuleHandleW only sees already loaded modules; shcore.dll is not
    // loaded by default on Windows 7/8, so try to load it on demand.
    HMODULE handle = GetModuleHandleW(module);
    if (!handle)
    {
        handle = LoadLibraryW(module);
    }
    return reinterpret_cast<Fn>(handle ? reinterpret_cast<void *>(GetProcAddress(handle, name)) : nullptr);
}

// DPI of the monitor's own display, without shcore: a DC created for the
// monitor's device carries the effective DPI.
HRESULT MonitorDpiFromDevice(HMONITOR monitor, UINT *dpi)
{
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
    {
        return E_FAIL;
    }
    const HDC dc = CreateDCW(info.szDevice, info.szDevice, nullptr, nullptr);
    if (!dc)
    {
        return E_FAIL;
    }
    const UINT monitorDpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
    DeleteDC(dc);
    if (monitorDpi == 0)
    {
        return E_FAIL;
    }
    *dpi = monitorDpi;
    return S_OK;
}
} // namespace

namespace compat
{
bool IsWindows8OrGreater()
{
    return IsWindowsOrGreater(6, 2);
}

bool IsWindows81OrGreater()
{
    return IsWindowsOrGreater(6, 3);
}

bool IsWindows10OrGreater()
{
    return IsWindowsOrGreater(10, 0);
}

UINT GetWindowDpi(HWND hwnd)
{
    // A zero return means "no window"; callers treat 0 as invalid and fall back
    // to 96, so keep the non-zero semantics the Win10 API has.
    if (hwnd)
    {
        using GetDpiForWindowFn = UINT(__stdcall *)(HWND);
        static const GetDpiForWindowFn fn = Resolve<GetDpiForWindowFn>(L"user32.dll", "GetDpiForWindow");
        if (fn)
        {
            const UINT dpi = fn(hwnd);
            if (dpi > 0)
            {
                return dpi;
            }
        }
    }
    return GetSystemDpi();
}

UINT GetSystemDpi()
{
    using GetDpiForSystemFn = UINT(__stdcall *)();
    static const GetDpiForSystemFn fn = Resolve<GetDpiForSystemFn>(L"user32.dll", "GetDpiForSystem");
    if (fn)
    {
        const UINT dpi = fn();
        if (dpi > 0)
        {
            return dpi;
        }
    }
    const HDC dc = GetDC(nullptr);
    if (!dc)
    {
        return 96;
    }
    const UINT dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
    ReleaseDC(nullptr, dc);
    return dpi > 0 ? dpi : 96;
}

bool SetProcessDpiAwarenessPerMonitorV2()
{
    using SetProcessDpiAwarenessContextFn = BOOL(__stdcall *)(HANDLE);
    static const SetProcessDpiAwarenessContextFn contextFn =
        Resolve<SetProcessDpiAwarenessContextFn>(L"user32.dll", "SetProcessDpiAwarenessContext");
    if (contextFn && contextFn(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) // PER_MONITOR_AWARE_V2
    {
        return true;
    }

    using SetProcessDpiAwarenessFn = HRESULT(__stdcall *)(int);
    static const SetProcessDpiAwarenessFn awarenessFn =
        Resolve<SetProcessDpiAwarenessFn>(L"shcore.dll", "SetProcessDpiAwareness");
    if (awarenessFn && SUCCEEDED(awarenessFn(2))) // PROCESS_PER_MONITOR_DPI_AWARE
    {
        return true;
    }

    using SetProcessDPIAwareFn = BOOL(__stdcall *)();
    static const SetProcessDPIAwareFn awareFn = Resolve<SetProcessDPIAwareFn>(L"user32.dll", "SetProcessDPIAware");
    return awareFn && awareFn() != FALSE;
}

bool AdjustWindowRectExForDpi(LPRECT rect, DWORD style, BOOL menu, DWORD exStyle, UINT dpi)
{
    using AdjustWindowRectExForDpiFn = BOOL(__stdcall *)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const AdjustWindowRectExForDpiFn fn =
        Resolve<AdjustWindowRectExForDpiFn>(L"user32.dll", "AdjustWindowRectExForDpi");
    if (fn && fn(rect, style, menu, exStyle, dpi))
    {
        return true;
    }
    return AdjustWindowRectEx(rect, style, menu, exStyle) != FALSE;
}

int GetSystemMetricsForDpi(int index, UINT dpi)
{
    using GetSystemMetricsForDpiFn = int(__stdcall *)(int, UINT);
    static const GetSystemMetricsForDpiFn fn =
        Resolve<GetSystemMetricsForDpiFn>(L"user32.dll", "GetSystemMetricsForDpi");
    if (fn)
    {
        return fn(index, dpi);
    }
    (void)dpi;
    return GetSystemMetrics(index);
}

HRESULT GetMonitorDpi(HMONITOR monitor, UINT *dpi)
{
    if (!monitor || !dpi)
    {
        return E_POINTER;
    }

    using GetDpiForMonitorFn = HRESULT(__stdcall *)(HMONITOR, int, UINT *, UINT *);
    static const GetDpiForMonitorFn fn = Resolve<GetDpiForMonitorFn>(L"shcore.dll", "GetDpiForMonitor");
    if (fn)
    {
        UINT dpiX = 0;
        UINT dpiY = 0;
        const HRESULT hr = fn(monitor, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY);
        if (SUCCEEDED(hr))
        {
            *dpi = dpiX;
            return hr;
        }
    }
    return MonitorDpiFromDevice(monitor, dpi);
}

bool SetWindowCloaked(HWND hwnd, bool cloaked)
{
    if (!hwnd || !IsWindows8OrGreater())
    {
        return false;
    }
    const BOOL value = cloaked ? TRUE : FALSE;
    return SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &value, sizeof(value)));
}

bool IsWindowCloaked(HWND hwnd, bool *cloaked)
{
    if (cloaked)
    {
        *cloaked = false;
    }
    if (!hwnd || !cloaked || !IsWindows8OrGreater())
    {
        return false;
    }
    BOOL value = FALSE;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &value, sizeof(value))))
    {
        return false;
    }
    *cloaked = value != FALSE;
    return true;
}
} // namespace compat
} // namespace msimeui
