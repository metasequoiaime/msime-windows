#pragma once

#include "settings/native/native_settings_pages.h"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Pieces shared by the dictionary page and the quick-phrase card of the tools page, both
// ported from the former web page's dict.css: `.dict-card`, `.dict-toolbar`, `.dict-input`,
// `.dict-table`, `.dict-pagination`, `.dict-dialog`, and the dictionaryRequest client.
namespace native_settings
{
// A card with explicit padding inside its 1px border. `.section` with overridden padding,
// or `.dict-card` (12px radius, --border-strong, --card-bg-solid).
class PageCard : public msimeui::Container
{
  public:
    enum class Style
    {
        Section,
        DictCard,
    };
    PageCard(VisualPtr child, Style style, Thickness padding);

    SizeF Measure(const SizeF &availableSize) override;
    void Render(msimeui::DeviceResources &dr) override;

  private:
    Style style_ = Style::Section;
    Thickness inner_ = {};
    msimeui::ShadowCache shadow_;
};

// `.dict-input`: 42px tall, --surface-sunken fill, --control-border, accent border and
// a 2px accent ring while focused. Text inherits the surrounding font size.
class DictInput : public msimeui::Container
{
  public:
    DictInput(float fontSize, std::wstring placeholder);

    std::wstring GetText() const;
    void SetText(const std::wstring &text);
    void SetPlaceholder(std::wstring placeholder);
    void SetOnSubmit(std::function<void()> handler);
    // 多行框：Enter 换行，Ctrl+Enter 提交，高度给三行。
    void SetMultiline(bool multiline);
    // A control at the right end of a single-line box, behind a short vertical divider; the
    // text stops before the divider. The box then draws the chrome around both.
    void SetTrailing(VisualPtr trailing);
    const std::shared_ptr<msimeui::TextBox> &Box() const
    {
        return box_;
    }

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    void Attach(msimeui::Window *window) override;
    msimeui::Visual *FindVisualAt(const PointF &point) override;
    msimeui::Visual *FindFocusableAt(const PointF &point) override;

  private:
    void ApplyAppearance(bool focused);
    // Where the divider in front of the trailing control is drawn.
    float DividerX() const;

    std::shared_ptr<msimeui::TextBox> box_;
    VisualPtr trailing_;
    SizeF trailingSize_ = {};
    int appliedState_ = -1;
    float height_ = 42.0f;
    bool multiline_ = false;
};

// `display: grid; gap: 7px; margin-top: 14px` label above an input in `.dict-dialog`.
VisualPtr MakeDialogField(const std::wstring &label, const std::shared_ptr<DictInput> &input);
// `.dict-dialog-title`
std::shared_ptr<Label> MakeDialogTitle(const std::wstring &title);

// `.dict-table` column layout: a 40px check column, a 64px index column, percentage columns and
// a 124px action column; what the percentages leave goes to the two fixed text columns in
// proportion (table-layout: fixed).
constexpr float kDictCheckColumn = 40.0f;
constexpr float kDictIndexWidth = 64.0f;
constexpr float kDictActionWidth = 124.0f;
// A text column keeps this much before its 18px cell padding: below it the cell shows a
// couple of characters at most, so the column is dropped instead.
constexpr float kDictMinTextColumn = 76.0f;

struct DictColumns
{
    std::array<float, 3> percents = {0.22f, 0.25f, 0.18f};
    // 0 check column | 1 index | 2..4 text cells | 5 action start | 6 table right edge.
    // Narrow tables drop whole columns (weight, then the code column, then the index) by
    // collapsing their edges; a collapsed cell measures zero or less and draws nothing.
    std::array<float, 7> Edges(float width) const;
    // 这种宽度下还剩哪几列（序号、编码、词条、权重）：和 Edges 用同一套预算，
    // 测试靠它锁住「收起的顺序是权重 → 编码 → 序号」这个语义。
    std::array<bool, 4> VisibleColumns(float width) const;
};

// The `<thead>` of `.dict-table-header-wrap`.
class DictTableHeader : public msimeui::Visual
{
  public:
    explicit DictTableHeader(DictColumns columns);
    void SetLabels(std::array<std::wstring, 4> labels);
    // padding-right of the header wrap: the body's scrollbar width.
    void SetScrollbarReserve(float width);
    float ScrollbarReserve() const
    {
        return reserve_;
    }
    // 表头的全选框：点它一次勾上当前页全部，再点一次全取消。
    void SetOnSelectAll(std::function<void()> handler);
    void SetAllSelected(bool selected);
    // 只读表（种子词库的词条）没有勾选框。
    void SetSelectable(bool selectable);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    bool OnMouseMove(const POINT &point, WPARAM keyState) override;
    void OnMouseLeave() override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  private:
    RectF CheckRect() const;
    bool HitCheck(const PointF &point) const;

    DictColumns columns_;
    std::array<std::wstring, 4> labels_;
    float reserve_ = 0.0f;
    bool selectable_ = true;
    bool allSelected_ = false;
    bool hovered_ = false;
    bool pressed_ = false;
    std::function<void()> onSelectAll_;
};

// The `<tbody>`: rows of checkbox + index + three text cells + 编辑 / 删除 actions, or one empty row.
class DictTableBody : public msimeui::Visual
{
  public:
    using ActionHandler = std::function<void(size_t row, bool remove)>;
    using SelectionHandler = std::function<void()>;

    explicit DictTableBody(DictColumns columns);
    void SetRows(std::vector<std::array<std::wstring, 3>> rows, size_t firstIndex);
    void SetEmpty(std::wstring message);
    void SetOnAction(ActionHandler handler);
    // 只读：不画编辑 / 删除，也没有勾选框（种子词库的词条只能整库启停、删除）。
    void SetReadOnly(bool readOnly);
    // 勾选只按当前页的行号记，翻页、换词库、重新查询都清空。
    const std::vector<size_t> &Selection() const
    {
        return selection_;
    }
    bool AllSelected() const;
    void SetAllSelected(bool selected);
    void ClearSelection();
    void SetOnSelectionChanged(SelectionHandler handler);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    bool OnMouseMove(const POINT &point, WPARAM keyState) override;
    void OnMouseLeave() override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;
    // 操作按钮的矩形：窄表下必须钳在表内，测试靠它锁住这个约束。
    RectF ActionRect(size_t row, bool remove) const;

  private:
    float RowHeight() const;
    RectF CheckRect(size_t row) const;
    // (row, remove) under the point, or row = -1.
    std::pair<size_t, bool> HitAction(const PointF &point) const;
    // Row whose checkbox is under the point, or kNone.
    size_t HitCheck(const PointF &point) const;
    void NotifySelection();
    bool IsSelected(size_t row) const;

    DictColumns columns_;
    std::vector<std::array<std::wstring, 3>> rows_;
    size_t firstIndex_ = 0;
    std::wstring empty_;
    std::pair<size_t, bool> hovered_ = {static_cast<size_t>(-1), false};
    std::pair<size_t, bool> pressed_ = {static_cast<size_t>(-1), false};
    ActionHandler onAction_;
    bool readOnly_ = false;
    std::vector<size_t> selection_;
    size_t hoveredCheck_ = static_cast<size_t>(-1);
    size_t pressedCheck_ = static_cast<size_t>(-1);
    SelectionHandler onSelectionChanged_;
};

// `.dict-table-wrap`: shows the body up to max-height: clamp(180px, 100vh - 350px, 550px)
// and scrolls the rest with the page's 8px scrollbar.
class DictTableScroll : public msimeui::Visual
{
  public:
    DictTableScroll(std::shared_ptr<DictTableBody> body, HostServices &host);
    bool HasScrollbar() const;
    void ScrollToTop();
    // Runs once per attach, i.e. when the page it belongs to is actually shown.
    void SetOnAttach(std::function<void()> handler);
    // 滚动条出现或消失时把占用的宽度告诉表头：表体让出了 8px，表头得跟着让，否则两边的列错开。
    void SetOnScrollbarReserve(std::function<void(float)> handler);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    void Attach(msimeui::Window *window) override;
    msimeui::Visual *FindVisualAt(const PointF &point) override;
    msimeui::Visual *FindFocusableAt(const PointF &point) override;

  private:
    float MaxHeight() const;

    std::shared_ptr<DictTableBody> body_;
    std::shared_ptr<msimeui::ScrollViewer> scroll_;
    HostServices &host_;
    float contentHeight_ = 0.0f;
    int appliedLight_ = -1;
    float appliedReserve_ = -1.0f;
    std::function<void()> onAttach_;
    std::function<void(float)> onScrollbarReserve_;
};

// `.dict-export-row`: 16px label, 导出 button (min-width 68px), divider on top.
VisualPtr MakeExportRow(const std::wstring &label, std::function<void()> onExport, float minHeight, bool topBorder);

// Reads a dictionary file like readDictionaryFile(): UTF-8 / UTF-16 BOMs, strict UTF-8,
// then GB18030. Empty optional when the user cancelled; throws nothing.
std::optional<std::wstring> PickAndReadTextFile(HWND owner, bool allowYaml, bool &readFailed);
// Saves `content` as UTF-8 with a BOM, like the page's download. False when cancelled or failed.
bool SaveTextFile(HWND owner, const std::wstring &filename, const std::string &content);

// The order the visible list is sorted in. It decides where a successful write should leave the
// pager: only a list ordered by updated_at moves the written row to the front.
enum class ListOrder
{
    ByUpdatedAt,
    ByWeight,
};
size_t OffsetAfterWrite(const std::string &action, size_t offset, ListOrder order);
size_t LastPageOffset(size_t total, size_t pageSize);
std::wstring PageStatus(size_t offset, size_t count, size_t total);

// 五笔标签页的词库 id 跟随配置 input.wubi_schema：wubi86 → "wubi"（沿用原来的 id），wubi98 → "wubi98"，
// wubixsj → "wubixsj"；不认识的值按 86。
std::string WubiDictionaryId(const std::string &wubiSchema);
bool IsWubiDictionaryId(const std::string &dictionary);
// 「86 五笔词库」「98 五笔词库」「新世纪五笔词库」；不是五笔词库 id 时为空。
std::wstring WubiDictionaryLabel(const std::string &dictionary);

// Toolbar + hint + table + pagination of a `.dict-card`, with the dictionaryRequest client
// behind it (modules/dict.ts and the quick-phrase half of modules/tools-settings.ts).
class DictionaryPanel : public std::enable_shared_from_this<DictionaryPanel>
{
  public:
    enum class Kind
    {
        Dictionary, // quanpin / wubi / english, requestId prefix "dict-"
        Quick,      // quick phrases, requestId prefix "quick-"
        Seed,       // read-only entries of one seed dictionary, requestId prefix "seed-"
    };

    DictionaryPanel(PageContext &context, Kind kind);

    // Adds toolbar, hint, header, table and pager to `column`.
    void AddTo(Column &column);
    // dict.ts updateMode(): switches quanpin / wubi / english.
    void SetDictionary(const std::string &dictionary);
    const std::string &Dictionary() const
    {
        return dictionary_;
    }
    void SetSourceId(const std::string &sourceId);
    // 批量导入 for the current dictionary (`import`) or 纯汉字导入 (`importHans`).
    void Import(const std::string &action);
    // 导入其他词库：按 `format`（foreign_dictionary.h 的格式标识）选文件，交给宿主读取并
    // 导入拼音用户词库。导入进行中再点只提示，不重复提交。
    void ImportForeign(const std::string &format, const std::string &name = {});
    void Export(const std::string &dictionary);
    // Called when the current dictionary changes, e.g. to retitle the hans import card.
    void SetOnModeChanged(std::function<void()> handler);

  private:
    struct Entry
    {
        std::string code;
        std::string word;
        std::string display;
        int64_t weight = 0;
        bool hasWeight = false;
    };
    struct Pending
    {
        std::string action;
        std::string dictionary;
        std::string sourceId;
    };

    std::string Post(const std::string &action, Json data);
    void Query(size_t offset, bool fromInput);
    void PostQuery(size_t offset);
    void RefreshQuery(const std::string &action);
    // Loads the default view the first time the page is shown. The page builder must not query:
    // the settings indexer builds every page up front (native_settings_window.cpp).
    void EnsureLoaded();
    void OpenDialog(const Entry *row);
    void SaveDialog();
    // 勾选状态变了：表头全选框、删除按钮和条数都要跟着走。
    void SyncSelection();
    void DeleteSelected();
    void OnResponse(const Json &message);
    void RenderRows();
    void SyncPager();
    std::wstring EmptyText() const;
    bool ExactMatch() const;
    std::array<std::wstring, 4> HeaderLabels() const;

    PageContext &context_;
    Kind kind_ = Kind::Dictionary;
    std::string dictionary_;
    std::string sourceId_;
    std::string prefix_;
    uint64_t counter_ = 0;
    std::map<std::string, Pending> pending_;
    std::string latestQueryId_;
    // 批量删除发出后、响应回来前按住按钮：再点一次会对同一批发第二个请求。
    std::string deletingRequestId_;
    std::wstring lastQuery_;
    std::vector<Entry> rows_;

    // Pager state (utils/dictionary-pagination.ts).
    size_t offset_ = 0;
    size_t total_ = 0;
    bool hasMore_ = false;
    bool loading_ = false;
    bool loaded_ = false;

    // Dialog state.
    std::optional<Entry> editing_;
    bool editingExisting_ = false;
    std::string editingDictionary_;
    std::shared_ptr<DictInput> dialogCode_;
    std::shared_ptr<DictInput> dialogWord_;
    std::shared_ptr<DictInput> dialogWeight_;
    bool dialogOpen_ = false;

    std::shared_ptr<DictInput> search_;
    // Quick phrases only: 前缀匹配 / 精确匹配 inside the search box.
    std::shared_ptr<Dropdown> match_;
    std::shared_ptr<Label> hint_;
    std::shared_ptr<DictTableHeader> header_;
    std::shared_ptr<DictTableBody> body_;
    std::shared_ptr<DictTableScroll> scroll_;
    std::shared_ptr<Label> status_;
    std::shared_ptr<Button> first_;
    std::shared_ptr<Button> previous_;
    std::shared_ptr<Button> next_;
    std::shared_ptr<Button> last_;
    std::shared_ptr<Button> removeSelected_;
    std::function<void()> onModeChanged_;
};
} // namespace native_settings
