#pragma once

#include "../../../engine/contracts/paired_punctuation.h"

#include <string>

// 成对标点自动补全的决定：补不补右半边、按到哪个键时跨过已补出的右半边。成对表见
// engine/contracts/paired_punctuation.h。不依赖 TSF 与 Windows 头文件，单独测试见
// windows/tests/custom_punctuation.cpp。
//
// keyIsCustom 表示这个键由自定义标点接管：它出的符号由用户指定，不走默认引号键的左右轮换。

// 按下的键能跨过的右半边；不能时为 0。默认映射下的 " 与 ' 两侧共用一个键，解析出的字符说明不了
// 用户想闭合，所以由键本身回答。
inline wchar_t PairedStepOverCandidate(wchar_t key, bool keyIsCustom, const std::wstring &resolved)
{
    if (resolved.size() != 1)
        return 0;
    if (!keyIsCustom && key == L'"')
        return L'”';
    if (!keyIsCustom && key == L'\'')
        return L'’';
    return FanyImePairedPunctuation::IsClosing(resolved[0]) ? resolved[0] : 0;
}

// 自定义标点接管的键再按一次：它出的是成对表的左半边，栈顶那一对正是这个键补出来的
// （openedByKey 是补出栈顶那一对的键，pendingClosing 是它补出的右半边）时，跨过那个右半边；否则为 0。
inline wchar_t PairedStepOverOwnClosing(wchar_t key, bool keyIsCustom, const std::wstring &resolved,
                                        wchar_t openedByKey, wchar_t pendingClosing)
{
    if (!keyIsCustom || resolved.size() != 1 || key != openedByKey)
        return 0;
    const wchar_t closing = FanyImePairedPunctuation::ClosingFor(resolved[0]);
    return closing != 0 && closing == pendingClosing ? closing : 0;
}

struct PairedCommit
{
    std::wstring text;
    // 补出的右半边；没有补全时为 0。
    wchar_t closing = 0;
};

// 成对补全生效（开关开着且宿主没被排除）时上屏的文本。默认引号键每次都开一对新的，不跟随左右轮换。
inline PairedCommit ComposePairedCommit(bool pairedActive, wchar_t key, bool keyIsCustom, std::wstring text)
{
    PairedCommit commit;
    commit.text = std::move(text);
    if (!pairedActive || commit.text.empty())
        return commit;
    wchar_t &last = commit.text.back();
    if (!keyIsCustom && key == L'"' && last == L'”')
        last = L'“';
    else if (!keyIsCustom && key == L'\'' && last == L'’')
        last = L'‘';
    commit.closing = FanyImePairedPunctuation::ClosingFor(last);
    if (commit.closing != 0)
        commit.text.push_back(commit.closing);
    return commit;
}
