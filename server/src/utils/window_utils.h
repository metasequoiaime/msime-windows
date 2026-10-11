#pragma once

#include "defines/base_structures.h"
#include <memory>
#include <utility>
#include <windows.h>

FLOAT GetWindowScale(HWND);
FLOAT GetForegroundWindowScale();
// Prefer the monitor that contains the caret / composition anchor. Foreground
// HWND can disagree with the caret on Office extended-display setups.
FLOAT GetScaleForPoint(POINT pt);

// Where the candidate scale came from — diagnostic logging must be able to
// tell the RDP foreground path apart from the plain monitor path.
enum class CandidateScaleSource
{
    Monitor,       // caret's monitor DPI (local sessions, and RDP fallback)
    RdpForeground, // foreground window DPI inside an RDP session
};

struct ResolvedCandidateScale
{
    FLOAT scale = 0.0f;
    CandidateScaleSource source = CandidateScaleSource::Monitor;
};

// Candidate window scale authority. RDP syncs the client's display scaling
// into the session's monitor DPI metadata (150% client -> 144), while the
// focused application in the session usually still renders at 96 DPI, so the
// monitor metadata is unreliable there and GetDpiForWindow(foreground) is the
// best proxy for what the user actually sees:
//   - DPI-unaware / System Aware host -> virtualized 96, matches the app;
//   - PMv2 and correctly following -> current monitor DPI, matches the app;
//   - PMv2 but not following (pathological) -> mismatch, cannot be detected
//     via public APIs; documented as a known residue, not probed.
// Local sessions keep the caret-monitor convention (mixed-DPI multi-monitor
// setups depend on it). Falls back to the monitor path when the foreground
// window is gone (lock screen, focus switch) so behavior matches pre-fix.
ResolvedCandidateScale ResolveCandidateScaleForCaret(POINT caret);
// Candidate placement uses that monitor's work area to avoid taskbars/app bars.
MonitorCoordinates GetMonitorCoordinatesFromPoint(POINT pt);

MonitorCoordinates GetMonitorCoordinates();
MonitorCoordinates GetMainMonitorCoordinates();
int GetTaskbarHeight();

// Half of the target monitor in CSS DIPs (physical/2 / dpiScale). Single source
// of truth for FTB / menu / candidate max content size.
struct HalfScreenDipLimits
{
    FLOAT scale = 1.0f;
    double maxWidthDip = 0.0;
    double maxHeightDip = 0.0;
    MonitorCoordinates monitor{};
};
HalfScreenDipLimits QueryHalfScreenDipLimitsForHwnd(HWND hwnd);
HalfScreenDipLimits QueryHalfScreenDipLimitsForPoint(POINT pt);
double ClampWidthDipToHalfScreen(double widthDip, const HalfScreenDipLimits &limits);
double ClampHeightDipToHalfScreen(double heightDip, const HalfScreenDipLimits &limits);

// Host placement on mixed-DPI multi-monitor setups (floating toolbar).
// MonitorFromWindow follows the monitor the window currently sits on, so
// clamping against it pins the host to the screen it came from and makes a
// caption drag across a seam bounce back. The visible region is the union of
// every monitor's area: a host may straddle a seam, and is only pulled back
// when part of it leaves that region.
enum class ScreenArea
{
    Monitor,  // full monitor rect; a host may overlap the taskbar
    WorkArea, // rcWork; excludes the taskbar and app bars
};
// Leaves `rect` alone while it lies inside the visible region; otherwise fits
// it (size preserved) into the monitor nearest to its center. Returns true
// when the rect was moved; `monitor` then receives the monitor used.
// `offscreenInsets` (physical px per side) is transparent margin, such as a
// drop shadow, that may hang off the screen: only the rect deflated by it has
// to stay visible, so the visible part can sit flush with an edge.
bool KeepRectOnVisibleScreens(RECT &rect, ScreenArea area, HMONITOR *monitor = nullptr,
                              const RECT &offscreenInsets = {});
// Clips `hwnd` to the part of it inside the visible region, so margin hanging
// off an edge or over the taskbar is neither drawn nor hit-tested there.
// Clears the region again once the whole window is visible.
void ClipWindowToVisibleScreens(HWND hwnd, ScreenArea area);
// True while `hwnd` is inside a native move/size loop (caption drag).
bool IsWindowInMoveSizeLoop(HWND hwnd);
// Top-left for a host about to be resized to `width` x `height` physical px.
// Starts from WM_DPICHANGED's `suggestedRect` when given, else the current
// position. Outside a move loop the result is kept on the visible monitors;
// during a caption drag the loop owns the position and nothing is clamped.
// `previousInsets`: the offscreenInsets the host currently has. When they
// differ, the visible part keeps its position and is kept in the work area.
POINT PlaceResizedHost(HWND hwnd, int width, int height, const RECT *suggestedRect, const RECT &offscreenInsets = {},
                       const RECT *previousInsets = nullptr);

int AdjustCandidateWindowPosition(        //
    const POINT *point,                   //
    const std::pair<double, double> &,    //
    std::shared_ptr<std::pair<int, int>>, //
    FLOAT layoutScale = 0.0f,             //
    double minWidthDip = 0.0              //
);

// Drop the "tallest list so far" flip memory. A bogus oversized measure would
// otherwise keep parking later cards at the top/left of the monitor.
void ResetCandidatePlacementMemory();

int AdjustWndPosition( //
    HWND hwnd,         //
    int crateX,        //
    int crateY,        //
    int width,         //
    int height,        //
    int properPos[2]   //
);
