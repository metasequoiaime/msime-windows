#pragma once

// ime_windows*.cpp 之间共享的内部声明：拆分前同在 ime_windows.cpp 里、现在跨文件使用的状态、常量、宏与函数。
// 只给 server/src/window/ime_windows*.cpp 包含，其他地方不要引用。

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include "config/ime_config.h"
#include "utils/window_utils.h"
#include "webview2/windows_webview2.h"
#include "log/candidate_diag_log.h"

#define WEBVIEW_DIAG_LOGF(...) DIAG_LOGF(__VA_ARGS__)

constexpr UINT_PTR TIMER_ID_INIT_WEBVIEW_MENU = 2;
constexpr UINT_PTR TIMER_ID_MOVE_WEBVIEW_FTB = 4;
constexpr UINT_PTR TIMER_ID_CONFIG_SYNC = 7;
constexpr UINT_PTR TIMER_ID_FTB_VISIBILITY_RECONCILE = 9;
// Remeasure FTB after live DPI / display-mode changes (mirrors menu's INIT timer).
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
// page-side mouse messages; the fade steps the layered alpha down.
constexpr UINT_PTR TIMER_ID_FTB_AUTO_HIDE = 13;
constexpr UINT_PTR TIMER_ID_FTB_AUTO_HIDE_FADE = 14;
constexpr UINT kFloatingToolbarAutoHidePollMs = 200;
constexpr UINT kFloatingToolbarAutoHideFadeStepMs = 16;
constexpr UINT kFloatingToolbarAutoHideFadeMs = 400;
// A show repeating the previous frame's preedit, page and caret within this
// window is a duplicate post rather than new state — the candidate rebuild and
// the English/cloud merge each request a show per keystroke. Rendering both
// paints the same picture twice. Keep the window short so anything the
// signature does not capture (skin reload, DPI change) self-heals on the next
// keystroke instead of sticking.
constexpr UINT kCandidateShowDedupWindowMs = 250;
// Long enough fallback when the page never posts ready (old HTML / failed JS).
// Page-ready normally ends the grace earlier; until then the toolbar stays shown.
constexpr UINT kFloatingToolbarPaintGraceMs = 6000;

int FineTuneWindow(HWND hwnd);
int FineTuneWindow(HWND hwnd, UINT firstFlag, UINT secondFlag);
std::wstring DescribeCandidateHostState();

namespace ime_windows_detail
{
extern bool g_is_ime_active;
extern std::atomic<uint64_t> g_candidate_finetune_generation;
extern std::atomic<bool> g_candidate_layout_inflight;
extern std::atomic<uint64_t> g_candidate_content_generation;
extern int g_last_placed_caret_x;
extern int g_last_placed_caret_y;
extern bool g_candidate_session_anchor_valid;
extern POINT g_candidate_session_anchor;
extern bool g_has_last_candidate_clip;
extern std::pair<double, double> g_last_candidate_clip_size;
extern FLOAT g_last_candidate_clip_scale;
extern bool g_candidate_placed_above_caret;
extern std::pair<double, double> g_last_candidate_card_size;
extern bool g_has_candidate_clip_envelope;
extern double g_clip_envelope_top_dip;
extern double g_clip_envelope_bottom_dip;
extern double g_clip_envelope_left_dip;
extern double g_clip_envelope_right_dip;
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
void SyncHostWebViewBounds(ICoreWebView2Controller *controller, HWND hwnd);
void SetHostWindowCloaked(HWND hwnd, bool cloaked);
bool IsHostWindowCloaked(HWND hwnd);
std::wstring DescribeFloatingToolbarHostState();

// ime_windows_candidate_layout.cpp
POINT GetCandidateLayoutCaret();
std::pair<int, int> ComputeQuarterScreenHostPixels(const MonitorCoordinates &monitor);
void LogCandidatePositionAudit(const wchar_t *stage, POINT rawCaret, POINT layoutCaret, POINT properPos, int hostX,
                               int hostY, int hostWidthPx, int hostHeightPx, FLOAT scale, double contentWidthDip,
                               double contentHeightDip, const MonitorCoordinates &monitor, int marginLeftBefore = -1,
                               int marginTopBefore = -1);
double GetCandidateDecorationTopDip();
int GetCandidatePackingMarginTopDip();
std::pair<double, double> AddCandidateDecorationToSize(const std::pair<double, double> &cardSize);
int GetCandidateOuterTopPx(int cardAnchorY, int packingTopDip, FLOAT scale);
void ClipCandidateWindowToContent(HWND hwnd, const std::pair<double, double> &containerSize, FLOAT scale,
                                  double extraTopDip = 0.0);
void ClearCandidateWindowRegion(HWND hwnd);
int GetCandidateOuterMarginDip(int desiredOuterTopPx, int hostY, FLOAT scale);
void KeepCandidateCardInsideHostAndMonitor(int &hostX, int &hostY, int hostWidthPx, int hostHeightPx,
                                           double contentWidthDip, double contentHeightDip, FLOAT layoutScale,
                                           const MonitorCoordinates &coordinates, double minContentWidthDip = 0.0);
void RememberCandidateClip(const std::pair<double, double> &decoratedSize, FLOAT scale, int caretX, int caretY);
void RememberCandidateClipSize(const std::pair<double, double> &decoratedSize, FLOAT scale);
void RememberCandidateFlip(int cardTopPx, int caretY);
bool CandidateCardFitsClipEnvelope(const std::pair<double, double> &decoratedSize);
double EstimateVerticalPageHeightDip(double currentHeightDip);
void ReanchorCandidateHostToPaintedCard(const POINT &layoutCaret, const std::pair<double, double> &cardSize,
                                        const MonitorCoordinates &coordinates, int hostHeightPx, FLOAT scale,
                                        int &hostX, int &hostY);
bool IsCandidateHostPaintedVisible(HWND hwnd);
bool CandidateCaretNeedsStableFollow(HWND hwnd, POINT caret);

// ime_windows_candidate_show.cpp
void RefreshCandidateClipAfterPaint(HWND hwnd, uint64_t contentGeneration, ULONGLONG updateStartedTick);
void PlaceCandidateHostNearCaret(HWND hwnd, std::pair<double, double> requestedCardSize = {});

// ime_windows_toolbar.cpp
int ConfiguredFloatingToolbarWidth();
int ConfiguredFloatingToolbarHeight();
bool FloatingToolbarItemsEqual(const FloatingToolbarItemsConfig &left, const FloatingToolbarItemsConfig &right);
} // namespace ime_windows_detail
