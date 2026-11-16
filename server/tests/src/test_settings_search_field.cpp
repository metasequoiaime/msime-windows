#include "tests/includes/settings_window_harness.h"
#include "tests/includes/test_framework.h"

#include "settings/native/native_settings_search_field.h"

#include "msimeui/Layout.h"

#include <memory>
#include <string>
#include <vector>

using native_settings::SearchField;

namespace
{
struct SearchFieldScene
{
    explicit SearchFieldScene(float dpi) : harness(dpi, {400.0f, 200.0f})
    {
        field = std::make_shared<SearchField>();
        field->SetOnChanged([this](const std::wstring &text) { reported.push_back(text); });
        auto root = std::make_shared<msimeui::OverlayPanel>();
        root->AddChildAt(field, 20.0f, 20.0f);
        harness.SetRoot(root);
    }

    // Focuses the text and types into it, the way a user starts a search. The click makes the text
    // box the window's focused visual; the box only hears it is focused once the (never shown)
    // window gets the keyboard focus a real session would have.
    void TypeQuery(const std::wstring &query)
    {
        const msimeui::RectF bounds = field->GetBounds();
        harness.Click(bounds.x + 20.0f, bounds.y + bounds.height * 0.5f);
        harness.GiveWindowKeyboardFocus();
        for (wchar_t ch : query)
            harness.Type(ch);
    }

    SettingsWindowHarness harness;
    std::shared_ptr<SearchField> field;
    std::vector<std::wstring> reported;
};

// The clear button only exists while the text box is focused and has text. The window blurs the
// focused visual on a click that has no focus target, before the press is delivered, so the button
// has to claim the press without taking the focus or it is gone by the time it sees the click.
void CheckClearButtonClearsText(float dpi)
{
    SearchFieldScene scene(dpi);
    scene.TypeQuery(L"u");
    REQUIRE(scene.field->IsFocused());
    REQUIRE_EQ(scene.field->GetText(), std::wstring(L"u"));
    REQUIRE(scene.field->ClearVisible());

    const msimeui::RectF clear = scene.field->ClearRect();
    scene.harness.Press(clear.x + clear.width * 0.5f, clear.y + clear.height * 0.5f);
    // Pressing the button leaves the search box focused.
    REQUIRE(scene.field->IsFocused());
    scene.harness.Release(clear.x + clear.width * 0.5f, clear.y + clear.height * 0.5f);

    REQUIRE(scene.field->GetText().empty());
    REQUIRE(scene.field->IsFocused());
    REQUIRE(!scene.reported.empty());
    REQUIRE(scene.reported.back().empty());
}

// Pressing the button and letting go elsewhere cancels, like any button.
void CheckReleaseOutsideKeepsText(float dpi)
{
    SearchFieldScene scene(dpi);
    scene.TypeQuery(L"ai");
    const msimeui::RectF clear = scene.field->ClearRect();
    scene.harness.Press(clear.x + clear.width * 0.5f, clear.y + clear.height * 0.5f);
    scene.harness.Release(clear.x + clear.width * 0.5f, clear.y + clear.height + 60.0f);
    REQUIRE_EQ(scene.field->GetText(), std::wstring(L"ai"));
    REQUIRE(scene.field->IsFocused());
}
} // namespace

TEST_CASE(SettingsSearchFieldClearButtonClearsTextAt100Percent)
{
    CheckClearButtonClearsText(96.0f);
}

TEST_CASE(SettingsSearchFieldClearButtonClearsTextAt150Percent)
{
    CheckClearButtonClearsText(144.0f);
}

TEST_CASE(SettingsSearchFieldClearButtonClearsTextAt200Percent)
{
    CheckClearButtonClearsText(192.0f);
}

TEST_CASE(SettingsSearchFieldClearButtonCancelsWhenReleasedOutside)
{
    CheckReleaseOutsideKeepsText(96.0f);
    CheckReleaseOutsideKeepsText(144.0f);
}
