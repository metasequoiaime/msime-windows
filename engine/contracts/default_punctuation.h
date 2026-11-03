#pragma once

#include <string>
#include <vector>

// 中文标点状态下的默认映射：TSF 按它出符号，设置页把它能输出的符号列进自定义标点的候选。
// 查找顺序与交替对、嵌套对的状态由 TSF（windows/src/Composition/PunctuationKeyMap.cpp）负责。
namespace FanyImeDefaultPunctuation
{
struct SimpleMapping
{
    wchar_t key;
    const wchar_t *text;
};

// 简单表：一个键固定出一段文本。\ 与 / 先按顿号设置（engine/contracts/enumeration_comma.h）判定。
// < 与 > 不在这里：它们是嵌套对，列在这里会挡住嵌套对，《》里永远出不了〈〉。
inline constexpr SimpleMapping kSimpleTable[] = {
    {L'`', L"·"},  {L'~', L"~"},  {L'!', L"！"}, {L'@', L"@"},  {L'#', L"#"},  {L'$', L"￥"},
    {L'%', L"%"},  {L'^', L"……"}, {L'&', L"&"},  {L'*', L"*"},  {L'(', L"（"}, {L')', L"）"},
    {L'_', L"——"}, {L'[', L"【"}, {L']', L"】"}, {L'{', L"{"},  {L'}', L"}"},  {L'\\', L"、"},
    {L';', L"；"}, {L':', L"："}, {L',', L"，"}, {L'.', L"。"}, {L'?', L"？"},
};

// 交替对（" '）与嵌套对（< >）能出的符号，以及顿号。
inline constexpr const wchar_t *kStatefulOutputs[] = {L"“", L"”", L"‘", L"’", L"《", L"〈", L"〉", L"》", L"、"};

// 默认映射能输出的全部符号，去重，按简单表、再交替对与嵌套对的顺序。
inline std::vector<std::wstring> AllOutputs()
{
    std::vector<std::wstring> outputs;
    const auto add = [&outputs](const wchar_t *text) {
        for (const std::wstring &existing : outputs)
        {
            if (existing == text)
                return;
        }
        outputs.emplace_back(text);
    };
    for (const SimpleMapping &mapping : kSimpleTable)
        add(mapping.text);
    for (const wchar_t *text : kStatefulOutputs)
        add(text);
    return outputs;
}
} // namespace FanyImeDefaultPunctuation
