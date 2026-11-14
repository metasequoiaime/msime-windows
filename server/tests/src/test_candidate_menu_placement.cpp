#include "tests/includes/test_framework.h"
#include "window/candidate_menu_placement.h"

namespace
{
constexpr float kMenuWidthDip = 108.0f;
constexpr float kMenuHeightDip = 168.0f;
constexpr float kSubmenuWidthDip = 96.0f;
constexpr float kSubmenuHeightDip = 180.0f;
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

        const auto plan = FanyImeUi::PlanCandidateMenuPlacement(host, click, work, scale, kMenuWidthDip, kMenuHeightDip,
                                                                kSubmenuWidthDip, kSubmenuHeightDip);

        REQUIRE(plan.menuAbove);
        REQUIRE_EQ(plan.anchorClient.x, click.x - plan.host.left);
        REQUIRE_EQ(plan.anchorClient.y, click.y - plan.host.top);
        // The top grows by the menu and the viewport margin; the bottom only
        // takes the submenu and never leaves the work area.
        REQUIRE(plan.host.top < host.top);
        REQUIRE(plan.host.top >= work.top);
        REQUIRE(plan.host.bottom >= host.bottom);
        REQUIRE(plan.host.bottom <= work.bottom);
        // The flipped menu keeps the popup margin at the top of the viewport,
        // so the popup itself does not have to flip back down.
        REQUIRE(plan.anchorClient.y - px(168) >= px(8));
        // The submenu hangs below its item and still fits inside the viewport.
        const int submenuBottom = plan.anchorClient.y - px(168) + px(26) + px(180);
        REQUIRE(submenuBottom <= plan.host.bottom - plan.host.top);
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

        const auto plan = FanyImeUi::PlanCandidateMenuPlacement(host, click, work, scale, kMenuWidthDip, 76.0f,
                                                                kSubmenuWidthDip, 130.0f);

        // The menu still fits below the click, but the submenu does not, so the
        // host grows upward and the submenu opens above its item.
        REQUIRE(!plan.menuAbove);
        REQUIRE(plan.submenuAbove);
        REQUIRE(!plan.submenuLeft);
        REQUIRE(plan.host.top < host.top);
        REQUIRE(plan.host.top >= work.top);
        REQUIRE(plan.host.bottom <= work.bottom);
        // The submenu starts at the viewport margin and ends level with the item.
        const int submenuClientTop = plan.anchorClient.y + px(26) + px(24) - px(130);
        REQUIRE(submenuClientTop >= px(8));
        REQUIRE(submenuClientTop + px(130) <= plan.host.bottom - plan.host.top);
        REQUIRE_EQ(static_cast<int>(std::lround(plan.contentOffsetYDip * scale)), host.top - plan.host.top);
    }
}

TEST_CASE(candidate_menu_flips_the_submenu_left_near_the_right_edge)
{
    for (const int dpi : {96, 120, 144, 192})
    {
        const auto px = [dpi](int dip) { return MulDiv(dip, dpi, 96); };
        const float scale = static_cast<float>(dpi) / 96.0f;
        const MonitorCoordinates work{0, 0, px(1920), px(1080)};
        const RECT host{px(1800), px(400), px(1900), px(480)};
        const POINT click{px(1850), px(430)};

        const auto plan = FanyImeUi::PlanCandidateMenuPlacement(host, click, work, scale, kMenuWidthDip, kMenuHeightDip,
                                                                kSubmenuWidthDip, kSubmenuHeightDip);

        REQUIRE(plan.submenuLeft);
        REQUIRE(!plan.menuAbove);
        REQUIRE(!plan.submenuAbove);
        REQUIRE(plan.host.right <= work.right);
        REQUIRE(plan.host.left < host.left);
        REQUIRE(plan.contentOffsetXDip > 0.0f);
        REQUIRE_EQ(static_cast<int>(std::lround(plan.contentOffsetXDip * scale)), host.left - plan.host.left);
        // The submenu's left edge keeps the viewport margin inside the host.
        REQUIRE(plan.anchorClient.x - px(96) + px(4) >= px(8));
        REQUIRE(plan.anchorClient.x - px(96) + px(4) <= plan.anchorClient.x);
    }
}

TEST_CASE(candidate_menu_keeps_the_card_still_when_everything_fits)
{
    const MonitorCoordinates work{0, 0, 1920, 1080};
    const RECT host{100, 100, 300, 180};
    const POINT click{150, 140};

    const auto plan = FanyImeUi::PlanCandidateMenuPlacement(host, click, work, 1.0f, kMenuWidthDip, kMenuHeightDip,
                                                            kSubmenuWidthDip, kSubmenuHeightDip);

    REQUIRE(!plan.menuAbove);
    REQUIRE(!plan.submenuLeft);
    REQUIRE(!plan.submenuAbove);
    REQUIRE_EQ(plan.host.left, host.left);
    REQUIRE_EQ(plan.host.top, host.top);
    // Grows right and down for the menu and submenu, which needs no content offset.
    REQUIRE(plan.host.right >= click.x + 108 + 96);
    REQUIRE(plan.host.bottom > host.bottom);
    REQUIRE_EQ(plan.contentOffsetXDip, 0.0f);
    REQUIRE_EQ(plan.contentOffsetYDip, 0.0f);
    REQUIRE_EQ(plan.anchorClient.x, click.x - host.left);
    REQUIRE_EQ(plan.anchorClient.y, click.y - host.top);
}

TEST_CASE(candidate_menu_plans_on_a_negative_coordinate_monitor)
{
    const MonitorCoordinates work{-1920, 0, 0, 1080};
    const RECT host{-500, 900, -400, 980};
    const POINT click{-450, 960};

    const auto plan = FanyImeUi::PlanCandidateMenuPlacement(host, click, work, 1.0f, kMenuWidthDip, kMenuHeightDip,
                                                            kSubmenuWidthDip, kSubmenuHeightDip);

    REQUIRE(plan.menuAbove);
    REQUIRE(plan.host.left >= work.left);
    REQUIRE(plan.host.top >= work.top);
    REQUIRE(plan.host.right <= work.right);
    REQUIRE(plan.host.bottom <= work.bottom);
    // The click keeps its screen position whatever the host did.
    REQUIRE_EQ(plan.host.left + plan.anchorClient.x, click.x);
    REQUIRE_EQ(plan.host.top + plan.anchorClient.y, click.y);
}
