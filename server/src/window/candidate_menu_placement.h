#pragma once

#include "defines/base_structures.h"

#include <algorithm>
#include <cmath>
#include <windows.h>

namespace FanyImeUi
{
// Physical-pixel placement plan for the candidate card's item menu. The host is
// the candidate-card window and the popup is clipped to its viewport, so the
// host grows around the card until the menu fits inside the monitor work area.
// Growing left or up moves the window origin; the content offsets tell the host
// how far to shift its scene so the card keeps its position on screen.
struct CandidateMenuPlacement
{
    RECT host{};
    POINT anchorClient{};
    float contentOffsetXDip = 0.0f;
    float contentOffsetYDip = 0.0f;
    bool menuAbove = false;
    bool submenuLeft = false;
    bool submenuAbove = false;
};

inline CandidateMenuPlacement PlanCandidateMenuPlacement(const RECT &host, POINT clickScreen,
                                                         const MonitorCoordinates &workArea, float scale,
                                                         float menuWidthDip, float menuHeightDip, float submenuWidthDip,
                                                         float submenuHeightDip)
{
    // 8 DIP is the popup's own viewport margin. 4 DIP is the menu inner padding
    // plus the 2 DIP overlap the submenu uses. The submenu hangs off the second
    // item (menu padding + one 24 DIP row), which is where "fix position" sits.
    constexpr float kViewportMarginDip = 8.0f;
    constexpr float kSubmenuOverlapDip = 4.0f;
    constexpr float kSecondItemTopDip = 2.0f + 24.0f;
    constexpr float kItemHeightDip = 24.0f;

    if (scale <= 0.0f)
        scale = 1.0f;
    const auto toPx = [scale](float dip) { return static_cast<int>(std::ceil(dip * scale)); };

    const int menuWidth = (std::max)(1, toPx(menuWidthDip));
    const int menuHeight = (std::max)(1, toPx(menuHeightDip));
    const int submenuWidth = (std::max)(1, toPx(submenuWidthDip));
    const int submenuHeight = (std::max)(1, toPx(submenuHeightDip));
    const int margin = (std::max)(1, toPx(kViewportMarginDip));

    const int leftLimit = workArea.left + margin;
    const int rightLimit = workArea.right - margin;
    const int topLimit = workArea.top + margin;
    const int bottomLimit = workArea.bottom - margin;

    CandidateMenuPlacement plan{};
    plan.host = host;

    // Menu: below the click when it fits, otherwise above when that fits,
    // otherwise on the roomier side (the popup then clamps in the viewport).
    const bool roomBelow = clickScreen.y + menuHeight <= bottomLimit;
    const bool roomAbove = clickScreen.y - menuHeight >= topLimit;
    plan.menuAbove = !roomBelow && (roomAbove || (clickScreen.y - workArea.top) > (workArea.bottom - clickScreen.y));
    const int menuTop = plan.menuAbove ? clickScreen.y - menuHeight : clickScreen.y;
    const int menuBottom = menuTop + menuHeight;

    // Submenu: it opens downward from its item by default. When that would run
    // past the work area it opens upward instead, ending level with the item.
    const int secondItemTop = menuTop + toPx(kSecondItemTopDip);
    const int secondItemBottom = secondItemTop + toPx(kItemHeightDip);
    plan.submenuAbove = secondItemTop + submenuHeight > bottomLimit && secondItemBottom - submenuHeight >= topLimit;
    const int submenuTop = plan.submenuAbove ? secondItemBottom - submenuHeight : secondItemTop;
    const int submenuBottom = submenuTop + submenuHeight;

    // Submenu opens to the right of its item; flip it left when the right edge
    // would leave the work area and the left side has room.
    const int submenuLeftEdge = clickScreen.x - submenuWidth + toPx(kSubmenuOverlapDip);
    plan.submenuLeft = clickScreen.x + menuWidth + submenuWidth > rightLimit && submenuLeftEdge >= leftLimit;
    const int menuRight = clickScreen.x + menuWidth;
    const int submenuRight = plan.submenuLeft ? clickScreen.x + toPx(kSubmenuOverlapDip) : menuRight + submenuWidth;
    const int neededLeft = plan.submenuLeft ? submenuLeftEdge - margin : clickScreen.x;
    const int neededRight = (std::max)(menuRight, submenuRight) + margin;
    const int neededTop = (std::min)(menuTop, submenuTop) - margin;
    const int neededBottom = (std::max)(menuBottom, submenuBottom) + margin;

    // Clamp only the growth: the card's own bounds stay where the presenter put
    // them, and a popup that cannot fit at all is left to the viewport clamp so
    // it stays clickable. RECT uses LONG while the work area uses int.
    plan.host.left = std::min<LONG>(host.left, std::max<LONG>(neededLeft, workArea.left));
    plan.host.top = std::min<LONG>(host.top, std::max<LONG>(neededTop, workArea.top));
    plan.host.right = std::max<LONG>(host.right, std::min<LONG>(neededRight, workArea.right));
    plan.host.bottom = std::max<LONG>(host.bottom, std::min<LONG>(neededBottom, workArea.bottom));

    plan.contentOffsetXDip = static_cast<float>(host.left - plan.host.left) / scale;
    plan.contentOffsetYDip = static_cast<float>(host.top - plan.host.top) / scale;
    plan.anchorClient = {clickScreen.x - plan.host.left, clickScreen.y - plan.host.top};
    return plan;
}
} // namespace FanyImeUi
