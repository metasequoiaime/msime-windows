#pragma once

#include <cstddef>
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

// U 模式候选的读音注解。拆字表用逗号分隔一个字的前两个读音（长 = zhǎng,cháng），
// 候选窗里换成「 / 」更易读；空串仍返回空串，调用方据此不显示注解。
inline std::string ReadingAnnotation(const std::string &reading)
{
    std::string text;
    std::size_t start = 0;
    while (start <= reading.size())
    {
        const std::size_t comma = reading.find(',', start);
        const std::size_t end = comma == std::string::npos ? reading.size() : comma;
        if (end > start)
        {
            if (!text.empty())
                text += " / ";
            text.append(reading, start, end - start);
        }
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return text;
}
} // namespace FanyImeIpc
