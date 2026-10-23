#pragma once

#include <algorithm>
#include <cstddef>

// The `.compact-scroll-menu` geometry the settings-window list tests work their
// expectations out from, independently of the code under test: rows are 28 DIPs, 2 apart,
// inside a 2 DIP inset. The scrollbar thumb sits 4 DIPs from each end of the list and is
// never shorter than 24; it is as long as the visible share of the space between those
// ends, and its travel maps linearly onto the scroll range.
namespace compact_menu
{
constexpr float kRow = 28.0f;
constexpr float kRowPitch = 30.0f;
constexpr float kInset = 2.0f;
constexpr float kThumbEndGap = 4.0f;
constexpr float kThumbMinLength = 24.0f;
constexpr size_t kNone = static_cast<size_t>(-1);

struct Geometry
{
    size_t rows = 0;
    float viewport = 0.0f;

    float Content() const
    {
        return kInset * 2.0f + static_cast<float>(rows) * kRow + static_cast<float>(rows - 1) * (kRowPitch - kRow);
    }

    float MaxScroll() const
    {
        return std::max(Content() - viewport, 0.0f);
    }

    float ThumbLength() const
    {
        const float space = viewport - kThumbEndGap * 2.0f;
        return std::min(std::max(space * viewport / Content(), kThumbMinLength), space);
    }

    float Travel() const
    {
        return viewport - kThumbEndGap * 2.0f - ThumbLength();
    }

    // Scroll for the thumb's top at list-local y.
    float ScrollForThumbTop(float top) const
    {
        return std::clamp(top - kThumbEndGap, 0.0f, Travel()) / Travel() * MaxScroll();
    }
};

// The row whose box holds content y (0 at the list's top edge), or kNone in a gap or
// above the first row.
inline size_t RowAtContentY(float y)
{
    const float fromFirst = y - kInset;
    if (fromFirst < 0.0f)
        return kNone;
    const size_t row = static_cast<size_t>(fromFirst / kRowPitch);
    return fromFirst - static_cast<float>(row) * kRowPitch < kRow ? row : kNone;
}
} // namespace compact_menu
