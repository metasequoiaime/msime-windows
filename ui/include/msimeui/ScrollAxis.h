#pragma once

#include <windows.h>

// Scroll arithmetic along one axis, in DIPs. It knows nothing about direction, drawing,
// colors, hover, timers, windows or mouse messages: a presenter draws its own scrollbar,
// takes the mouse itself, converts pixels to DIPs, and asks these functions where the
// thumb goes and which offset a gesture means.
namespace msimeui
{
// How long the content is and how much of it fits in view. Negative lengths count as 0.
struct ScrollExtent
{
    float content = 0.0f;
    float viewport = 0.0f;
};

// The scrollbar track along the axis, in the presenter's coordinates.
struct ScrollTrack
{
    float start = 0.0f;
    float length = 0.0f;
    // Gap kept between the thumb and each end of the track.
    float thumbInset = 0.0f;
    float thumbMinLength = 0.0f;
};

struct ThumbSpan
{
    float start = 0.0f;
    float length = 0.0f;
};

// A press on the scrollbar: the offset right after it, and where along the thumb the
// pointer holds it (pointer minus thumb start) for the drag that follows.
struct ThumbGrab
{
    float offset = 0.0f;
    float grip = 0.0f;
};

// Content length minus viewport length, never below 0.
float MaxScrollOffset(const ScrollExtent &extent);
float ClampScrollOffset(float offset, const ScrollExtent &extent);
// The content overflows the viewport by more than half a DIP, the threshold below which
// no scrollbar is shown.
bool CanScroll(const ScrollExtent &extent);

// The thumb is the visible share of the track between the insets, no shorter than
// thumbMinLength and no longer than that space; it travels the rest of the space as
// the offset goes from 0 to the maximum.
ThumbSpan ThumbAt(const ScrollExtent &extent, const ScrollTrack &track, float offset);
// Offset that puts the thumb's start at `thumbStart`, clamped to the thumb's travel.
// A thumb with no travel is treated as able to move 1 DIP.
float OffsetForThumbStart(const ScrollExtent &extent, const ScrollTrack &track, float thumbStart);
// Pressing at `pointer` on the thumb keeps the offset and grips the thumb where it was
// pressed. Pressing on the track beside the thumb jumps so the thumb is centred on the
// pointer, as far as its travel allows, and grips it from there.
ThumbGrab PressScrollbar(const ScrollExtent &extent, const ScrollTrack &track, float offset, float pointer);
// Offset while dragging with the grip PressScrollbar returned.
float DragThumb(const ScrollExtent &extent, const ScrollTrack &track, float grip, float pointer);

// Control Panel's "lines to scroll at a time" (SPI_GETWHEELSCROLLLINES); 3 when it
// cannot be read. WHEEL_PAGESCROLL means one screen per notch.
UINT SystemWheelScrollLines();
// Replaces where SystemWheelScrollLines reads the setting from, so tests can scroll at
// any setting without changing the machine's; nullptr goes back to the system setting.
// Returns the source it replaces.
using WheelScrollLinesSource = UINT (*)();
WheelScrollLinesSource SetWheelScrollLinesSource(WheelScrollLinesSource source);
// Distance of one wheel notch: `stepAtThreeLines` scaled by lines / 3, 0 for 0 lines,
// and 0.875 of the viewport (at least `stepAtThreeLines`) for WHEEL_PAGESCROLL.
float WheelNotchDistance(UINT lines, float stepAtThreeLines, float viewport);
// Change of offset for a wheel delta: a positive delta (wheel away from the user)
// scrolls towards the start, WHEEL_DELTA per notch.
float WheelScrollDelta(short wheelDelta, float notchDistance);

// Offset, clamped, that brings the span [spanStart, spanStart + spanLength) of the
// content into view while moving as little as possible; a span longer than the
// viewport shows its start.
float OffsetToReveal(const ScrollExtent &extent, float offset, float spanStart, float spanLength);

// Smooth scrolling from `from` to `target`, easing out (cubic) over `durationMs`.
struct ScrollGlide
{
    float from = 0.0f;
    float target = 0.0f;
    double startMs = 0.0;
    double durationMs = 0.0;

    // A glide that starts at `nowMs` from `from`, where the content moves at `velocity`
    // DIPs per ms. When it already moves towards `target` faster than a glide of
    // `durationMs` would start, the glide is shortened so the speed carries on.
    static ScrollGlide Start(float from, float velocity, float target, double nowMs, double durationMs);
    float OffsetAt(double nowMs) const;
    // DIPs per ms.
    float VelocityAt(double nowMs) const;
    bool IsDoneAt(double nowMs) const;
};
} // namespace msimeui
