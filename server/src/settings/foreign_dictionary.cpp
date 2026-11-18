// 其他输入法词库格式的读取。
//
// 各格式的解析逻辑移植自「深蓝词库转换」imewlconverter（ImeWlConverter.Formats）：
//   https://github.com/studyzy/imewlconverter
//   Copyright studyzy and contributors, licensed under the GNU General Public License v3.0.
//
// 搜狗备份词库（sgpybin）部分在 imewlconverter 中又译自：
//   Sogou-User-Dict-Converter, Copyright h4x3rotab, GPL-3.0
//   rose, Copyright nopdan, GPL-3.0
// LibIME 文本词库的引号转义沿用 fcitx5 stringutils 的规则，二进制词库的 DATrie 布局与
// libime（LGPL-2.1+）的 datrie.cpp / pinyindictionary.cpp 一致。
//
// 本项目同样以 GPL-3.0 发布，见仓库根目录 LICENSE 与 THIRD_PARTY_NOTICES.txt。

#include "settings/foreign_dictionary.h"

#include <windows.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ForeignDictionary
{
namespace
{
// 整份文件读不下去时抛出；what() 是给用户看的中文原因。
struct ParseFailure : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

constexpr const char *kTruncated = "文件不完整或格式不符";
// 解压后的上限：真实词库远小于此，超出只可能是损坏或恶意构造的文件。
constexpr size_t kMaxDecompressedSize = size_t{1} << 30;

// ---------------------------------------------------------------------------------
// 编码
// ---------------------------------------------------------------------------------
std::string WideToUtf8(const wchar_t *text, size_t length)
{
    if (length == 0)
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Utf16LeUnits(std::string_view bytes)
{
    std::wstring wide(bytes.size() / 2, L'\0');
    for (size_t i = 0; i < wide.size(); ++i)
        wide[i] =
            static_cast<wchar_t>(static_cast<uint8_t>(bytes[2 * i]) | (static_cast<uint8_t>(bytes[2 * i + 1]) << 8));
    return wide;
}

// 二进制词库里的 UTF-16LE 字段；定长字段常以 NUL 补齐，一并去掉。
std::string Utf16LeToUtf8(std::string_view bytes)
{
    std::wstring wide = Utf16LeUnits(bytes);
    while (!wide.empty() && wide.back() == L'\0')
        wide.pop_back();
    return WideToUtf8(wide.data(), wide.size());
}

bool MultiByteToUtf8(UINT codepage, std::string_view bytes, bool strict, std::string &out)
{
    out.clear();
    if (bytes.empty())
        return true;
    const DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    const int size = MultiByteToWideChar(codepage, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (size <= 0)
        return false;
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(codepage, flags, bytes.data(), static_cast<int>(bytes.size()), wide.data(), size);
    out = WideToUtf8(wide.data(), wide.size());
    return true;
}

// 文本词库的编码：有 BOM 按 BOM；没有 BOM 时，零字节成片出现的是 UTF-16（QQ、百度导出的
// 文本默认如此），其余先试 UTF-8，不是合法 UTF-8 再按 GBK（搜狗导出的文本默认如此）。
std::string DecodeText(const std::string &bytes)
{
    const std::string_view view(bytes);
    const auto starts = [&view](std::initializer_list<uint8_t> prefix) {
        if (view.size() < prefix.size())
            return false;
        size_t i = 0;
        for (const uint8_t byte : prefix)
            if (static_cast<uint8_t>(view[i++]) != byte)
                return false;
        return true;
    };
    std::string out;
    if (starts({0xEF, 0xBB, 0xBF}))
    {
        MultiByteToUtf8(CP_UTF8, view.substr(3), false, out);
        return out;
    }
    if (starts({0xFF, 0xFE}))
        return Utf16LeToUtf8(view.substr(2));
    if (starts({0xFE, 0xFF}))
    {
        std::string swapped(view.substr(2));
        for (size_t i = 0; i + 1 < swapped.size(); i += 2)
            std::swap(swapped[i], swapped[i + 1]);
        return Utf16LeToUtf8(swapped);
    }

    size_t evenZeros = 0;
    size_t oddZeros = 0;
    for (size_t i = 0; i < view.size(); ++i)
    {
        if (view[i] == '\0')
            ++(i % 2 ? oddZeros : evenZeros);
    }
    if (view.size() >= 2 && oddZeros * 10 >= view.size())
        return Utf16LeToUtf8(view);
    if (view.size() >= 2 && evenZeros * 10 >= view.size())
    {
        std::string swapped(view);
        for (size_t i = 0; i + 1 < swapped.size(); i += 2)
            std::swap(swapped[i], swapped[i + 1]);
        return Utf16LeToUtf8(swapped);
    }
    if (MultiByteToUtf8(CP_UTF8, view, true, out))
        return out;
    MultiByteToUtf8(936, view, false, out);
    return out;
}

// ---------------------------------------------------------------------------------
// 文本工具
// ---------------------------------------------------------------------------------
void ForEachLine(const std::string &text, const std::function<void(std::string_view)> &visit)
{
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string_view line(text.data() + start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        visit(line);
        start = end + 1;
    }
}

std::vector<std::string_view> Split(std::string_view text, char separator)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (;;)
    {
        const size_t end = text.find(separator, start);
        if (end == std::string_view::npos)
        {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
}

std::string_view Trim(std::string_view text)
{
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos)
        return {};
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool AllDigits(std::string_view text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
}

// 把「ni'hao」「ni hao」这类读音规范成小写、' 分隔的音节串；ü 写成 v。
std::string NormalizeReading(std::string_view reading)
{
    std::string source(reading);
    for (size_t pos = 0; (pos = source.find("\xC3\xBC", pos)) != std::string::npos;) // ü
        source.replace(pos, 2, "v");
    std::string out;
    std::string syllable;
    const auto flush = [&]() {
        if (syllable.empty())
            return;
        if (!out.empty())
            out.push_back('\'');
        out += syllable;
        syllable.clear();
    };
    for (const char raw : source)
    {
        if (raw == '\'' || raw == ' ' || raw == '\t')
        {
            flush();
            continue;
        }
        syllable.push_back(raw >= 'A' && raw <= 'Z' ? static_cast<char>(raw - 'A' + 'a') : raw);
    }
    flush();
    return out;
}

// 音节表里任何一个音节解不出来，整个读音都不可信：交给自动注音，不要静默丢字。
std::string JoinSyllables(const std::vector<std::string> &syllables)
{
    std::string out;
    for (const auto &syllable : syllables)
    {
        if (syllable.empty())
            return {};
        if (!out.empty())
            out.push_back('\'');
        out += syllable;
    }
    return out;
}

// 文本词库里的词频列：整数或小数（libime 的对数概率是负的小数）。Rime 单字常写成「50%」，
// 百分号去掉照样比高低。读不成数的当作没给。
std::optional<double> ParseFrequency(std::string_view text)
{
    text = Trim(text);
    if (!text.empty() && text.back() == '%')
        text.remove_suffix(1);
    if (text.empty())
        return std::nullopt;
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

void AddEntry(std::vector<Entry> &entries, std::string_view word, std::string pinyin,
              std::optional<double> frequency = std::nullopt)
{
    const std::string_view trimmed = Trim(word);
    if (trimmed.empty())
        return;
    entries.push_back({std::string(trimmed), std::move(pinyin), frequency});
}

// ---------------------------------------------------------------------------------
// 二进制读取
// ---------------------------------------------------------------------------------
class Reader
{
  public:
    explicit Reader(std::string_view data) : data_(data)
    {
    }

    size_t Size() const
    {
        return data_.size();
    }
    size_t Pos() const
    {
        return pos_;
    }
    size_t Remaining() const
    {
        return data_.size() - pos_;
    }

    void Seek(int64_t pos)
    {
        if (pos < 0 || static_cast<uint64_t>(pos) > data_.size())
            throw ParseFailure(kTruncated);
        pos_ = static_cast<size_t>(pos);
    }
    void Skip(size_t count)
    {
        if (count > Remaining())
            throw ParseFailure(kTruncated);
        pos_ += count;
    }
    std::string_view Take(size_t count)
    {
        if (count > Remaining())
            throw ParseFailure(kTruncated);
        const std::string_view out = data_.substr(pos_, count);
        pos_ += count;
        return out;
    }

    uint8_t U8()
    {
        return static_cast<uint8_t>(Take(1)[0]);
    }
    uint16_t U16()
    {
        const std::string_view b = Take(2);
        return static_cast<uint16_t>(Byte(b, 0) | (Byte(b, 1) << 8));
    }
    uint32_t U32()
    {
        const std::string_view b = Take(4);
        return Byte(b, 0) | (Byte(b, 1) << 8) | (Byte(b, 2) << 16) | (Byte(b, 3) << 24);
    }
    int32_t I32()
    {
        return static_cast<int32_t>(U32());
    }
    uint32_t U32BE()
    {
        const std::string_view b = Take(4);
        return (Byte(b, 0) << 24) | (Byte(b, 1) << 16) | (Byte(b, 2) << 8) | Byte(b, 3);
    }
    int32_t I32BE()
    {
        return static_cast<int32_t>(U32BE());
    }

  private:
    static uint32_t Byte(std::string_view bytes, size_t index)
    {
        return static_cast<uint8_t>(bytes[index]);
    }

    std::string_view data_;
    size_t pos_ = 0;
};

uint16_t U16At(std::string_view bytes, size_t index)
{
    return static_cast<uint16_t>(static_cast<uint8_t>(bytes[index]) | (static_cast<uint8_t>(bytes[index + 1]) << 8));
}

// ---------------------------------------------------------------------------------
// 文本格式
// ---------------------------------------------------------------------------------

// Rime：词语<Tab>读音[<Tab>权重]，读音以空格分隔；跳过 # 注释与 dict.yaml 的 YAML 头。
// 只有「词语<Tab>权重」两列的词库没有读音，留给自动注音。
void ParseRime(const std::string &text, std::vector<Entry> &entries)
{
    bool inHeader = false;
    ForEachLine(text, [&](std::string_view line) {
        const std::string_view trimmed = Trim(line);
        if (trimmed == "---")
        {
            inHeader = true;
            return;
        }
        if (trimmed == "...")
        {
            inHeader = false;
            return;
        }
        if (inHeader || trimmed.empty() || trimmed.front() == '#')
            return;
        const auto fields = Split(line, '\t');
        if (fields.size() < 2)
            return;
        const std::string_view code = Trim(fields[1]);
        if (AllDigits(code))
            AddEntry(entries, fields[0], std::string(), ParseFrequency(code));
        else
            AddEntry(entries, fields[0], NormalizeReading(code),
                     fields.size() >= 3 ? ParseFrequency(fields[2]) : std::nullopt);
    });
}

// 搜狗拼音文本：'ni'hao 你好
void ParseSogouText(const std::string &text, std::vector<Entry> &entries)
{
    ForEachLine(text, [&](std::string_view line) {
        if (line.empty() || line.front() != '\'')
            return;
        const size_t space = line.find(' ');
        if (space == std::string_view::npos)
            return;
        std::string_view word = line.substr(space + 1);
        word = word.substr(0, word.find(' '));
        AddEntry(entries, word, NormalizeReading(line.substr(0, space)));
    });
}

// QQ 拼音文本：ni'hao 你好 1[,……]
void ParseQqText(const std::string &text, std::vector<Entry> &entries)
{
    ForEachLine(text, [&](std::string_view line) {
        line = line.substr(0, line.find(','));
        std::vector<std::string_view> parts;
        for (const auto part : Split(line, ' '))
            if (!part.empty())
                parts.push_back(part);
        if (parts.size() < 2)
            return;
        AddEntry(entries, parts[1], NormalizeReading(parts[0]),
                 parts.size() >= 3 ? ParseFrequency(parts[2]) : std::nullopt);
    });
}

// 百度拼音文本：词语<Tab>ni'hao<Tab>词频；只有两列的是英文词条，不进拼音词库。
void ParseBaiduText(const std::string &text, std::vector<Entry> &entries)
{
    ForEachLine(text, [&](std::string_view line) {
        const auto fields = Split(line, '\t');
        if (fields.size() < 3)
            return;
        AddEntry(entries, fields[0], NormalizeReading(fields[1]), ParseFrequency(fields[2]));
    });
}

// fcitx consumeMaybeEscapedValue：引号包裹的值按转义表还原，否则读到下一个空白。
std::vector<std::string> TokenizeLibime(std::string_view line)
{
    static constexpr std::string_view kWhitespace = "\f\n\r\t\v ";
    std::vector<std::string> tokens;
    while (!line.empty())
    {
        const size_t start = line.find_first_not_of(kWhitespace);
        if (start == std::string_view::npos)
            break;
        line.remove_prefix(start);
        if (line.front() == '"')
        {
            std::string value;
            bool escaping = false;
            size_t end = 0;
            for (size_t i = 1; i < line.size() && end == 0; ++i)
            {
                const char ch = line[i];
                if (escaping)
                {
                    switch (ch)
                    {
                    case 'n':
                        value.push_back('\n');
                        break;
                    case 'f':
                        value.push_back('\f');
                        break;
                    case 'r':
                        value.push_back('\r');
                        break;
                    case 't':
                        value.push_back('\t');
                        break;
                    case 'v':
                        value.push_back('\v');
                        break;
                    default:
                        value.push_back(ch);
                        break;
                    }
                    escaping = false;
                }
                else if (ch == '\\')
                {
                    escaping = true;
                }
                else if (ch == '"')
                {
                    end = i + 1;
                }
                else
                {
                    value.push_back(ch);
                }
            }
            if (end > 0)
            {
                tokens.push_back(std::move(value));
                line.remove_prefix(end);
                continue;
            }
        }
        // 未加引号，或引号没有闭合：读到下一个空白。
        size_t stop = line.find_first_of(kWhitespace, 1);
        if (stop == std::string_view::npos)
            stop = line.size();
        tokens.emplace_back(line.substr(0, stop));
        line.remove_prefix(stop);
    }
    return tokens;
}

// LibIME 文本：词语 ni'hao [词频]，与 libime loadText 一样只认两列或三列。
void ParseLibimeText(const std::string &text, std::vector<Entry> &entries)
{
    ForEachLine(text, [&](std::string_view line) {
        const auto tokens = TokenizeLibime(line);
        if (tokens.size() != 2 && tokens.size() != 3)
            return;
        AddEntry(entries, tokens[0], NormalizeReading(tokens[1]),
                 tokens.size() == 3 ? ParseFrequency(tokens[2]) : std::nullopt);
    });
}

std::string XmlUnescape(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '&')
        {
            out.push_back(text[i]);
            continue;
        }
        const size_t semicolon = text.find(';', i);
        if (semicolon == std::string_view::npos)
        {
            out.push_back(text[i]);
            continue;
        }
        const std::string_view name = text.substr(i + 1, semicolon - i - 1);
        uint32_t codepoint = 0;
        if (name == "lt")
            codepoint = '<';
        else if (name == "gt")
            codepoint = '>';
        else if (name == "amp")
            codepoint = '&';
        else if (name == "quot")
            codepoint = '"';
        else if (name == "apos")
            codepoint = '\'';
        else if (name.size() > 1 && name[0] == '#')
        {
            const bool hex = name[1] == 'x' || name[1] == 'X';
            try
            {
                codepoint =
                    static_cast<uint32_t>(std::stoul(std::string(name.substr(hex ? 2 : 1)), nullptr, hex ? 16 : 10));
            }
            catch (...)
            {
                codepoint = 0;
            }
        }
        if (codepoint == 0 || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
        {
            out.push_back(text[i]);
            continue;
        }
        if (codepoint < 0x80)
            out.push_back(static_cast<char>(codepoint));
        else if (codepoint < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else if (codepoint < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        i = semicolon;
    }
    return out;
}

// Mac 简体拼音（文本替换导出的 plist）：每个 <dict> 里有 phrase 与 shortcut 两个 <string>。
// shortcut 常是简拼，读不成全拼时由调用方自动注音。
void ParsePlist(const std::string &text, std::vector<Entry> &entries)
{
    size_t pos = 0;
    while ((pos = text.find("<dict>", pos)) != std::string::npos)
    {
        const size_t end = text.find("</dict>", pos);
        if (end == std::string::npos)
            break;
        const std::string_view body(text.data() + pos + 6, end - pos - 6);
        pos = end + 7;

        std::vector<std::pair<std::string, std::string>> values; // (key, string)
        std::string pendingKey;
        size_t cursor = 0;
        for (;;)
        {
            const size_t open = body.find('<', cursor);
            if (open == std::string_view::npos)
                break;
            const size_t close = body.find('>', open);
            if (close == std::string_view::npos)
                break;
            const std::string_view tag = body.substr(open + 1, close - open - 1);
            cursor = close + 1;
            const bool selfClosing = !tag.empty() && tag.back() == '/';
            const std::string_view name = tag.substr(0, tag.find_first_of(" /"));
            if (name != "key" && name != "string")
                continue;
            std::string value;
            if (!selfClosing)
            {
                const std::string endTag = "</" + std::string(name) + ">";
                const size_t valueEnd = body.find(endTag, cursor);
                if (valueEnd == std::string_view::npos)
                    break;
                value = XmlUnescape(body.substr(cursor, valueEnd - cursor));
                cursor = valueEnd + endTag.size();
            }
            if (name == "key")
                pendingKey = std::move(value);
            else
                values.emplace_back(std::exchange(pendingKey, std::string()), std::move(value));
        }

        const auto find = [&values](const char *key) -> const std::string * {
            for (const auto &item : values)
                if (item.first == key)
                    return &item.second;
            return nullptr;
        };
        const std::string *phrase = find("phrase");
        const std::string *shortcut = find("shortcut");
        if (!phrase || !shortcut)
        {
            if (values.size() < 2)
                continue;
            phrase = &values[0].second;
            shortcut = &values[1].second;
        }
        AddEntry(entries, *phrase, NormalizeReading(*shortcut));
    }
}

// ---------------------------------------------------------------------------------
// 搜狗细胞词库 scel / QQ 分类词库 qcel
// ---------------------------------------------------------------------------------

// 拼音表里查不到的序号是字母：超出表长的部分依次对应 a、b、c……
std::string ScelSyllable(const std::map<uint16_t, std::string> &table, uint16_t key)
{
    const auto found = table.find(key);
    if (found != table.end())
        return found->second;
    if (key >= table.size() && key - table.size() < 26)
        return std::string(1, static_cast<char>('a' + (key - table.size())));
    return {};
}

void ParseScel(const std::string &bytes, std::vector<Entry> &entries)
{
    Reader reader(bytes);
    if (reader.Size() < 0x1544)
        throw ParseFailure("不是有效的搜狗细胞词库（.scel）");
    reader.Seek(0x120);
    const uint32_t groupCount = reader.U32();
    reader.Seek(0x1540);
    const uint32_t tableSize = reader.U32();
    std::map<uint16_t, std::string> table;
    for (uint32_t i = 0; i < tableSize; ++i)
    {
        const uint16_t index = reader.U16();
        const uint16_t size = reader.U16();
        table[index] = Utf16LeToUtf8(reader.Take(size));
    }

    // 词条区：同音词一组，组头是同音词数和拼音序号，每个词后跟一段扩展信息。扩展信息常被当成
    // 词频，实测（唐诗 3563 条、诗词名句 34 万条）却是每条各不相同的编号，其余字节全 0，
    // 所以 scel 只当作「只有位置」，不出词频。qcel 的对应字段同理。
    for (uint32_t group = 0; group < groupCount; ++group)
    {
        try
        {
            const uint16_t sameCount = reader.U16();
            const uint16_t pinyinBytes = reader.U16();
            const std::string_view keys = reader.Take(pinyinBytes);
            std::vector<std::string> syllables;
            for (size_t i = 0; i + 1 < keys.size(); i += 2)
                syllables.push_back(ScelSyllable(table, U16At(keys, i)));
            const std::string pinyin = JoinSyllables(syllables);
            for (uint16_t s = 0; s < sameCount; ++s)
            {
                const uint16_t wordBytes = reader.U16();
                const std::string word = Utf16LeToUtf8(reader.Take(wordBytes));
                const uint16_t extensionBytes = reader.U16();
                reader.Skip(extensionBytes);
                AddEntry(entries, word, pinyin);
            }
        }
        catch (const ParseFailure &)
        {
            // 词条区截断：保留已经读出的部分。
            break;
        }
    }
}

void ParseQcel(const std::string &bytes, std::vector<Entry> &entries)
{
    Reader reader(bytes);
    if (reader.Size() < 0x1544)
        throw ParseFailure("不是有效的 QQ 分类词库（.qcel）");
    reader.Seek(0x124);
    const uint32_t wordCount = reader.U32();
    reader.Seek(0x1544);
    std::map<uint16_t, std::string> table;
    while (reader.Remaining() >= 4)
    {
        const std::string_view head = reader.Take(4);
        const uint16_t mark = U16At(head, 0);
        const uint8_t size = static_cast<uint8_t>(head[2]);
        if (size == 0)
            break;
        const std::string syllable = Utf16LeToUtf8(reader.Take(size));
        table[mark] = syllable;
        // 拼音表按字母序排列，zuo 是最后一个。
        if (syllable == "zuo")
            break;
    }

    size_t read = 0;
    try
    {
        while (reader.Remaining() >= 4 && read < wordCount)
        {
            const uint16_t sameCount = reader.U16();
            const uint16_t pinyinBytes = reader.U16();
            if (sameCount == 0 && pinyinBytes == 0)
                break;
            const std::string_view keys = reader.Take(pinyinBytes);
            std::vector<std::string> syllables;
            for (size_t i = 0; i + 1 < keys.size(); i += 2)
                syllables.push_back(ScelSyllable(table, U16At(keys, i)));
            const std::string pinyin = JoinSyllables(syllables);
            for (uint16_t s = 0; s < sameCount; ++s)
            {
                const uint16_t wordBytes = reader.U16();
                const std::string word = Utf16LeToUtf8(reader.Take(wordBytes));
                // 2 字节 + 4 字节未知字段，再跟 6 字节。
                reader.Skip(2 + 4 + 6);
                AddEntry(entries, word, pinyin);
                ++read;
            }
        }
    }
    catch (const ParseFailure &)
    {
        // 截断：保留已经读出的部分。
    }
}

// ---------------------------------------------------------------------------------
// 百度分类词库 bdict
// ---------------------------------------------------------------------------------
void ParseBdict(const std::string &bytes, std::vector<Entry> &entries)
{
    static const std::array<const char *, 24> kInitials = {"c", "d", "b",  "f",  "g", "h", "ch", "j",
                                                           "k", "l", "m",  "n",  "",  "p", "q",  "r",
                                                           "s", "t", "sh", "zh", "w", "x", "y",  "z"};
    static const std::array<const char *, 33> kFinals = {
        "uang", "iang", "iong", "ang", "eng", "ian", "iao", "ing", "ong", "uai", "uan",
        "ai",   "an",   "ao",   "ei",  "en",  "er",  "ua",  "ie",  "in",  "iu",  "ou",
        "ia",   "ue",   "ui",   "un",  "uo",  "a",   "e",   "i",   "o",   "u",   "v"};

    Reader reader(bytes);
    if (reader.Size() < 0x350)
        throw ParseFailure("不是有效的百度分类词库（.bdict）");
    // 词条区结束位置：新版头部在 0x60；旧版该字段为 0，用 0x44 处的词条区长度推算。
    reader.Seek(0x60);
    int64_t end = reader.I32();
    if (end <= 0)
    {
        reader.Seek(0x44);
        end = 0x350 + static_cast<int64_t>(reader.U32());
    }
    end = (std::min)(end, static_cast<int64_t>(reader.Size()));

    reader.Seek(0x350);
    while (static_cast<int64_t>(reader.Pos()) < end)
    {
        const uint32_t length = reader.U32();
        if (length > 1000)
            throw ParseFailure("百度分类词库内容异常，无法解析");
        if (length == 0)
            break;
        std::vector<std::string> syllables;
        for (uint32_t i = 0; i < length; ++i)
        {
            const uint8_t initial = reader.U8();
            const uint8_t final = reader.U8();
            syllables.push_back(initial < kInitials.size() && final < kFinals.size()
                                    ? std::string(kInitials[initial]) + kFinals[final]
                                    : std::string());
        }
        const std::string word = Utf16LeToUtf8(reader.Take(2 * static_cast<size_t>(length)));
        AddEntry(entries, word, JoinSyllables(syllables));
    }
}

// ---------------------------------------------------------------------------------
// 搜狗拼音备份词库 sgpybin
// ---------------------------------------------------------------------------------
const std::vector<std::string> &SogouBinSyllables()
{
    static const std::vector<std::string> table = {
        "a", "ai", "an", "ang", "ao", "ba", "bai", "ban", "bang", "bao", "bei", "ben", "beng", "bi", "bian", "biao",
        "bie", "bin", "bing", "bo", "bu", "ca", "cai", "can", "cang", "cao", "ce", "cen", "ceng", "cha", "chai", "chan",
        "chang", "chao", "che", "chen", "cheng", "chi", "chong", "chou", "chu", "chua", "chuai", "chuan", "chuang",
        "chui", "chun", "chuo", "ci", "cong", "cou", "cu", "cuan", "cui", "cun", "cuo", "da", "dai", "dan", "dang",
        "dao", "de", "dei", "den", "deng", "di", "dia", "dian", "diao", "die", "ding", "diu", "dong", "dou", "du",
        "duan", "dui", "dun", "duo", "e", "ei", "en", "eng", "er", "fa", "fan", "fang", "fei", "fen", "feng", "fiao",
        "fo", "fou", "fu", "ga", "gai", "gan", "gang", "gao", "ge", "gei", "gen", "geng", "gong", "gou", "gu", "gua",
        "guai", "guan", "guang", "gui", "gun", "guo", "ha", "hai", "han", "hang", "hao", "he", "hei", "hen", "heng",
        "hong", "hou", "hu", "hua", "huai", "huan", "huang", "hui", "hun", "huo", "ji", "jia", "jian", "jiang", "jiao",
        "jie", "jin", "jing", "jiong", "jiu", "ju", "juan", "jue", "jun", "ka", "kai", "kan", "kang", "kao", "ke",
        "kei", "ken", "keng", "kong", "kou", "ku", "kua", "kuai", "kuan", "kuang", "kui", "kun", "kuo", "la", "lai",
        "lan", "lang", "lao", "le", "lei", "leng", "li", "lia", "lian", "liang", "liao", "lie", "lin", "ling", "liu",
        "lo", "long", "lou", "lu", "luan", "lve", "lun", "luo", "lv", "ma", "mai", "man", "mang", "mao", "me", "mei",
        "men", "meng", "mi", "mian", "miao", "mie", "min", "ming", "miu", "mo", "mou", "mu", "na", "nai", "nan", "nang",
        "nao", "ne", "nei", "nen", "neng", "ni", "nian", "niang", "niao", "nie", "nin", "ning", "niu", "nong", "nou",
        "nu", "nuan", "nve", "nun", "nuo", "nv", "o", "ou", "pa", "pai", "pan", "pang", "pao", "pei", "pen", "peng",
        "pi", "pian", "piao", "pie", "pin", "ping", "po", "pou", "pu", "qi", "qia", "qian", "qiang", "qiao", "qie",
        "qin", "qing", "qiong", "qiu", "qu", "quan", "que", "qun", "ran", "rang", "rao", "re", "ren", "reng", "ri",
        "rong", "rou", "ru", "rua", "ruan", "rui", "run", "ruo", "sa", "sai", "san", "sang", "sao", "se", "sen", "seng",
        "sha", "shai", "shan", "shang", "shao", "she", "shei", "shen", "sheng", "shi", "shou", "shu", "shua", "shuai",
        "shuan", "shuang", "shui", "shun", "shuo", "si", "song", "sou", "su", "suan", "sui", "sun", "suo", "ta", "tai",
        "tan", "tang", "tao", "te", "tei", "teng", "ti", "tian", "tiao", "tie", "ting", "tong", "tou", "tu", "tuan",
        "tui", "tun", "tuo", "wa", "wai", "wan", "wang", "wei", "wen", "weng", "wo", "wu", "xi", "xia", "xian", "xiang",
        "xiao", "xie", "xin", "xing", "xiong", "xiu", "xu", "xuan", "xue", "xun", "ya", "yan", "yang", "yao", "ye",
        "yi", "yin", "ying", "yo", "yong", "you", "yu", "yuan", "yue", "yun", "za", "zai", "zan", "zang", "zao", "ze",
        "zei", "zen", "zeng", "zha", "zhai", "zhan", "zhang", "zhao", "zhe", "zhei", "zhen", "zheng", "zhi", "zhong",
        "zhou", "zhu", "zhua", "zhuai", "zhuan", "zhuang", "zhui", "zhun", "zhuo", "zi", "zong", "zou", "zu", "zuan",
        "zui", "zun", "zuo",
        // 新版格式里的单个字母、数字。
        "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v",
        "w", "x", "y", "z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "#"};
    return table;
}

std::string SogouBinPinyin(Reader &reader)
{
    const size_t count = reader.U16() / 2;
    const auto &table = SogouBinSyllables();
    std::vector<std::string> syllables;
    for (size_t i = 0; i < count; ++i)
    {
        const uint16_t index = reader.U16();
        // 与原实现一致：超出表的序号直接略过。
        if (index < table.size())
            syllables.push_back(table[index]);
    }
    return JoinSyllables(syllables);
}

// 新版（文件头 "SGPU"），@nopdan。
void ParseSogouBinNew(Reader &reader, std::vector<Entry> &entries)
{
    reader.Seek(0x38);
    const uint32_t indexBegin = reader.U32();
    reader.U32(); // index size
    const uint32_t wordCount = reader.U32();
    const uint32_t dictBegin = reader.U32();
    for (uint32_t i = 0; i < wordCount; ++i)
    {
        reader.Seek(static_cast<int64_t>(indexBegin) + 4 * static_cast<int64_t>(i));
        const uint32_t index = reader.U32();
        reader.Seek(static_cast<int64_t>(index) + dictBegin);
        const uint16_t frequency = reader.U16();
        reader.Skip(2 + 5);
        const std::string pinyin = SogouBinPinyin(reader);
        reader.Skip(2);
        const uint16_t wordBytes = reader.U16();
        AddEntry(entries, Utf16LeToUtf8(reader.Take(wordBytes)), pinyin, frequency);
    }
}

// 旧版：哈希索引 + 属性区 + 加密数据区，结构译自 Sogou-User-Dict-Converter。
void ParseSogouBinOld(Reader &reader, std::vector<Entry> &entries)
{
    struct KeyItem
    {
        uint16_t dictTypeDef = 0;
        std::vector<uint16_t> dataType;
        int32_t attrIdx = 0;
        int32_t keyDataIdx = 0;
    };
    struct AttrItem
    {
        int32_t count = 0;
        int32_t dataId = 0;
    };
    struct HeaderItem
    {
        uint32_t offset = 0;
        int32_t dataSize = 0;
        int32_t usedDataSize = 0;
    };
    static constexpr std::array<int, 8> kDatatypeHashSize = {0, 27, 414, 512, -1, -1, 512, 0};
    static constexpr std::array<int, 16> kKeyItemDataTypeSize = {4, 1, 1, 2, 1, 2, 2, 4, 4, 8, 4, 4, 4, 0, 0, 0};

    const auto at = [](auto &list, int64_t index) -> auto & {
        if (index < 0 || static_cast<uint64_t>(index) >= list.size())
            throw ParseFailure(kTruncated);
        return list[static_cast<size_t>(index)];
    };
    const auto typeSize = [](uint16_t type) {
        if (type >= kKeyItemDataTypeSize.size())
            throw ParseFailure(kTruncated);
        return kKeyItemDataTypeSize[type];
    };

    reader.Seek(4);
    reader.U32();
    const uint32_t keyCount = reader.U32();
    const uint32_t attrCount = reader.U32();
    const uint32_t aintCount = reader.U32();

    reader.Seek(20);
    std::vector<KeyItem> keys;
    for (uint32_t i = 0; i < keyCount; ++i)
    {
        KeyItem key;
        key.dictTypeDef = reader.U16();
        const uint16_t types = reader.U16();
        for (uint16_t t = 0; t < types; ++t)
            key.dataType.push_back(reader.U16());
        key.attrIdx = reader.I32();
        key.keyDataIdx = reader.I32();
        reader.I32(); // data index
        reader.U32(); // v6
        keys.push_back(std::move(key));
    }
    std::vector<AttrItem> attrs;
    for (uint32_t i = 0; i < attrCount; ++i)
    {
        AttrItem attr;
        attr.count = reader.I32();
        reader.U32();
        attr.dataId = reader.I32();
        reader.U32();
        attrs.push_back(attr);
    }
    reader.Skip(4 * static_cast<size_t>(aintCount));
    reader.Skip(8);
    reader.U32(); // total size
    const uint32_t indexHeaders = reader.U32();
    const uint32_t attrHeaders = reader.U32();
    const uint32_t dataHeaders = reader.U32();
    const auto readHeaders = [&reader](uint32_t count) {
        std::vector<HeaderItem> list;
        for (uint32_t i = 0; i < count; ++i)
        {
            HeaderItem item;
            item.offset = reader.U32();
            item.dataSize = reader.I32();
            item.usedDataSize = reader.I32();
            list.push_back(item);
        }
        return list;
    };
    const std::vector<HeaderItem> indexList = readHeaders(indexHeaders);
    const std::vector<HeaderItem> attrList = readHeaders(attrHeaders);
    const std::vector<HeaderItem> dataStore = readHeaders(dataHeaders);
    const int64_t dsBase = static_cast<int64_t>(reader.Pos());
    if (keys.empty())
        throw ParseFailure(kTruncated);

    // 每个 key 的索引项长度、属性项长度与哈希桶数。
    std::vector<int> dataTypeSize;
    std::vector<int> baseHashSize;
    std::vector<int> attrSize(attrs.size(), 0);
    for (size_t i = 0; i < keys.size(); ++i)
    {
        const KeyItem &key = keys[i];
        int size = (key.dictTypeDef >> 2) & 4;
        const uint32_t masked = key.dictTypeDef & 0xFFFFFF8Fu;
        // 只有第一个 key 的桶数会用到，它固定是 500。
        baseHashSize.push_back(i == 0 ? 500 : masked < kDatatypeHashSize.size() ? kDatatypeHashSize[masked] : 0);
        const auto countedSize = [&](size_t count) {
            int value = 0;
            for (size_t t = 0; t < count && t < key.dataType.size(); ++t)
                if (t > 0 || masked != 4)
                    value += typeSize(key.dataType[t]);
            return value;
        };
        if (key.attrIdx < 0)
        {
            size += countedSize(key.dataType.size());
            if (key.attrIdx == -1)
                size += 4;
        }
        else
        {
            const AttrItem &attr = at(attrs, key.attrIdx);
            const size_t attrFields = static_cast<size_t>((std::max)(0, attr.count));
            const size_t plain = key.dataType.size() > attrFields ? key.dataType.size() - attrFields : 0;
            size += countedSize(plain) + ((key.dictTypeDef & 0x60) ? 4 : 0) + 4;
            int attributeSize = 0;
            for (size_t t = plain; t < key.dataType.size(); ++t)
                attributeSize += typeSize(key.dataType[t]);
            if ((key.dictTypeDef & 0x40) == 0)
                attributeSize += 4;
            at(attrSize, key.attrIdx) = attributeSize;
        }
        dataTypeSize.push_back(size);
    }

    // 用户头里的两个解密参数。
    reader.Seek(static_cast<int64_t>(reader.Size()) - 0x4c + 56);
    const uint32_t p2 = reader.U32();
    const uint32_t p3 = reader.U32();

    const KeyItem &key = keys[0];
    const HeaderItem &attrHeader = at(attrList, key.attrIdx);
    const int64_t attrLimit = attrHeader.usedDataSize == 0 ? attrHeader.dataSize : attrHeader.usedDataSize;
    const int64_t indexBase = dsBase + at(indexList, 0).offset;
    const int64_t attrBase = dsBase + attrHeader.offset;
    const HeaderItem &pinyinStore = at(dataStore, key.keyDataIdx);
    const HeaderItem &wordStore = at(dataStore, at(attrs, key.attrIdx).dataId);
    const int keyAttrSize = at(attrSize, key.attrIdx);

    int64_t hashCursor = indexBase - 8 * static_cast<int64_t>(baseHashSize[0]);
    for (int bucket = 0; bucket < baseHashSize[0]; ++bucket)
    {
        reader.Seek(hashCursor);
        const int32_t bucketOffset = reader.I32();
        const int32_t bucketCount = reader.I32();
        hashCursor = static_cast<int64_t>(reader.Pos());
        for (int32_t item = 0; item < bucketCount; ++item)
        {
            const int64_t indexPos = indexBase + bucketOffset + static_cast<int64_t>(item) * dataTypeSize[0];
            reader.Seek(indexPos + dataTypeSize[0] - 4);
            int32_t offset = reader.I32();
            for (int64_t a = 0; a < attrLimit; ++a)
            {
                const int64_t attrPos = attrBase + offset;
                if (attrPos > static_cast<int64_t>(reader.Size()))
                    break;
                reader.Seek(attrPos + keyAttrSize - 4);
                offset = reader.I32();

                reader.Seek(indexPos);
                const uint32_t pinyinOffset = reader.U32();
                reader.Seek(dsBase + pinyinStore.offset + pinyinOffset);
                const std::string pinyin = SogouBinPinyin(reader);

                reader.Seek(attrPos);
                const int32_t wordOffset = reader.I32();
                const uint16_t frequency = reader.U16();
                reader.U16();
                reader.U32();
                const uint32_t p1 = reader.U16();
                reader.Seek(dsBase + wordStore.offset + wordOffset);
                const uint32_t xk = ((((p1 + p2) << 2) + ((p1 + p3) << 2)) & 0xffffu);
                const uint32_t shift = p2 % 8;
                const size_t units = reader.U16() / 2;
                std::wstring word;
                for (size_t u = 0; u < units; ++u)
                {
                    const uint32_t ch = reader.U16();
                    uint32_t decoded = ((ch << (16 - shift)) | (ch >> shift)) & 0xffffu;
                    decoded ^= xk;
                    word.push_back(static_cast<wchar_t>(decoded));
                }
                AddEntry(entries, WideToUtf8(word.data(), word.size()), pinyin, frequency);
                if (offset == -1)
                    break;
            }
        }
    }
}

void ParseSogouBin(const std::string &bytes, std::vector<Entry> &entries)
{
    Reader reader(bytes);
    if (reader.Size() < 0x60)
        throw ParseFailure("不是有效的搜狗拼音备份词库（.bin）");
    try
    {
        if (reader.U32() == 0x55504753) // "SGPU"
            ParseSogouBinNew(reader, entries);
        else
            ParseSogouBinOld(reader, entries);
    }
    catch (const ParseFailure &)
    {
        throw ParseFailure(
            "搜狗备份词库解析失败：文件不完整或格式不兼容，请确认是搜狗输入法「导出词库」生成的完整备份");
    }
}

// ---------------------------------------------------------------------------------
// LibIME 二进制拼音词库（fcitx5 的 *.dict）
// ---------------------------------------------------------------------------------
std::string DecompressZstd(std::string_view input)
{
    std::unique_ptr<ZSTD_DCtx, size_t (*)(ZSTD_DCtx *)> context(ZSTD_createDCtx(), ZSTD_freeDCtx);
    if (!context)
        throw ParseFailure("解压失败");
    std::string out;
    std::vector<char> buffer(ZSTD_DStreamOutSize());
    ZSTD_inBuffer in{input.data(), input.size(), 0};
    size_t pending = 1;
    // 输入读完之后仍要再调，直到解码器把缓冲里剩下的输出也交出来。
    while (in.pos < in.size || pending != 0)
    {
        ZSTD_outBuffer outBuffer{buffer.data(), buffer.size(), 0};
        const size_t before = in.pos;
        pending = ZSTD_decompressStream(context.get(), &outBuffer, &in);
        if (ZSTD_isError(pending))
            throw ParseFailure(std::string("解压失败：") + ZSTD_getErrorName(pending));
        out.append(buffer.data(), outBuffer.pos);
        if (out.size() > kMaxDecompressedSize)
            throw ParseFailure("文件解压后过大");
        if (in.pos == before && outBuffer.pos == 0)
        {
            if (pending != 0)
                throw ParseFailure(kTruncated);
            break;
        }
    }
    return out;
}

// libime DATrie<float>（cedar）的只读子集：读入序列化数据并按 cedar begin/next/suffix 枚举全部键。
class CedarTrie
{
  public:
    explicit CedarTrie(std::string_view data)
    {
        Reader reader(data);
        const uint32_t tailLength = reader.U32BE();
        const uint32_t count = reader.U32BE();
        tail_ = std::string(reader.Take(tailLength));
        if (static_cast<uint64_t>(count) * 10 > reader.Remaining())
            throw ParseFailure(kTruncated);
        array_.resize(count);
        for (auto &node : array_)
        {
            node.base = reader.I32BE();
            node.check = reader.I32BE();
        }
        reader.Skip(12); // bheadF / bheadC / bheadO
        sibling_.resize(count);
        child_.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            sibling_[i] = reader.U8();
            child_[i] = reader.U8();
        }
        // 剩下的 block 信息只用于插入，枚举用不到。
    }

    void Enumerate(const std::function<void(const std::string &key, int32_t value)> &visit)
    {
        Npos npos;
        int len = 0;
        int32_t result = Begin(npos, len);
        while (result != kNoPath)
        {
            if (result != kNoValue)
                visit(Suffix(len, npos), result);
            result = Next(npos, len);
        }
    }

  private:
    static constexpr int32_t kNoValue = 0x7fc00001;
    static constexpr int32_t kNoPath = 0x7fc00002;
    // 损坏的数据可能让游标绕圈；正常词库远远到不了这个步数。
    static constexpr uint64_t kMaxSteps = 400'000'000;

    struct Node
    {
        int32_t base = 0;
        int32_t check = 0;
    };
    struct Npos
    {
        int64_t offset = 0;
        int32_t index = 0;
    };

    Node &At(int64_t index)
    {
        if (index < 0 || static_cast<uint64_t>(index) >= array_.size())
            throw ParseFailure(kTruncated);
        return array_[static_cast<size_t>(index)];
    }
    uint8_t Sibling(int64_t index) const
    {
        if (index < 0 || static_cast<uint64_t>(index) >= sibling_.size())
            throw ParseFailure(kTruncated);
        return sibling_[static_cast<size_t>(index)];
    }
    uint8_t Child(int64_t index) const
    {
        if (index < 0 || static_cast<uint64_t>(index) >= child_.size())
            throw ParseFailure(kTruncated);
        return child_[static_cast<size_t>(index)];
    }
    void Step()
    {
        if (++steps_ > kMaxSteps)
            throw ParseFailure(kTruncated);
    }
    int StrLen(int64_t offset) const
    {
        if (offset < 0)
            throw ParseFailure(kTruncated);
        const size_t end = tail_.find('\0', static_cast<size_t>(offset));
        if (end == std::string::npos)
            throw ParseFailure(kTruncated);
        return static_cast<int>(end - static_cast<size_t>(offset));
    }
    int32_t TailValue(int64_t offset) const
    {
        if (offset < 0 || static_cast<uint64_t>(offset) + 4 > tail_.size())
            throw ParseFailure(kTruncated);
        const auto byte = [this, offset](int i) {
            return static_cast<uint32_t>(static_cast<uint8_t>(tail_[static_cast<size_t>(offset) + i]));
        };
        return static_cast<int32_t>(byte(0) | (byte(1) << 8) | (byte(2) << 16) | (byte(3) << 24));
    }

    int32_t Begin(Npos &npos, int &len)
    {
        int64_t from = npos.index;
        int64_t base = npos.offset != 0 ? -npos.offset : At(from).base;
        if (base >= 0)
        {
            uint8_t c = Child(from);
            if (from == 0)
            {
                c = Sibling(base ^ c);
                if (c == 0)
                    return kNoPath;
            }
            while (c != 0 && base >= 0)
            {
                Step();
                from = base ^ c;
                base = At(from).base;
                c = Child(from);
                ++len;
            }
            if (base >= 0)
            {
                npos.index = static_cast<int32_t>(from);
                return At(base ^ c).base;
            }
        }
        npos.index = static_cast<int32_t>(from);
        const int tailLength = StrLen(-base);
        npos.offset = -base + tailLength;
        len += tailLength;
        return TailValue(-base + tailLength + 1);
    }

    int32_t Next(Npos &npos, int &len)
    {
        uint8_t c = 0;
        if (npos.offset != 0)
        {
            const int64_t offset = npos.offset;
            npos.offset = 0;
            len -= static_cast<int>(offset + At(npos.index).base);
        }
        else
        {
            c = Sibling(At(npos.index).base);
        }
        while (c == 0 && npos.index != 0)
        {
            Step();
            c = Sibling(npos.index);
            npos.index = At(npos.index).check;
            --len;
        }
        if (c == 0)
            return kNoPath;
        npos.index = At(npos.index).base ^ c;
        ++len;
        return Begin(npos, len);
    }

    std::string Suffix(int len, const Npos &npos)
    {
        if (len < 0)
            throw ParseFailure(kTruncated);
        std::string key(static_cast<size_t>(len), '\0');
        int64_t to = npos.index;
        if (npos.offset != 0)
        {
            int tailLength = StrLen(-At(to).base);
            if (len > tailLength)
            {
                len -= tailLength;
            }
            else
            {
                tailLength = len;
                len = 0;
            }
            const int64_t source = npos.offset - tailLength;
            if (source < 0 || static_cast<uint64_t>(source) + tailLength > tail_.size())
                throw ParseFailure(kTruncated);
            key.replace(static_cast<size_t>(len), static_cast<size_t>(tailLength), tail_, static_cast<size_t>(source),
                        static_cast<size_t>(tailLength));
        }
        while (len-- > 0)
        {
            Step();
            const int64_t from = At(to).check;
            key[static_cast<size_t>(len)] = static_cast<char>(At(from).base ^ to);
            to = from;
        }
        return key;
    }

    std::vector<Node> array_;
    std::vector<uint8_t> sibling_;
    std::vector<uint8_t> child_;
    std::string tail_;
    uint64_t steps_ = 0;
};

// libime PinyinInitial / PinyinFinal 的字符编码（'A' 起依次排列）。
std::string LibimeSyllable(uint8_t initial, uint8_t final)
{
    static const std::array<const char *, 24> kInitials = {"b",  "p", "m", "f", "d", "t", "n",  "l",
                                                           "g",  "k", "h", "j", "q", "x", "zh", "ch",
                                                           "sh", "r", "z", "c", "s", "y", "w",  ""};
    static const std::array<const char *, 36> kFinals = {"a",   "ai",  "an",   "ang",  "ao",   "e",  "ei", "en",  "eng",
                                                         "er",  "o",   "ong",  "ou",   "i",    "ia", "ie", "iao", "iu",
                                                         "ian", "in",  "iang", "ing",  "iong", "u",  "ua", "uo",  "uai",
                                                         "ui",  "uan", "un",   "uang", "v",    "ve", "ue", "ng",  ""};
    if (initial < 'A' || initial - 'A' >= static_cast<int>(kInitials.size()) || final < 'A')
        return {};
    const std::string head = kInitials[initial - 'A'];
    const size_t finalIndex = static_cast<size_t>(final - 'A');
    if (finalIndex < kFinals.size())
        return head + kFinals[finalIndex];
    // Letter_A..Letter_Z 紧跟在 Zero 之后：单个英文字母。
    const size_t letter = finalIndex - kFinals.size();
    if (letter < 26)
        return head + static_cast<char>('a' + letter);
    return {};
}

void ParseLibimeBinary(const std::string &bytes, std::vector<Entry> &entries)
{
    Reader reader(bytes);
    if (reader.Size() < 8 || reader.U32BE() != 0x000fc613)
        throw ParseFailure("不是 LibIME 二进制拼音词库（.dict）");
    const uint32_t version = reader.U32BE();
    const std::string_view payload = reader.Take(reader.Remaining());
    std::string decompressed;
    if (version == 0x2)
        decompressed = DecompressZstd(payload);
    else if (version == 0x1)
        decompressed = std::string(payload);
    else
        throw ParseFailure("不支持的 LibIME 词库版本：" + std::to_string(version));

    CedarTrie trie(decompressed);
    trie.Enumerate([&entries](const std::string &key, int32_t bits) {
        // 键形如「每音节 2 字节的拼音编码 + '!' + 汉字 UTF-8」。
        const size_t separator = key.find('!');
        if (separator == std::string::npos || separator == 0 || separator % 2 != 0 || separator + 1 >= key.size())
            return;
        std::vector<std::string> syllables;
        for (size_t i = 0; i < separator; i += 2)
            syllables.push_back(LibimeSyllable(static_cast<uint8_t>(key[i]), static_cast<uint8_t>(key[i + 1])));
        // 值是 float 词频（libime 的对数概率，越大越常用）。
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        AddEntry(entries, std::string_view(key).substr(separator + 1), JoinSyllables(syllables),
                 std::isfinite(value) ? std::optional<double>(value) : std::nullopt);
    });
}
} // namespace

const std::vector<Format> &Formats()
{
    static const std::vector<Format> formats = {
        {"rime", L"Rime 中州韵（rime）", L"Rime 词库 (*.txt;*.yaml)", L"*.txt;*.yaml;*.yml"},
        {"scel", L"搜狗细胞词库（scel）", L"搜狗细胞词库 (*.scel)", L"*.scel"},
        {"sgpy", L"搜狗拼音文本（sgpy）", L"文本文件 (*.txt)", L"*.txt"},
        {"sgpybin", L"搜狗拼音备份词库（sgpybin）", L"搜狗备份词库 (*.bin)", L"*.bin"},
        {"qqpy", L"QQ 拼音文本（qqpy）", L"文本文件 (*.txt)", L"*.txt"},
        {"qcel", L"QQ 分类词库（qcel）", L"QQ 分类词库 (*.qcel)", L"*.qcel"},
        {"bdpy", L"百度拼音文本（bdpy）", L"文本文件 (*.txt)", L"*.txt"},
        {"bdict", L"百度分类词库（bdict）", L"百度分类词库 (*.bdict)", L"*.bdict"},
        {"libimetxt", L"LibIME 拼音文本（libimetxt）", L"文本文件 (*.txt)", L"*.txt"},
        {"libimebin", L"LibIME 拼音二进制（libimebin）", L"LibIME 词库 (*.dict)", L"*.dict"},
        {"plist", L"Mac 简体拼音（plist）", L"属性列表 (*.plist)", L"*.plist"},
    };
    return formats;
}

const Format *FindFormat(const std::string &id)
{
    for (const auto &format : Formats())
        if (id == format.id)
            return &format;
    return nullptr;
}

bool Parse(const std::string &format, const std::string &bytes, std::vector<Entry> &entries, std::string &error)
{
    entries.clear();
    error.clear();
    try
    {
        if (format == "rime")
            ParseRime(DecodeText(bytes), entries);
        else if (format == "sgpy")
            ParseSogouText(DecodeText(bytes), entries);
        else if (format == "qqpy")
            ParseQqText(DecodeText(bytes), entries);
        else if (format == "bdpy")
            ParseBaiduText(DecodeText(bytes), entries);
        else if (format == "libimetxt")
            ParseLibimeText(DecodeText(bytes), entries);
        else if (format == "plist")
            ParsePlist(DecodeText(bytes), entries);
        else if (format == "scel")
            ParseScel(bytes, entries);
        else if (format == "qcel")
            ParseQcel(bytes, entries);
        else if (format == "bdict")
            ParseBdict(bytes, entries);
        else if (format == "sgpybin")
            ParseSogouBin(bytes, entries);
        else if (format == "libimebin")
            ParseLibimeBinary(bytes, entries);
        else
        {
            error = "不支持的词库格式";
            return false;
        }
    }
    catch (const ParseFailure &failure)
    {
        entries.clear();
        error = failure.what();
        return false;
    }
    catch (const std::exception &)
    {
        entries.clear();
        error = "词库文件解析失败";
        return false;
    }
    return true;
}
} // namespace ForeignDictionary
