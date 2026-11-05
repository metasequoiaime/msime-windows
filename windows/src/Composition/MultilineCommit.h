#pragma once

#include <cstddef>
#include <string>
#include <vector>

// 多行文本按行提交时的片段序列：每行正文一次插入，行间用 CRLF。调用方按顺序写进文档，
// 最后一个是最后一行正文（文本以换行结尾时为空串）。
//
// 分行提交是 Chromium 系宿主（QQ、Electron 应用）的要求：整段含换行的文本一次写进文档
// 会被重排，行尾标点聚到末尾。拆分逻辑放在这里，不依赖 TSF，便于单测。
inline std::vector<std::wstring> MultilineCommitSegments(const std::wstring &text)
{
    std::vector<std::wstring> segments;
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t newline = text.find(L'\n', start);
        const std::size_t end = newline == std::wstring::npos ? text.size() : newline;
        segments.push_back(text.substr(start, end - start));
        if (newline == std::wstring::npos)
        {
            break;
        }
        segments.push_back(L"\r\n");
        start = newline + 1;
    }
    return segments;
}
