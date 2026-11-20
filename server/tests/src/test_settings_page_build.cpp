#include "settings/native/native_settings_pages.h"
#include "settings/native/native_page_skin_common.h"
#include "tests/includes/settings_window_harness.h"
#include "tests/includes/test_framework.h"

#include <memory>
#include <string>
#include <vector>

namespace
{
using native_settings::HostServices;
using native_settings::PageContext;
using native_settings::SettingsModel;
using native_settings::VisualPtr;

class NullHost : public HostServices
{
  public:
    explicit NullHost(msimeui::Window &window) : window_(window)
    {
    }
    HWND Hwnd() const override
    {
        return nullptr;
    }
    msimeui::Window *Window() const override
    {
        return &window_;
    }
    void ShowToast(const std::wstring &, bool) override
    {
    }
    void Confirm(const std::wstring &, std::function<void(bool)>, const std::wstring &, const std::wstring &,
                 const std::wstring &) override
    {
    }
    void ShowModal(VisualPtr, std::function<void()>) override
    {
    }
    void CloseModal() override
    {
    }
    void NavigateTo(const std::string &) override
    {
    }
    void ScrollIntoView(const msimeui::Visual &) override
    {
    }

  private:
    msimeui::Window &window_;
};
} // namespace

// 设置窗口打开后，索引器会把还没被访问过的页面逐个建出来，只为收集搜索索引
// （native_settings_window.cpp 的 BuildPageAndIndex / IndexNextPage）。builder 因此只搭控件：
// 查词库、解码图片这类动作会为一张用户没打开的页面付出代价，失败时还会在无关页面上弹提示。
// 词典面板的首次查询就挂在页面 Attach 上，而不是构造函数。
TEST_CASE(BuildingEverySettingsPageSendsNoMessage)
{
    SettingsWindowHarness window(96.0f, {800.0f, 1200.0f});
    NullHost host(window.Window());
    std::vector<std::string> sent;
    SettingsModel model([&sent](const std::string &message) { sent.push_back(message); });
    PageContext context(model, host);

    std::vector<std::string> offenders;
    for (const native_settings::PageInfo &page : native_settings::Pages())
    {
        REQUIRE(page.build != nullptr);
        sent.clear();
        const VisualPtr visual = page.build(context);
        REQUIRE(visual != nullptr);
        // 建页面期间发给宿主的任何消息都意味着索引器替用户触发了它。一次列出全部违规页面，
        // 否则修掉一个才能看见下一个。
        if (!sent.empty())
            offenders.push_back(std::string(page.id) + " posted " + sent.front());
    }
    if (!offenders.empty())
    {
        std::string joined;
        for (const std::string &offender : offenders)
            joined += (joined.empty() ? "" : "; ") + offender;
        throw std::runtime_error(joined);
    }
}

// A skin card is one button covering its title and its preview. The preview holds a focusable
// candidate list, and Window dispatches a press to the focusable visual under the cursor, so
// the card must answer for focus as well as for hit testing. Otherwise the largest part of
// the card changes the preview's selected row and never selects the skin.
TEST_CASE(SettingsSkinCardKeepsThePressOverFocusableContent)
{
    for (float dpi : {96.0f, 144.0f})
    {
        SettingsWindowHarness window(dpi, {300.0f, 200.0f});
        auto card = std::make_shared<native_settings::skinpreview::PickCard>();
        auto inner = std::make_shared<native_settings::ToggleSwitch>(false);
        card->Add(inner);
        window.SetRoot(card);

        int activations = 0;
        card->SetOnActivate([&activations]() { ++activations; });

        const msimeui::RectF innerBounds = inner->GetBounds();
        REQUIRE(innerBounds.width > 0.0f);
        const float x = innerBounds.x + innerBounds.width * 0.5f;
        const float y = innerBounds.y + innerBounds.height * 0.5f;
        auto *scene = window.Window().GetScene();
        REQUIRE(scene->FindVisualAt({x, y}) == card.get());
        REQUIRE(scene->FindFocusableAt({x, y}) == card.get());

        window.GiveWindowKeyboardFocus();
        window.Click(x, y);
        REQUIRE_EQ(activations, 1);
        REQUIRE(!inner->IsChecked());
        window.Key(VK_RETURN);
        REQUIRE_EQ(activations, 2);
        window.Key(VK_SPACE);
        REQUIRE_EQ(activations, 3);

        // Releasing outside the card cancels the press, like every other button here.
        const POINT inside = window.Pixels(x, y);
        const POINT outside = window.Pixels(400.0f, 400.0f);
        REQUIRE(card->OnMouseDown(inside, 0));
        REQUIRE(card->OnMouseUp(outside, 0));
        REQUIRE_EQ(activations, 3);

        // An unusable skin is not a focus target and takes no input.
        card->SetSelectable(false);
        REQUIRE(!card->IsFocusable());
        REQUIRE(scene->FindFocusableAt({x, y}) == nullptr);
        window.Click(x, y);
        window.Key(VK_RETURN);
        REQUIRE_EQ(activations, 3);
    }
}

// Clicking a card posts the skin it shows, not the one above or below it. The press lands in
// the preview, the part of the card the candidate list would otherwise take.
TEST_CASE(SettingsSkinPageSwitchesSkinFromTheCard)
{
    SettingsWindowHarness window(96.0f, {800.0f, 1400.0f});
    NullHost host(window.Window());
    std::vector<std::string> sent;
    SettingsModel model([&sent](const std::string &message) { sent.push_back(message); });
    PageContext context(model, host);
    model.Receive(native_settings::Json{{"type", "configSnapshot"},
                                        {"data",
                                         {{"appearance",
                                           {{"candidate_skin", "fluent"},
                                            {"external_candidate_skins", native_settings::Json::array()},
                                            {"candidate_skin_catalog_scanned", true},
                                            {"candidate_skin_directory", "C:\\skins"}}}}}}
                      .dump());

    const VisualPtr page = native_settings::BuildSkinPage(context);
    window.SetRoot(page);
    sent.clear();

    std::vector<native_settings::skinpreview::PickCard *> cards;
    for (float y = 0.0f; y < 1400.0f; y += 4.0f)
    {
        auto *card = dynamic_cast<native_settings::skinpreview::PickCard *>(window.VisualAt(300.0f, y));
        if (card && (cards.empty() || cards.back() != card))
            cards.push_back(card);
    }
    REQUIRE(cards.size() >= 2);

    const msimeui::RectF bounds = cards[1]->GetBounds();
    const msimeui::PointF preview{300.0f, bounds.y + bounds.height - 55.0f};
    REQUIRE(window.Window().GetScene()->FindFocusableAt(preview) == cards[1]);
    window.Click(preview.x, preview.y);

    std::string picked;
    for (const std::string &raw : sent)
    {
        const native_settings::Json message = native_settings::Json::parse(raw, nullptr, false);
        if (message.is_object() && message.value("type", std::string()) == "configUpdate")
            picked = message["data"].value("value", std::string());
    }
    REQUIRE_EQ(picked, native_settings::skinpreview::BuiltinSkins()[1]);
    REQUIRE(cards[1]->IsActive());
    REQUIRE(!cards[0]->IsActive());
}
