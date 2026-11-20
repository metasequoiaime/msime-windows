#include "settings/native/native_page_dictionary_panel.h"
#include "tests/includes/settings_window_harness.h"
#include "tests/includes/test_framework.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
using Json = native_settings::Json;
using native_settings::Column;
using native_settings::DictionaryPanel;
using native_settings::HostServices;
using native_settings::PageContext;
using native_settings::SettingsModel;
using native_settings::VisualPtr;

class StubHost : public HostServices
{
  public:
    explicit StubHost(msimeui::Window &window) : window_(window)
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
    void ShowToast(const std::wstring &message, bool success) override
    {
        toasts.emplace_back(message, success);
    }
    void Confirm(const std::wstring &, std::function<void(bool)> done, const std::wstring &, const std::wstring &,
                 const std::wstring &) override
    {
        if (done)
            done(false);
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

    std::vector<std::pair<std::wstring, bool>> toasts;

  private:
    msimeui::Window &window_;
};

// 一个面板与它跟宿主之间的全部往返：面板发出的消息在这里被捕获，宿主本该回的消息由 Respond 注入。
// 测试只经由这两个方向观察面板，不碰它的内部字段。
class PanelHarness
{
  public:
    explicit PanelHarness(DictionaryPanel::Kind kind)
        : window_(96.0f, {800.0f, 1600.0f}), host_(window_.Window()),
          model_([this](const std::string &text) { sent_.push_back(Json::parse(text)); }), context_(model_, host_),
          panel_(std::make_shared<DictionaryPanel>(context_, kind))
    {
    }

    DictionaryPanel &Panel()
    {
        return *panel_;
    }

    SettingsWindowHarness &Window()
    {
        return window_;
    }

    void Build()
    {
        // 生产里页面视觉由 pageCache_ 持有；这里也要让 column 活过整个用例，
        // 否则控件内部的 parent_ 会指向已析构的容器。
        column_ = std::make_shared<Column>();
        panel_->AddTo(*column_);
    }

    // 页面真正显示时窗口会把页面视觉挂进场景，Panel::Attach 再递归到每一个控件。
    // 测试走同一条链，而不是直接叫面板去加载。
    void Show()
    {
        window_.SetRoot(column_);
    }

    // 面板发给宿主的 dictionaryRequest 的 data 部分，按发送顺序。
    std::vector<Json> Queries() const
    {
        std::vector<Json> queries;
        for (const Json &message : sent_)
        {
            if (message.value("type", std::string()) == "dictionaryRequest")
                queries.push_back(message.at("data"));
        }
        return queries;
    }

    native_settings::Button *FindButton(const std::wstring &text)
    {
        for (float y = 0; y < 1600; y += 6)
            for (float x = 0; x < 800; x += 6)
                if (auto *button = dynamic_cast<native_settings::Button *>(window_.VisualAt(x, y));
                    button && button->Text() == text)
                    return button;
        return nullptr;
    }

    void Click(native_settings::Button *button)
    {
        const auto rect = button->GetBounds();
        window_.Click(rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f);
    }

    bool HasLabel(const std::wstring &text)
    {
        for (float y = 0; y < 1600; y += 6)
            for (float x = 0; x < 800; x += 6)
                if (auto *label = dynamic_cast<native_settings::Label *>(window_.VisualAt(x, y));
                    label && label->GetText() == text)
                    return true;
        return false;
    }

    void Respond(const Json &request, Json fields)
    {
        Json message = {{"type", "dictionaryResponse"},
                        {"protocolVersion", 1},
                        {"requestId", request.at("requestId")},
                        {"action", request.at("action")},
                        {"dictionary", request.at("dictionary")}};
        message.update(fields);
        model_.Receive(message.dump());
        window_.Layout();
    }

  private:
    SettingsWindowHarness window_;
    StubHost host_;
    std::vector<Json> sent_;
    SettingsModel model_;
    PageContext context_;
    std::shared_ptr<DictionaryPanel> panel_;
    std::shared_ptr<Column> column_;
};

Json QueryPage(size_t offset, size_t rows, bool has_more, size_t total)
{
    Json rows_json = Json::array();
    for (size_t i = 0; i < rows; ++i)
        rows_json.push_back({{"code", "ci" + std::to_string(i)}, {"word", "词" + std::to_string(i)}, {"weight", 10}});
    return {{"ok", true},       {"message", "查询成功"}, {"rows", std::move(rows_json)},
            {"offset", offset}, {"hasMore", has_more},   {"total", total}};
}
} // namespace

// 设置窗口打开时，索引器会把还没被访问过的页面逐个建出来（native_settings_window.cpp 的
// IndexNextPage）。builder 因此不能自己去查词库——那是「打开页面」才该发生的事。
TEST_CASE(DictionaryPanelLoadsTheUserViewOnFirstShowNotOnBuild)
{
    PanelHarness harness(DictionaryPanel::Kind::Dictionary);
    harness.Build();
    REQUIRE(harness.Queries().empty());

    harness.Show();
    const auto queries = harness.Queries();
    REQUIRE_EQ(queries.size(), size_t{1});
    REQUIRE_EQ(queries.at(0).at("scope").get<std::string>(), std::string("user"));
    REQUIRE(!queries.at(0).contains("code"));

    // 每次切回这一页都会重新 Attach，只该加载一次。
    harness.Show();
    REQUIRE_EQ(harness.Queries().size(), size_t{1});
}

TEST_CASE(QuickPhrasePanelLoadsEveryPhraseOnFirstShow)
{
    PanelHarness harness(DictionaryPanel::Kind::Quick);
    harness.Build();
    REQUIRE(harness.Queries().empty());

    harness.Show();
    const auto queries = harness.Queries();
    REQUIRE_EQ(queries.size(), size_t{1});
    REQUIRE(!queries.at(0).contains("scope"));
    REQUIRE_EQ(queries.at(0).at("code").get<std::string>(), std::string());
}

// 快捷短语搜索框右端嵌着「前缀匹配 / 精确匹配」下拉，竖线隔开：它在输入框里面、不压住文字区，
// 选了精确匹配之后立刻按框里的编码重查，并在请求里带上 match。
TEST_CASE(QuickPhraseSearchBoxSwitchesToExactMatch)
{
    PanelHarness harness(DictionaryPanel::Kind::Quick);
    harness.Build();
    harness.Show();
    REQUIRE_EQ(harness.Queries().size(), size_t{1});
    REQUIRE(!harness.Queries().at(0).contains("match"));

    // 工具栏那一行（padding-top 20，输入框高 42）上从左往右找：先是文本框，再是下拉。
    SettingsWindowHarness &window = harness.Window();
    const float row = 20.0f + 21.0f;
    native_settings::Dropdown *match = nullptr;
    msimeui::TextBox *text = nullptr;
    float matchLeft = 0.0f;
    float textRight = 0.0f;
    for (float x = 0.0f; x < 800.0f; x += 1.0f)
    {
        msimeui::Visual *visual = window.VisualAt(x, row);
        if (auto *box = dynamic_cast<msimeui::TextBox *>(visual); box && !match)
        {
            text = box;
            textRight = x;
        }
        if (auto *dropdown = dynamic_cast<native_settings::Dropdown *>(visual); dropdown && !match)
        {
            match = dropdown;
            matchLeft = x;
        }
    }
    REQUIRE(text != nullptr);
    REQUIRE(match != nullptr);
    REQUIRE_EQ(match->GetValue(), std::string("prefix"));
    // 文本区停在竖线处，下拉在竖线后 5 DIP：两者的命中区不重叠，中间是竖线和间距。
    REQUIRE(matchLeft - textRight >= 4.0f);
    const msimeui::RectF toggle = match->GetBounds();
    REQUIRE(toggle.height < 42.0f);

    window.GiveWindowKeyboardFocus();
    window.Click(toggle.x + toggle.width * 0.5f, toggle.y + toggle.height * 0.5f);
    window.Key(VK_DOWN);
    window.Key(VK_DOWN);
    window.Key(VK_RETURN);
    REQUIRE_EQ(match->GetValue(), std::string("exact"));

    const auto queries = harness.Queries();
    REQUIRE_EQ(queries.size(), size_t{2});
    REQUIRE_EQ(queries.at(1).at("match").get<std::string>(), std::string("exact"));
    REQUIRE_EQ(queries.at(1).at("offset").get<size_t>(), size_t{0});
}

// 拼音用户词库按 updated_at 排序：新增和修改都会把该行顶到第一页，留在原页等于什么都没发生。
// 快捷短语按 weight 排序，新增的那条未必在第一页，硬跳回去反而把用户丢在原地的位置也丢了。
TEST_CASE(WritingTheUserListReturnsToTheFirstPage)
{
    using native_settings::ListOrder;
    REQUIRE_EQ(native_settings::OffsetAfterWrite("create", 200, ListOrder::ByUpdatedAt), size_t{0});
    REQUIRE_EQ(native_settings::OffsetAfterWrite("update", 200, ListOrder::ByUpdatedAt), size_t{0});
    REQUIRE_EQ(native_settings::OffsetAfterWrite("delete", 200, ListOrder::ByUpdatedAt), size_t{200});
    REQUIRE_EQ(native_settings::OffsetAfterWrite("create", 200, ListOrder::ByWeight), size_t{200});
    REQUIRE_EQ(native_settings::OffsetAfterWrite("update", 200, ListOrder::ByWeight), size_t{200});
}

TEST_CASE(DictionaryPagerShowsTheTotalAndCalculatesTheLastPage)
{
    REQUIRE_EQ(native_settings::PageStatus(0, 100, 205), L"第 1–100 条，共计 205 条");
    REQUIRE_EQ(native_settings::PageStatus(200, 5, 205), L"第 201–205 条，共计 205 条");
    REQUIRE_EQ(native_settings::PageStatus(0, 0, 0), L"没有结果，共计 0 条");
    REQUIRE_EQ(native_settings::LastPageOffset(0, 100), size_t{0});
    REQUIRE_EQ(native_settings::LastPageOffset(100, 100), size_t{0});
    REQUIRE_EQ(native_settings::LastPageOffset(101, 100), size_t{100});
    REQUIRE_EQ(native_settings::LastPageOffset(200, 100), size_t{100});
    REQUIRE_EQ(native_settings::LastPageOffset(205, 100), size_t{200});
}

TEST_CASE(DictionaryPagerClicksTheLastAndFirstPages)
{
    PanelHarness harness(DictionaryPanel::Kind::Dictionary);
    harness.Build();
    harness.Show();
    harness.Respond(harness.Queries().front(), QueryPage(0, 100, true, 205));

    native_settings::Button *first = harness.FindButton(L"首页");
    native_settings::Button *previous = harness.FindButton(L"上一页");
    native_settings::Button *next = harness.FindButton(L"下一页");
    native_settings::Button *last = harness.FindButton(L"末页");
    REQUIRE(first != nullptr);
    REQUIRE(previous != nullptr);
    REQUIRE(next != nullptr);
    REQUIRE(last != nullptr);
    REQUIRE(first->IsDisabled());
    REQUIRE(previous->IsDisabled());
    REQUIRE(!next->IsDisabled());
    REQUIRE(!last->IsDisabled());
    REQUIRE(harness.HasLabel(L"第 1–100 条，共计 205 条"));

    harness.Click(last);
    auto queries = harness.Queries();
    REQUIRE_EQ(queries.size(), size_t{2});
    REQUIRE_EQ(queries.at(1).at("offset").get<size_t>(), size_t{200});
    REQUIRE(last->IsDisabled());
    harness.Respond(queries.at(1), QueryPage(200, 5, false, 205));
    REQUIRE(!first->IsDisabled());
    REQUIRE(!previous->IsDisabled());
    REQUIRE(next->IsDisabled());
    REQUIRE(last->IsDisabled());
    REQUIRE(harness.HasLabel(L"第 201–205 条，共计 205 条"));

    harness.Click(first);
    queries = harness.Queries();
    REQUIRE_EQ(queries.size(), size_t{3});
    REQUIRE_EQ(queries.at(2).at("offset").get<size_t>(), size_t{0});
}

// 查询失败不能沿用上一次的总数：末页按钮会按旧总数跳到与失败查询无关的偏移。
TEST_CASE(DictionaryQueryFailureResetsThePager)
{
    PanelHarness harness(DictionaryPanel::Kind::Dictionary);
    harness.Build();
    harness.Show();
    harness.Respond(harness.Queries().front(), QueryPage(0, 100, true, 205));

    native_settings::Button *next = harness.FindButton(L"下一页");
    native_settings::Button *last = harness.FindButton(L"末页");
    native_settings::Button *search = harness.FindButton(L"查询");
    REQUIRE(next != nullptr);
    REQUIRE(last != nullptr);
    REQUIRE(search != nullptr);
    REQUIRE(!next->IsDisabled());
    REQUIRE(!last->IsDisabled());

    harness.Click(search);
    const auto queries = harness.Queries();
    REQUIRE_EQ(queries.size(), size_t{2});
    harness.Respond(queries.at(1), {{"ok", false}, {"message", "查询失败"}});
    REQUIRE(next->IsDisabled());
    REQUIRE(last->IsDisabled());
    REQUIRE(harness.HasLabel(L"查询失败，请重试"));
}

// 删掉最后一页仅剩的条目后，那一页会查成空页；停在空页上会误报「还没有添加过词条」。
TEST_CASE(AnEmptiedPageStepsBackTowardsTheFirstPage)
{
    PanelHarness harness(DictionaryPanel::Kind::Dictionary);
    harness.Build();
    harness.Show();
    REQUIRE_EQ(harness.Queries().size(), size_t{1});

    harness.Respond(harness.Queries().front(), QueryPage(200, 0, false, 200));
    const auto queries = harness.Queries();
    REQUIRE_EQ(queries.size(), size_t{2});
    REQUIRE_EQ(queries.at(1).at("offset").get<size_t>(), size_t{100});
}
