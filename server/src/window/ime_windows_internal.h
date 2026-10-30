#pragma once

// ime_windows*.cpp 之间共享的内部声明：拆分前同在 ime_windows.cpp 里、现在跨文件使用的状态、常量、宏与函数。
// 只给 server/src/window/ime_windows*.cpp 包含，其他地方不要引用。

#include <windows.h>
#include <string>
#include "config/ime_config.h"
#include "utils/window_utils.h"
#include "window/small_windows.h"
#include "log/candidate_diag_log.h"

// Places the floating toolbar on the primary monitor's bottom-right slot once
// the host exists.
constexpr UINT_PTR TIMER_ID_PLACE_FTB = 4;
constexpr UINT_PTR TIMER_ID_CONFIG_SYNC = 7;
// Remeasure FTB after live DPI / display-mode changes.
constexpr UINT_PTR TIMER_ID_FTB_DPI_REMEASURE = 10;
constexpr UINT_PTR TIMER_ID_CANDIDATE_MOVE_SETTLE = 11;
constexpr UINT kCandidateMoveSettleMs = 50;
// A hide that is immediately followed by another show is not a teardown: it is
// one keystroke of a burst the worker thread drained late (backspacing the
// composition empty and typing again). Acting on it cloaks a visible candidate
// window and un-cloaks it a few frames later, which is the flicker seen while
// the machine is busy. Defer the hide by this much and drop it outright when a
// show arrives first; a real commit or focus loss still hides, just this many
// milliseconds later.
constexpr UINT_PTR TIMER_ID_CANDIDATE_HIDE_GRACE = 12;
constexpr UINT kCandidateHideGraceMs = 50;
// Floating toolbar auto-hide. ID 1 on the toolbar HWND belongs to msimeui's
// caret-blink timer. The countdown polls so hover can be sampled without any
// mouse messages; the fade steps the layered alpha down.
constexpr UINT_PTR TIMER_ID_FTB_AUTO_HIDE = 13;
constexpr UINT_PTR TIMER_ID_FTB_AUTO_HIDE_FADE = 14;
constexpr UINT kFloatingToolbarAutoHidePollMs = 200;
constexpr UINT kFloatingToolbarAutoHideFadeStepMs = 16;
constexpr UINT kFloatingToolbarAutoHideFadeMs = 400;
// The DLL sends (0, INVALID_Y) when the host gives no usable text extent. Chromium
// does that for a moment at composition start and then reports the real one, so
// only ask the host for its caret ourselves (window/caret_fallback.h) once the
// anchor has stayed unresolved this long. Firefox's overflowed address bar never
// resolves it (#618).
constexpr UINT_PTR TIMER_ID_CANDIDATE_CARET_FALLBACK = 15;
constexpr UINT kCandidateCaretFallbackDelayMs = 100;
// Focus reaches the foreground hook (out-of-context, posted) a step before the
// incoming TIP activation does (it travels the Main pipe, and the reveal is
// posted back to this thread). Re-evaluating on the raw edge would blink the
// toolbar on every switch between two windows that both use this IME, so a
// foreground change settles for this long before the visibility is re-decided.
constexpr UINT_PTR TIMER_ID_FTB_FOREGROUND_SETTLE = 16;
constexpr UINT kFloatingToolbarForegroundSettleMs = 120;
// Windows does not deliver EVENT_SYSTEM_FOREGROUND to this process for windows of
// higher integrity (Task Manager, an elevated console): UIPI filters the
// out-of-context hook. Without a fallback the toolbar keeps the previous window's
// visibility for as long as such a window is focused. Polling the foreground
// window costs one API call and closes that hole; the event stays the fast path.
constexpr UINT_PTR TIMER_ID_FTB_FOREGROUND_POLL = 17;
constexpr UINT kFloatingToolbarForegroundPollMs = 500;
// A failed lookup is retried on a later keystroke, at most this many times per
// composition: the first MSAA/UIA query may only wake the host's accessibility.
constexpr int kCandidateCaretFallbackMaxAttempts = 3;
// When every lookup fails the card's top-left goes to this many physical pixels
// from the top-left of the foreground window's monitor.
constexpr LONG kCandidateCornerFallbackInsetPx = 2;
// A show repeating the previous frame's preedit, page and caret within this
// window is a duplicate post rather than new state — the candidate rebuild and
// the English/cloud merge each request a show per keystroke. Rendering both
// paints the same picture twice. Keep the window short so anything the
// signature does not capture (skin reload, DPI change) self-heals on the next
// keystroke instead of sticking.
constexpr UINT kCandidateShowDedupWindowMs = 250;

int FineTuneWindow(HWND hwnd);
std::wstring DescribeCandidateHostState();

namespace ime_windows_detail
{
extern bool g_is_ime_active;
extern int g_last_placed_caret_x;
extern int g_last_placed_caret_y;
extern bool g_candidate_session_anchor_valid;
extern POINT g_candidate_session_anchor;
extern int g_candidate_dpi_change_suppress_count;

struct SuppressCandidateDpiChange
{
    SuppressCandidateDpiChange()
    {
        ++g_candidate_dpi_change_suppress_count;
    }
    ~SuppressCandidateDpiChange()
    {
        --g_candidate_dpi_change_suppress_count;
    }
    SuppressCandidateDpiChange(const SuppressCandidateDpiChange &) = delete;
    SuppressCandidateDpiChange &operator=(const SuppressCandidateDpiChange &) = delete;
};

// ime_windows.cpp
bool IsSystemLightDarkToggle(UINT message, LPARAM lParam);
void SetHostWindowCloaked(HWND hwnd, bool cloaked);
bool IsHostWindowCloaked(HWND hwnd);
std::wstring DescribeFloatingToolbarHostState();

// ime_windows_candidate.cpp
POINT GetCandidateLayoutCaret();
bool IsCandidateHostPaintedVisible(HWND hwnd);
bool CandidateCaretNeedsStableFollow(HWND hwnd, POINT caret);

// ime_windows_toolbar.cpp
int ConfiguredFloatingToolbarWidth();
int ConfiguredFloatingToolbarHeight();
bool FloatingToolbarItemsEqual(const FloatingToolbarItemsConfig &left, const FloatingToolbarItemsConfig &right);
} // namespace ime_windows_detail
