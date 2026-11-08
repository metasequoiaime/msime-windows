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

    void Respond(const Json &request, Json fields)
    {
        Json message = {{"type", "dictionaryResponse"},
                        {"protocolVersion", 1},
                        {"requestId", request.at("requestId")},
                        {"action", request.at("action")},
                        {"dictionary", request.at("dictionary")}};
        message.update(fields);
        model_.Receive(message.dump());
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

Json QueryPage(size_t offset, size_t rows, bool has_more)
{
    Json rows_json = Json::array();
    for (size_t i = 0; i < rows; ++i)
        rows_json.push_back({{"code", "ci" + std::to_string(i)}, {"word", "词" + std::to_string(i)}, {"weight", 10}});
    return {{"ok", true},
            {"message", "查询成功"},
            {"rows", std::move(rows_json)},
            {"offset", offset},
            {"hasMore", has_more}};
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

// 删掉最后一页仅剩的条目后，那一页会查成空页；停在空页上会误报「还没有添加过词条」。
TEST_CASE(AnEmptiedPageStepsBackTowardsTheFirstPage)
{
    PanelHarness harness(DictionaryPanel::Kind::Dictionary);
    harness.Build();
    harness.Show();
    REQUIRE_EQ(harness.Queries().size(), size_t{1});

    harness.Respond(harness.Queries().front(), QueryPage(200, 0, false));
    const auto queries = harness.Queries();
    REQUIRE_EQ(queries.size(), size_t{2});
    REQUIRE_EQ(queries.at(1).at("offset").get<size_t>(), size_t{100});
}
