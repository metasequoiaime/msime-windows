#pragma once

#include <dwmapi.h>

// Windows 10+ DWM appearance attributes. The real SDK headers declare these
// unconditionally (no NTDDI/WINVER gate at all -- the attributes ship in every
// SDK since 10.0.22000 and the enum members are always visible), so the only
// configuration that needs them reproduced here is one whose dwmapi.h predates
// the attribute entirely. The values are public ABI constants and
// DwmSetWindowAttribute simply fails on systems that predate them, which the
// call sites already treat as non-fatal.
//
// Everything is a #define, never an enum: enum member names with the same
// spelling as the SDK's would be a redefinition even with identical values,
// while macros only take effect for the text after this include and leave the
// SDK's own (already-parsed) declarations untouched. The NTDDI guard keeps the
// block out of the way on SDKs that do declare the attribute; NTDDI_VERSION is
// 0x06010000 under a Windows 7 target, so the block compiles in there.

#if !defined(NTDDI_WIN10_RS5) || NTDDI_VERSION < NTDDI_WIN10_RS5
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#if !defined(NTDDI_WIN10_CO) || NTDDI_VERSION < NTDDI_WIN10_CO
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#define DWMWCP_DEFAULT 0
#define DWMWCP_DONOTROUND 1
#define DWMWCP_ROUND 2
#define DWMWCP_ROUNDSMALL 3
#endif

#if !defined(NTDDI_WIN10_21H1) || NTDDI_VERSION < NTDDI_WIN10_21H1
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#define DWMSBT_AUTO 0
#define DWMSBT_NONE 1
#define DWMSBT_MAINWINDOW 2
#define DWMSBT_TRANSIENTWINDOW 3
#define DWMSBT_TABBEDWINDOW 4
#endif
