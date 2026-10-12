#include "dictionary_validation.h"

#include "ipc/ipc_protocol_limits.h"
#include "utils/common_utils.h"
#include "engine/quanpin/quanpin_utils.h"

#include <utf8.h>
#include <algorithm>
#include <cctype>
#include <vector>

namespace SettingsDictionary::Validation
{
namespace
{
// 五笔码表里一个字可能有多个 4 级全码（实测 468 个字）。取字典序第一个即可：
// 全部 62662 条多字词条里，含「前 2 位互相冲突」的字的有 0 条，消歧是为不存在的问题写代码。
const std::string &PreferredWubiCode(const WubiCharCodes &char_codes, const std::string &ch)
{
    static const std::string kEmpty;
    const auto found = char_codes.find(ch);
    return found == char_codes.end() ? kEmpty : found->second;
}
} // namespace

std::string ComposeWubiPhraseCode(const std::string &word, const WubiCharCodes &char_codes)
{
    if (word.empty())
        return {};

    // 按码位切分，不能按字节：扩展区汉字一个码位 3~4 字节，按字节切会切出半个字。
    // utf8::next 按引用推进 it 并返回码位，所以要先记下起点再取 [start, it) 这一段。
    std::vector<std::string> chars;
    try
    {
        auto it = word.begin();
        while (it != word.end())
        {
            const auto start = it;
            utf8::next(it, word.end());
            chars.emplace_back(start, it);
        }
    }
    catch (...)
    {
        return {};
    }

    std::string code;
    const auto take = [&](size_t index, size_t width) {
        const std::string &full = PreferredWubiCode(char_codes, chars[index]);
        if (full.empty())
            return false;
        // 多字词要求该位恰好取到 width 个字母。取不满说明码表里这个字的码太短，硬拼会
        // 得到一个短一位的 key——它能查得到、能上屏，但永远凑不满 has_complete_code()
        // 要求的 4 位。宁可让这一步失败，也不能把这样的码写进 wubi86。
        // 单字传 npos 表示「全取」，不适用这条约束。
        if (width != std::string::npos && full.size() < width)
            return false;
        code.append(full, 0, width);
        return true;
    };

    const size_t n = chars.size();
    if (n == 1)
    {
        if (!take(0, std::string::npos))
            return {};
        return code;
    }

    bool ok = false;
    if (n == 2)
    {
        ok = take(0, 2) && take(1, 2);
    }
    else if (n == 3)
    {
        ok = take(0, 1) && take(1, 1) && take(2, 2);
    }
    else
    {
        // 4 字以上取首、次、三、末——末字是最后一个字，不是第 4 个字。
        // 5~8 字词实测 100% 命中这条规则。
        ok = take(0, 1) && take(1, 1) && take(2, 1) && take(n - 1, 1);
    }
    return ok ? code : std::string{};
}

bool NormalizeFullPinyin(const std::string &input, quanpin::Segments &segments, std::string &normalized,
                         std::size_t expected_syllables)
{
    std::string source = input;
    source.erase(std::remove_if(source.begin(), source.end(), [](unsigned char ch) { return std::isspace(ch); }),
                 source.end());
    std::transform(source.begin(), source.end(), source.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (source.empty() || source.front() == '\'' || source.back() == '\'' || source.find("''") != std::string::npos)
    {
        return false;
    }

    if (source.find('\'') != std::string::npos)
    {
        segments = quanpin::split_segments(source);
    }
    else
    {
        const auto cuts = quanpin::cut_pinyin_by_mode(source, "correction", true);
        if (cuts.empty())
            return false;
        segments = cuts.front();
        if (expected_syllables != 0 && segments.size() != expected_syllables)
        {
            const auto alternatives = quanpin::enumerate_complete_segmentations(quanpin::build_syllable_graph(source));
            const auto match = std::find_if(
                alternatives.begin(), alternatives.end(),
                [expected_syllables](const quanpin::Segments &cut) { return cut.size() == expected_syllables; });
            if (match != alternatives.end())
                segments = *match;
        }
    }

    const auto &valid = quanpin::intact_pinyin_set();
    if (segments.empty() || !std::all_of(segments.begin(), segments.end(), [&valid](const std::string &segment) {
            return !segment.empty() && valid.find(segment) != valid.end();
        }))
    {
        return false;
    }

    normalized = quanpin::join_segments(segments);
    std::string without_delimiters = normalized;
    without_delimiters.erase(std::remove(without_delimiters.begin(), without_delimiters.end(), '\''),
                             without_delimiters.end());
    std::string source_without_delimiters = source;
    source_without_delimiters.erase(
        std::remove(source_without_delimiters.begin(), source_without_delimiters.end(), '\''),
        source_without_delimiters.end());
    return without_delimiters == source_without_delimiters;
}

bool ShouldSkipImportLine(const std::string &line, bool &in_yaml_header)
{
    const auto begin = line.find_first_not_of(" \t");
    if (begin == std::string::npos)
        return true;
    const auto end = line.find_last_not_of(" \t");
    const auto size = end - begin + 1;
    if (size == 3 && line.compare(begin, 3, "---") == 0)
    {
        in_yaml_header = true;
        return true;
    }
    if (size == 3 && line.compare(begin, 3, "...") == 0)
    {
        in_yaml_header = false;
        return true;
    }
    if (in_yaml_header)
        return true;
    return line[begin] == '#';
}

bool ParseCodedImportLine(const std::string &line, std::string &word, std::string &code, int &weight,
                          std::string &message, int default_weight)
{
    const auto trim = [](std::string value) {
        const auto begin = value.find_first_not_of(' ');
        if (begin == std::string::npos)
            return std::string{};
        const auto end = value.find_last_not_of(' ');
        return value.substr(begin, end - begin + 1);
    };

    std::vector<std::string> fields;
    std::string::size_type start = 0;
    for (;;)
    {
        const auto separator = line.find('\t', start);
        fields.push_back(trim(line.substr(start, separator == std::string::npos ? separator : separator - start)));
        if (separator == std::string::npos)
            break;
        start = separator + 1;
    }
    if (fields.size() != 2 && fields.size() != 3)
    {
        message = "格式错误，应为：词语<Tab>编码[<Tab>权重]";
        return false;
    }

    word = fields[0];
    code = fields[1];
    if (word.empty() || code.empty())
    {
        message = "词语和编码不能为空";
        return false;
    }
    if (fields.size() == 2)
    {
        weight = default_weight;
        return true;
    }

    const std::string &weight_text = fields[2];
    if (weight_text.empty() || weight_text.find('=') != std::string::npos)
    {
        weight = default_weight;
        return true;
    }
    if (!std::all_of(weight_text.begin(), weight_text.end(), [](unsigned char ch) { return std::isdigit(ch); }))
    {
        message = "权重必须是非负整数";
        return false;
    }
    try
    {
        weight = std::stoi(weight_text);
        return true;
    }
    catch (...)
    {
        message = "权重数值无效";
        return false;
    }
}

bool QuickPhraseFitsNamedPipe(const std::string &phrase)
{
    try
    {
        return string_to_wstring(phrase).size() <= FanyImePipeLimits::CandidateTextMaxLength;
    }
    catch (...)
    {
        return false;
    }
}
} // namespace SettingsDictionary::Validation
