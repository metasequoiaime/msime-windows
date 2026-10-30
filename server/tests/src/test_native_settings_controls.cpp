#include "tests/includes/test_framework.h"

#include "settings/native/native_settings_controls.h"
#include "settings/native/native_page_help_widgets.h"

#include "msimeui/Window.h"

#include <functional>
#include <memory>
#include <string>

namespace
{
using native_settings::Button;

// Counts live copies of itself and destroys the visual that owns the stored callback, the way a
// handler that closes its modal dialog does. Reading only after teardown is the point: a handler
// invoked through its owner's member is destroyed mid-call, a handler invoked from a copy is not.
struct HandlerProbe
{
    static int live;
    static int liveDuringCall;

    explicit HandlerProbe(std::function<void()> *owner) : owner_(owner)
    {
        ++live;
    }

    HandlerProbe(const HandlerProbe &other) : owner_(other.owner_)
    {
        ++live;
    }

    HandlerProbe &operator=(const HandlerProbe &) = delete;

    ~HandlerProbe()
    {
        --live;
    }

    void operator()() const
    {
        (*owner_)();
        liveDuringCall = live;
    }

  private:
    std::function<void()> *owner_;
};

int HandlerProbe::live = 0;
int HandlerProbe::liveDuringCall = -1;

void ResetProbe()
{
    HandlerProbe::live = 0;
    HandlerProbe::liveDuringCall = -1;
}

void CheckHandlerLifetime(WPARAM key)
{
    // No HWND is created, so these tests do not open a window or take input focus
    msimeui::Window window(L"settings-controls-tests", L"", 100, 100);
    auto button = std::make_shared<Button>(L"Delete");
    Button *raw = button.get();
    std::weak_ptr<Button> watch = button;
    raw->Attach(&window);
    raw->Arrange({0.0f, 0.0f, 100.0f, 30.0f});
    ResetProbe();
    std::function<void()> release = [&button]() { button.reset(); };
    raw->SetOnClick(HandlerProbe(&release));
    REQUIRE_EQ(HandlerProbe::live, 1);

    if (key)
        REQUIRE(raw->OnKeyDown(key, 0));
    else
    {
        const POINT point{10, 10};
        REQUIRE(raw->OnMouseDown(point, 0));
        REQUIRE(raw->OnMouseUp(point, 0));
    }

    REQUIRE(!button);
    REQUIRE(watch.expired());
    REQUIRE(HandlerProbe::liveDuringCall >= 1);
    REQUIRE_EQ(HandlerProbe::live, 0);
}
} // namespace

TEST_CASE(SettingsButtonMouseHandlerOutlivesTheDialogItCloses)
{
    CheckHandlerLifetime(0);
}

TEST_CASE(SettingsButtonEnterHandlerOutlivesTheDialogItCloses)
{
    CheckHandlerLifetime(VK_RETURN);
}

TEST_CASE(SettingsButtonSpaceHandlerOutlivesTheDialogItCloses)
{
    CheckHandlerLifetime(VK_SPACE);
}

TEST_CASE(SettingsLabelHandlerOutlivesTheVisualItDestroys)
{
    msimeui::Window window(L"settings-controls-tests", L"", 200, 200);
    auto label = std::make_shared<native_settings::Label>(L"打开", native_settings::BodyStyle());
    native_settings::Label *raw = label.get();
    raw->Attach(&window);
    raw->Arrange({0.0f, 0.0f, 100.0f, 30.0f});
    ResetProbe();
    std::function<void()> release = [&label]() { label.reset(); };
    raw->SetOnClick(HandlerProbe(&release));

    const POINT point{10, 10};
    REQUIRE(raw->OnMouseDown(point, 0));
    REQUIRE(raw->OnMouseUp(point, 0));

    REQUIRE(!label);
    REQUIRE(HandlerProbe::liveDuringCall >= 1);
    REQUIRE_EQ(HandlerProbe::live, 0);
}

// CssButton is the one the update dialog's download buttons use, and it is the one that shipped
// broken. The generic probes above use native_settings::Button, so this gap went unnoticed.
TEST_CASE(SettingsCssButtonHandlerOutlivesTheDialogItCloses)
{
    msimeui::Window window(L"settings-controls-tests", L"", 200, 200);
    auto button = std::make_shared<native_settings::CssButton>(L"前往下载", native_settings::CssButton::Style());
    native_settings::CssButton *raw = button.get();
    raw->Attach(&window);
    raw->Arrange({0.0f, 0.0f, 100.0f, 30.0f});
    ResetProbe();
    std::function<void()> release = [&button]() { button.reset(); };
    raw->SetOnClick(HandlerProbe(&release));

    const POINT point{10, 10};
    REQUIRE(raw->OnMouseDown(point, 0));
    REQUIRE(raw->OnMouseUp(point, 0));

    REQUIRE(!button);
    REQUIRE(HandlerProbe::liveDuringCall >= 1);
    REQUIRE_EQ(HandlerProbe::live, 0);
}

// The update dialog's handlers do exactly this: close the modal, which destroys the button, and
// then post a message. The message must still arrive. Before the handler copy, the closure was
// gone by the time the post ran and the download buttons silently did nothing.
TEST_CASE(SettingsCssButtonFinishesPostingAfterClosingItself)
{
    msimeui::Window window(L"settings-controls-tests", L"", 200, 200);
    auto button = std::make_shared<native_settings::CssButton>(L"国内镜像下载", native_settings::CssButton::Style());
    native_settings::CssButton *raw = button.get();
    raw->Attach(&window);
    raw->Arrange({0.0f, 0.0f, 100.0f, 30.0f});

    int posted = 0;
    std::string postedUrl;
    const std::string mirror = "https://dl.msime.app/gh/example";
    raw->SetOnClick([&button, &posted, &postedUrl, mirror]() {
        button.reset();
        ++posted;
        postedUrl = mirror;
    });

    const POINT point{10, 10};
    REQUIRE(raw->OnMouseDown(point, 0));
    REQUIRE(raw->OnMouseUp(point, 0));

    REQUIRE(!button);
    REQUIRE_EQ(posted, 1);
    REQUIRE_EQ(postedUrl, mirror);
}

TEST_CASE(SettingsButtonIgnoresDisabledAndOutsideClicks)
{
    msimeui::Window window(L"settings-controls-tests", L"", 100, 100);
    Button button(L"Delete");
    button.Attach(&window);
    button.Arrange({0.0f, 0.0f, 100.0f, 30.0f});
    int calls = 0;
    button.SetOnClick([&calls]() { ++calls; });
    const POINT inside{10, 10};
    const POINT outside{110, 10};

    button.SetDisabled(true);
    REQUIRE(!button.OnMouseDown(inside, 0));
    REQUIRE(!button.OnMouseUp(inside, 0));
    REQUIRE(!button.OnKeyDown(VK_RETURN, 0));
    REQUIRE(!button.OnKeyDown(VK_SPACE, 0));
    button.SetDisabled(false);
    REQUIRE(!button.OnMouseDown(outside, 0));
    REQUIRE(!button.OnMouseUp(inside, 0));
    REQUIRE(button.OnMouseDown(inside, 0));
    REQUIRE(button.OnMouseUp(outside, 0));
    REQUIRE(!button.OnKeyDown(VK_ESCAPE, 0));
    REQUIRE_EQ(calls, 0);

    REQUIRE(button.OnMouseDown(inside, 0));
    REQUIRE(button.OnMouseUp(inside, 0));
    REQUIRE(button.OnKeyDown(VK_RETURN, 0));
    REQUIRE(button.OnKeyDown(VK_SPACE, 0));
    REQUIRE_EQ(calls, 3);
}

// A search hit inside a collapsed toggle section ("emoji 混输" under 候选混输) has to open the
// section, chevron included, or the jump lands on a row that is not on screen.
TEST_CASE(SettingsSearchRevealOpensCollapsedExpander)
{
    using namespace native_settings;
    auto row = MakeRow(L"emoji 混输", L"", nullptr, true);
    auto details = std::make_shared<Column>(0.0f);
    details->Add(row);
    auto hideable = std::make_shared<Hideable>(details, false);
    auto expander = std::make_shared<Expander>(L"候选混输");
    BindExpander(expander, hideable);
    auto section = std::make_shared<Column>(0.0f);
    section->Add(std::make_shared<SettingRow>(expander, nullptr));
    section->Add(hideable);

    REQUIRE(!hideable->IsVisible());
    REQUIRE(RevealInAncestors(*row));
    REQUIRE(hideable->IsVisible());
    REQUIRE(expander->IsExpanded());
}

// Search used to index only rows built by MakeRow, so a toggle-section header (智能标点) and the
// sub items a page assembles from MakeSubTitle into its own SettingRow were not findable. Titles
// are indexed where they are made now; MakeRow still counts once.
TEST_CASE(SettingsIndexCoversTitlesHoweverTheRowIsBuilt)
{
    using namespace native_settings;
    std::vector<SettingsIndexScope::Row> rows;
    {
        SettingsIndexScope scope("input", [&rows](const SettingsIndexScope::Row &row) { rows.push_back(row); });
        auto header = std::make_shared<SettingRow>(MakeExpander(L"智能标点"), nullptr);
        auto left = std::make_shared<Column>(0.0f);
        left->Add(MakeSubTitle(L"数字后直出", L"数字后输入 , . : 直接输出英文标点"));
        auto handBuilt = std::make_shared<SettingRow>(left, nullptr);
        auto row = MakeRow(L"成对标点自动补全", L"输入左侧符号时自动补全右侧符号", nullptr);
    }
    REQUIRE_EQ(rows.size(), static_cast<size_t>(3));
    REQUIRE(rows[0].title == L"智能标点");
    REQUIRE(rows[1].title == L"数字后直出");
    REQUIRE(rows[1].description == L"数字后输入 , . : 直接输出英文标点");
    REQUIRE(rows[2].title == L"成对标点自动补全");
    REQUIRE(rows[2].description == L"输入左侧符号时自动补全右侧符号");
}

// A group whose visibility follows a setting has no reveal handler and must stay hidden; the
// caller is told so it does not scroll to a row that takes no room.
TEST_CASE(SettingsSearchRevealLeavesSettingDrivenGroupsHidden)
{
    using namespace native_settings;
    auto row = MakeRow(L"英文触发字符数", L"", nullptr, true);
    auto hideable = std::make_shared<Hideable>(row, false);
    auto page = std::make_shared<Column>(0.0f);
    page->Add(hideable);

    REQUIRE(!RevealInAncestors(*row));
    REQUIRE(!hideable->IsVisible());
}
