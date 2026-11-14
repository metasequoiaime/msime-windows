#include "tests/includes/test_framework.h"
#include "window/candidate_menu_placement.h"

namespace
{
// The candidate item menu as the presenter lays it out: 2 DIP padding, 24 DIP
// rows, and "fix position" in the second row.
FanyImeUi::CandidateMenuMetrics Metrics(float menuHeightDip = 76.0f, float submenuHeightDip = 155.0f)
{
    FanyImeUi::CandidateMenuMetrics metrics{};
    metrics.menuWidthDip = 108.0f;
    metrics.menuHeightDip = menuHeightDip;
    metrics.submenuWidthDip = 96.0f;
    metrics.submenuHeightDip = submenuHeightDip;
    metrics.submenuItemLeftDip = 2.0f;
    metrics.submenuItemTopDip = 26.0f;
    metrics.submenuItemWidthDip = 104.0f;
    metrics.submenuItemHeightDip = 24.0f;
    return metrics;
}

struct Rect
{
    int left;
    int top;
    int right;
    int bottom;
};

// Where msimeui::Popup puts the menu and the submenu for this plan, in host
// client pixels, replaying the presenter's anchors and offsets.
struct Laid
{
    Rect menu;
    Rect submenu;
};

Laid LayOut(const FanyImeUi::CandidateMenuPlacement &plan, const FanyImeUi::CandidateMenuMetrics &m, float scale)
{
    const auto px = [scale](float dip) { return static_cast<int>(std::lround(dip * scale)); };
    Laid laid{};
    laid.menu = {plan.menuClient.x, plan.menuClient.y, plan.menuClient.x + px(m.menuWidthDip),
                 plan.menuClient.y + px(m.menuHeightDip)};
    const int itemLeft = plan.menuClient.x + px(m.submenuItemLeftDip);
    const int itemTop = plan.menuClient.y + px(m.submenuItemTopDip);
    const int subLeft = plan.submenuLeft
                            ? itemLeft + px(FanyImeUi::kCandidateSubmenuOverlapDip) - px(m.submenuWidthDip)
                            : itemLeft + px(m.submenuItemWidthDip) - px(FanyImeUi::kCandidateSubmenuOverlapDip);
    const int subTop = plan.submenuAbove ? itemTop + px(m.submenuItemHeightDip) - px(m.submenuHeightDip) : itemTop;
    laid.submenu = {subLeft, subTop, subLeft + px(m.submenuWidthDip), subTop + px(m.submenuHeightDip)};
    return laid;
}

// The popup only leaves a rect where the plan put it when the rect keeps its
// 8 DIP margin inside the viewport; otherwise it would clamp or flip it.
bool KeepsPopupMargin(const Rect &r, const RECT &host, float scale)
{
    const int margin = static_cast<int>(std::floor(FanyImeUi::kCandidateMenuViewportMarginDip * scale));
    const int width = host.right - host.left;
    const int height = host.bottom - host.top;
    return r.left >= margin && r.top >= margin && r.right <= width - margin && r.bottom <= height - margin;
}
} // namespace

TEST_CASE(candidate_menu_opens_upward_when_the_click_is_near_the_bottom)
{
    for (const int dpi : {96, 120, 144, 192})
    {
        const auto px = [dpi](int dip) { return MulDiv(dip, dpi, 96); };
        const float scale = static_cast<float>(dpi) / 96.0f;
        const MonitorCoordinates work{0, 0, px(1920), px(1080)};
        const RECT host{px(800), px(900), px(1000), px(980)};
        const POINT click{px(850), px(960)};
        const auto metrics = Metrics(168.0f, 180.0f);

        const auto plan = FanyImeUi::PlanCandidateMenuPlacement(host, click, work, scale, metrics);

        REQUIRE(plan.menuAbove);
        // The menu ends at the click and keeps its horizontal position.
        REQUIRE_EQ(plan.host.left + plan.menuClient.x, click.x);
        REQUIRE_EQ(plan.host.top + plan.menuClient.y + px(168), click.y);
        REQUIRE(plan.host.top < host.top);
        REQUIRE(plan.host.top >= work.top);
        REQUIRE(plan.host.bottom >= host.bottom);
        REQUIRE(plan.host.bottom <= work.bottom);
        const Laid laid = LayOut(plan, metrics, scale);
        REQUIRE(KeepsPopupMargin(laid.menu, plan.host, scale));
        REQUIRE(KeepsPopupMargin(laid.submenu, plan.host, scale));
        REQUIRE(plan.contentOffsetYDip > 0.0f);
        REQUIRE_EQ(static_cast<int>(std::lround(plan.contentOffsetYDip * scale)), host.top - plan.host.top);
    }
}

TEST_CASE(candidate_submenu_flips_up_when_only_the_menu_fits_below)
{
    for (const int dpi : {96, 120, 144, 192})
    {
        const auto px = [dpi](int dip) { return MulDiv(dip, dpi, 96); };
        const float scale = static_cast<float>(dpi) / 96.0f;
        const MonitorCoordinates work{0, 0, px(1920), px(1032)};
        // Candidate window sitting on the bottom edge, click in its only row.
        const RECT host{px(240), px(899), px(816), px(1027)};
        const POINT click{px(280), px(941)};
        const auto metrics = Metrics(76.0f, 130.0f);

        const auto plan = FanyImeUi::PlanCandidateMenuPlacement(host, click, work, scale, metrics);

        // The menu still fits below the click, but the submenu does not, so the
        // host grows upward and the submenu opens above its item.
        REQUIRE(!plan.menuAbove);
        REQUIRE(plan.submenuAbove);
        REQUIRE(!plan.submenuLeft);
        REQUIRE(plan.host.top < host.top);
        REQUIRE(plan.host.top >= work.top);
        REQUIRE(plan.host.bottom <= work.bottom);
        const Laid laid = LayOut(plan, metrics, scale);
        REQUIRE(KeepsPopupMargin(laid.menu, plan.host, scale));
        REQUIRE(KeepsPopupMargin(laid.submenu, plan.host, scale));
        REQUIRE_EQ(static_cast<int>(std::lround(plan.contentOffsetYDip * scale)), host.top - plan.host.top);
    }
}

TEST_CASE(candidate_menu_pushed_left_by_the_right_edge_still_fits_its_left_submenu)
{
    // Review case: a card at x 1800-1900 on a 1920 work area, right-clicked at
    // x=1850. The menu cannot open at the click, so it moves left; the submenu
    // side has to be decided from that moved menu or it lands left of the host.
    for (const int dpi : {96, 120, 144, 192})
    {
        const auto px = [dpi](int dip) { return MulDiv(dip, dpi, 96); };
        const float scale = static_cast<float>(dpi) / 96.0f;
        const MonitorCoordinates work{0, 0, px(1920), px(1080)};
        const RECT host{px(1800), px(400), px(1900), px(480)};
        const POINT click{px(1850), px(430)};
        const auto metrics = Metrics();

        const auto plan = FanyImeUi::PlanCandidateMenuPlacement(host, click, work, scale, metrics);

        REQUIRE(plan.submenuLeft);
        REQUIRE(!plan.menuAbove);
        REQUIRE(!plan.submenuAbove);
        REQUIRE(plan.host.right <= work.right);
        REQUIRE(plan.host.left >= work.left);
        REQUIRE(plan.host.left < host.left);
        // The menu was pushed left so its right edge keeps the margin to the
        // work area; it never opens past the screen.
        REQUIRE(plan.host.left + plan.menuClient.x < click.x);
        REQUIRE(plan.host.left + plan.menuClient.x + px(108) <= work.right - px(8));
        const Laid laid = LayOut(plan, metrics, scale);
        REQUIRE(KeepsPopupMargin(laid.menu, plan.host, scale));
        REQUIRE(KeepsPopupMargin(laid.submenu, plan.host, scale));
        REQUIRE(plan.contentOffsetXDip > 0.0f);
        REQUIRE_EQ(static_cast<int>(std::lround(plan.contentOffsetXDip * scale)), host.left - plan.host.left);
    }
}

TEST_CASE(candidate_menu_that_fits_neither_side_stays_below_and_clamped)
{
    // Review case: a short work area where the menu fits neither below nor
    // above the click, with more room above. Planning it above would grow the
    // host up while the popup puts the menu back below.
    const MonitorCoordinates work{0, 0, 1920, 200};
    const RECT host{100, 120, 300, 190};
    const POINT click{150, 130};
    const auto metrics = Metrics(168.0f, 155.0f);

    const auto plan = FanyImeUi::PlanCandidateMenuPlacement(host, click, work, 1.0f, metrics);

    REQUIRE(!plan.menuAbove);
    REQUIRE(plan.host.top >= work.top);
    REQUIRE(plan.host.bottom <= work.bottom);
    // Clamped against the work area, so the menu keeps the popup margin and the
    // popup has nothing to move.
    const Laid laid = LayOut(plan, metrics, 1.0f);
    REQUIRE(KeepsPopupMargin(laid.menu, plan.host, 1.0f));
    // Pulled up just far enough to end at the bottom margin.
    REQUIRE_EQ(plan.host.top + plan.menuClient.y + 168, work.bottom - 8);
}

TEST_CASE(candidate_menu_keeps_the_card_still_when_everything_fits)
{
    const MonitorCoordinates work{0, 0, 1920, 1080};
    const RECT host{100, 100, 300, 180};
    const POINT click{150, 140};
    const auto metrics = Metrics(76.0f, 155.0f);

    const auto plan = FanyImeUi::PlanCandidateMenuPlacement(host, click, work, 1.0f, metrics);

    REQUIRE(!plan.menuAbove);
    REQUIRE(!plan.submenuLeft);
    REQUIRE(!plan.submenuAbove);
    REQUIRE_EQ(plan.host.left, host.left);
    REQUIRE_EQ(plan.host.top, host.top);
    // Grows right and down for the menu and submenu, which needs no content offset.
    REQUIRE(plan.host.bottom > host.bottom);
    REQUIRE_EQ(plan.contentOffsetXDip, 0.0f);
    REQUIRE_EQ(plan.contentOffsetYDip, 0.0f);
    REQUIRE_EQ(plan.menuClient.x, click.x - host.left);
    REQUIRE_EQ(plan.menuClient.y, click.y - host.top);
    const Laid laid = LayOut(plan, metrics, 1.0f);
    REQUIRE(KeepsPopupMargin(laid.menu, plan.host, 1.0f));
    REQUIRE(KeepsPopupMargin(laid.submenu, plan.host, 1.0f));
}

TEST_CASE(candidate_menu_reserves_room_from_the_measured_item_bounds)
{
    // A taller first row moves the submenu item down; the plan has to follow
    // the measured bounds instead of a fixed 26 DIP.
    const MonitorCoordinates work{0, 0, 1920, 1080};
    const RECT host{100, 800, 300, 880};
    const POINT click{150, 840};
    auto metrics = Metrics(100.0f, 155.0f);
    metrics.submenuItemTopDip = 50.0f;
    metrics.submenuItemHeightDip = 30.0f;

    const auto plan = FanyImeUi::PlanCandidateMenuPlacement(host, click, work, 1.0f, metrics);

    const Laid laid = LayOut(plan, metrics, 1.0f);
    REQUIRE(KeepsPopupMargin(laid.menu, plan.host, 1.0f));
    REQUIRE(KeepsPopupMargin(laid.submenu, plan.host, 1.0f));
    REQUIRE_EQ(laid.submenu.top, plan.menuClient.y + 50);
}

TEST_CASE(candidate_menu_plans_on_a_negative_coordinate_monitor)
{
    const MonitorCoordinates work{-1920, 0, 0, 1080};
    const RECT host{-500, 900, -400, 980};
    const POINT click{-450, 960};
    const auto metrics = Metrics(168.0f, 180.0f);

    const auto plan = FanyImeUi::PlanCandidateMenuPlacement(host, click, work, 1.0f, metrics);

    REQUIRE(plan.menuAbove);
    REQUIRE(plan.host.left >= work.left);
    REQUIRE(plan.host.top >= work.top);
    REQUIRE(plan.host.right <= work.right);
    REQUIRE(plan.host.bottom <= work.bottom);
    // The menu keeps its screen position whatever the host did.
    REQUIRE_EQ(plan.host.left + plan.menuClient.x, click.x);
    REQUIRE_EQ(plan.host.top + plan.menuClient.y + 168, click.y);
    const Laid laid = LayOut(plan, metrics, 1.0f);
    REQUIRE(KeepsPopupMargin(laid.menu, plan.host, 1.0f));
    REQUIRE(KeepsPopupMargin(laid.submenu, plan.host, 1.0f));
}
