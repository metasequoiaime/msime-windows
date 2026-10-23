#include "msimeui/ScrollAxis.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace msimeui
{
namespace
{
// Below this much overflow the content counts as fitting: layout rounding leaves
// fractions of a DIP that must not bring up a scrollbar.
constexpr float kOverflowThreshold = 0.5f;
// Control Panel's default for "lines to scroll at a time".
constexpr UINT kDefaultWheelLines = 3;
// One screen per notch scrolls this share of the viewport, keeping a little of the
// previous screen in view.
constexpr float kPageScrollShare = 0.875f;

float Length(float value)
{
    return std::max(value, 0.0f);
}

// The part of the track the thumb can occupy, between the insets.
float ThumbSpace(const ScrollTrack &track)
{
    return Length(Length(track.length) - track.thumbInset * 2.0f);
}

float ThumbLength(const ScrollExtent &extent, const ScrollTrack &track)
{
    const float space = ThumbSpace(track);
    const float viewport = Length(extent.viewport);
    const float visibleShare =
        viewport > 0.0f ? std::clamp(viewport / std::max(Length(extent.content), 1.0f), 0.0f, 1.0f) : 1.0f;
    return std::min(std::max(space * visibleShare, track.thumbMinLength), space);
}
} // namespace

float MaxScrollOffset(const ScrollExtent &extent)
{
    return Length(Length(extent.content) - Length(extent.viewport));
}

float ClampScrollOffset(float offset, const ScrollExtent &extent)
{
    return std::clamp(offset, 0.0f, MaxScrollOffset(extent));
}

bool CanScroll(const ScrollExtent &extent)
{
    return Length(extent.content) > Length(extent.viewport) + kOverflowThreshold;
}

ThumbSpan ThumbAt(const ScrollExtent &extent, const ScrollTrack &track, float offset)
{
    const float length = ThumbLength(extent, track);
    const float travel = Length(ThumbSpace(track) - length);
    const float maxOffset = MaxScrollOffset(extent);
    const float progress = maxOffset > 0.0f ? ClampScrollOffset(offset, extent) / maxOffset : 0.0f;
    return {track.start + track.thumbInset + travel * progress, length};
}

float OffsetForThumbStart(const ScrollExtent &extent, const ScrollTrack &track, float thumbStart)
{
    // A thumb that fills its space still answers a drag; dividing by 1 DIP instead of 0
    // keeps the result finite.
    const float travel = std::max(ThumbSpace(track) - ThumbLength(extent, track), 1.0f);
    const float first = track.start + track.thumbInset;
    const float start = std::clamp(thumbStart, first, first + travel);
    return (start - first) / travel * MaxScrollOffset(extent);
}

ThumbGrab PressScrollbar(const ScrollExtent &extent, const ScrollTrack &track, float offset, float pointer)
{
    float current = ClampScrollOffset(offset, extent);
    ThumbSpan thumb = ThumbAt(extent, track, current);
    if (pointer < thumb.start || pointer > thumb.start + thumb.length)
    {
        current = OffsetForThumbStart(extent, track, pointer - thumb.length * 0.5f);
        thumb = ThumbAt(extent, track, current);
    }
    return {current, pointer - thumb.start};
}

float DragThumb(const ScrollExtent &extent, const ScrollTrack &track, float grip, float pointer)
{
    return OffsetForThumbStart(extent, track, pointer - grip);
}

namespace
{
WheelScrollLinesSource g_wheelScrollLinesSource = nullptr;
} // namespace

WheelScrollLinesSource SetWheelScrollLinesSource(WheelScrollLinesSource source)
{
    return std::exchange(g_wheelScrollLinesSource, source);
}

UINT SystemWheelScrollLines()
{
    if (g_wheelScrollLinesSource)
    {
        return g_wheelScrollLinesSource();
    }
    UINT lines = kDefaultWheelLines;
    if (!SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0))
    {
        return kDefaultWheelLines;
    }
    return lines;
}

float WheelNotchDistance(UINT lines, float stepAtThreeLines, float viewport)
{
    if (lines == WHEEL_PAGESCROLL)
    {
        return std::max(Length(viewport) * kPageScrollShare, stepAtThreeLines);
    }
    return stepAtThreeLines * static_cast<float>(lines) / static_cast<float>(kDefaultWheelLines);
}

float WheelScrollDelta(short wheelDelta, float notchDistance)
{
    return -static_cast<float>(wheelDelta) / static_cast<float>(WHEEL_DELTA) * notchDistance;
}

float OffsetToReveal(const ScrollExtent &extent, float offset, float spanStart, float spanLength)
{
    const float spanEnd = spanStart + Length(spanLength);
    const float viewport = Length(extent.viewport);
    float revealed = offset;
    if (spanStart < offset)
    {
        revealed = spanStart;
    }
    else if (spanEnd > offset + viewport)
    {
        revealed = std::min(spanStart, spanEnd - viewport);
    }
    return ClampScrollOffset(revealed, extent);
}

ScrollGlide ScrollGlide::Start(float from, float velocity, float target, double nowMs, double durationMs)
{
    // An ease-out cubic starts at 3 * distance / duration. When the content already moves
    // faster than that in the same direction, the duration shrinks until the glide starts
    // at the current speed.
    const double distance = static_cast<double>(target) - static_cast<double>(from);
    const double speed = std::fabs(static_cast<double>(velocity));
    double duration = durationMs;
    if (static_cast<double>(velocity) * distance > 0.0 && speed * duration > 3.0 * std::fabs(distance))
    {
        duration = std::max(3.0 * std::fabs(distance) / speed, 1.0);
    }
    return {from, target, nowMs, duration};
}

float ScrollGlide::OffsetAt(double nowMs) const
{
    const double progress = std::clamp((nowMs - startMs) / std::max(durationMs, 1.0), 0.0, 1.0);
    const double rest = 1.0 - progress;
    return from + (target - from) * static_cast<float>(1.0 - rest * rest * rest);
}

float ScrollGlide::VelocityAt(double nowMs) const
{
    const double duration = std::max(durationMs, 1.0);
    const double progress = std::clamp((nowMs - startMs) / duration, 0.0, 1.0);
    const double rest = 1.0 - progress;
    return static_cast<float>(3.0 * (target - from) * rest * rest / duration);
}

bool ScrollGlide::IsDoneAt(double nowMs) const
{
    return nowMs >= startMs + std::max(durationMs, 1.0);
}
} // namespace msimeui
