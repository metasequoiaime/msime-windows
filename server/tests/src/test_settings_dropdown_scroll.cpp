#include "tests/includes/compact_menu_geometry.h"
#include "tests/includes/settings_window_harness.h"
#include "tests/includes/test_framework.h"
#include "tests/includes/wheel_lines_override.h"

#include "settings/native/native_settings_controls.h"

#include "msimeui/Controls.h"
#include "msimeui/Layout.h"

#include <cmath>
#include <string>
#include <vector>

using native_settings::Dropdown;
using native_settings::DropdownOption;

// Expectations are worked out from the `.compact-scroll-menu` geometry, not from the code:
// rows are 28 DIPs, 2 apart, inside a 2 DIP inset, and six of them show (180 DIPs inside
// the 1 DIP border). The scrollbar thumb is 4 DIPs wide, 2 from the right edge, 4 from
// each end of the menu, and never shorter than 24.
//
// With 20 rows the content is 4 + 20 * 28 + 19 * 2 = 602 DIPs, so the list scrolls up to
// 422. The thumb is 172 * 180 / 602 long and travels 172 * 422 / 602, so every DIP the
// thumb moves scrolls 602 / 172 = 3.5 DIPs of content.
namespace
{
constexpr float kRow = 28.0f;
constexpr float kRowPitch = 30.0f;
constexpr float kInset = 2.0f;
constexpr float kVisible = 180.0f;
constexpr float kThumbEndGap = 4.0f;
constexpr float kContentPerThumbDip = 3.5f;

std::vector<DropdownOption> NumberedOptions(size_t count)
{
    std::vector<DropdownOption> options;
    for (size_t i = 0; i < count; ++i)
        options.push_back({L"Option " + std::to_wstring(i), "v" + std::to_string(i)});
    return options;
}

class DropdownScene
{
  public:
    // `beneath`, when given, sits in the page at `beneathAt`, under where the menu opens.
    DropdownScene(float dpi, size_t count, bool searchable = false, bool compact = true,
                  msimeui::SizeF viewport = {800.0f, 900.0f}, float top = 40.0f,
                  std::shared_ptr<msimeui::Visual> beneath = nullptr, msimeui::PointF beneathAt = {})
        : harness(dpi, viewport)
    {
        Dropdown::Options options;
        options.compact = compact;
        options.searchable = searchable;
        dropdown = std::make_shared<Dropdown>(NumberedOptions(count), options);
        dropdown->SetOnChanged([this](size_t index, const DropdownOption &) { committed.push_back(index); });
        auto root = std::make_shared<msimeui::OverlayPanel>();
        if (beneath)
            root->AddChildAt(beneath, beneathAt.x, beneathAt.y);
        root->AddChildAt(dropdown, 300.0f, top);
        harness.SetRoot(root);
    }

    // Opens the menu and checks it shows the compact six rows.
    void Open()
    {
        OpenAnyHeight();
        REQUIRE(std::fabs(bounds.height - kVisible) < 0.01f);
    }

    void OpenAnyHeight()
    {
        const msimeui::RectF toggle = dropdown->GetBounds();
        harness.Click(toggle.x + 20.0f, toggle.y + toggle.height * 0.5f);
        FindMenu();
    }

    // A searchable dropdown opens when its search box takes the focus.
    void OpenSearch()
    {
        const msimeui::RectF toggle = dropdown->GetBounds();
        harness.Click(toggle.x + 20.0f, toggle.y + toggle.height * 0.5f);
        harness.GiveWindowKeyboardFocus();
        FindMenu();
    }

    void FindMenu()
    {
        const msimeui::RectF toggle = dropdown->GetBounds();
        menu = harness.VisualAt(toggle.x + toggle.width * 0.5f, toggle.y + toggle.height + 20.0f);
        REQUIRE(menu != nullptr);
        REQUIRE(menu != dropdown.get());
        bounds = menu->GetBounds();
    }

    bool MenuIsOpen()
    {
        return harness.VisualAt(bounds.x + bounds.width * 0.5f, bounds.y + 10.0f) == menu;
    }

    // Menu-local y (0 at the menu's top edge) to window y.
    float Y(float localY) const
    {
        return bounds.y + localY;
    }

    float ThumbX() const
    {
        return bounds.x + bounds.width - 4.0f;
    }

    float RowX() const
    {
        return bounds.x + bounds.width * 0.5f;
    }

    // Window y of the middle of `row` when the list is scrolled by `scroll`.
    float RowCentre(size_t row, float scroll) const
    {
        return bounds.y + kInset + static_cast<float>(row) * kRowPitch + kRow * 0.5f - scroll;
    }

    SettingsWindowHarness harness;
    std::shared_ptr<Dropdown> dropdown;
    std::vector<size_t> committed;
    msimeui::Visual *menu = nullptr;
    msimeui::RectF bounds = {};
};

void CheckThumbDrag(float dpi)
{
    DropdownScene scene(dpi, 20);
    scene.Open();
    // The thumb starts 4 below the top; grab it 16 into it and pull it down 60.
    scene.harness.Press(scene.ThumbX(), scene.Y(kThumbEndGap + 16.0f));
    scene.harness.MoveHeld(scene.ThumbX(), scene.Y(kThumbEndGap + 76.0f));
    scene.harness.Release(scene.ThumbX(), scene.Y(kThumbEndGap + 76.0f));
    REQUIRE(scene.committed.empty());
    REQUIRE(scene.MenuIsOpen());

    const float scroll = 60.0f * kContentPerThumbDip;
    // Row 10 is where row 3 would be without the scroll, so the pick tells them apart.
    scene.harness.Click(scene.RowX(), scene.RowCentre(10, scroll));
    REQUIRE(scene.committed == std::vector<size_t>{10});
}
} // namespace

TEST_CASE(settings_dropdown_dragging_the_thumb_scrolls_the_list)
{
    CheckThumbDrag(96.0f);
}

TEST_CASE(settings_dropdown_track_click_jumps_so_the_thumb_centres_on_the_pointer)
{
    DropdownScene scene(96.0f, 20);
    scene.Open();
    // The thumb (51.4 long) centred on local 150 would start at 124.3, inside its travel
    // (4 .. 124.6).
    scene.harness.Click(scene.ThumbX(), scene.Y(150.0f));
    REQUIRE(scene.committed.empty());
    const float thumbLength = 172.0f * kVisible / 602.0f;
    const float scroll = (150.0f - thumbLength * 0.5f - kThumbEndGap) * kContentPerThumbDip;
    scene.harness.Click(scene.RowX(), scene.RowCentre(19, scroll));
    REQUIRE(scene.committed == std::vector<size_t>{19});
}

TEST_CASE(settings_dropdown_press_on_the_scrollbar_selects_no_row)
{
    DropdownScene scene(96.0f, 20);
    scene.Open();
    // On the thumb, over row 0.
    scene.harness.Click(scene.ThumbX(), scene.RowCentre(0, 0.0f));
    REQUIRE(scene.committed.empty());
    REQUIRE(scene.MenuIsOpen());
    // On the track at the bottom, which jumps the list to its end; the row under the
    // pointer before and after the jump is not picked either.
    scene.harness.Click(scene.ThumbX(), scene.RowCentre(5, 0.0f));
    REQUIRE(scene.committed.empty());
    REQUIRE(scene.MenuIsOpen());
}

TEST_CASE(settings_dropdown_release_on_a_row_after_dragging_the_thumb_selects_nothing)
{
    DropdownScene scene(96.0f, 20);
    scene.Open();
    scene.harness.Press(scene.ThumbX(), scene.Y(kThumbEndGap + 10.0f));
    scene.harness.MoveHeld(scene.RowX(), scene.Y(kThumbEndGap + 40.0f));
    scene.harness.Release(scene.RowX(), scene.Y(kThumbEndGap + 40.0f));
    REQUIRE(scene.committed.empty());
    REQUIRE(scene.MenuIsOpen());
}

namespace
{
// One notch down at `lines` per notch, then a click 16 DIPs below the menu's top, which
// picks the row the scroll brought there.
size_t RowAfterOneNotch(UINT lines)
{
    WheelLinesOverride setting(lines);
    DropdownScene scene(96.0f, 20);
    scene.Open();
    scene.harness.Wheel(scene.RowX(), scene.Y(50.0f), -WHEEL_DELTA);
    scene.harness.Click(scene.RowX(), scene.Y(16.0f));
    REQUIRE(scene.committed.size() == 1);
    return scene.committed.front();
}
} // namespace

// 3 lines per notch scrolls two rows (60 DIPs), and other settings scale it.
TEST_CASE(settings_dropdown_wheel_follows_the_lines_to_scroll_setting)
{
    // 60: local 16 is content 76, row 2.
    REQUIRE(RowAfterOneNotch(3) == 2);
    // 20: content 36, row 1.
    REQUIRE(RowAfterOneNotch(1) == 1);
    // 0 lines: no scroll, row 0.
    REQUIRE(RowAfterOneNotch(0) == 0);
}

TEST_CASE(settings_dropdown_keyboard_reveals_the_focused_row_in_a_menu_cut_short_by_the_window)
{
    // A 120 DIP window leaves the menu well under the 180 DIPs it asks for.
    DropdownScene scene(96.0f, 20, false, true, {800.0f, 120.0f});
    scene.OpenAnyHeight();
    REQUIRE(scene.bounds.height < 100.0f);
    const float visible = scene.bounds.height;
    for (int i = 0; i < 10; ++i)
        scene.harness.Key(VK_DOWN);
    // Row 9 and the 2 DIP inset below it end at 9 * 30 + 28 + 2 * 2 = 302, which lands on
    // the bottom of what the menu shows.
    const float scroll = 302.0f - visible;
    // 1 DIP below the top of row 9. Measured against the asked-for 180 the list scrolls
    // less and this is row 8; without the inset it is the gap above row 9.
    const float probe = scene.RowCentre(9, scroll) - kRow * 0.5f + 1.0f;
    REQUIRE(compact_menu::RowAtContentY(probe - scene.bounds.y + scroll) == 9);
    scene.harness.Click(scene.RowX(), probe);
    REQUIRE(scene.committed == std::vector<size_t>{9});
}

namespace
{
// "1" keeps Option 1 and Option 10..19: eleven rows, more than fit.
void OpenFilteredByOne(DropdownScene &scene)
{
    scene.OpenSearch();
    scene.harness.Type(L'1');
    scene.bounds = scene.menu->GetBounds();
    REQUIRE(std::fabs(scene.bounds.height - kVisible) < 0.01f);
}

// Drags the thumb of the eleven-row list down 30 DIPs; returns the scroll.
float DragFilteredThumb(DropdownScene &scene)
{
    const compact_menu::Geometry geometry{11, kVisible};
    scene.harness.Press(scene.ThumbX(), scene.Y(20.0f));
    scene.harness.MoveHeld(scene.ThumbX(), scene.Y(50.0f));
    scene.harness.Release(scene.ThumbX(), scene.Y(50.0f));
    REQUIRE(scene.committed.empty());
    REQUIRE(scene.MenuIsOpen());
    return geometry.ScrollForThumbTop(kThumbEndGap + 30.0f);
}
} // namespace

TEST_CASE(settings_dropdown_typing_after_dragging_a_filtered_list_still_filters)
{
    DropdownScene scene(96.0f, 20, true);
    OpenFilteredByOne(scene);
    DragFilteredThumb(scene);
    // The search box kept the focus and its "1"; "15" leaves only Option 15.
    scene.harness.Type(L'5');
    scene.bounds = scene.menu->GetBounds();
    scene.harness.Click(scene.RowX(), scene.RowCentre(0, 0.0f));
    REQUIRE(scene.committed == std::vector<size_t>{15});
}

namespace
{
// Window y 1 DIP below the top of the last of 20 rows, with that row and the 2 DIP inset
// below it at the bottom of what the menu shows. The list is then scrolled by
// 602 - visible. Without the bottom inset the list scrolls 2 less and this is the gap
// above the row.
float JustBelowTheLastRowTop(const DropdownScene &scene)
{
    const float scroll = 602.0f - scene.bounds.height;
    const float probe = scene.RowCentre(19, scroll) - kRow * 0.5f + 1.0f;
    REQUIRE(compact_menu::RowAtContentY(probe - scene.bounds.y + scroll) == 19);
    return probe;
}
} // namespace

TEST_CASE(settings_dropdown_up_key_opening_a_deeply_cut_menu_shows_the_last_row)
{
    // The 120 DIP window leaves the menu 94 of the 180 DIPs it asks for, and it has never
    // been open before.
    DropdownScene scene(96.0f, 20, false, true, {800.0f, 120.0f});
    scene.harness.Window().FocusVisual(scene.dropdown.get());
    scene.harness.Key(VK_UP);
    scene.FindMenu();
    REQUIRE(scene.bounds.height < 100.0f);
    // Revealed against the asked-for 180 instead, the list would scroll to 422 and this
    // would be row 16.
    scene.harness.Click(scene.RowX(), JustBelowTheLastRowTop(scene));
    REQUIRE(scene.committed == std::vector<size_t>{19});
}

namespace
{
// A searchable dropdown whose menu opens over a text box in the page.
struct DropdownOverTextBox
{
    DropdownOverTextBox()
        : textBox(MakeTextBox()), scene(96.0f, 20, true, true, {800.0f, 900.0f}, 40.0f, textBox, {250.0f, 90.0f})
    {
    }

    static std::shared_ptr<msimeui::TextBox> MakeTextBox()
    {
        auto box = std::make_shared<msimeui::TextBox>(300.0f, L"");
        box->SetWidth(400.0f);
        box->SetHeight(300.0f);
        return box;
    }

    // Checks the text box is what lies under the menu at window point (x, y).
    void RequireTextBoxUnder(float x, float y) const
    {
        const msimeui::RectF box = textBox->GetBounds();
        REQUIRE(x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height);
    }

    std::shared_ptr<msimeui::TextBox> textBox;
    DropdownScene scene;
};
} // namespace

TEST_CASE(settings_dropdown_presses_on_the_menu_do_not_reach_a_text_box_beneath)
{
    DropdownOverTextBox page;
    DropdownScene &scene = page.scene;
    OpenFilteredByOne(scene);
    page.RequireTextBoxUnder(scene.ThumbX(), scene.Y(20.0f));
    const float scroll = DragFilteredThumb(scene);
    // A press passed to the text box neither drags the thumb nor picks the row.
    page.RequireTextBoxUnder(scene.RowX(), scene.RowCentre(3, scroll));
    scene.harness.Click(scene.RowX(), scene.RowCentre(3, scroll));
    REQUIRE(scene.committed == std::vector<size_t>{12});
}
