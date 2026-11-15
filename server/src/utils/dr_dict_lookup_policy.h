#pragma once

// DR Dict 词典联动的纯函数部分：查询词的整理、WM_COPYDATA 负载和 --lookup 命令行参数。
// 与 Win32 副作用（找窗口、发消息、启动进程）分开放，便于不依赖 DR Dict 单独测试。
//
// 协议来自 DR Dict（app/src/popup.h、docs/architecture.md「供其他程序调用」）：
//   - 按窗口类 DrDictPopup 找到弹窗宿主，发 WM_COPYDATA，dwData = 0x4B4C5244（"DRLK"），
//     数据是 UTF-8 JSON {"word": "...", "x": 屏幕 x, "y": 屏幕 y}，弹窗在该点旁打开；
//   - DR Dict 没在运行时用 `dr-dict.exe --lookup <词>` 启动，dr-dict.exe 经它自己登记的 App Paths 找到。
//   - 键盘唤起（查词快捷键）时另带 "caret": [l, t, r, b]（光标所在行，弹窗像候选窗一样开在它下方）
//     和 "keyboard": true（弹窗不按鼠标位置自动关闭）；命令行对应 --caret l,t,r,b 与 --keyboard。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace DrDictLookup
{
// 屏幕物理像素的矩形，与 Win32 RECT 同义；单独定义是为了这个头不依赖 <windows.h>。
struct CaretRect
{
    long left = 0;
    long top = 0;
    long right = 0;
    long bottom = 0;
};

// 弹窗开在哪：point 是鼠标位置（右键菜单），或拿不到光标时快捷键退回的鼠标位置；caret 有值时
// DR Dict 以它为准。keyboard 表示由快捷键唤起。
struct Placement
{
    long x = 0;
    long y = 0;
    std::optional<CaretRect> caret;
    bool keyboard = false;
};

// 光标锚点（TSF 报来的是光标所在行的左下角）换成行矩形。行高不知道，按候选窗翻到光标上方时
// 用的同一个 24 DIP 估算（window_utils.cpp 的 AdjustCandidateWindowPosition），scale 是该显示器的缩放。
inline CaretRect CaretRectFromAnchor(long x, long bottom, double scale)
{
    const long line = static_cast<long>(24.0 * (scale > 0.0 ? scale : 1.0) + 0.5);
    return CaretRect{x, bottom - line, x + 1, bottom};
}
inline constexpr wchar_t kPopupWindowClass[] = L"DrDictPopup";
inline constexpr std::uintptr_t kLookupRequest = 0x4B4C5244; // "DRLK"
inline constexpr wchar_t kExecutable[] = L"dr-dict.exe";
// 查询词的上限（UTF-16 码元）。候选和预编辑都远短于此；只是防止异常长串整段塞进命令行和消息里。
inline constexpr std::size_t kMaxWordLength = 256;

// 去掉首尾空白和控制字符，过长截断（不拆开代理对）。返回空串表示没有可查的内容。
inline std::wstring NormalizeWord(const std::wstring &word)
{
    auto blank = [](wchar_t c) { return c <= 0x20 || c == 0x7F || c == 0x3000 || c == 0xFEFF; };
    std::size_t begin = 0;
    std::size_t end = word.size();
    while (begin < end && blank(word[begin]))
        ++begin;
    while (end > begin && blank(word[end - 1]))
        --end;
    std::wstring result;
    result.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i)
    {
        const wchar_t c = word[i];
        // 中间的控制字符（换行、制表符等）换成空格；DR Dict 按一行查。
        result.push_back(c < 0x20 || c == 0x7F ? L' ' : c);
    }
    if (result.size() > kMaxWordLength)
    {
        std::size_t cut = kMaxWordLength;
        if (cut > 0 && result[cut - 1] >= 0xD800 && result[cut - 1] <= 0xDBFF)
            --cut;
        result.resize(cut);
    }
    return result;
}

// 预编辑查询用的文字：造词时已选的前缀 + 剩余的原始按键。R 模式（临时日语）的原始串开头有一个
// 只用于显示的 'R'，查询时去掉。
inline std::wstring PreeditLookupText(const std::wstring &prefix_text, const std::wstring &raw, bool r_mode_prefix)
{
    std::wstring rest = raw;
    if (r_mode_prefix && !rest.empty() && rest.front() == L'R')
        rest.erase(0, 1);
    return NormalizeWord(prefix_text + rest);
}

// UTF-16 -> UTF-8，孤立的代理项换成 U+FFFD，保证 DR Dict 收到的是合法 UTF-8。
inline std::string ToUtf8(const std::wstring &text)
{
    std::string out;
    out.reserve(text.size() * 3);
    auto put = [&out](std::uint32_t cp) {
        if (cp < 0x80)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    };
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const std::uint32_t c = static_cast<std::uint16_t>(text[i]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size())
        {
            const std::uint32_t low = static_cast<std::uint16_t>(text[i + 1]);
            if (low >= 0xDC00 && low <= 0xDFFF)
            {
                put(0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00));
                ++i;
                continue;
            }
        }
        put(c >= 0xD800 && c <= 0xDFFF ? 0xFFFD : c);
    }
    return out;
}

// {"word": "...", "x": 1, "y": 2[, "caret": [l, t, r, b]][, "keyboard": true]}，UTF-8。
inline std::string BuildCopyDataPayload(const std::wstring &word, const Placement &placement)
{
    std::string escaped;
    for (const char c : ToUtf8(word))
    {
        switch (c)
        {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                static constexpr char kHex[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0xF]);
                escaped.push_back(kHex[static_cast<unsigned char>(c) & 0xF]);
            }
            else
            {
                escaped.push_back(c);
            }
        }
    }
    std::string payload =
        "{\"word\":\"" + escaped + "\",\"x\":" + std::to_string(placement.x) + ",\"y\":" + std::to_string(placement.y);
    if (placement.caret)
    {
        const CaretRect &c = *placement.caret;
        payload += ",\"caret\":[" + std::to_string(c.left) + "," + std::to_string(c.top) + "," +
                   std::to_string(c.right) + "," + std::to_string(c.bottom) + "]";
    }
    if (placement.keyboard)
        payload += ",\"keyboard\":true";
    return payload + "}";
}

// 一个命令行参数，按 CommandLineToArgvW 的规则加引号：内部引号及其前面的反斜杠转义。
inline std::wstring QuoteCommandLineArgument(const std::wstring &text)
{
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : text)
    {
        if (c == L'\\')
        {
            ++backslashes;
            quoted += c;
            continue;
        }
        if (c == L'"')
            quoted.append(backslashes + 1, L'\\');
        backslashes = 0;
        quoted += c;
    }
    quoted.append(backslashes, L'\\');
    return quoted + L"\"";
}

inline std::wstring BuildLaunchArguments(const std::wstring &word, const Placement &placement)
{
    std::wstring arguments = L"--lookup " + QuoteCommandLineArgument(word);
    if (placement.caret)
    {
        const CaretRect &c = *placement.caret;
        arguments += L" --caret " + std::to_wstring(c.left) + L"," + std::to_wstring(c.top) + L"," +
                     std::to_wstring(c.right) + L"," + std::to_wstring(c.bottom);
    }
    if (placement.keyboard)
        arguments += L" --keyboard";
    return arguments;
}
} // namespace DrDictLookup
