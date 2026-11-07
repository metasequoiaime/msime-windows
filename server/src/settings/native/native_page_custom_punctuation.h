#pragma once

#include "settings/native/native_settings_pages.h"

#include "engine/contracts/custom_punctuation.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// 输入页「自定义标点」：总开关下面一张表，每行「是否按 Shift + 哪个标点键 → 出什么符号」。
// 读写 input.custom_punctuation 与 input.custom_punctuation_map，元素格式见 engine/contracts/custom_punctuation.h。
namespace native_settings
{
// 第二个下拉框的候选符号，去重，依次是：符号目录里 parent_category 为 '标点' 的符号（一个都没有时用
// 'Punctuation'，按 sort_order，超过符号长度上限的不列）；成对表 29 对的左右两半；默认中文标点映射能输出的
// 符号；键盘的 32 个半角标点。库打不开或两种分类都没有时只有后三样。
std::vector<std::wstring> LoadPunctuationSymbolCatalog(const std::filesystem::path &othersDb);

// 第二个下拉框的选项。成对表里的一对合成一项（值是左半边，标签是两半），左半边在目录里时右半边不再单列。
// 值是 UTF-8 符号。
std::vector<DropdownOption> BuildSymbolOptions(const std::vector<std::wstring> &catalog);
// 已选符号在第二个下拉框里显示成什么：成对表的左半边显示成一对，其余显示它自己。
std::wstring SymbolLabel(const std::string &symbol);

struct CustomPunctuationRow
{
    FanyImeCustomPunctuation::Trigger trigger;
    // UTF-8；空表示还没选。
    std::string symbol;
};

// 表格的内容。键和符号都选好的行才写进配置；同一触发键不能出现在两行。
class CustomPunctuationRows
{
  public:
    // 从配置元素载入；不合法的元素丢掉，同一触发键留先出现的。
    void Load(const std::vector<std::string> &entries);
    const std::vector<CustomPunctuationRow> &Rows() const
    {
        return rows_;
    }
    bool CanAdd() const;
    // 末尾加一行空行；已满时不加。
    void Add();
    void Remove(size_t row);
    // 改成这个键或这个修饰键。会和别的行触发键相同时不改，返回那一行的下标。
    std::optional<size_t> SetKey(size_t row, char key);
    std::optional<size_t> SetShift(size_t row, bool shift);
    void SetSymbol(size_t row, std::string symbol);
    // 键和符号都选好的行，格式化成配置元素，按表格顺序。
    std::vector<std::string> Entries() const;

  private:
    std::optional<size_t> ConflictWith(size_t row, FanyImeCustomPunctuation::Trigger trigger) const;

    std::vector<CustomPunctuationRow> rows_;
};

class CustomPunctuationEditor;

// 总开关与表格，放在「成对标点自动补全」正下方。catalog 在表格第一次显示时调用一次（总开关关着时不调用），
// 之后沿用它的结果。
VisualPtr BuildCustomPunctuationSection(PageContext &context, std::function<std::vector<std::wstring>()> catalog,
                                        std::shared_ptr<CustomPunctuationEditor> *editorOut = nullptr);

// 表格的控件。测试用下面的访问函数找到控件，再用真实的鼠标与键盘消息操作它们。
class CustomPunctuationEditor
{
  public:
    struct RowControls
    {
        std::shared_ptr<Button> record;
        std::shared_ptr<Button> clear;
        std::shared_ptr<Dropdown> key;
        std::shared_ptr<Dropdown> symbol;
        std::shared_ptr<Button> remove;
    };

    // 建好编辑器并接上配置：之后每份快照都同步表格，编辑器释放后不再同步。
    static std::shared_ptr<CustomPunctuationEditor> Create(PageContext &context,
                                                           std::function<std::vector<std::wstring>()> catalog);

    VisualPtr View() const;
    // 总开关。
    const std::shared_ptr<ToggleSwitch> &Toggle() const;
    // 表格（总开关关着时隐藏）。
    const std::shared_ptr<Hideable> &Table() const;
    size_t VisibleRowCount() const;
    RowControls Controls(size_t row) const;
    const std::shared_ptr<Button> &AddButton() const;
    bool IsRecording(size_t row) const;
    // 冲突提示；没有时为空串。
    std::wstring Status() const;
    const CustomPunctuationRows &Rows() const
    {
        return rows_;
    }

  private:
    struct Slot;

    CustomPunctuationEditor(PageContext &context, std::function<std::vector<std::wstring>()> catalog);

    void Sync();
    void Refresh();
    // 在表格末尾加一行空行，不写配置；键和符号都填了才由 Commit 写。
    void AddBlankRow();
    // 使用者点开总开关时调用：表格一行都没有就加一行空行并显示出来。
    void AddBlankRowIfEmpty();
    void RefreshSymbolOptions();
    void Commit();
    void ShowConflict(size_t row, size_t other);
    void ClearStatus();
    void StartRecording(size_t row);
    void StopRecording();
    bool OnRecordKey(size_t row, WPARAM key);

    PageContext &context_;
    std::function<std::vector<std::wstring>()> catalogSource_;
    std::vector<std::wstring> catalog_;
    bool catalogLoaded_ = false;
    // 第二个下拉框的选项已经列过。
    bool optionsListed_ = false;
    CustomPunctuationRows rows_;
    std::vector<std::string> lastEntries_;
    bool synced_ = false;
    std::optional<size_t> recording_;
    std::shared_ptr<Column> view_;
    std::shared_ptr<ToggleSwitch> toggle_;
    std::shared_ptr<Hideable> table_;
    std::vector<std::shared_ptr<Slot>> slots_;
    std::shared_ptr<Label> status_;
    std::shared_ptr<Hideable> statusBox_;
    std::shared_ptr<Button> add_;
};
} // namespace native_settings
