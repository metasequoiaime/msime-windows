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

// The skin page is a list of cards, and a card is one button: the title and the preview are
// part of it, so a press anywhere selects the skin. Left as a plain panel, the press would
// land on a label inside it and the card would never see it.
TEST_CASE(SettingsSkinCardIsOneClickTarget)
{
    msimeui::Window window(L"settings-window-test", L"", 200, 200);
    auto card = std::make_shared<native_settings::skinpreview::PickCard>();
    card->Add(std::make_shared<native_settings::Label>(L"标题", native_settings::SectionTitleStyle()));
    card->Attach(&window);
    card->MeasureInLayout({200.0f, 100.0f});
    card->ArrangeInLayout({0.0f, 0.0f, 200.0f, 100.0f});

    int activations = 0;
    card->SetOnActivate([&activations]() { ++activations; });

    REQUIRE(card->FindVisualAt({5.0f, 5.0f}) == card.get());
    REQUIRE(card->FindVisualAt({250.0f, 5.0f}) == nullptr);

    const POINT inside{10, 10};
    const POINT outside{250, 10};
    REQUIRE(card->OnMouseDown(inside, 0));
    REQUIRE(card->OnMouseUp(inside, 0));
    REQUIRE_EQ(activations, 1);
    // Releasing outside the card cancels the press, like every other button here.
    REQUIRE(card->OnMouseDown(inside, 0));
    REQUIRE(card->OnMouseUp(outside, 0));
    REQUIRE_EQ(activations, 1);
    REQUIRE(card->OnKeyDown(VK_RETURN, 0));
    REQUIRE(card->OnKeyDown(VK_SPACE, 0));
    REQUIRE(!card->OnKeyDown(VK_ESCAPE, 0));
    REQUIRE_EQ(activations, 3);

    // A skin the current layout cannot use takes no input and leaves the tab order.
    card->SetSelectable(false);
    REQUIRE(!card->IsFocusable());
    REQUIRE(!card->OnMouseDown(inside, 0));
    REQUIRE(!card->OnKeyDown(VK_RETURN, 0));
    REQUIRE_EQ(activations, 3);
}

// Clicking a card posts the skin it shows, not the one above or below it. Cards are found by
// walking the page, because what matters is that the whole card answers, wherever it is.
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
    window.Click(300.0f, bounds.y + 8.0f);

    std::string picked;
    for (const std::string &raw : sent)
    {
        const native_settings::Json message = native_settings::Json::parse(raw, nullptr, false);
        if (message.is_object() && message.value("type", std::string()) == "configUpdate")
            picked = message["data"].value("value", std::string());
    }
    REQUIRE_EQ(picked, std::string("wechat"));
    REQUIRE(cards[1]->IsActive());
    REQUIRE(!cards[0]->IsActive());
}
