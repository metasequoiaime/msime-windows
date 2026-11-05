#pragma once

#include <string>

namespace FanyImeIpc
{
// 多行快捷短语在候选窗、展开面板和 UI-less 候选串里只显示第一行，末尾补 ↵ 表示还有内容。
// 纯展示函数：上屏读的是 page_words 里的原文，不要拿这个结果去提交。
inline std::string QuickPhraseDisplayText(const std::string &text)
{
    const auto newline = text.find('\n');
    if (newline == std::string::npos)
        return text;
    return text.substr(0, newline) + " \xE2\x86\xB5";
}

inline std::wstring QuickPhraseDisplayText(const std::wstring &text)
{
    const auto newline = text.find(L'\n');
    if (newline == std::wstring::npos)
        return text;
    return text.substr(0, newline) + L" \u21B5";
}
} // namespace FanyImeIpc
