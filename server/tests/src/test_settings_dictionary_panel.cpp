#include "settings/native/native_page_dictionary_panel.h"
#include "tests/includes/settings_window_harness.h"
#include "tests/includes/test_framework.h"

#include <memory>
#include <set>
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
    void Confirm(const std::wstring &message, std::function<void(bool)> done, const std::wstring &,
                 const std::wstring &, const std::wstring &) override
    {
        confirmations.push_back(message);
        if (done)
            done(confirm);
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
    std::vector<std::wstring> confirmations;
    // 真实宿主弹框等用户点确认；这里直接给答案。
    bool confirm = true;

  private:
    msimeui::Window &window_;
};

// 一个面板与它跟宿主之间的全部往返：面板发出的消息在这里被捕获，宿主本该回的消息由 Respond 注入。
// 测试只经由这两个方向观察面板，不碰它的内部字段。
class PanelHarness
{
  public:
    explicit PanelHarness(DictionaryPanel::Kind kind, float viewportWidth = 800.0f)
        : window_(96.0f, {viewportWidth, 1600.0f}), host_(window_.Window()),
          model_([this](const std::string &text) { sent_.push_back(Json::parse(text)); }), context_(model_, host_),
          panel_(std::make_shared<DictionaryPanel>(context_, kind))
    {
    }

    Column &Root()
    {
        return *column_;
    }

    DictionaryPanel &Panel()
    {
        return *panel_;
    }

    SettingsWindowHarness &Window()
    {
        return window_;
    }

    StubHost &Host()
    {
        return host_;
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

namespace
{
template <class T> std::set<T *> Visible(SettingsWindowHarness &window, float bottom)
{
    std::set<T *> found;
    for (float y = 0.0f; y < bottom; y += 6.0f)
    {
        for (float x = 0.0f; x < 800.0f; x += 6.0f)
        {
            for (msimeui::Visual *visual = window.VisualAt(x, y); visual != nullptr; visual = visual->GetParentVisual())
            {
                if (auto *target = dynamic_cast<T *>(visual))
                {
                    found.insert(target);
                    break;
                }
            }
        }
    }
    return found;
}

native_settings::Button *FindToolbarButton(SettingsWindowHarness &window, const std::wstring &prefix)
{
    for (native_settings::Button *button : Visible<native_settings::Button>(window, 120.0f))
    {
        if (button->Text().rfind(prefix, 0) == 0)
            return button;
    }
    return nullptr;
}
} // namespace

// 勾选若干行之后，工具栏的「删除选中」从禁用变成带计数的可用按钮；点它先问一句总数，
// 确认后只发一条 deleteMany，把这一页选中的条目一起交给后端——不是逐条发 delete。
TEST_CASE(DictionaryRowsCanBeSelectedAndDeletedInOneRequest)
{
    PanelHarness harness(DictionaryPanel::Kind::Dictionary);
    harness.Build();
    harness.Show();
    REQUIRE_EQ(harness.Queries().size(), size_t{1});
    harness.Respond(harness.Queries().front(), QueryPage(0, 3, false, 3));

    SettingsWindowHarness &window = harness.Window();
    // 视口里的几何是上一次布局的结果，响应注入之后要重新排一次才能拿到行高。
    window.Layout();
    const auto bodies = Visible<native_settings::DictTableBody>(window, 1600.0f);
    REQUIRE_EQ(bodies.size(), size_t{1});
    const msimeui::RectF body = (*bodies.begin())->GetBounds();
    // 三行平分表体高度：勾选框在行左端 40px 的列里居中。
    const float rowHeight = body.height / 3.0f;
    const float checkX = body.x + 20.0f;

    native_settings::Button *remove = FindToolbarButton(window, L"删除选中");
    REQUIRE(remove != nullptr);
    REQUIRE(remove->IsDisabled());

    window.Click(checkX, body.y + rowHeight * 0.5f);
    window.Click(checkX, body.y + rowHeight * 2.5f);
    REQUIRE_EQ(remove->Text(), std::wstring(L"删除选中 (2)"));
    REQUIRE(!remove->IsDisabled());

    const msimeui::RectF area = remove->GetBounds();
    window.Click(area.x + area.width * 0.5f, area.y + area.height * 0.5f);
    REQUIRE_EQ(harness.Host().confirmations.size(), size_t{1});
    REQUIRE(harness.Host().confirmations.front().find(L"2 条词条") != std::wstring::npos);

    const auto queries = harness.Queries();
    REQUIRE_EQ(queries.size(), size_t{2});
    const Json &request = queries.at(1);
    REQUIRE_EQ(request.at("action").get<std::string>(), std::string("deleteMany"));
    REQUIRE_EQ(request.at("dictionary").get<std::string>(), std::string("quanpin"));
    const auto &entries = request.at("entries");
    REQUIRE_EQ(entries.size(), size_t{2});
    REQUIRE_EQ(entries.at(0).at("word").get<std::string>(), std::string("词0"));
    REQUIRE_EQ(entries.at(1).at("word").get<std::string>(), std::string("词2"));
    // 编码是后端定位词条的键（拼音表按 key+value 删，不是按词面删），必须原样带上。
    REQUIRE_EQ(entries.at(0).at("code").get<std::string>(), std::string("ci0"));
    REQUIRE_EQ(entries.at(1).at("code").get<std::string>(), std::string("ci2"));

    // 删完之后列表会重查，勾选不该跟着旧行号留下来：新的一页从干净状态开始。
    harness.Respond(request, Json{{"ok", true}, {"message", "已删除 2 条词条"}});
    REQUIRE_EQ(harness.Queries().size(), size_t{3});
    harness.Respond(harness.Queries().at(2), QueryPage(0, 1, false, 1));
    const auto refreshed = Visible<native_settings::DictTableBody>(window, 1600.0f);
    REQUIRE_EQ(refreshed.size(), size_t{1});
    REQUIRE((*refreshed.begin())->Selection().empty());
    REQUIRE(remove->IsDisabled());
    REQUIRE_EQ(remove->Text(), std::wstring(L"删除选中"));
}

// 请求发出到响应回来这段时间里，按钮和表头全选框都得按住：再点一次会对同一批发第二个
// deleteMany，第二批注定全失败，用户还得自己收拾那条报错。
TEST_CASE(DictionaryDeleteIgnoresClicksWhileTheRequestIsInFlight)
{
    PanelHarness harness(DictionaryPanel::Kind::Dictionary);
    harness.Build();
    harness.Show();
    harness.Respond(harness.Queries().front(), QueryPage(0, 3, false, 3));

    SettingsWindowHarness &window = harness.Window();
    window.Layout();
    const auto bodies = Visible<native_settings::DictTableBody>(window, 1600.0f);
    REQUIRE_EQ(bodies.size(), size_t{1});
    native_settings::DictTableBody &body = **bodies.begin();
    const msimeui::RectF bounds = body.GetBounds();
    const float rowHeight = bounds.height / 3.0f;
    window.Click(bounds.x + 20.0f, bounds.y + rowHeight * 0.5f);

    native_settings::Button *remove = FindToolbarButton(window, L"删除选中");
    REQUIRE(remove != nullptr);
    REQUIRE(!remove->IsDisabled());
    const msimeui::RectF area = remove->GetBounds();
    window.Click(area.x + area.width * 0.5f, area.y + area.height * 0.5f);

    const auto queries = harness.Queries();
    REQUIRE_EQ(queries.size(), size_t{2});
    REQUIRE_EQ(queries.at(1).at("action").get<std::string>(), std::string("deleteMany"));
    REQUIRE(remove->IsDisabled());

    // 再点按钮、再点全选框，都不该多发一条，也不该改掉已经发出去的那批勾选。
    window.Click(area.x + area.width * 0.5f, area.y + area.height * 0.5f);
    const auto headers = Visible<native_settings::DictTableHeader>(window, 1600.0f);
    REQUIRE_EQ(headers.size(), size_t{1});
    const msimeui::RectF header = (*headers.begin())->GetBounds();
    window.Click(header.x + 20.0f, header.y + header.height * 0.5f);
    REQUIRE_EQ(harness.Queries().size(), size_t{2});
    REQUIRE_EQ(body.Selection().size(), size_t{1});

    // 响应回来才解禁：这一批只有一条，重查之后勾选清空、按钮重新变灰。
    harness.Respond(queries.at(1), Json{{"ok", true}, {"failed", 0}, {"message", "已删除 1 条词条"}});
    REQUIRE_EQ(harness.Queries().size(), size_t{3});
    REQUIRE(remove->IsDisabled());
    REQUIRE(!harness.Host().toasts.empty());
    REQUIRE(harness.Host().toasts.back().second);
}

// 一部分删不掉时，后端仍然算「成功」（删掉的要留下、列表要刷新），但提示不能是绿的：
// 绿勾会让用户以为整批都删了。
TEST_CASE(DictionaryPartialDeleteShowsAWarningToast)
{
    PanelHarness harness(DictionaryPanel::Kind::Dictionary);
    harness.Build();
    harness.Show();
    harness.Respond(harness.Queries().front(), QueryPage(0, 3, false, 3));

    SettingsWindowHarness &window = harness.Window();
    window.Layout();
    const auto bodies = Visible<native_settings::DictTableBody>(window, 1600.0f);
    REQUIRE_EQ(bodies.size(), size_t{1});
    native_settings::DictTableBody &body = **bodies.begin();
    const msimeui::RectF bounds = body.GetBounds();
    const float rowHeight = bounds.height / 3.0f;
    window.Click(bounds.x + 20.0f, bounds.y + rowHeight * 0.5f);

    native_settings::Button *remove = FindToolbarButton(window, L"删除选中");
    REQUIRE(remove != nullptr);
    const msimeui::RectF area = remove->GetBounds();
    window.Click(area.x + area.width * 0.5f, area.y + area.height * 0.5f);

    const auto queries = harness.Queries();
    REQUIRE_EQ(queries.size(), size_t{2});
    harness.Respond(
        queries.at(1),
        Json{{"ok", true}, {"failed", 1}, {"message", "已删除 0 条词条，1 条未删除：原词条不存在或已被删除"}});

    REQUIRE(!harness.Host().toasts.empty());
    REQUIRE(!harness.Host().toasts.back().second);
    REQUIRE(harness.Host().toasts.back().first.find(L"1 条未删除") != std::wstring::npos);
    // 列表照样刷新，删掉的那部分用户能立刻看到。
    REQUIRE_EQ(harness.Queries().size(), size_t{3});
}

// 换词库前发出的批量删除，响应回来时已经不在那一页上了：不能拿旧批次的「删完清勾选」
// 去抹掉新页面上用户刚勾的行。
TEST_CASE(DictionaryStaleDeleteResponseKeepsTheNewSelection)
{
    PanelHarness harness(DictionaryPanel::Kind::Dictionary);
    harness.Build();
    harness.Show();
    harness.Respond(harness.Queries().front(), QueryPage(0, 3, false, 3));

    SettingsWindowHarness &window = harness.Window();
    window.Layout();
    const auto bodies = Visible<native_settings::DictTableBody>(window, 1600.0f);
    REQUIRE_EQ(bodies.size(), size_t{1});
    native_settings::DictTableBody &body = **bodies.begin();
    const msimeui::RectF bounds = body.GetBounds();
    window.Click(bounds.x + 20.0f, bounds.y + bounds.height / 3.0f * 0.5f);

    native_settings::Button *remove = FindToolbarButton(window, L"删除选中");
    REQUIRE(remove != nullptr);
    const msimeui::RectF area = remove->GetBounds();
    window.Click(area.x + area.width * 0.5f, area.y + area.height * 0.5f);
    const auto queries = harness.Queries();
    REQUIRE_EQ(queries.size(), size_t{2});
    const Json stale = queries.at(1);

    // 请求还在路上就换了词库：新页面自己会重查一次。
    harness.Panel().SetDictionary("english");
    REQUIRE_EQ(harness.Queries().size(), size_t{3});
    harness.Respond(harness.Queries().at(2), QueryPage(0, 2, false, 2));
    window.Layout();

    const auto refreshed = Visible<native_settings::DictTableBody>(window, 1600.0f);
    REQUIRE_EQ(refreshed.size(), size_t{1});
    native_settings::DictTableBody &english = **refreshed.begin();
    const msimeui::RectF englishBounds = english.GetBounds();
    window.Click(englishBounds.x + 20.0f, englishBounds.y + englishBounds.height / 2.0f * 0.5f);
    REQUIRE_EQ(english.Selection().size(), size_t{1});

    // 旧的响应现在才回来：不属于当前词库，勾选不能跟着清。
    harness.Respond(stale, Json{{"ok", true}, {"failed", 0}, {"message", "已删除 1 条词条"}});
    REQUIRE_EQ(english.Selection().size(), size_t{1});
    // 提示仍然要带词库名前缀，让用户知道这条说的是哪个词库。
    REQUIRE(!harness.Host().toasts.empty());
    REQUIRE(harness.Host().toasts.back().first.find(L"：") != std::wstring::npos);
}

// 窄表把操作列压到只剩勾选列之后那点余量时，「编辑」「删除」也必须留在表里：
// 越界的那截会被滚动区裁掉，看得见一半、点不到另一半。
TEST_CASE(DictionaryActionButtonsStayInsideANarrowTable)
{
    for (const float width : {340.0f, 240.0f, 200.0f})
    {
        PanelHarness harness(DictionaryPanel::Kind::Dictionary, width);
        harness.Build();
        harness.Show();
        harness.Respond(harness.Queries().front(), QueryPage(0, 2, false, 2));
        harness.Window().Layout();
        const auto bodies = Visible<native_settings::DictTableBody>(harness.Window(), 1600.0f);
        REQUIRE_EQ(bodies.size(), size_t{1});
        native_settings::DictTableBody &body = **bodies.begin();
        const msimeui::RectF bounds = body.GetBounds();
        for (size_t row = 0; row < 2; ++row)
        {
            for (const bool remove : {false, true})
            {
                const msimeui::RectF rect = body.ActionRect(row, remove);
                REQUIRE(rect.x >= bounds.x - 0.01f);
                REQUIRE(rect.x + rect.width <= bounds.x + bounds.width + 0.01f);
            }
        }
        // 再直接摆一张 120 DIP 宽的窄表：操作列只剩 80 DIP，两枚按钮必须一起挤进表内。
        body.Arrange({bounds.x, bounds.y, 120.0f, bounds.height});
        for (const bool remove : {false, true})
        {
            const msimeui::RectF rect = body.ActionRect(0, remove);
            REQUIRE(rect.x >= bounds.x - 0.01f);
            REQUIRE(rect.x + rect.width <= bounds.x + 120.0f + 0.01f);
        }
    }
}

// 表头的全选框勾上当前页全部行，再点一次全取消；勾选本身不产生请求。
TEST_CASE(DictionaryHeaderSelectsEveryRowOnThePage)
{
    PanelHarness harness(DictionaryPanel::Kind::Dictionary);
    harness.Build();
    harness.Show();
    harness.Respond(harness.Queries().front(), QueryPage(0, 3, false, 3));

    SettingsWindowHarness &window = harness.Window();
    const auto headers = Visible<native_settings::DictTableHeader>(window, 1600.0f);
    REQUIRE_EQ(headers.size(), size_t{1});
    const msimeui::RectF header = (*headers.begin())->GetBounds();
    const float x = header.x + 20.0f;
    const float y = header.y + header.height * 0.5f;

    window.Click(x, y);
    native_settings::Button *remove = FindToolbarButton(window, L"删除选中");
    REQUIRE(remove != nullptr);
    REQUIRE_EQ(remove->Text(), std::wstring(L"删除选中 (3)"));

    window.Click(x, y);
    REQUIRE(remove->IsDisabled());
    REQUIRE_EQ(remove->Text(), std::wstring(L"删除选中"));
    REQUIRE_EQ(harness.Queries().size(), size_t{1});
}

// 设置窗口能缩到 600 DIP 宽，表格过窄时左右两侧的固定列（勾选、序号、操作）不能被挤出右边界：
// 被挤出去的部分会被滚动区裁掉，删除链接既看不见也点不到。百分比列先让出空间，
// 让不动了就整列收起——留下的文字列必须还放得下内容，不能压成 0 宽的空白列。
TEST_CASE(DictionaryColumnsKeepTheActionColumnInsideANarrowTable)
{
    native_settings::DictColumns columns;
    for (const float width : {900.0f, 650.0f, 590.0f, 518.0f, 458.0f, 380.0f, 300.0f, 240.0f})
    {
        const auto edges = columns.Edges(width);
        REQUIRE(edges[1] >= native_settings::kDictCheckColumn - 0.01f);
        REQUIRE(edges[2] >= edges[1]);
        REQUIRE(edges[3] >= edges[2]);
        REQUIRE(edges[4] >= edges[3]);
        REQUIRE(edges[5] >= edges[4]);
        // 操作列固定 124px，它的右边界必须留在表内（正常宽度下还要加上剩余空白）。
        REQUIRE(edges[5] + native_settings::kDictActionWidth <= width + 0.01f);

        float widest = 0.0f;
        for (size_t i = 0; i < 3; ++i)
        {
            const float cell = edges[i + 3] - edges[i + 2];
            REQUIRE(cell >= 0.0f);
            widest = (std::max)(widest, cell);
        }
        // 至少有一列文字撑得下 76px 内容和左右各 18px 的内边距。
        REQUIRE(widest >= native_settings::kDictMinTextColumn);
    }

    // 极端窄表（比勾选列加操作列还窄）：操作列只能跟着缩，绝不能伸到表外去。
    for (const float width : {200.0f, 164.0f, 160.0f, 100.0f})
    {
        const auto edges = columns.Edges(width);
        for (size_t i = 1; i < edges.size(); ++i)
            REQUIRE(edges[i] >= edges[i - 1] - 0.01f);
        REQUIRE(edges[6] <= width + 0.01f);
    }
}

// 收起顺序是有语义的：词条列最能说明一行是什么，留到最后；权重最先走，其次是编码。
// 只断言「有一列够宽」抓不到顺序被改反，这里逐宽度锁住留下的是哪几列。
TEST_CASE(DictionaryColumnsDropWeightThenCodeThenIndex)
{
    native_settings::DictColumns columns;
    // {序号, 编码, 词条, 权重}
    const std::array<bool, 4> all = {true, true, true, true};
    const std::array<bool, 4> withoutWeight = {true, true, true, false};
    const std::array<bool, 4> onlyWord = {false, false, true, false};

    const auto roomy = columns.VisibleColumns(900.0f);
    REQUIRE(roomy == all);

    // 权重先让位，编码和词条都还在。
    const auto medium = columns.VisibleColumns(458.0f);
    REQUIRE(medium == withoutWeight);

    // 再窄下去连编码也走，只剩词条；序号在最后一步才走。
    const auto narrow = columns.VisibleColumns(300.0f);
    REQUIRE(narrow == onlyWord);
    const auto narrowEdges = columns.Edges(300.0f);
    REQUIRE(narrowEdges[4] - narrowEdges[3] >= native_settings::kDictMinTextColumn);

    // 可见列只会随宽度收窄越来越少：一个宽度下已经收起的列，更窄时不可能再回来。
    std::array<bool, 4> previous = roomy;
    for (const float width : {900.0f, 700.0f, 600.0f, 520.0f, 460.0f, 400.0f, 340.0f, 280.0f, 240.0f})
    {
        const auto current = columns.VisibleColumns(width);
        for (size_t i = 0; i < current.size(); ++i)
            REQUIRE(!current[i] || previous[i]);
        previous = current;
        // 词条列永远留着：全空白的表没人看得懂自己在删什么。
        REQUIRE(current[2]);
    }
}

// 表体和表头是两块分开的视觉：表格出现滚动条时表体让出 8px，表头必须跟着让，
// 否则同一列的起点差半个滚动条，勾选框和文字都会错位。
TEST_CASE(DictionaryHeaderReservesTheScrollbarTheBodyGives)
{
    {
        PanelHarness harness(DictionaryPanel::Kind::Dictionary);
        harness.Build();
        harness.Show();
        // 三行时表体比可视高度矮，滚动条不出现，表头也不用让。
        harness.Respond(harness.Queries().front(), QueryPage(0, 3, false, 3));
        SettingsWindowHarness &window = harness.Window();
        window.Layout();
        const auto headers = Visible<native_settings::DictTableHeader>(window, 1600.0f);
        REQUIRE_EQ(headers.size(), size_t{1});
        REQUIRE_EQ((*headers.begin())->ScrollbarReserve(), 0.0f);
    }

    {
        PanelHarness harness(DictionaryPanel::Kind::Dictionary);
        harness.Build();
        harness.Show();
        // 行多到超出最大高度，表体开始滚动，表头要占住同样的 8px。
        harness.Respond(harness.Queries().front(), QueryPage(0, 12, false, 12));
        SettingsWindowHarness &window = harness.Window();
        window.Layout();
        const auto headers = Visible<native_settings::DictTableHeader>(window, 1600.0f);
        REQUIRE_EQ(headers.size(), size_t{1});
        REQUIRE_EQ((*headers.begin())->ScrollbarReserve(), 8.0f);
    }
}

// 设置窗口最小 600 DIP，扣掉侧栏和两层留白后卡片只剩 300 出头：工具栏里五个控件排不进一行，
// 而 Row 不裁剪，末尾按钮会越过卡片边框。工具栏因此分成两行，操作那一行还能继续换行。
TEST_CASE(DictionaryToolbarFitsTheNarrowestSettingsWindow)
{
    for (const auto kind : {DictionaryPanel::Kind::Dictionary, DictionaryPanel::Kind::Quick})
    {
        PanelHarness harness(kind, 340.0f);
        harness.Build();
        harness.Show();
        harness.Respond(harness.Queries().front(), QueryPage(0, 2, false, 2));

        SettingsWindowHarness &window = harness.Window();
        window.Layout();
        const msimeui::RectF root = harness.Root().GetBounds();

        // 搜索框也是被挤扁的那个：它必须还留得下可输入的宽度。
        const auto inputs = Visible<native_settings::DictInput>(window, 400.0f);
        REQUIRE_EQ(inputs.size(), size_t{1});
        REQUIRE((*inputs.begin())->GetBounds().width > 100.0f);

        const std::vector<std::wstring> labels = {L"查询", L"批量导入", L"删除选中",
                                                  kind == DictionaryPanel::Kind::Quick ? L"新增短语" : L"新增词条"};
        const auto buttons = Visible<native_settings::Button>(window, 400.0f);
        for (const std::wstring &label : labels)
        {
            bool found = false;
            for (native_settings::Button *button : buttons)
            {
                if (button->Text() != label)
                    continue;
                found = true;
                const msimeui::RectF bounds = button->GetBounds();
                // 每个按钮都要有可见宽度，且右缘留在页面内。
                REQUIRE(bounds.width > 20.0f);
                REQUIRE(bounds.x + bounds.width <= root.x + root.width + 0.01f);
            }
            REQUIRE(found);
        }
    }
}

// 英文词库的行没有编码（后端查询只产出 word/display），删除靠「单词 + 显示内容」定位，
// 所以请求里的每一条都要带上 display，显示内容为空时用单词本身。
TEST_CASE(EnglishRowsCarryTheDisplayInABatchDelete)
{
    PanelHarness harness(DictionaryPanel::Kind::Dictionary);
    harness.Build();
    harness.Show();
    harness.Panel().SetDictionary("english");
    const auto queries = harness.Queries();
    REQUIRE_EQ(queries.size(), size_t{2});
    harness.Respond(queries.at(1),
                    Json{{"ok", true},
                         {"message", "查询成功"},
                         {"rows", Json::array({{{"word", "hello"}, {"display", "哈喽"}, {"weight", 10}},
                                               {{"word", "world"}, {"display", "世界"}, {"weight", 10}}})},
                         {"offset", 0},
                         {"hasMore", false}});

    SettingsWindowHarness &window = harness.Window();
    window.Layout();
    const auto headers = Visible<native_settings::DictTableHeader>(window, 1600.0f);
    REQUIRE_EQ(headers.size(), size_t{1});
    const msimeui::RectF header = (*headers.begin())->GetBounds();
    window.Click(header.x + 20.0f, header.y + header.height * 0.5f);

    native_settings::Button *remove = FindToolbarButton(window, L"删除选中");
    REQUIRE(remove != nullptr);
    REQUIRE_EQ(remove->Text(), std::wstring(L"删除选中 (2)"));
    const msimeui::RectF area = remove->GetBounds();
    window.Click(area.x + area.width * 0.5f, area.y + area.height * 0.5f);

    const auto sent = harness.Queries();
    REQUIRE_EQ(sent.size(), size_t{3});
    const Json &request = sent.at(2);
    REQUIRE_EQ(request.at("dictionary").get<std::string>(), std::string("english"));
    const auto &entries = request.at("entries");
    REQUIRE_EQ(entries.size(), size_t{2});
    REQUIRE_EQ(entries.at(0).at("word").get<std::string>(), std::string("hello"));
    REQUIRE_EQ(entries.at(0).at("display").get<std::string>(), std::string("哈喽"));
    REQUIRE_EQ(entries.at(1).at("display").get<std::string>(), std::string("世界"));
}

// 整批都失败时后端回 ok=false：勾选要原地留着，让用户能直接重试，而不是清空重选一遍。
TEST_CASE(FailedBatchDeleteKeepsTheSelection)
{
    PanelHarness harness(DictionaryPanel::Kind::Dictionary);
    harness.Build();
    harness.Show();
    harness.Respond(harness.Queries().front(), QueryPage(0, 2, false, 2));

    SettingsWindowHarness &window = harness.Window();
    window.Layout();
    const auto bodies = Visible<native_settings::DictTableBody>(window, 1600.0f);
    REQUIRE_EQ(bodies.size(), size_t{1});
    const msimeui::RectF body = (*bodies.begin())->GetBounds();
    window.Click(body.x + 20.0f, body.y + body.height * 0.25f);

    native_settings::Button *remove = FindToolbarButton(window, L"删除选中");
    REQUIRE(remove != nullptr);
    const msimeui::RectF area = remove->GetBounds();
    window.Click(area.x + area.width * 0.5f, area.y + area.height * 0.5f);

    const auto queries = harness.Queries();
    REQUIRE_EQ(queries.size(), size_t{2});
    harness.Respond(queries.at(1), Json{{"ok", false}, {"message", "原词条不存在或已被删除"}});

    const auto after = Visible<native_settings::DictTableBody>(window, 1600.0f);
    REQUIRE_EQ(after.size(), size_t{1});
    REQUIRE_EQ((*after.begin())->Selection().size(), size_t{1});
    REQUIRE(!remove->IsDisabled());
    REQUIRE_EQ(remove->Text(), std::wstring(L"删除选中 (1)"));
    REQUIRE_EQ(harness.Host().toasts.size(), size_t{1});
    REQUIRE(!harness.Host().toasts.front().second);
}
