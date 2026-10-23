#include "tests/includes/test_framework.h"

#include "window/floating_toolbar_image_layout.h"

#include <string>
#include <vector>

namespace
{
using CandidateSkinCatalog::Package;
using CandidateSkinCatalog::ToolbarButton;
using CandidateSkinCatalog::ToolbarButtonFlow;

// A skin with a vertical column of 30 DIP round buttons from (170, 0), 2 DIP apart.
Package ColumnSkin()
{
    Package package;
    package.toolbarMode = "image";
    package.toolbarButtonDefaults.widthDip = 30.0;
    package.toolbarButtonDefaults.heightDip = 30.0;
    package.toolbarButtonDefaults.iconSizeDip = 20.0;
    package.toolbarButtonDefaults.background = "assets/plate.png";
    ToolbarButtonFlow flow;
    flow.xDip = 170.0;
    flow.vertical = true;
    flow.gapDip = 2.0;
    package.toolbarButtonFlow = flow;
    return package;
}

const std::vector<ToolbarImageButtonRequest> kAllButtons = {
    {"status", "cn"}, {"fullwidth", "half"}, {"punctuation", "cn"}, {"character_set", "simplified"},
    {"emoji", ""},    {"settings", ""},
};

std::vector<ToolbarImageButtonRequest> Without(const std::string &key)
{
    std::vector<ToolbarImageButtonRequest> shown;
    for (const auto &request : kAllButtons)
    {
        if (request.key != key)
            shown.push_back(request);
    }
    return shown;
}
} // namespace

TEST_CASE(toolbar_image_layout_flows_shown_buttons)
{
    const auto slots = LayoutToolbarImageButtons(ColumnSkin(), kAllButtons);
    REQUIRE_EQ(slots.size(), kAllButtons.size());
    for (size_t i = 0; i < slots.size(); ++i)
    {
        REQUIRE_EQ(slots[i].key, kAllButtons[i].key);
        REQUIRE_EQ(slots[i].xDip, 170.0);
        REQUIRE_EQ(slots[i].yDip, 32.0 * static_cast<double>(i));
        REQUIRE_EQ(slots[i].widthDip, 30.0);
        REQUIRE_EQ(slots[i].iconSizeDip, 20.0);
        REQUIRE_EQ(slots[i].background, std::string("assets/plate.png"));
    }
}

// Switching a button off in the settings closes its gap; its plate goes with it.
TEST_CASE(toolbar_image_layout_closes_the_gap_of_a_hidden_button)
{
    const auto slots = LayoutToolbarImageButtons(ColumnSkin(), Without("fullwidth"));
    REQUIRE_EQ(slots.size(), kAllButtons.size() - 1);
    REQUIRE_EQ(slots[0].key, std::string("status"));
    REQUIRE_EQ(slots[1].key, std::string("punctuation"));
    REQUIRE_EQ(slots[1].yDip, 32.0);
    REQUIRE_EQ(slots.back().key, std::string("settings"));
    REQUIRE_EQ(slots.back().yDip, 32.0 * 4.0);
}

// A button the skin gave no table at all still joins the flow when the user switches it on.
TEST_CASE(toolbar_image_layout_places_buttons_without_a_table)
{
    std::vector<ToolbarImageButtonRequest> shown = kAllButtons;
    shown.insert(shown.begin() + 5, {"screen_keyboard", ""});
    const auto slots = LayoutToolbarImageButtons(ColumnSkin(), shown);
    REQUIRE_EQ(slots.size(), shown.size());
    REQUIRE_EQ(slots[5].key, std::string("screen_keyboard"));
    REQUIRE_EQ(slots[5].yDip, 32.0 * 5.0);
    REQUIRE_EQ(slots[6].yDip, 32.0 * 6.0);
}

// Fixed coordinates stay put and take no place in the flow; a skin-hidden button takes none either.
TEST_CASE(toolbar_image_layout_keeps_fixed_and_hidden_buttons_out_of_the_flow)
{
    Package package = ColumnSkin();
    ToolbarButton fixed;
    fixed.xDip = 10.0;
    fixed.yDip = 5.0;
    fixed.widthDip = 40.0;
    package.toolbarButtons["status"] = fixed;
    ToolbarButton hidden;
    hidden.visible = false;
    package.toolbarButtons["emoji"] = hidden;
    const auto slots = LayoutToolbarImageButtons(package, kAllButtons);
    REQUIRE_EQ(slots.size(), kAllButtons.size() - 1);
    REQUIRE_EQ(slots[0].key, std::string("status"));
    REQUIRE_EQ(slots[0].xDip, 10.0);
    REQUIRE_EQ(slots[0].yDip, 5.0);
    REQUIRE_EQ(slots[0].widthDip, 40.0);
    REQUIRE_EQ(slots[0].heightDip, 30.0);
    REQUIRE_EQ(slots[1].key, std::string("fullwidth"));
    REQUIRE_EQ(slots[1].yDip, 0.0);
    REQUIRE_EQ(slots.back().key, std::string("settings"));
    REQUIRE_EQ(slots.back().yDip, 32.0 * 3.0);
}

// Without a flow only buttons with both coordinates show, as before button_flow existed.
TEST_CASE(toolbar_image_layout_without_flow_needs_coordinates)
{
    Package package;
    package.toolbarMode = "image";
    ToolbarButton placed;
    placed.xDip = 4.0;
    placed.yDip = 6.0;
    package.toolbarButtons["settings"] = placed;
    ToolbarButton half;
    half.xDip = 30.0;
    package.toolbarButtons["fullwidth"] = half;
    const auto slots = LayoutToolbarImageButtons(package, kAllButtons);
    REQUIRE_EQ(slots.size(), size_t{1});
    REQUIRE_EQ(slots[0].key, std::string("settings"));
    REQUIRE_EQ(slots[0].widthDip, CandidateSkinCatalog::kDefaultToolbarButtonSizeDip);
    REQUIRE_EQ(slots[0].iconSizeDip, 0.0);
    REQUIRE(slots[0].background.empty());
}

// A button's own keys win over button_defaults; its image follows the state, then falls back to image.
TEST_CASE(toolbar_image_layout_prefers_the_buttons_own_keys)
{
    Package package = ColumnSkin();
    ToolbarButton status;
    status.background = "assets/status-plate.png";
    status.iconSizeDip = 24.0;
    status.image = "assets/any.png";
    status.images["en"] = "assets/en.png";
    package.toolbarButtons["status"] = status;
    const auto chinese = LayoutToolbarImageButtons(package, {{"status", "cn"}});
    REQUIRE_EQ(chinese[0].background, std::string("assets/status-plate.png"));
    REQUIRE_EQ(chinese[0].iconSizeDip, 24.0);
    REQUIRE_EQ(chinese[0].image, std::string("assets/any.png"));
    const auto english = LayoutToolbarImageButtons(package, {{"status", "en"}});
    REQUIRE_EQ(english[0].image, std::string("assets/en.png"));
}
