#include "Composition/MultilineCommit.h"

#include <string>
#include <vector>

namespace
{
bool Matches(const std::vector<std::wstring> &actual, const std::vector<std::wstring> &expected)
{
    return actual == expected;
}
} // namespace

int main()
{
    // Do not use assert: Release builds must execute these checks too.

    // 单行：整段一次写入。
    if (!Matches(MultilineCommitSegments(L"你好"), {L"你好"}))
    {
        return 1;
    }

    // 两行：正文、CRLF、正文。行间必须是 CRLF，文档里的换行就是它。
    if (!Matches(MultilineCommitSegments(L"A\nB"), {L"A", L"\r\n", L"B"}))
    {
        return 2;
    }

    // 文本以换行结尾：最后一段是空正文，保证末尾的换行确实被写进去。
    if (!Matches(MultilineCommitSegments(L"A\n"), {L"A", L"\r\n", L""}))
    {
        return 3;
    }

    // 中间的空行：空正文 + CRLF 连续出现，不能把两行合并。
    if (!Matches(MultilineCommitSegments(L"A\n\nB"), {L"A", L"\r\n", L"", L"\r\n", L"B"}))
    {
        return 4;
    }

    // 只有换行：一个空正文、一个 CRLF、再一个空正文。
    if (!Matches(MultilineCommitSegments(L"\n"), {L"", L"\r\n", L""}))
    {
        return 5;
    }

    // 空文本：仍返回一段空正文，调用方写入空串是无操作，不必特判。
    if (!Matches(MultilineCommitSegments(L""), {L""}))
    {
        return 6;
    }

    // 多行中文，确认正文内容不丢、顺序不变。
    if (!Matches(MultilineCommitSegments(L"静夜思\n床前明月光，\n低头思故乡。"),
                 {L"静夜思", L"\r\n", L"床前明月光，", L"\r\n", L"低头思故乡。"}))
    {
        return 7;
    }

    // 片段拼回去应当等于原文（把 CRLF 还原成 \n），保证没有吞字或重排。
    const std::wstring source = L"第一行\n第二行\n\n第四行";
    std::wstring rebuilt;
    for (const std::wstring &segment : MultilineCommitSegments(source))
    {
        rebuilt += segment == L"\r\n" ? L"\n" : segment;
    }
    if (rebuilt != source)
    {
        return 8;
    }

    return 0;
}
