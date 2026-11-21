#pragma once

#include "defines/base_structures.h"

#include <algorithm>
#include <cmath>
#include <windows.h>

namespace FanyImeUi
{
// The popup's own viewport margin (msimeui::Popup::LayoutOverlay keeps 8 DIP to
// every viewport edge) and how far the submenu overlaps its item. The presenter
// opens the submenu with the same overlap, so the plan and the popup agree.
inline constexpr float kCandidateMenuViewportMarginDip = 8.0f;
inline constexpr float kCandidateSubmenuOverlapDip = 2.0f;

// Measured sizes, all in DIP. The item rect is the "fix position" row the
// submenu hangs off, relative to the menu's top-left corner, as laid out by the
// menu itself rather than assumed from a row height.
struct CandidateMenuMetrics
{
    float menuWidthDip = 0.0f;
    float menuHeightDip = 0.0f;
    float submenuWidthDip = 0.0f;
    float submenuHeightDip = 0.0f;
    float submenuItemLeftDip = 0.0f;
    float submenuItemTopDip = 0.0f;
    float submenuItemWidthDip = 0.0f;
    float submenuItemHeightDip = 0.0f;
};

// Physical-pixel placement plan for the candidate card's item menu. The host is
// the candidate-card window and the popup is clipped to its viewport, so the
// host grows around the card until the menu fits inside the monitor work area.
// Growing left or up moves the window origin; the content offsets tell the host
// how far to shift its scene so the card keeps its position on screen.
//
// The plan decides the final menu and submenu rects itself, clamped to the work
// area, and sizes the host so both keep the popup margin inside it. The popup
// then lays them out exactly there and never has to clamp or flip on its own,
// which is what used to make the plan and the screen disagree.
struct CandidateMenuPlacement
{
    RECT host{};
    // Top-left corner of the menu in host client pixels. The menu opens
    // downward from here.
    POINT menuClient{};
    float contentOffsetXDip = 0.0f;
    float contentOffsetYDip = 0.0f;
    // Whether the menu sits above the click (informational: the menu is always
    // placed by its top-left corner).
    bool menuAbove = false;
    bool submenuLeft = false;
    bool submenuAbove = false;
};

inline CandidateMenuPlacement PlanCandidateMenuPlacement(const RECT &host, POINT clickScreen,
                                                         const MonitorCoordinates &workArea, float scale,
                                                         const CandidateMenuMetrics &metrics)
{
    if (scale <= 0.0f)
        scale = 1.0f;
    const auto toPx = [scale](float dip) { return static_cast<int>(std::ceil(dip * scale)); };

    const int menuWidth = (std::max)(1, toPx(metrics.menuWidthDip));
    const int menuHeight = (std::max)(1, toPx(metrics.menuHeightDip));
    const int submenuWidth = (std::max)(1, toPx(metrics.submenuWidthDip));
    const int submenuHeight = (std::max)(1, toPx(metrics.submenuHeightDip));
    const int margin = (std::max)(1, toPx(kCandidateMenuViewportMarginDip));
    const int overlap = toPx(kCandidateSubmenuOverlapDip);

    const int leftLimit = workArea.left + margin;
    const int rightLimit = workArea.right - margin;
    const int topLimit = workArea.top + margin;
    const int bottomLimit = workArea.bottom - margin;

    CandidateMenuPlacement plan{};

    // Menu horizontally: at the click, pushed left when it would leave the work
    // area. Doing the push here (not in the popup) lets the submenu side below
    // be decided from where the menu really ends up.
    int menuLeft = clickScreen.x;
    if (menuLeft + menuWidth > rightLimit)
        menuLeft = (std::max)(rightLimit - menuWidth, leftLimit);

    // Menu vertically: below the click when it fits, above when that fits.
    // When neither does it stays below, clamped into the work area; picking the
    // "roomier" side there would grow the host up for a menu the popup would
    // then put back down.
    int menuTop = clickScreen.y;
    if (clickScreen.y + menuHeight > bottomLimit)
    {
        if (clickScreen.y - menuHeight >= topLimit)
        {
            menuTop = clickScreen.y - menuHeight;
            plan.menuAbove = true;
        }
        else
        {
            menuTop = (std::max)(topLimit, bottomLimit - menuHeight);
        }
    }

    // The submenu hangs off its item: to the right of it, overlapping by the
    // overlap, and level with its top. It flips left / up only when the default
    // side leaves the work area and the other side fits.
    const int itemLeft = menuLeft + static_cast<int>(std::lround(metrics.submenuItemLeftDip * scale));
    const int itemTop = menuTop + static_cast<int>(std::lround(metrics.submenuItemTopDip * scale));
    const int itemRight = itemLeft + static_cast<int>(std::lround(metrics.submenuItemWidthDip * scale));
    const int itemBottom = itemTop + static_cast<int>(std::lround(metrics.submenuItemHeightDip * scale));

    int submenuLeft = itemRight - overlap;
    const int flippedSubmenuLeft = itemLeft + overlap - submenuWidth;
    if (submenuLeft + submenuWidth > rightLimit && flippedSubmenuLeft >= leftLimit)
    {
        submenuLeft = flippedSubmenuLeft;
        plan.submenuLeft = true;
    }
    int submenuTop = itemTop;
    const int flippedSubmenuTop = itemBottom - submenuHeight;
    if (submenuTop + submenuHeight > bottomLimit && flippedSubmenuTop >= topLimit)
    {
        submenuTop = flippedSubmenuTop;
        plan.submenuAbove = true;
    }

    const int neededLeft = (std::min)(menuLeft, submenuLeft) - margin;
    const int neededTop = (std::min)(menuTop, submenuTop) - margin;
    const int neededRight = (std::max)(menuLeft + menuWidth, submenuLeft + submenuWidth) + margin;
    const int neededBottom = (std::max)(menuTop + menuHeight, submenuTop + submenuHeight) + margin;

    // Clamp only the growth: the card's own bounds stay where the presenter put
    // them, and a popup that cannot fit at all is left to the viewport clamp so
    // it stays clickable. RECT uses LONG while the work area uses int.
    plan.host = host;
    plan.host.left = std::min<LONG>(host.left, std::max<LONG>(neededLeft, workArea.left));
    plan.host.top = std::min<LONG>(host.top, std::max<LONG>(neededTop, workArea.top));
    plan.host.right = std::max<LONG>(host.right, std::min<LONG>(neededRight, workArea.right));
    plan.host.bottom = std::max<LONG>(host.bottom, std::min<LONG>(neededBottom, workArea.bottom));

    plan.contentOffsetXDip = static_cast<float>(host.left - plan.host.left) / scale;
    plan.contentOffsetYDip = static_cast<float>(host.top - plan.host.top) / scale;
    plan.menuClient = {menuLeft - plan.host.left, menuTop - plan.host.top};
    return plan;
}
} // namespace FanyImeUi
