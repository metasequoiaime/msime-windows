#pragma once
#include <string>
#include <string_view>
#include <unordered_set>
#include <Windows.h>

namespace Global
{
inline std::wstring ZEN_BROWSER = L"zen.exe";
// inline std::unordered_set<std::wstring> VSCodeSeries = {L"Code.exe", L"Code - Insiders.exe", L"VSCodium.exe"};
// inline bool IsVSCodeLike = false;
inline LONG INVALID_Y = -100000;

// 成对标点自动补全在这些宿主中禁用。Excel 的单元格编辑不接受补全后的光标
// 左移：右半边上屏后光标回到两半之间的动作会被单元格吞掉，表现为焦点单元格
// 往左偏一格。微信输入法同样把 Excel 排除在这个功能之外。
inline const wchar_t *const PAIRED_PUNCTUATION_EXCLUDED_APPS[] = {L"EXCEL.EXE"};

// 起源 2 引擎的 Valve 游戏：BeginUIElement 回 pbShow=FALSE 声称自己画候选，但 imemanager.dll 只替白名单
// （按 profile 名称全等比对）里的输入法画；不在名单里的，组合落到系统默认组合窗（左上角只显示编码的小框），
// 等于没有候选。这些宿主不理会该声明，照常弹我们自己的候选窗——除非设置页开了游戏兼容名称
// （Global::ReadGameCompatProfileName），那时游戏会自己画，见 MetasequoiaIME::ActivateEx。
inline const wchar_t *const UILESS_IGNORED_APPS[] = {L"cs2.exe", L"dota2.exe"};

// 我的世界基岩版：聊天框不向输入法报位置。组合开始前 GetTextExt 给的是工作区右下角，开始后给的是左上角系统
// 默认组合窗里的位置，候选窗每次都从右下角闪到左上角。这些宿主里候选窗固定在所在显示器工作区的左上角。
inline const wchar_t *const CANDIDATE_PINNED_TOP_LEFT_APPS[] = {L"Minecraft.Windows.exe"};

template <size_t N> inline bool IsProcessInList(const std::wstring &processName, const wchar_t *const (&apps)[N])
{
    if (processName.empty())
    {
        return false;
    }
    for (const wchar_t *app : apps)
    {
        if (CompareStringOrdinal(processName.c_str(), -1, app, -1, TRUE) == CSTR_EQUAL)
        {
            return true;
        }
    }
    return false;
}

inline bool IsPairedPunctuationExcludedProcess(const std::wstring &processName)
{
    return IsProcessInList(processName, PAIRED_PUNCTUATION_EXCLUDED_APPS);
}

inline bool IsUiLessIgnoredProcess(const std::wstring &processName)
{
    return IsProcessInList(processName, UILESS_IGNORED_APPS);
}

inline bool IsCandidatePinnedTopLeftProcess(const std::wstring &processName)
{
    return IsProcessInList(processName, CANDIDATE_PINNED_TOP_LEFT_APPS);
}
} // namespace Global

namespace GlobalSettings
{
//
// 支持的 TSF 预编辑格式
//  - raw: 原始按键序列
//  - pinyin: 分词后的拼音序列
//  - empty: 行内不显示预编辑
//  - cand: 当前高亮的候选词序列（预留）
//
namespace TsfPreeditStyle
{
constexpr std::string_view Raw = "raw";
constexpr std::string_view Pinyin = "pinyin";
constexpr std::string_view Empty = "empty";
constexpr std::string_view Cand = "cand";
} // namespace TsfPreeditStyle

inline bool isKnownTsfPreeditStyle(std::string_view style)
{
    return style == TsfPreeditStyle::Raw || style == TsfPreeditStyle::Pinyin || style == TsfPreeditStyle::Empty;
}

inline std::string normalizeTsfPreeditStyle(std::string_view style)
{
    if (style == TsfPreeditStyle::Pinyin || style == TsfPreeditStyle::Empty)
    {
        return std::string(style);
    }
    return std::string(TsfPreeditStyle::Raw);
}

inline std::string &tsfPreeditStyleStorage()
{
    static std::string style = std::string(TsfPreeditStyle::Raw); // 默认的原始按键序列
    return style;
}

inline const std::string &getTsfPreeditStyle()
{
    return tsfPreeditStyleStorage();
}

inline void setTsfPreeditStyle(std::string_view newStyle)
{
    tsfPreeditStyleStorage() = normalizeTsfPreeditStyle(newStyle);
}

inline void setTsfPreeditStyleFromWide(const wchar_t *style)
{
    if (!style)
    {
        setTsfPreeditStyle(TsfPreeditStyle::Raw);
        return;
    }
    if (wcscmp(style, L"pinyin") == 0)
    {
        setTsfPreeditStyle(TsfPreeditStyle::Pinyin);
    }
    else if (wcscmp(style, L"empty") == 0)
    {
        setTsfPreeditStyle(TsfPreeditStyle::Empty);
    }
    else
    {
        setTsfPreeditStyle(TsfPreeditStyle::Raw);
    }
}

inline bool isKnownTsfPreeditStyleWide(const wchar_t *style)
{
    return style && (wcscmp(style, L"raw") == 0 || wcscmp(style, L"pinyin") == 0 || wcscmp(style, L"empty") == 0);
}
} // namespace GlobalSettings

namespace GlobalIme
{
inline thread_local std::wstring word_for_creating_word = L"";
// One-shot override for TSF inline preedit after NeedToCreateWord (pinyin mode).
// Consumed by _HandleCompositionInputWorker, then cleared.
inline thread_local std::wstring pending_create_word_preedit = L"";
} // namespace GlobalIme

namespace Global
{
inline thread_local HWND msgWndHandle = nullptr;
}
