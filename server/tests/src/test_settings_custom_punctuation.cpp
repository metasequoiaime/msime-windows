// 输入页「自定义标点」：符号目录、表格内容、以及用真实鼠标与键盘消息操作表格控件。
// 期望值按施工规格手写：触发键 11 个、同一触发键不能两行、第二个下拉框把成对的符号合成一项。
#include "tests/includes/settings_window_harness.h"
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include "settings/native/native_page_custom_punctuation.h"
#include "settings/native/native_settings_model.h"
#include "settings/native/native_settings_pages.h"
#include "settings/native/native_settings_search.h"

#include "msimeui/Controls.h"
#include "msimeui/Layout.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

using native_settings::BuildSymbolOptions;
using native_settings::CustomPunctuationEditor;
using native_settings::CustomPunctuationRows;
using native_settings::DropdownOption;
using native_settings::Json;
using native_settings::LoadPunctuationSymbolCatalog;
using native_settings::SymbolLabel;

namespace
{
// 键盘上的 32 个半角标点。
const std::wstring kAsciiPunctuation = L"!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";

// ---- 符号目录 ----

class CatalogFile
{
  public:
    explicit CatalogFile(const wchar_t *name)
        : path_(test::TempRoot() /
                (std::wstring(L"msime-符号目录-") + name + L"-" + std::to_wstring(GetCurrentProcessId()) + L".db"))
    {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        REQUIRE(sqlite3_open(test::Utf8(path_).c_str(), &db_) == SQLITE_OK);
        Exec("CREATE TABLE symbol_catalog(symbol TEXT, category TEXT, parent_category TEXT, sort_order INTEGER, "
             "keywords TEXT)");
    }
    ~CatalogFile()
    {
        if (db_)
            sqlite3_close(db_);
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    void Add(const char *symbol, const char *parent, int order)
    {
        sqlite3_stmt *stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(db_, "INSERT INTO symbol_catalog VALUES(?, 'x', ?, ?, '')", -1, &stmt, nullptr) ==
                SQLITE_OK);
        sqlite3_bind_text(stmt, 1, symbol, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, parent, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, order);
        REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }

    // 关掉连接，让被测函数自己打开。
    const std::filesystem::path &Close()
    {
        sqlite3_close(db_);
        db_ = nullptr;
        return path_;
    }

  private:
    void Exec(const char *sql)
    {
        REQUIRE(sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    }

    std::filesystem::path path_;
    sqlite3 *db_ = nullptr;
};

// 成对表 29 对的左右两半，按规格第 13 条的顺序。
const std::wstring kPairHalves =
    L"{}「」『』“”‘’（）《》〈〉【】〖〗〔〕［］｛｝«»‹›⟨⟩〘〙〚〛〝〞﹁﹂﹃﹄︻︼︗︘︵︶︷︸︹︺︿﹀︽︾﹇﹈";
// 默认中文标点映射能输出的符号：23 项简单表，再是交替对、嵌套对与顿号。
const std::vector<std::wstring> kDefaultOutputs = {
    L"·", L"~",  L"！", L"@",  L"#",  L"￥", L"%",  L"……", L"&", L"*", L"（", L"）", L"——", L"【", L"】", L"{",
    L"}", L"、", L"；", L"：", L"，", L"。", L"？", L"“",  L"”", L"‘", L"’",  L"《", L"〈", L"〉", L"》",
};

void AddMissing(std::vector<std::wstring> &symbols, const std::wstring &symbol)
{
    if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end())
        symbols.push_back(symbol);
}

// 符号目录查到的 symbols，再接上不随词库变的三样：成对表的两半、默认映射的输出、32 个半角标点。
std::vector<std::wstring> WithBuiltIns(std::vector<std::wstring> symbols)
{
    for (const wchar_t ch : kPairHalves)
        AddMissing(symbols, std::wstring(1, ch));
    for (const auto &output : kDefaultOutputs)
        AddMissing(symbols, output);
    for (const wchar_t ch : kAsciiPunctuation)
        AddMissing(symbols, std::wstring(1, ch));
    return symbols;
}

// 三样去重后的个数：成对表 58 个，默认映射另有 17 个，半角标点另有 24 个。
constexpr size_t kBuiltInCount = 58 + 17 + 24;

std::vector<std::wstring> WithAscii(std::vector<std::wstring> symbols)
{
    for (const wchar_t ch : kAsciiPunctuation)
    {
        if (std::find(symbols.begin(), symbols.end(), std::wstring(1, ch)) == symbols.end())
            symbols.emplace_back(1, ch);
    }
    return symbols;
}
} // namespace

TEST_CASE(custom_punctuation_catalog_prefers_the_chinese_category)
{
    CatalogFile file(L"chinese");
    file.Add("」", "标点", 3);
    file.Add("「", "标点", 1);
    file.Add("「", "标点", 2);
    file.Add("-", "标点", 4);
    file.Add("※", "Punctuation", 0);
    file.Add("→", "箭头与图形", 0);
    // 超过 7 个 UTF-16 单元的不列。
    file.Add("12345678", "标点", 5);
    const auto catalog = LoadPunctuationSymbolCatalog(file.Close());
    // 按 sort_order，去重，再接上 32 个半角标点（- 已经有了不重复）。
    REQUIRE((catalog == WithBuiltIns({L"「", L"」", L"-"})));
    REQUIRE(catalog.size() == kBuiltInCount);
}

TEST_CASE(custom_punctuation_catalog_falls_back_to_the_english_category)
{
    CatalogFile file(L"english");
    file.Add("‹", "Punctuation", 2);
    file.Add("«", "Punctuation", 1);
    file.Add("🙆‍♂️", "Punctuation", 3);
    file.Add("→", "Arrows and lines", 0);
    const auto catalog = LoadPunctuationSymbolCatalog(file.Close());
    REQUIRE((catalog == WithBuiltIns({L"«", L"‹", L"🙆‍♂️"})));
}

TEST_CASE(custom_punctuation_catalog_without_either_category_lists_the_built_in_symbols)
{
    CatalogFile file(L"none");
    file.Add("→", "Arrows and lines", 0);
    const auto catalog = LoadPunctuationSymbolCatalog(file.Close());
    REQUIRE(catalog == WithBuiltIns({}));
    REQUIRE(catalog.size() == kBuiltInCount);
    // 库不存在时也一样。
    REQUIRE(LoadPunctuationSymbolCatalog(test::TempRoot() / L"msime-不存在的目录.db") == WithBuiltIns({}));
}

TEST_CASE(custom_punctuation_symbol_options_merge_pairs)
{
    const std::vector<std::wstring> catalog = WithAscii({L"「", L"」", L"【", L"】", L"。", L"‹"});
    const auto options = BuildSymbolOptions(catalog);
    std::set<std::wstring> labels;
    for (const auto &option : options)
        labels.insert(option.label);
    // 成对表里的 「」 【】 {} 合成一项，每对少一项；() [] <> 不在成对表里，照旧单列。
    // ‹ 的右半边 › 不在目录里，‹ 照样显示成对。
    REQUIRE(labels.count(L"「」") == 1);
    REQUIRE(labels.count(L"【】") == 1);
    REQUIRE(labels.count(L"{}") == 1);
    REQUIRE(labels.count(L"‹›") == 1);
    REQUIRE(labels.count(L"「") == 0);
    REQUIRE(labels.count(L"」") == 0);
    REQUIRE(labels.count(L"】") == 0);
    REQUIRE(labels.count(L"}") == 0);
    REQUIRE(labels.count(L"。") == 1);
    REQUIRE(labels.count(L"(") == 1);
    REQUIRE(labels.count(L")") == 1);
    REQUIRE(options.size() == catalog.size() - 3);
    for (const auto &option : options)
    {
        if (option.label == L"「」")
            REQUIRE(option.value == "「");
        else if (option.label == L"。")
            REQUIRE(option.value == "。");
    }

    REQUIRE(SymbolLabel("「") == L"「」");
    REQUIRE(SymbolLabel("」") == L"」");
    REQUIRE(SymbolLabel("——") == L"——");
    REQUIRE(SymbolLabel("。") == L"。");
}

TEST_CASE(custom_punctuation_rows_refuse_a_second_row_with_the_same_trigger)
{
    CustomPunctuationRows rows;
    rows.Load({"Shift+BracketLeft=「", "bogus", "Shift+BracketLeft=『", "Equal=="});
    REQUIRE(rows.Rows().size() == 2);
    REQUIRE(rows.Entries() == (std::vector<std::string>{"Shift+BracketLeft=「", "Equal=="}));

    rows.Add();
    REQUIRE(rows.Rows().size() == 3);
    // 新行没选键：不写进配置。
    REQUIRE(rows.Entries().size() == 2);
    // 留空 + [ 与 Shift + [ 不冲突。
    REQUIRE(!rows.SetKey(2, '[').has_value());
    // 录上 Shift 会和第 0 行相同：不改。
    const auto conflict = rows.SetShift(2, true);
    REQUIRE(conflict.has_value() && *conflict == 0);
    REQUIRE(!rows.Rows()[2].trigger.shift);
    // 选到 = 会和第 1 行相同：不改。
    const auto keyConflict = rows.SetKey(2, '=');
    REQUIRE(keyConflict.has_value() && *keyConflict == 1);
    REQUIRE(rows.Rows()[2].trigger.key == '[');
    rows.SetSymbol(2, "〔");
    REQUIRE(rows.Entries() == (std::vector<std::string>{"Shift+BracketLeft=「", "Equal==", "BracketLeft=〔"}));
    // 删掉第 0 行之后，第 2 行（现在是第 1 行）可以录 Shift。
    rows.Remove(0);
    REQUIRE(!rows.SetShift(1, true).has_value());
    REQUIRE(rows.Entries() == (std::vector<std::string>{"Equal==", "Shift+BracketLeft=〔"}));
    // 还没选键的两行不算冲突。
    rows.Add();
    rows.Add();
    REQUIRE(!rows.SetShift(2, true).has_value());
    REQUIRE(!rows.SetShift(3, true).has_value());
}

TEST_CASE(custom_punctuation_rows_hold_at_most_twenty_two)
{
    CustomPunctuationRows rows;
    for (int i = 0; i < 22; ++i)
    {
        REQUIRE(rows.CanAdd());
        rows.Add();
    }
    REQUIRE(!rows.CanAdd());
    rows.Add();
    REQUIRE(rows.Rows().size() == 22);
}

// ---- 表格控件 ----

namespace
{
class NullHost : public native_settings::HostServices
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
    void ShowModal(native_settings::VisualPtr, std::function<void()>) override
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

constexpr float kRowPitch = 30.0f;
constexpr float kRow = 28.0f;
constexpr float kInset = 2.0f;
// 第一个下拉框的选项顺序：` - = [ ] \ ; ' , . /。
constexpr size_t kKeyEqual = 2;
constexpr size_t kKeyBracketLeft = 3;
constexpr size_t kKeyBracketRight = 4;

// 页面会一直持有 PageContext，所以场景不析构。
class EditorScene
{
  public:
    static EditorScene &Create()
    {
        return *new EditorScene([]() { return WithAscii({L"「", L"」", L"『", L"』", L"〔", L"〕", L"。"}); });
    }

    // catalog 是第二个下拉框读的符号目录。
    static EditorScene &Create(std::function<std::vector<std::wstring>()> catalog)
    {
        return *new EditorScene(std::move(catalog));
    }

    // 窗口按 dpi 缩放（96 是 100%），页面宽 width。
    static EditorScene &CreateAt(float dpi, float width = 800.0f)
    {
        return *new EditorScene([]() { return WithAscii({L"「", L"」", L"〔", L"〕", L"。"}); }, dpi, width);
    }

    void Snapshot(bool enabled, const std::vector<std::string> &entries, bool paired = true)
    {
        Json map = Json::array();
        for (const auto &entry : entries)
            map.push_back(entry);
        model.Receive(Json{
            {"type", "configSnapshot"},
            {"data",
             {{"input",
               {{"custom_punctuation", enabled}, {"custom_punctuation_map", map}, {"paired_punctuation", paired}}}}}}
                          .dump());
        harness.Layout();
    }

    void ClickCentre(const std::shared_ptr<msimeui::Visual> &visual)
    {
        const msimeui::RectF r = visual->GetBounds();
        harness.Click(r.x + r.width * 0.5f, r.y + r.height * 0.5f);
    }

    // 打开下拉框，点第 index 项（在可见的前六项里）。
    void PickOption(const std::shared_ptr<native_settings::Dropdown> &dropdown, size_t index)
    {
        REQUIRE(index < 6);
        const msimeui::RectF toggle = dropdown->GetBounds();
        harness.Click(toggle.x + 20.0f, toggle.y + toggle.height * 0.5f);
        msimeui::Visual *menu = harness.VisualAt(toggle.x + toggle.width * 0.5f, toggle.y + toggle.height + 20.0f);
        REQUIRE(menu != nullptr && menu != dropdown.get());
        const msimeui::RectF bounds = menu->GetBounds();
        harness.Click(bounds.x + bounds.width * 0.5f,
                      bounds.y + kInset + static_cast<float>(index) * kRowPitch + kRow * 0.5f);
    }

    // 在可搜索的下拉框里输入 text，点过滤后的第一项。
    void SearchAndPick(const std::shared_ptr<native_settings::Dropdown> &dropdown, const std::wstring &text)
    {
        const msimeui::RectF toggle = dropdown->GetBounds();
        harness.Click(toggle.x + 20.0f, toggle.y + toggle.height * 0.5f);
        harness.GiveWindowKeyboardFocus();
        for (const wchar_t ch : text)
            harness.Type(ch);
        msimeui::Visual *menu = harness.VisualAt(toggle.x + toggle.width * 0.5f, toggle.y + toggle.height + 20.0f);
        REQUIRE(menu != nullptr && menu != dropdown.get());
        const msimeui::RectF bounds = menu->GetBounds();
        harness.Click(bounds.x + bounds.width * 0.5f, bounds.y + kInset + kRow * 0.5f);
    }

    // 写进配置的映射，按发出顺序。
    std::vector<std::vector<std::string>> MapUpdates() const
    {
        std::vector<std::vector<std::string>> updates;
        for (const std::string &raw : sent)
        {
            const Json message = Json::parse(raw, nullptr, false);
            if (!message.is_object() || message.value("type", std::string()) != "configUpdate")
                continue;
            const Json &data = message["data"];
            if (data.value("path", std::string()) != "input.custom_punctuation_map")
                continue;
            // value 是 JSON 数组序列化成的字符串，与 appearance.fallback_fonts 同形。
            REQUIRE(data["value"].is_string());
            const Json array = Json::parse(data["value"].get<std::string>());
            REQUIRE(array.is_array());
            updates.push_back(array.get<std::vector<std::string>>());
        }
        return updates;
    }

    std::vector<Json> SwitchUpdates() const
    {
        std::vector<Json> updates;
        for (const std::string &raw : sent)
        {
            const Json message = Json::parse(raw, nullptr, false);
            if (message.is_object() && message.value("type", std::string()) == "configUpdate" &&
                message["data"].value("path", std::string()) == "input.custom_punctuation")
                updates.push_back(message["data"]["value"]);
        }
        return updates;
    }

    std::wstring SymbolShown(size_t row) const
    {
        const auto dropdown = editor->Controls(row).symbol;
        const size_t index = dropdown->GetSelectedIndex();
        return index < dropdown->GetOptions().size() ? dropdown->GetOptions()[index].label : std::wstring();
    }

    SettingsWindowHarness harness;
    std::vector<std::string> sent;
    native_settings::SettingsModel model;
    NullHost host;
    native_settings::PageContext context;
    std::shared_ptr<CustomPunctuationEditor> editor;

  private:
    explicit EditorScene(std::function<std::vector<std::wstring>()> catalog, float dpi = 96.0f, float width = 800.0f)
        : harness(dpi, {width, 1600.0f}), model([this](const std::string &message) { sent.push_back(message); }),
          host(harness.Window()), context(model, host)
    {
        auto view = native_settings::BuildCustomPunctuationSection(context, std::move(catalog), &editor);
        REQUIRE(editor != nullptr);
        // 页面根直接放这一节，宽度跟着窗口：开关在行的右端，要在窗口里点得到。
        harness.SetRoot(view);
    }
};
} // namespace

TEST_CASE(settings_custom_punctuation_table_hides_while_the_switch_is_off)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(false, {"Equal=="});
    REQUIRE(!scene.editor->Table()->IsVisible());
    // 隐藏的表格不接收点击。
    const msimeui::RectF add = scene.editor->AddButton()->GetBounds();
    REQUIRE(scene.harness.VisualAt(add.x + add.width * 0.5f, add.y + add.height * 0.5f) !=
            scene.editor->AddButton().get());

    scene.Snapshot(true, {"Equal=="});
    REQUIRE(scene.editor->Table()->IsVisible());
    REQUIRE(scene.editor->VisibleRowCount() == 1);

    // 点总开关写的是 input.custom_punctuation。
    scene.ClickCentre(scene.editor->Toggle());
    const auto switches = scene.SwitchUpdates();
    REQUIRE(switches.size() == 1 && switches[0] == Json(false));
    REQUIRE(!scene.editor->Table()->IsVisible());
}

TEST_CASE(settings_custom_punctuation_snapshot_fills_the_rows)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(true, {"Shift+BracketLeft=「", "Equal==", "bogus"});
    REQUIRE(scene.editor->VisibleRowCount() == 2);
    REQUIRE(scene.editor->Rows().Rows()[0].trigger.shift);
    REQUIRE(scene.editor->Controls(0).key->GetValue() == "[");
    REQUIRE(scene.editor->Controls(0).symbol->GetValue() == "「");
    REQUIRE(!scene.editor->Rows().Rows()[1].trigger.shift);
    REQUIRE(scene.editor->Controls(1).key->GetValue() == "=");
    REQUIRE(scene.editor->Controls(1).symbol->GetValue() == "=");
    // 回显不写配置。
    REQUIRE(scene.MapUpdates().empty());

    // 别处改了配置：表格跟着换。
    scene.Snapshot(true, {"BracketRight=」"});
    REQUIRE(scene.editor->VisibleRowCount() == 1);
    REQUIRE(scene.editor->Controls(0).key->GetValue() == "]");
    REQUIRE(scene.MapUpdates().empty());
}

TEST_CASE(settings_custom_punctuation_new_row_is_written_once_complete)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(true, {"Shift+BracketLeft=「"});
    scene.ClickCentre(scene.editor->AddButton());
    REQUIRE(scene.editor->VisibleRowCount() == 2);
    // 新行两个下拉框都是空的。
    REQUIRE(scene.editor->Controls(1).key->GetValue().empty());
    REQUIRE(scene.editor->Controls(1).symbol->GetValue().empty());
    REQUIRE(scene.MapUpdates().empty());

    scene.PickOption(scene.editor->Controls(1).key, kKeyBracketLeft);
    REQUIRE(scene.editor->Controls(1).key->GetValue() == "[");
    // 只选了键：还不写。
    REQUIRE(scene.MapUpdates().empty());

    scene.SearchAndPick(scene.editor->Controls(1).symbol, L"〔");
    const auto updates = scene.MapUpdates();
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0] == (std::vector<std::string>{"Shift+BracketLeft=「", "BracketLeft=〔"}));

    // 快照回显同样的内容：表格不重建，也不再写。
    scene.Snapshot(true, updates[0]);
    REQUIRE(scene.editor->VisibleRowCount() == 2);
    REQUIRE(scene.MapUpdates().size() == 1);
}

TEST_CASE(settings_custom_punctuation_records_only_shift_and_cancels_on_escape)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(true, {"Equal=="});
    const auto controls = scene.editor->Controls(0);
    scene.ClickCentre(controls.record);
    scene.harness.GiveWindowKeyboardFocus();
    REQUIRE(scene.editor->IsRecording(0));
    // 别的键不认，录制继续。
    scene.harness.Key('A');
    scene.harness.Key(VK_CONTROL);
    scene.harness.Key(VK_OEM_4);
    REQUIRE(scene.editor->IsRecording(0));
    REQUIRE(!scene.editor->Rows().Rows()[0].trigger.shift);
    REQUIRE(scene.MapUpdates().empty());
    scene.harness.Key(VK_SHIFT);
    REQUIRE(!scene.editor->IsRecording(0));
    REQUIRE(scene.editor->Rows().Rows()[0].trigger.shift);
    auto updates = scene.MapUpdates();
    REQUIRE(updates.size() == 1 && updates[0] == std::vector<std::string>{"Shift+Equal=="});

    // Esc 取消：录制结束，修饰键不变。
    scene.ClickCentre(controls.record);
    REQUIRE(scene.editor->IsRecording(0));
    scene.harness.Key(VK_ESCAPE);
    REQUIRE(!scene.editor->IsRecording(0));
    REQUIRE(scene.editor->Rows().Rows()[0].trigger.shift);
    REQUIRE(scene.MapUpdates().size() == 1);

    // 清空回到留空。
    scene.ClickCentre(controls.clear);
    REQUIRE(!scene.editor->Rows().Rows()[0].trigger.shift);
    updates = scene.MapUpdates();
    REQUIRE(updates.size() == 2 && updates[1] == std::vector<std::string>{"Equal=="});
}

TEST_CASE(settings_custom_punctuation_duplicate_trigger_is_refused_with_a_message)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(true, {"Shift+BracketLeft=「", "BracketLeft=〔", "BracketRight=〕"});
    REQUIRE(scene.editor->Status().empty());

    // 录到重复：第 1 行录 Shift 会变成 Shift + [，和第 0 行相同。
    scene.ClickCentre(scene.editor->Controls(1).record);
    scene.harness.GiveWindowKeyboardFocus();
    scene.harness.Key(VK_SHIFT);
    REQUIRE(!scene.editor->Status().empty());
    REQUIRE(!scene.editor->Rows().Rows()[1].trigger.shift);
    REQUIRE(scene.MapUpdates().empty());

    // 选到重复：第 2 行改选 [，和第 1 行相同；下拉框退回原来的键。
    scene.PickOption(scene.editor->Controls(2).key, kKeyBracketLeft);
    REQUIRE(!scene.editor->Status().empty());
    REQUIRE(scene.editor->Rows().Rows()[2].trigger.key == ']');
    REQUIRE(scene.editor->Controls(2).key->GetValue() == "]");
    REQUIRE(scene.MapUpdates().empty());

    // 改成不冲突的键：提示消失，写入。
    scene.PickOption(scene.editor->Controls(2).key, kKeyEqual);
    REQUIRE(scene.editor->Status().empty());
    const auto updates = scene.MapUpdates();
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0] == (std::vector<std::string>{"Shift+BracketLeft=「", "BracketLeft=〔", "Equal=〕"}));

    // 新加的空行选到重复的键：退回空。
    scene.ClickCentre(scene.editor->AddButton());
    scene.PickOption(scene.editor->Controls(3).key, kKeyBracketLeft);
    REQUIRE(!scene.editor->Status().empty());
    REQUIRE(scene.editor->Controls(3).key->GetValue().empty());
    REQUIRE(scene.editor->Rows().Rows()[3].trigger.key == 0);
    REQUIRE(scene.MapUpdates().size() == 1);
}

TEST_CASE(settings_custom_punctuation_delete_removes_the_row)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(true, {"Shift+BracketLeft=「", "Shift+BracketRight=」", "Equal=="});
    scene.ClickCentre(scene.editor->Controls(0).remove);
    REQUIRE(scene.editor->VisibleRowCount() == 2);
    REQUIRE(scene.editor->Controls(0).key->GetValue() == "]");
    REQUIRE(scene.editor->Controls(1).key->GetValue() == "=");
    const auto updates = scene.MapUpdates();
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0] == (std::vector<std::string>{"Shift+BracketRight=」", "Equal=="}));

    // 删掉还没写进配置的空行不写配置。
    scene.ClickCentre(scene.editor->AddButton());
    REQUIRE(scene.editor->VisibleRowCount() == 3);
    scene.ClickCentre(scene.editor->Controls(2).remove);
    REQUIRE(scene.editor->VisibleRowCount() == 2);
    REQUIRE(scene.MapUpdates().size() == 1);
}

TEST_CASE(settings_custom_punctuation_add_stops_at_twenty_two_rows)
{
    EditorScene &scene = EditorScene::Create();
    const char *names[] = {"Backquote", "Minus", "Equal", "BracketLeft", "BracketRight", "Backslash",
                           "Semicolon", "Quote", "Comma", "Period",      "Slash"};
    std::vector<std::string> entries;
    for (const char *name : names)
    {
        entries.push_back(std::string(name) + "=。");
        entries.push_back(std::string("Shift+") + name + "=。");
    }
    scene.Snapshot(true, entries);
    REQUIRE(scene.editor->VisibleRowCount() == 22);
    REQUIRE(scene.editor->AddButton()->IsDisabled());
    scene.ClickCentre(scene.editor->Controls(21).remove);
    REQUIRE(scene.editor->VisibleRowCount() == 21);
    REQUIRE(!scene.editor->AddButton()->IsDisabled());
}

TEST_CASE(settings_custom_punctuation_symbol_display_ignores_the_pairing_switch)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(true, {"Shift+BracketLeft=「", "Shift+BracketRight=」"}, true);
    REQUIRE(scene.SymbolShown(0) == L"「」");
    // 右半边不单列，已选的右半边照样显示它自己。
    REQUIRE(scene.editor->Controls(1).symbol->GetValue().empty());
    REQUIRE(scene.editor->Controls(1).symbol->CurrentLabel() == L"」");
    const auto options = scene.editor->Controls(0).symbol->GetOptions();

    // 关掉成对标点自动补全：显示与选项都不变，也不写配置。
    scene.Snapshot(true, {"Shift+BracketLeft=「", "Shift+BracketRight=」"}, false);
    REQUIRE(scene.SymbolShown(0) == L"「」");
    REQUIRE(scene.editor->Controls(1).symbol->CurrentLabel() == L"」");
    const auto afterSwitch = scene.editor->Controls(0).symbol->GetOptions();
    REQUIRE(afterSwitch.size() == options.size());
    for (size_t i = 0; i < options.size(); ++i)
    {
        REQUIRE(afterSwitch[i].label == options[i].label);
        REQUIRE(afterSwitch[i].value == options[i].value);
    }
    REQUIRE(scene.MapUpdates().empty());
}

TEST_CASE(custom_punctuation_catalog_reads_the_real_symbol_database)
{
    // 测试脚本把 METASEQUOIA_IME_DATA_DIR 指到装好词库的数据目录（本地 run-server-tests.ps1、CI test-server.ps1）。
    wchar_t *dataDir = nullptr;
    size_t length = 0;
    REQUIRE(_wdupenv_s(&dataDir, &length, L"METASEQUOIA_IME_DATA_DIR") == 0 && dataDir != nullptr);
    const std::filesystem::path othersDb = std::filesystem::path(dataDir) / L"others.db";
    std::free(dataDir);
    REQUIRE(std::filesystem::exists(othersDb));
    const auto catalog = LoadPunctuationSymbolCatalog(othersDb);
    // 比不随词库变的三样多，而且有 ¿ 与 ‽：这两个只在发布词库 dict-v1.0.0 的 Punctuation 分类里，
    // 不在成对表、默认映射与半角标点里。说明真库的表名、列名与英文分类的回退都对得上。
    REQUIRE(catalog.size() > kBuiltInCount);
    for (const wchar_t *symbol : {L"¿", L"‽"})
        REQUIRE(std::find(catalog.begin(), catalog.end(), std::wstring(symbol)) != catalog.end());
}

TEST_CASE(custom_punctuation_options_keep_the_built_in_symbols_without_a_catalog)
{
    // 符号目录读不到时，第二个下拉框照样能选到【】《》「」和默认映射的每个输出。
    const auto catalog = LoadPunctuationSymbolCatalog(test::TempRoot() / L"msime-不存在的目录.db");
    std::set<std::string> values;
    std::set<std::wstring> labels;
    for (const auto &option : BuildSymbolOptions(catalog))
    {
        values.insert(option.value);
        labels.insert(option.label);
    }
    for (const wchar_t *label : {L"【】", L"《》", L"「」", L"〈〉"})
        REQUIRE(labels.count(label) == 1);
    // 默认映射的输出要么自己是一项，要么是某一对的右半边、随左半边那一项出。
    for (const auto &output : kDefaultOutputs)
    {
        const bool listed = values.count(native_settings::Narrow(output)) == 1;
        bool closingOfListedPair = false;
        for (const auto &label : labels)
            closingOfListedPair = closingOfListedPair || (label.size() == 2 && output == label.substr(1));
        REQUIRE(listed || closingOfListedPair);
    }
}

TEST_CASE(custom_punctuation_symbol_options_keep_a_closing_half_whose_opening_is_missing)
{
    // 目录里只有右半边 」、没有 「：」 照样单列，不会选不到。
    const auto on = BuildSymbolOptions(WithAscii({L"」", L"【", L"】"}));
    std::set<std::wstring> labels;
    for (const auto &option : on)
        labels.insert(option.label);
    REQUIRE(labels.count(L"」") == 1);
    REQUIRE(labels.count(L"【】") == 1);
    REQUIRE(labels.count(L"】") == 0);
}

TEST_CASE(settings_custom_punctuation_editing_a_row_keeps_an_unfinished_row)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(true, {"Shift+BracketLeft=「"});
    // 加一行只选了键、还没选符号：这一行不写进配置。
    scene.ClickCentre(scene.editor->AddButton());
    scene.PickOption(scene.editor->Controls(1).key, kKeyEqual);
    REQUIRE(scene.MapUpdates().empty());

    // 改第一行的符号：点开第二个下拉框（打开时列出全部选项），点第 3 项 〔（成对显示成 〔〕）。
    // 写进配置，没填完的那一行还在。
    const auto symbol = scene.editor->Controls(0).symbol;
    const msimeui::RectF box = symbol->GetBounds();
    scene.harness.Click(box.x + 20.0f, box.y + box.height * 0.5f);
    scene.harness.GiveWindowKeyboardFocus();
    msimeui::Visual *menu = scene.harness.VisualAt(box.x + box.width * 0.5f, box.y + box.height + 20.0f);
    REQUIRE(menu != nullptr && menu != symbol.get());
    const msimeui::RectF bounds = menu->GetBounds();
    scene.harness.Click(bounds.x + bounds.width * 0.5f, bounds.y + kInset + 2.0f * kRowPitch + kRow * 0.5f);
    const auto updates = scene.MapUpdates();
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0] == std::vector<std::string>{"Shift+BracketLeft=〔"});
    REQUIRE(scene.editor->VisibleRowCount() == 2);
    REQUIRE(scene.editor->Rows().Rows()[1].trigger.key == '=');
    REQUIRE(scene.editor->Controls(1).key->GetValue() == "=");
}

TEST_CASE(settings_custom_punctuation_reads_the_catalog_only_once_the_table_shows)
{
    int calls = 0;
    EditorScene &scene = EditorScene::Create([&calls]() {
        ++calls;
        return WithAscii({L"「", L"」"});
    });
    // 总开关关着：打开设置页不读符号目录。
    scene.Snapshot(false, {"Shift+BracketLeft=「"});
    scene.Snapshot(false, {"Shift+BracketLeft=「"}, false);
    REQUIRE(calls == 0);
    // 打开总开关，表格显示：读一次。
    scene.Snapshot(true, {"Shift+BracketLeft=「"}, false);
    REQUIRE(calls == 1);
    REQUIRE(scene.editor->Controls(0).symbol->CurrentLabel() == L"「」");
    REQUIRE(scene.editor->Controls(0).symbol->GetValue() == "「");
    // 之后再收到快照（切换成对补全、改映射）不再读。
    scene.Snapshot(true, {"Shift+BracketLeft=「"}, true);
    scene.Snapshot(true, {"Shift+BracketLeft=「", "Minus=。"}, true);
    REQUIRE(calls == 1);
}

TEST_CASE(settings_custom_punctuation_clear_refuses_a_duplicate_trigger_with_a_message)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(true, {"BracketLeft=〔", "Shift+BracketLeft=「"});
    // 清空第 1 行会变成留空 + [，和第 0 行相同：提示冲突，不改、不写。
    scene.ClickCentre(scene.editor->Controls(1).clear);
    REQUIRE(!scene.editor->Status().empty());
    REQUIRE(scene.editor->Rows().Rows()[1].trigger.shift);
    REQUIRE(scene.MapUpdates().empty());
}

// ---- 使用者点开总开关时表格为空：自动加一行空行 ----

namespace
{
// 第 row 行在界面上显示着、点得到，两个下拉框都空着。
bool ShowsBlankRow(EditorScene &scene, size_t row)
{
    scene.harness.Layout();
    const auto controls = scene.editor->Controls(row);
    const auto *slot =
        dynamic_cast<const native_settings::Hideable *>(controls.record->GetParentVisual()->GetParentVisual());
    if (slot == nullptr || !slot->IsVisible())
        return false;
    const msimeui::RectF r = controls.record->GetBounds();
    return scene.harness.VisualAt(r.x + r.width * 0.5f, r.y + r.height * 0.5f) == controls.record.get() &&
           controls.key->GetValue().empty() && controls.symbol->GetValue().empty();
}
} // namespace

TEST_CASE(settings_custom_punctuation_switching_on_an_empty_table_adds_one_blank_row)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(false, {});
    REQUIRE(scene.editor->VisibleRowCount() == 0);

    scene.ClickCentre(scene.editor->Toggle());
    REQUIRE(scene.SwitchUpdates() == std::vector<Json>{Json(true)});
    REQUIRE(scene.editor->Table()->IsVisible());
    REQUIRE(scene.editor->VisibleRowCount() == 1);
    // 和点一次「+」一样：这一行显示出来，两个下拉框都空着，不写映射。
    REQUIRE(ShowsBlankRow(scene, 0));
    REQUIRE(scene.MapUpdates().empty());

    // Server 回显开关已开、映射仍空：空行留着，不再加。
    scene.Snapshot(true, {});
    REQUIRE(scene.editor->VisibleRowCount() == 1);
    REQUIRE(scene.MapUpdates().empty());

    // 填完键和符号才写。
    scene.PickOption(scene.editor->Controls(0).key, kKeyEqual);
    REQUIRE(scene.MapUpdates().empty());
    scene.SearchAndPick(scene.editor->Controls(0).symbol, L"〔");
    REQUIRE(scene.MapUpdates() == (std::vector<std::vector<std::string>>{std::vector<std::string>{"Equal=〔"}}));
}

TEST_CASE(settings_custom_punctuation_config_turning_the_switch_on_adds_no_row)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(false, {});
    // 配置热更新把开关从 false 改成 true：只显示表格，不加行。
    scene.Snapshot(true, {});
    REQUIRE(scene.editor->Table()->IsVisible());
    REQUIRE(scene.editor->VisibleRowCount() == 0);
    REQUIRE(scene.MapUpdates().empty());
}

TEST_CASE(settings_custom_punctuation_stale_snapshots_do_not_bring_back_a_deleted_row)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(false, {});
    scene.ClickCentre(scene.editor->Toggle());
    REQUIRE(scene.editor->VisibleRowCount() == 1);
    scene.ClickCentre(scene.editor->Controls(0).remove);
    REQUIRE(scene.editor->VisibleRowCount() == 0);
    // 点开之前发出的关着的快照晚到，接着才是开着的回显。
    scene.Snapshot(false, {});
    scene.Snapshot(true, {});
    REQUIRE(scene.editor->Table()->IsVisible());
    REQUIRE(scene.editor->VisibleRowCount() == 0);
    REQUIRE(scene.MapUpdates().empty());
}

TEST_CASE(settings_custom_punctuation_switching_on_keeps_existing_rows_as_they_are)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(false, {"Equal=="});
    scene.ClickCentre(scene.editor->Toggle());
    REQUIRE(scene.editor->Table()->IsVisible());
    REQUIRE(scene.editor->VisibleRowCount() == 1);
    REQUIRE(scene.editor->Controls(0).key->GetValue() == "=");

    // 配置热更新打开、已有两行：仍是两行。
    EditorScene &other = EditorScene::Create();
    other.Snapshot(false, {"Equal==", "Minus=。"});
    other.Snapshot(true, {"Equal==", "Minus=。"});
    REQUIRE(other.editor->VisibleRowCount() == 2);
    REQUIRE(scene.MapUpdates().empty());
    REQUIRE(other.MapUpdates().empty());
}

TEST_CASE(settings_custom_punctuation_page_opened_with_the_switch_on_shows_an_empty_table_as_empty)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(true, {});
    REQUIRE(scene.editor->Table()->IsVisible());
    REQUIRE(scene.editor->VisibleRowCount() == 0);
    // 开关一直开着，后来的快照也不加。
    scene.Snapshot(true, {}, false);
    REQUIRE(scene.editor->VisibleRowCount() == 0);
}

TEST_CASE(settings_custom_punctuation_deleting_the_last_row_while_on_leaves_the_table_empty)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(true, {"Equal=="});
    scene.ClickCentre(scene.editor->Controls(0).remove);
    REQUIRE(scene.editor->VisibleRowCount() == 0);
    REQUIRE(scene.MapUpdates() == (std::vector<std::vector<std::string>>{std::vector<std::string>{}}));
    // Server 回显空映射：开关没变，不补回。
    scene.Snapshot(true, {});
    REQUIRE(scene.editor->VisibleRowCount() == 0);
    REQUIRE(scene.MapUpdates().size() == 1);
}

TEST_CASE(settings_custom_punctuation_switching_off_and_on_again_adds_a_row_only_while_empty)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(true, {});
    REQUIRE(scene.editor->VisibleRowCount() == 0);
    scene.ClickCentre(scene.editor->Toggle());
    REQUIRE(!scene.editor->Table()->IsVisible());
    REQUIRE(scene.editor->VisibleRowCount() == 0);
    scene.ClickCentre(scene.editor->Toggle());
    REQUIRE(scene.editor->VisibleRowCount() == 1);
    REQUIRE(ShowsBlankRow(scene, 0));

    // 自动加的空行还在：再关再开不叠第二行。
    scene.ClickCentre(scene.editor->Toggle());
    scene.ClickCentre(scene.editor->Toggle());
    REQUIRE(scene.editor->VisibleRowCount() == 1);

    // 删掉它、关了再开：表格又是空的，再加一行。
    scene.ClickCentre(scene.editor->Controls(0).remove);
    REQUIRE(scene.editor->VisibleRowCount() == 0);
    scene.ClickCentre(scene.editor->Toggle());
    scene.ClickCentre(scene.editor->Toggle());
    REQUIRE(scene.editor->VisibleRowCount() == 1);
    REQUIRE(ShowsBlankRow(scene, 0));
    REQUIRE(scene.SwitchUpdates() ==
            (std::vector<Json>{Json(false), Json(true), Json(false), Json(true), Json(false), Json(true)}));
    REQUIRE(scene.MapUpdates().empty());
}

TEST_CASE(settings_custom_punctuation_snapshot_after_the_page_is_released_reaches_no_editor)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(false, {"Equal=="});
    // 换掉页面、丢掉编辑器的引用，与 DPI 变化时重建页面相同。
    std::weak_ptr<CustomPunctuationEditor> released = scene.editor;
    scene.harness.SetRoot(std::make_shared<native_settings::Column>(0.0f));
    scene.editor.reset();
    REQUIRE(released.expired());
    // 用垃圾字节占满刚释放的堆块：之后还有人用那块内存里的指针，就会访问非法地址、整个测试程序崩溃。
    std::vector<std::vector<unsigned char>> filler;
    for (size_t size = 16; size <= 8192; size += 8)
        filler.emplace_back(size, static_cast<unsigned char>(0xA5));
    // 开关从关到开、映射也换：编辑器还在的话，会重建行、读符号目录。
    scene.Snapshot(true, {"Minus=。"}, false);
    scene.Snapshot(false, {}, true);
    scene.Snapshot(true, {"Equal==", "Minus=。"}, true);
    REQUIRE(scene.MapUpdates().empty());
}

// ---- 文案与布局 ----

namespace
{
const std::wstring kRecordIdle = L"设置快捷键";
const std::wstring kRecordWaiting = L"按下快捷键";
const std::wstring kRecordShift = L"Shift";
const std::wstring kSwitchSubtitle = L"可自定义标点输入，内含非常用符号（如「」『』）";

// 布局用 float 算，相等比较留一点余量。
bool Near(float a, float b)
{
    return std::fabs(a - b) < 0.01f;
}

float Bottom(const msimeui::RectF &r)
{
    return r.y + r.height;
}

// 一行控件所在的那一行（录键按钮的父容器）的矩形。
msimeui::RectF RowBounds(const CustomPunctuationEditor &editor, size_t row)
{
    return editor.Controls(row).record->GetParentVisual()->GetBounds();
}

// area 里点得到的文字标签，逐 2 DIP 扫一遍，按出现顺序去重。
std::vector<native_settings::Label *> LabelsIn(EditorScene &scene, const msimeui::RectF &area)
{
    std::vector<native_settings::Label *> labels;
    for (float y = area.y + 1.0f; y < Bottom(area); y += 2.0f)
    {
        for (float x = area.x + 1.0f; x < area.x + area.width; x += 2.0f)
        {
            auto *label = dynamic_cast<native_settings::Label *>(scene.harness.VisualAt(x, y));
            if (label && std::find(labels.begin(), labels.end(), label) == labels.end())
                labels.push_back(label);
        }
    }
    return labels;
}

native_settings::Label *LabelWithText(EditorScene &scene, const msimeui::RectF &area, const std::wstring &text)
{
    for (auto *label : LabelsIn(scene, area))
    {
        if (label->GetText() == text)
            return label;
    }
    return nullptr;
}

// 文字放得下所需的按钮宽度：画字的宽度，两边各加 12 DIP 内边距和一道边框（.restart-server-button）。
float FitWidth(const std::wstring &text)
{
    return native_settings::MeasureTextWidth(text, native_settings::BodyStyle()) + 2.0f * 12.0f +
           2.0f * native_settings::CssBorder();
}

// 第 1 行录 Shift 会和第 0 行的 Shift + [ 相同：表格下面出现冲突提示。
void ShowConflict(EditorScene &scene)
{
    scene.ClickCentre(scene.editor->Controls(1).record);
    scene.harness.GiveWindowKeyboardFocus();
    scene.harness.Key(VK_SHIFT);
    REQUIRE(!scene.editor->Status().empty());
}
} // namespace

TEST_CASE(settings_custom_punctuation_record_button_names_its_three_states)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(true, {"Equal=="});
    const auto record = scene.editor->Controls(0).record;
    REQUIRE(record->GetText() == kRecordIdle);

    scene.ClickCentre(record);
    scene.harness.GiveWindowKeyboardFocus();
    REQUIRE(record->GetText() == kRecordWaiting);
    // 别的键不录，按钮仍在等。
    scene.harness.Key('A');
    REQUIRE(record->GetText() == kRecordWaiting);
    scene.harness.Key(VK_SHIFT);
    REQUIRE(record->GetText() == kRecordShift);

    // Esc 结束录制，Shift 不变。
    scene.ClickCentre(record);
    REQUIRE(record->GetText() == kRecordWaiting);
    scene.harness.Key(VK_ESCAPE);
    REQUIRE(record->GetText() == kRecordShift);

    // 清空回到没录 Shift；再录、点到行外的空白处失焦，录制结束，文字回到没录。
    scene.ClickCentre(scene.editor->Controls(0).clear);
    REQUIRE(record->GetText() == kRecordIdle);
    scene.ClickCentre(record);
    REQUIRE(record->GetText() == kRecordWaiting);
    const msimeui::RectF row = RowBounds(*scene.editor, 0);
    const msimeui::RectF table = scene.editor->Table()->GetBounds();
    scene.harness.Click(table.x + table.width - 4.0f, row.y + row.height * 0.5f);
    REQUIRE(!scene.editor->IsRecording(0));
    REQUIRE(record->GetText() == kRecordIdle);
}

// 尺寸都按 DIP 算，换显示缩放只换 DIP 到像素的倍数；随缩放变的只有边框（缩放越大，边框占的 DIP 越少），
// 所以按 100% 量是最宽的情形。
TEST_CASE(settings_custom_punctuation_record_button_fits_its_longest_text)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(true, {"Equal==", "Shift+Minus=。"});
    const auto idle = scene.editor->Controls(0).record;
    const auto shift = scene.editor->Controls(1).record;
    // 去掉最小宽度：宽度只由按钮要显示的文字决定。
    idle->SetMinWidth(0.0f);
    shift->SetMinWidth(0.0f);
    scene.harness.Layout();
    REQUIRE(idle->GetText() == kRecordIdle);
    REQUIRE(shift->GetText() == kRecordShift);
    const float idleWidth = idle->GetBounds().width;
    scene.ClickCentre(idle);
    scene.harness.GiveWindowKeyboardFocus();
    REQUIRE(idle->GetText() == kRecordWaiting);
    const float waitingWidth = idle->GetBounds().width;
    // 三种文字下按钮一样宽，都放得下其中最长的那段。
    REQUIRE(Near(idleWidth, waitingWidth));
    REQUIRE(Near(idleWidth, shift->GetBounds().width));
    for (const std::wstring &text : {kRecordIdle, kRecordWaiting, kRecordShift})
        REQUIRE(idleWidth + 0.01f >= FitWidth(text));
}

TEST_CASE(settings_custom_punctuation_switch_row_says_what_the_feature_covers)
{
    std::vector<native_settings::SettingsIndexEntry> entries;
    native_settings::SettingsIndexScope scope("input", [&entries](const native_settings::SettingsIndexScope::Row &row) {
        entries.push_back({"input", L"输入", row.title, row.description, row.target});
    });
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(true, {"Equal=="});
    const auto row = std::find_if(entries.begin(), entries.end(), [](const native_settings::SettingsIndexEntry &e) {
        return e.title == L"自定义标点";
    });
    REQUIRE(row != entries.end());
    REQUIRE(row->description == kSwitchSubtitle);
    // 设置页左上角的搜索按标题找到这一项。放两个排在它前面的竞争项：标题里这几个字隔开出现的一项，
    // 只有说明里含这几个字的一项。标题紧凑匹配的这一项排第一。
    std::vector<native_settings::SettingsIndexEntry> competing = {
        {"input", L"输入", L"自动定义的标点", L"", row->target},
        {"input", L"输入", L"顿号", L"不经自定义标点，单独设置顿号", row->target},
    };
    competing.insert(competing.end(), entries.begin(), entries.end());
    const auto hits = native_settings::SearchSettings(competing, L"自定义标点");
    REQUIRE(hits.size() == 3);
    REQUIRE(hits[0].title == L"自定义标点");

    // 开关那一行显示的副标题就是这段文字。
    const msimeui::RectF view = scene.editor->View()->GetBounds();
    const msimeui::RectF table = scene.editor->Table()->GetBounds();
    REQUIRE(LabelWithText(scene, {view.x, view.y, view.width, table.y - view.y}, kSwitchSubtitle) != nullptr);
}

TEST_CASE(settings_custom_punctuation_switch_subtitle_wraps_without_clipping_in_a_narrow_page)
{
    EditorScene &scene = EditorScene::CreateAt(96.0f, 240.0f);
    scene.Snapshot(true, {"Equal=="});
    const msimeui::RectF view = scene.editor->View()->GetBounds();
    const msimeui::RectF table = scene.editor->Table()->GetBounds();
    const msimeui::RectF header = {view.x, view.y, view.width, table.y - view.y};
    auto *subtitle = LabelWithText(scene, header, kSwitchSubtitle);
    REQUIRE(subtitle != nullptr);
    const msimeui::RectF shown = subtitle->GetBounds();
    // 240 宽放不下一行：折成多行。
    const float oneLine = subtitle->Measure({10000.0f, 10000.0f}).height;
    const float wrapped = subtitle->Measure({shown.width, 10000.0f}).height;
    scene.harness.Layout();
    REQUIRE(wrapped > oneLine + 0.5f);
    // 给它的高度放得下折行后的每一行，也没超出开关那一行。
    REQUIRE(shown.height + 0.01f >= wrapped);
    REQUIRE(Bottom(shown) <= Bottom(header) + 0.01f);
    REQUIRE(shown.x + shown.width <= view.x + view.width + 0.01f);
}

TEST_CASE(settings_custom_punctuation_section_shows_no_explanation_besides_the_subtitle)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(true, {"Equal==", "Minus=。"});
    // 整节的文字标签只有标题、副标题和每行的「→」；按钮与下拉框自己画字，不是标签。
    std::vector<std::wstring> texts;
    for (const auto *label : LabelsIn(scene, scene.editor->View()->GetBounds()))
        texts.push_back(label->GetText());
    REQUIRE((texts == std::vector<std::wstring>{L"自定义标点", kSwitchSubtitle, L"→", L"→"}));
}

TEST_CASE(settings_custom_punctuation_rows_message_and_add_button_sit_at_the_normal_gap)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(true, {"Shift+BracketLeft=「", "BracketLeft=〔"});
    const auto add = scene.editor->AddButton();
    // 没有提示时，「+」到最后一行的距离。
    const float addGap = add->GetBounds().y - Bottom(RowBounds(*scene.editor, 1));

    ShowConflict(scene);
    const auto *message = LabelWithText(scene, scene.editor->Table()->GetBounds(), scene.editor->Status());
    REQUIRE(message != nullptr);
    const msimeui::RectF shown = message->GetBounds();
    const float rowToMessage = shown.y - Bottom(RowBounds(*scene.editor, 1));
    const float messageToAdd = add->GetBounds().y - Bottom(shown);
    // 最后一行、提示、「+」之间是同一个间距；提示隐藏时不占位置，「+」到最后一行也是这个间距。
    REQUIRE(rowToMessage > 0.0f);
    REQUIRE(Near(rowToMessage, messageToAdd));
    REQUIRE(Near(addGap, messageToAdd));
    // 行与行之间不比这个间距大。
    const float rowToRow = RowBounds(*scene.editor, 1).y - Bottom(RowBounds(*scene.editor, 0));
    REQUIRE(rowToRow > 0.0f);
    REQUIRE(rowToRow <= messageToAdd + 0.01f);
}

TEST_CASE(settings_custom_punctuation_add_button_takes_the_first_row_place_when_the_table_is_empty)
{
    EditorScene &scene = EditorScene::Create();
    scene.Snapshot(true, {"Equal=="});
    const float firstRow = RowBounds(*scene.editor, 0).y - scene.editor->Table()->GetBounds().y;
    scene.Snapshot(true, {});
    REQUIRE(scene.editor->VisibleRowCount() == 0);
    const float add = scene.editor->AddButton()->GetBounds().y - scene.editor->Table()->GetBounds().y;
    REQUIRE(Near(add, firstRow));
}

// ---- 通用容器 Column ----

namespace
{
std::shared_ptr<native_settings::Label> Text(const wchar_t *text)
{
    return std::make_shared<native_settings::Label>(text, native_settings::BodyStyle());
}
} // namespace

TEST_CASE(settings_column_puts_no_gap_around_a_hidden_child)
{
    SettingsWindowHarness harness(144.0f, {400.0f, 600.0f});
    auto first = Text(L"一");
    auto middle = Text(L"二");
    auto last = Text(L"三");
    auto hideable = std::make_shared<native_settings::Hideable>(middle, true);
    // 子项全隐藏的 Column 自己没有尺寸，外面的 Column 也不给它留间距。
    auto empty = std::make_shared<native_settings::Column>(6.0f);
    empty->Add(std::make_shared<native_settings::Hideable>(Text(L"四"), false));
    empty->Add(std::make_shared<native_settings::Hideable>(Text(L"五"), false));
    auto column = std::make_shared<native_settings::Column>(8.0f);
    column->Add(first);
    column->Add(hideable);
    column->Add(empty);
    column->Add(last);
    harness.SetRoot(column);
    const float gap = middle->GetBounds().y - Bottom(first->GetBounds());
    REQUIRE(gap > 0.0f);
    REQUIRE(Near(last->GetBounds().y - Bottom(middle->GetBounds()), gap));

    hideable->SetVisible(false);
    harness.Layout();
    REQUIRE(Near(last->GetBounds().y - Bottom(first->GetBounds()), gap));
    // 没有 padding：第一项贴着顶边，最后一项贴着底边，中间只有一份间距。
    REQUIRE(Near(first->GetBounds().y, column->GetBounds().y));
    REQUIRE(Near(Bottom(last->GetBounds()), Bottom(column->GetBounds())));
    REQUIRE(Near(column->GetBounds().height, first->GetBounds().height + gap + last->GetBounds().height));
}

TEST_CASE(settings_column_gives_a_zero_width_spacer_its_height_and_gaps)
{
    // 皮肤页用 Spacer(0, 10) 留出 10 DIP：宽为 0、高不为 0 的子项照样占高度、拿间距。
    SettingsWindowHarness harness(96.0f, {400.0f, 600.0f});
    auto first = Text(L"一");
    auto spacer = std::make_shared<native_settings::Spacer>(0.0f, 10.0f);
    auto last = Text(L"三");
    auto column = std::make_shared<native_settings::Column>(8.0f);
    column->Add(first);
    column->Add(spacer);
    column->Add(last);
    harness.SetRoot(column);
    REQUIRE(Near(spacer->GetBounds().height, 10.0f));
    REQUIRE(Near(spacer->GetBounds().y - Bottom(first->GetBounds()), 8.0f));
    REQUIRE(Near(last->GetBounds().y - Bottom(spacer->GetBounds()), 8.0f));
}

TEST_CASE(settings_column_places_narrow_children_by_its_horizontal_alignment)
{
    // 皮肤页卡片右侧的开关与预览按钮按 Trailing 靠右。
    for (const auto alignment : {msimeui::HorizontalAlignment::Trailing, msimeui::HorizontalAlignment::Center,
                                 msimeui::HorizontalAlignment::Stretch})
    {
        SettingsWindowHarness harness(96.0f, {400.0f, 600.0f});
        auto wide = Text(L"一二三四五六七八");
        auto narrow = Text(L"一");
        auto column = std::make_shared<native_settings::Column>(8.0f);
        column->SetHorizontalContentAlignment(alignment);
        column->Add(wide);
        column->Add(narrow);
        harness.SetRoot(column);
        const msimeui::RectF inside = column->GetBounds();
        const msimeui::RectF shown = narrow->GetBounds();
        if (alignment == msimeui::HorizontalAlignment::Stretch)
        {
            REQUIRE(Near(shown.x, inside.x));
            REQUIRE(Near(shown.width, inside.width));
            continue;
        }
        REQUIRE(shown.width < inside.width - 1.0f);
        if (alignment == msimeui::HorizontalAlignment::Trailing)
            REQUIRE(Near(shown.x + shown.width, inside.x + inside.width));
        else
            REQUIRE(Near(shown.x - inside.x, inside.x + inside.width - (shown.x + shown.width)));
    }
}
