#pragma once
#include "key_chord.h"
#include <array>
#include <cstddef>

namespace FanyImeInputStateShortcuts
{
using FanyImeKeyChord::Binding;
enum class Action : unsigned
{
    EnglishCandidates = 1,
    FullWidth = 2,
    Punctuation = 3,
    ReconvertEnglish = 4
};
struct Definition
{
    Action action;
    std::string_view key, label, defaultBinding, editor;
};
inline constexpr std::array<Definition, 3> Definitions = {
    {{Action::EnglishCandidates, "toggle_english_candidates", "英文候选", "Ctrl+Shift+KeyE", "englishCandidatesHotkey"},
     {Action::FullWidth, "toggle_full_width", "全角 / 半角", "Ctrl+Shift+Space", "fullWidthHotkey"},
     {Action::Punctuation, "toggle_punctuation", "中英文标点", "Ctrl+Period", "punctuationHotkey"}}};
using Values = std::array<std::string_view, Definitions.size()>;
inline constexpr Values Defaults()
{
    Values values{};
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = Definitions[i].defaultBinding;
    return values;
}
struct Rule
{
    std::string_view binding, label;
};
inline constexpr Rule Reserved[] = {{"Ctrl+Alt+KeyE", "英文单词重选"},
                                    {"Ctrl+Shift+KeyF", "简繁切换"},
                                    {"Ctrl+F9", "语音输入"},
                                    {"Ctrl+Shift+F9", "语音输入"}};
inline constexpr Rule Warnings[] = {{"Ctrl+KeyA", "全选"},
                                    {"Ctrl+KeyC", "复制"},
                                    {"Ctrl+KeyV", "粘贴"},
                                    {"Ctrl+KeyX", "剪切"},
                                    {"Ctrl+KeyZ", "撤销"},
                                    {"Ctrl+KeyY", "重做"},
                                    {"Ctrl+KeyS", "保存"},
                                    {"Ctrl+KeyF", "查找"},
                                    {"Ctrl+KeyH", "替换"},
                                    {"Ctrl+KeyP", "打印"},
                                    {"Ctrl+KeyO", "打开"},
                                    {"Ctrl+KeyN", "新建"},
                                    {"Ctrl+KeyW", "关闭标签页或文档"},
                                    {"Ctrl+F4", "关闭文档"},
                                    {"Ctrl+KeyT", "新建标签页"},
                                    {"Ctrl+KeyL", "定位地址栏"},
                                    {"Ctrl+KeyR", "刷新"},
                                    {"Ctrl+KeyB", "加粗"},
                                    {"Ctrl+KeyI", "斜体"},
                                    {"Ctrl+KeyU", "下划线"},
                                    {"Ctrl+Shift+KeyV", "粘贴为纯文本"},
                                    {"Ctrl+Shift+KeyZ", "重做"},
                                    {"Ctrl+Shift+KeyS", "另存为"},
                                    {"Ctrl+Shift+KeyT", "重新打开关闭的标签页"},
                                    {"Ctrl+Space", "Windows 输入法切换"}};
inline std::string_view Validate(std::size_t index, std::string_view value, const Values &values)
{
    Binding parsed;
    if (!FanyImeKeyChord::Parse(value, parsed))
        return "快捷键格式无效";
    if (value.empty())
        return {};
    for (const auto &rule : Reserved)
        if (value == rule.binding)
            return rule.label;
    for (std::size_t i = 0; i < values.size(); ++i)
        if (i != index && value == values[i])
            return Definitions[i].label;
    return {};
}
struct Snapshot
{
    std::array<Binding, Definitions.size()> bindings{};
    constexpr Snapshot()
    {
        for (std::size_t i = 0; i < bindings.size(); ++i)
            FanyImeKeyChord::Parse(Definitions[i].defaultBinding, bindings[i]);
    }
    Action Match(unsigned code, unsigned modifiers) const
    {
        for (std::size_t i = 0; i < bindings.size(); ++i)
            if (bindings[i].Matches(code, modifiers))
                return Definitions[i].action;
        return static_cast<Action>(0);
    }
};
inline Snapshot Resolve(const Values &values)
{
    Snapshot result;
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        result.bindings[i] = {};
        if (Validate(i, values[i], values).empty())
            FanyImeKeyChord::Parse(values[i], result.bindings[i]);
    }
    return result;
}
} // namespace FanyImeInputStateShortcuts
