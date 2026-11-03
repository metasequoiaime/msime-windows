#include "settings/native/native_page_custom_punctuation.h"

#include "engine/contracts/default_punctuation.h"
#include "engine/contracts/paired_punctuation.h"
#include "engine/core/data_path.h"

#include <sqlite3.h>

#include <algorithm>
#include <string_view>

namespace native_settings
{
namespace
{
namespace cp = FanyImeCustomPunctuation;

constexpr std::wstring_view kAsciiPunctuation = L"!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
constexpr const char *kEnabledPath = "input.custom_punctuation";
constexpr const char *kMapPath = "input.custom_punctuation_map";
constexpr const char *kPairedPath = "input.paired_punctuation";

// 一个分类的符号，按 sort_order；去重留第一次出现的位置。
std::vector<std::wstring> QueryCategory(sqlite3 *db, const char *parentCategory)
{
    std::vector<std::wstring> symbols;
    sqlite3_stmt *stmt = nullptr;
    const char *sql = "SELECT symbol FROM symbol_catalog WHERE parent_category = ? ORDER BY sort_order";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return symbols;
    sqlite3_bind_text(stmt, 1, parentCategory, -1, SQLITE_STATIC);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const auto *text = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
        if (text == nullptr)
            continue;
        std::wstring symbol;
        if (!cp::SymbolToUtf16(text, symbol))
            continue;
        if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end())
            symbols.push_back(std::move(symbol));
    }
    sqlite3_finalize(stmt);
    return symbols;
}

std::vector<std::wstring> QueryPunctuation(const std::filesystem::path &othersDb)
{
    sqlite3 *db = nullptr;
    const std::string path = metasequoia::path_to_utf8(othersDb);
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        if (db)
            sqlite3_close(db);
        return {};
    }
    sqlite3_busy_timeout(db, 3000);
    // 本地按 msime-dictionary 构建的词库用中文分类名，发布词库 dict-v1.0.0 用英文分类名。
    std::vector<std::wstring> symbols = QueryCategory(db, "标点");
    if (symbols.empty())
        symbols = QueryCategory(db, "Punctuation");
    sqlite3_close(db);
    return symbols;
}

// 成对表里以 closing 为右半边的那一对的左半边；不是右半边时为 0。
wchar_t OpeningFor(wchar_t closing)
{
    for (const auto &pair : FanyImePairedPunctuation::kPairs)
    {
        if (pair.closing == closing)
            return pair.opening;
    }
    return 0;
}

wchar_t SingleUnit(const std::wstring &symbol)
{
    return symbol.size() == 1 ? symbol[0] : 0;
}

std::vector<std::string> ReadEntries(const Json *value)
{
    std::vector<std::string> entries;
    if (value == nullptr)
        return entries;
    // 自己刚写的值在本地快照里是 JSON 数组序列化成的字符串，Server 回的快照是数组。
    const Json parsed = value->is_string() ? Json::parse(value->get<std::string>(), nullptr, false) : *value;
    if (!parsed.is_array())
        return entries;
    for (const auto &item : parsed)
    {
        if (item.is_string())
            entries.push_back(item.get<std::string>());
    }
    return entries;
}

std::wstring KeyLabel(char key)
{
    return std::wstring(1, static_cast<wchar_t>(key));
}

std::wstring TriggerText(cp::Trigger trigger)
{
    return (trigger.shift ? L"Shift + " : L"") + KeyLabel(trigger.key);
}

constexpr const wchar_t *kRecordIdle = L"设置快捷键";
constexpr const wchar_t *kRecordShift = L"Shift";
constexpr const wchar_t *kRecordWaiting = L"按下快捷键";

// 录键按钮：录制中把按键交给编辑器，失去焦点时结束录制。三种文字下一样宽，放得下最长的那段，
// 换文字时这一行的其他控件不挪位置。
class RecordButton : public Button
{
  public:
    RecordButton() : Button(kRecordIdle, ButtonKind::Secondary)
    {
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        SizeF size = Button::Measure(availableSize);
        for (const wchar_t *text : {kRecordIdle, kRecordShift, kRecordWaiting})
            size.width = std::max(size.width, WidthFor(text));
        size.width = std::min(size.width, availableSize.width);
        return size;
    }

    void SetOnKey(std::function<bool(WPARAM)> handler)
    {
        onKey_ = std::move(handler);
    }

    void SetOnBlur(std::function<void()> handler)
    {
        onBlur_ = std::move(handler);
    }

    bool OnKeyDown(WPARAM key, LPARAM lParam) override
    {
        if (onKey_)
        {
            const auto handler = onKey_;
            if (handler(key))
                return true;
        }
        return Button::OnKeyDown(key, lParam);
    }

    void OnFocusChanged(bool focused) override
    {
        Button::OnFocusChanged(focused);
        if (!focused && onBlur_)
        {
            const auto handler = onBlur_;
            handler();
        }
    }

  private:
    std::function<bool(WPARAM)> onKey_;
    std::function<void()> onBlur_;
};
} // namespace

std::vector<std::wstring> LoadPunctuationSymbolCatalog(const std::filesystem::path &othersDb)
{
    std::vector<std::wstring> symbols = QueryPunctuation(othersDb);
    const auto add = [&symbols](std::wstring symbol) {
        if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end())
            symbols.push_back(std::move(symbol));
    };
    // 常用符号不随词库版本变：成对表的两半、默认映射能出的符号、半角标点都列上。
    for (const auto &pair : FanyImePairedPunctuation::kPairs)
    {
        add(std::wstring(1, pair.opening));
        add(std::wstring(1, pair.closing));
    }
    for (std::wstring &output : FanyImeDefaultPunctuation::AllOutputs())
        add(std::move(output));
    for (const wchar_t ch : kAsciiPunctuation)
        add(std::wstring(1, ch));
    return symbols;
}

std::vector<DropdownOption> BuildSymbolOptions(const std::vector<std::wstring> &catalog, bool paired)
{
    std::vector<DropdownOption> options;
    for (const auto &symbol : catalog)
    {
        if (paired)
        {
            const wchar_t opening = OpeningFor(SingleUnit(symbol));
            if (opening != 0 && std::find(catalog.begin(), catalog.end(), std::wstring(1, opening)) != catalog.end())
                continue;
        }
        const std::string value = Narrow(symbol);
        options.push_back({SymbolLabel(value, paired), value, false, {}});
    }
    return options;
}

std::wstring SymbolLabel(const std::string &symbol, bool paired)
{
    std::wstring label = Widen(symbol);
    if (!paired)
        return label;
    const wchar_t closing = FanyImePairedPunctuation::ClosingFor(SingleUnit(label));
    if (closing != 0)
        label.push_back(closing);
    return label;
}

void CustomPunctuationRows::Load(const std::vector<std::string> &entries)
{
    rows_.clear();
    for (cp::Entry &entry : cp::NormalizeEntries(entries))
        rows_.push_back({entry.trigger, std::move(entry.symbol)});
}

bool CustomPunctuationRows::CanAdd() const
{
    return rows_.size() < cp::kMaxRows;
}

void CustomPunctuationRows::Add()
{
    if (CanAdd())
        rows_.emplace_back();
}

void CustomPunctuationRows::Remove(size_t row)
{
    if (row < rows_.size())
        rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(row));
}

std::optional<size_t> CustomPunctuationRows::SetKey(size_t row, char key)
{
    if (row >= rows_.size())
        return std::nullopt;
    const cp::Trigger trigger{rows_[row].trigger.shift, key};
    if (const auto other = ConflictWith(row, trigger))
        return other;
    rows_[row].trigger = trigger;
    return std::nullopt;
}

std::optional<size_t> CustomPunctuationRows::SetShift(size_t row, bool shift)
{
    if (row >= rows_.size())
        return std::nullopt;
    const cp::Trigger trigger{shift, rows_[row].trigger.key};
    if (const auto other = ConflictWith(row, trigger))
        return other;
    rows_[row].trigger = trigger;
    return std::nullopt;
}

void CustomPunctuationRows::SetSymbol(size_t row, std::string symbol)
{
    if (row < rows_.size())
        rows_[row].symbol = std::move(symbol);
}

std::vector<std::string> CustomPunctuationRows::Entries() const
{
    std::vector<std::string> entries;
    for (const auto &row : rows_)
    {
        const std::string element = cp::FormatEntry({row.trigger, row.symbol});
        if (!element.empty())
            entries.push_back(element);
    }
    return entries;
}

std::optional<size_t> CustomPunctuationRows::ConflictWith(size_t row, cp::Trigger trigger) const
{
    // 还没选键的行不和任何行冲突。
    if (trigger.key == 0)
        return std::nullopt;
    for (size_t i = 0; i < rows_.size(); ++i)
    {
        if (i != row && rows_[i].trigger == trigger)
            return i;
    }
    return std::nullopt;
}

struct CustomPunctuationEditor::Slot
{
    RowControls controls;
    std::shared_ptr<Hideable> box;
};

CustomPunctuationEditor::CustomPunctuationEditor(PageContext &context,
                                                 std::function<std::vector<std::wstring>()> catalog)
    : context_(context), catalogSource_(std::move(catalog))
{
    toggle_ = BindToggle(context, kEnabledPath, [this](bool on) {
        if (on)
            AddBlankRowIfEmpty();
    });
    auto header = MakeRow(L"自定义标点", L"可自定义标点输入，内含非常用符号（如「」『』〖〗）；联动成对补全", toggle_);

    auto rows = std::make_shared<Column>(6.0f);
    std::vector<DropdownOption> keyOptions;
    for (const auto &info : cp::kKeys)
        keyOptions.push_back({KeyLabel(info.key), std::string(1, info.key), false, {}});

    for (size_t i = 0; i < cp::kMaxRows; ++i)
    {
        auto slot = std::make_shared<Slot>();
        auto record = std::make_shared<RecordButton>();
        record->SetMinWidth(96.0f);
        slot->controls.record = record;
        slot->controls.clear = std::make_shared<Button>(L"清空", ButtonKind::Soft);

        Dropdown::Options keySettings;
        keySettings.compact = true;
        keySettings.fixedWidth = 72.0f;
        keySettings.menuAlignLeft = true;
        slot->controls.key = std::make_shared<Dropdown>(keyOptions, keySettings);

        Dropdown::Options symbolSettings;
        symbolSettings.compact = true;
        symbolSettings.searchable = true;
        symbolSettings.fixedWidth = 120.0f;
        symbolSettings.menuAlignLeft = true;
        symbolSettings.menuMinWidth = 120.0f;
        symbolSettings.menuMaxWidth = 240.0f;
        symbolSettings.placeholder = L"搜索符号";
        slot->controls.symbol = std::make_shared<Dropdown>(std::vector<DropdownOption>{}, symbolSettings);
        slot->controls.remove = std::make_shared<Button>(L"删除", ButtonKind::Soft);

        auto line = std::make_shared<Row>(8.0f);
        line->SetAlign(Row::Align::Center);
        line->Add(slot->controls.record);
        line->Add(slot->controls.clear);
        line->Add(slot->controls.key);
        line->Add(std::make_shared<Label>(L"→", BodyStyle(), TextRole::Muted));
        line->Add(slot->controls.symbol);
        line->Add(slot->controls.remove);
        slot->box = std::make_shared<Hideable>(line, false);
        rows->Add(slot->box);

        record->SetOnClick([this, i]() {
            if (recording_ == i)
                StopRecording();
            else
                StartRecording(i);
        });
        record->SetOnKey([this, i](WPARAM key) { return OnRecordKey(i, key); });
        record->SetOnBlur([this, i]() {
            if (recording_ == i)
                StopRecording();
        });
        slot->controls.clear->SetOnClick([this, i]() {
            StopRecording();
            if (const auto other = rows_.SetShift(i, false))
            {
                ShowConflict(i, *other);
                return;
            }
            ClearStatus();
            Refresh();
            Commit();
        });
        slot->controls.key->SetOnChanged([this, i](size_t, const DropdownOption &option) {
            if (option.value.size() != 1)
                return;
            if (const auto other = rows_.SetKey(i, option.value[0]))
            {
                ShowConflict(i, *other);
                Refresh();
                return;
            }
            ClearStatus();
            Commit();
        });
        slot->controls.symbol->SetOnChanged([this, i](size_t, const DropdownOption &option) {
            rows_.SetSymbol(i, option.value);
            Commit();
        });
        slot->controls.remove->SetOnClick([this, i]() {
            StopRecording();
            rows_.Remove(i);
            ClearStatus();
            Refresh();
            Commit();
        });
        slots_.push_back(std::move(slot));
    }

    status_ = std::make_shared<Label>(L"", CaptionStyle(), TextRole::Danger);
    statusBox_ = std::make_shared<Hideable>(status_, false);
    add_ = std::make_shared<Button>(L"+", ButtonKind::Secondary);
    add_->SetMinWidth(36.0f);
    add_->SetOnClick([this]() {
        AddBlankRow();
        Refresh();
    });
    auto addRow = std::make_shared<Row>(0.0f);
    addRow->SetAlign(Row::Align::Start);
    addRow->Add(add_);

    auto body = std::make_shared<Column>(8.0f);
    body->Add(rows);
    body->Add(statusBox_);
    body->Add(addRow);
    body->SetMargin({0.0f, 10.0f, 0.0f, 0.0f});
    table_ = std::make_shared<Hideable>(body, false);

    view_ = std::make_shared<Column>(0.0f);
    view_->Add(header);
    view_->Add(table_);
}

std::shared_ptr<CustomPunctuationEditor> CustomPunctuationEditor::Create(
    PageContext &context, std::function<std::vector<std::wstring>()> catalog)
{
    std::shared_ptr<CustomPunctuationEditor> editor(new CustomPunctuationEditor(context, std::move(catalog)));
    // 页面重建时编辑器会释放，配置监听器却留在 SettingsModel 里。
    std::weak_ptr<CustomPunctuationEditor> weak = editor;
    context.OnConfig([weak]() {
        if (auto alive = weak.lock())
            alive->Sync();
    });
    return editor;
}

VisualPtr CustomPunctuationEditor::View() const
{
    return view_;
}

const std::shared_ptr<ToggleSwitch> &CustomPunctuationEditor::Toggle() const
{
    return toggle_;
}

const std::shared_ptr<Hideable> &CustomPunctuationEditor::Table() const
{
    return table_;
}

size_t CustomPunctuationEditor::VisibleRowCount() const
{
    return rows_.Rows().size();
}

CustomPunctuationEditor::RowControls CustomPunctuationEditor::Controls(size_t row) const
{
    return row < slots_.size() ? slots_[row]->controls : RowControls{};
}

const std::shared_ptr<Button> &CustomPunctuationEditor::AddButton() const
{
    return add_;
}

bool CustomPunctuationEditor::IsRecording(size_t row) const
{
    return recording_ == row;
}

std::wstring CustomPunctuationEditor::Status() const
{
    return statusBox_->IsVisible() ? status_->GetText() : std::wstring();
}

void CustomPunctuationEditor::Sync()
{
    const SettingsModel &model = context_.model;
    const bool enabled = model.GetBool(kEnabledPath, false);
    table_->SetVisible(enabled);

    const bool paired = model.GetBool(kPairedPath, true);
    // 第二个下拉框的选项只在表格显示时才列：总开关关着时不读符号目录。
    const bool optionsStale = enabled && (!optionsListed_ || paired != optionsPaired_);
    const bool pairedChanged = !synced_ || paired != paired_;
    paired_ = paired;

    const std::vector<std::string> entries = ReadEntries(model.Find(kMapPath));
    const bool entriesChanged = !synced_ || entries != lastEntries_;
    synced_ = true;
    if (entriesChanged)
    {
        StopRecording();
        ClearStatus();
        rows_.Load(entries);
        lastEntries_ = entries;
    }
    if (optionsStale)
        RefreshSymbolOptions();
    if (entriesChanged || pairedChanged || optionsStale)
        Refresh();
}

void CustomPunctuationEditor::AddBlankRow()
{
    StopRecording();
    rows_.Add();
}

void CustomPunctuationEditor::AddBlankRowIfEmpty()
{
    if (!rows_.Rows().empty())
        return;
    AddBlankRow();
    Refresh();
}

void CustomPunctuationEditor::Refresh()
{
    const auto &rows = rows_.Rows();
    for (size_t i = 0; i < slots_.size(); ++i)
    {
        Slot &slot = *slots_[i];
        const bool visible = i < rows.size();
        slot.box->SetVisible(visible);
        if (!visible)
            continue;
        const CustomPunctuationRow &row = rows[i];
        slot.controls.record->SetText(recording_ == i     ? kRecordWaiting
                                      : row.trigger.shift ? kRecordShift
                                                          : kRecordIdle);
        if (row.trigger.key == 0)
            slot.controls.key->ClearSelection();
        else
            slot.controls.key->SetValue(std::string(1, row.trigger.key));
        if (row.symbol.empty())
        {
            slot.controls.symbol->ClearSelection();
        }
        else
        {
            // 不在选项里的符号（例成对显示时的右半边）显示它自己。
            slot.controls.symbol->SetValue(row.symbol);
        }
    }
    add_->SetDisabled(!rows_.CanAdd());
    view_->InvalidateMeasure();
}

void CustomPunctuationEditor::RefreshSymbolOptions()
{
    if (!catalogLoaded_)
    {
        catalog_ = catalogSource_ ? catalogSource_() : std::vector<std::wstring>{};
        catalogLoaded_ = true;
    }
    const std::vector<DropdownOption> options = BuildSymbolOptions(catalog_, paired_);
    for (const auto &slot : slots_)
        slot->controls.symbol->SetOptions(options);
    optionsListed_ = true;
    optionsPaired_ = paired_;
}

void CustomPunctuationEditor::Commit()
{
    const std::vector<std::string> entries = rows_.Entries();
    if (entries == lastEntries_)
        return;
    lastEntries_ = entries;
    Json array = Json::array();
    for (const auto &entry : entries)
        array.push_back(entry);
    context_.model.Update(kMapPath, array.dump());
}

void CustomPunctuationEditor::ShowConflict(size_t row, size_t other)
{
    (void)row;
    const cp::Trigger trigger = rows_.Rows()[other].trigger;
    status_->SetText(L"「" + TriggerText(trigger) + L"」已在第 " + std::to_wstring(other + 1) +
                     L" 行使用，同一个键只能定一行");
    statusBox_->SetVisible(true);
    view_->InvalidateMeasure();
}

void CustomPunctuationEditor::ClearStatus()
{
    status_->SetText({});
    statusBox_->SetVisible(false);
    view_->InvalidateMeasure();
}

void CustomPunctuationEditor::StartRecording(size_t row)
{
    StopRecording();
    if (row >= rows_.Rows().size())
        return;
    recording_ = row;
    slots_[row]->controls.record->SetText(kRecordWaiting);
}

void CustomPunctuationEditor::StopRecording()
{
    if (!recording_)
        return;
    const size_t row = *recording_;
    recording_.reset();
    if (row < rows_.Rows().size())
        slots_[row]->controls.record->SetText(rows_.Rows()[row].trigger.shift ? kRecordShift : kRecordIdle);
}

bool CustomPunctuationEditor::OnRecordKey(size_t row, WPARAM key)
{
    if (recording_ != row)
        return false;
    switch (key)
    {
    case VK_ESCAPE:
        StopRecording();
        return true;
    case VK_TAB:
        StopRecording();
        return false;
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT: {
        StopRecording();
        if (const auto other = rows_.SetShift(row, true))
        {
            ShowConflict(row, *other);
            return true;
        }
        ClearStatus();
        Refresh();
        Commit();
        return true;
    }
    default:
        // 只认 Shift，别的键不录也不交给别处。
        return true;
    }
}

VisualPtr BuildCustomPunctuationSection(PageContext &context, std::function<std::vector<std::wstring>()> catalog,
                                        std::shared_ptr<CustomPunctuationEditor> *editorOut)
{
    auto editor = CustomPunctuationEditor::Create(context, std::move(catalog));
    if (editorOut)
        *editorOut = editor;
    // 编辑器跟着这一节的视图活：页面释放（例如 DPI 变化时重建页面）时它也随之释放。
    struct Holder : public Column
    {
        std::shared_ptr<CustomPunctuationEditor> editor;
    };
    auto holder = std::make_shared<Holder>();
    holder->editor = editor;
    holder->Add(editor->View());
    return holder;
}
} // namespace native_settings
