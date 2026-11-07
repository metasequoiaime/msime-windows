#include "translation_gloss.h"

#include <utf8.h>
#include <algorithm>
#include <cstdint>
#include <cctype>

namespace CloudTranslation
{
namespace
{
constexpr size_t kMaxPersistChars = 32;
constexpr auto kFirstRetryDelay = std::chrono::seconds(15);
constexpr auto kMaxRetryDelay = std::chrono::minutes(8);

bool IsAsciiSpace(unsigned char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::string CollapseWhitespace(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;
    for (const unsigned char ch : text)
    {
        if (IsAsciiSpace(ch))
        {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space)
        {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

std::string AsciiLower(std::string value)
{
    for (char &ch : value)
    {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    return value;
}

bool IsHanCodepoint(uint32_t cp)
{
    return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x20000 && cp <= 0x2CEAF) || cp == 0x3007;
}

bool IsEmojiOrPictograph(uint32_t cp)
{
    if (cp == 0x200D || cp == 0xFE0E || cp == 0xFE0F || cp == 0x20E3)
        return true;
    if (cp >= 0x2600 && cp <= 0x27BF)
        return true;
    if (cp >= 0x1F000 && cp <= 0x1FAFF)
        return true;
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF)
        return true;
    return false;
}
} // namespace

std::chrono::seconds FailureRetryDelay(unsigned failures)
{
    if (failures == 0)
        return std::chrono::seconds(0);
    // 15s << 5 正好是 8 分钟，再往上移位只会被封顶，也避免大次数移位溢出。
    const unsigned shift = (std::min)(failures - 1, 5u);
    return (std::min)(std::chrono::seconds(kFirstRetryDelay * (1u << shift)),
                      std::chrono::duration_cast<std::chrono::seconds>(kMaxRetryDelay));
}

size_t Utf8Length(const std::string &text)
{
    if (text.empty() || !utf8::is_valid(text.begin(), text.end()))
        return text.size();
    return static_cast<size_t>(utf8::distance(text.begin(), text.end()));
}

std::string FormatGloss(const std::string &text)
{
    std::string collapsed = CollapseWhitespace(text);
    if (collapsed.empty() || !utf8::is_valid(collapsed.begin(), collapsed.end()))
        return {};
    return collapsed;
}

bool ShouldPersistGloss(const std::string &key, const std::string &formatted, size_t max_chars)
{
    if (formatted.empty() || key.empty())
        return false;
    if (Utf8Length(formatted) > max_chars)
        return false;
    return AsciiLower(formatted) != AsciiLower(key);
}

bool IsUsableSecret(const std::string &value)
{
    const std::string trimmed = TrimSecret(value);
    if (trimmed.empty())
        return false;
    // Same rule as VoiceInput::IsPlaceholderToken, and same reason: config.default.toml ships
    // "<YOUR_TENCENT_SECRET_ID>", which the old "<YOUR_OWN_" prefix test accepted as a real secret. With
    // tencent_tmt.enabled defaulting to true that made a fresh install send candidate text to
    // tmt.tencentcloudapi.com signed with the placeholder.
    if (trimmed.front() == '<' && trimmed.back() == '>')
        return false;
    return trimmed.find("FAKESECRET_") != 0;
}

std::string TrimSecret(const std::string &value)
{
    size_t begin = 0;
    while (begin < value.size() && IsAsciiSpace(static_cast<unsigned char>(value[begin])))
        ++begin;
    size_t end = value.size();
    while (end > begin && IsAsciiSpace(static_cast<unsigned char>(value[end - 1])))
        --end;
    return value.substr(begin, end - begin);
}

bool IsCloudTranslatableChinese(const std::string &text)
{
    if (text.empty() || !utf8::is_valid(text.begin(), text.end()))
        return false;
    bool has_han = false;
    for (auto it = text.begin(); it != text.end();)
    {
        const uint32_t cp = utf8::next(it, text.end());
        if (IsEmojiOrPictograph(cp))
            return false;
        has_han = has_han || IsHanCodepoint(cp);
    }
    return has_han;
}

bool IsCloudTranslatableEnglish(const std::string &text)
{
    if (text.empty())
        return false;
    bool has_ascii_letter = false;
    for (const unsigned char ch : text)
    {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))
        {
            has_ascii_letter = true;
            continue;
        }
        if (ch != ' ' && ch != '-' && ch != '\'')
            return false;
    }
    return has_ascii_letter;
}
} // namespace CloudTranslation
