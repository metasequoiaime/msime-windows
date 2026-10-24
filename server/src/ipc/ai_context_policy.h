#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace FanyImeIpc
{
// 一段连续、不含空白的 ASCII 可见字符，只要够长且混了字母、数字、符号中的两类以上，就当作
// 密码、token 或密钥一类的凭证：AI 联想的上下文要发到云端，宁可少带也不能带出去。普通英文
// 单词、单独的数字和短缩写都留着，它们对联想有用，也不像凭证。
inline constexpr std::size_t kCredentialLikeMinLength = 8;

constexpr bool IsCredentialLikeAsciiRun(std::string_view run)
{
    if (run.size() < kCredentialLikeMinLength)
        return false;
    bool letter = false;
    bool digit = false;
    bool symbol = false;
    for (const char ch : run)
    {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))
            letter = true;
        else if (ch >= '0' && ch <= '9')
            digit = true;
        else
            symbol = true;
    }
    return static_cast<int>(letter) + static_cast<int>(digit) + static_cast<int>(symbol) >= 2;
}

// 把上下文里像凭证的 ASCII 串整段去掉，其余字节（包括 UTF-8 的中文）原样保留。
// Header-only pure policy so tests can pin it without linking the server stack.
inline std::string StripCredentialLikeText(std::string_view text)
{
    const auto visible_ascii = [](char ch) { return ch > ' ' && ch < 0x7F; };
    std::string result;
    result.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size())
    {
        if (!visible_ascii(text[i]))
        {
            result.push_back(text[i++]);
            continue;
        }
        std::size_t end = i;
        while (end < text.size() && visible_ascii(text[end]))
            ++end;
        const std::string_view run = text.substr(i, end - i);
        if (!IsCredentialLikeAsciiRun(run))
            result.append(run);
        i = end;
    }
    return result;
}

// 上屏历史在 AI 联想上下文里最多保留的字节数，超出时从开头裁。
inline constexpr std::size_t kAiContextMaxBytes = 1024;

// 配置 input.character_set 取这个值时是繁体模式。
inline constexpr std::string_view kTraditionalCharacterSet = "traditional";

// 从开头裁掉超出 kAiContextMaxBytes 的部分，裁后不以 UTF-8 续字节开头，
// 输入是合法 UTF-8 时，结果最多比上限短 3 个字节。
inline std::string TrimAiContextFront(std::string context)
{
    if (context.size() <= kAiContextMaxBytes)
        return context;
    std::size_t cut = context.size() - kAiContextMaxBytes;
    while (cut < context.size() && (static_cast<unsigned char>(context[cut]) & 0xC0) == 0x80)
        ++cut;
    context.erase(0, cut);
    return context;
}

// AI 候选被选中后，落进用户词库、追加进 AI 上下文的文字。两处都按简体工作，而 AI 可能回繁体。
// 非繁体模式返回 word。繁体模式下 to_simplified 不改动 word 时返回 word；否则满足下面任一条件时
// 返回 to_simplified(word)，都不满足时返回 word：
// - to_traditional(word) == word：word 本来就是繁体（含台湾、香港写法），转回简体；
// - to_traditional(to_simplified(word)) == to_traditional(word)：接住转繁会变的 word（多半是简体），
//   只在转简不改变它的标准繁体显示时才转。简体原文「萧乾」「乾清宫」因此保持原样，不会存成
//   「萧干」「干清宫」。
// 规则不保证繁体显示不变：to_simplified 本身一对多，繁体原文偶尔会转成别的简体字，例如
// 「孫乾」转成「孙干」，之后在繁体模式下显示为「孫幹」。
template <typename ToSimplified, typename ToTraditional>
std::string SimplifiedAiCandidateText(const std::string &word, std::string_view character_set,
                                      ToSimplified &&to_simplified, ToTraditional &&to_traditional)
{
    if (character_set != kTraditionalCharacterSet)
        return word;
    std::string simplified = to_simplified(word);
    if (simplified == word)
        return word;
    const std::string traditional = to_traditional(word);
    if (traditional == word || to_traditional(simplified) == traditional)
        return simplified;
    return word;
}
} // namespace FanyImeIpc
