// 其他输入法词库格式的读取：Rime 与 LibIME（fcitx5）这两种开源输入法的开放格式。
// 商业输入法的私有格式（搜狗、QQ、百度、Mac 等）有意不支持，曾经的实现见 git 历史。
//
// 各格式的解析逻辑移植自「深蓝词库转换」imewlconverter（ImeWlConverter.Formats）：
//   https://github.com/studyzy/imewlconverter
//   Copyright studyzy and contributors, licensed under the GNU General Public License v3.0.
//
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

// 文本词库的编码：Rime、LibIME 本是 UTF-8，但用户经 Windows 工具转存过的文件也常见 UTF-16 或
// GBK。有 BOM 按 BOM；没有 BOM 时，零字节成片出现的是 UTF-16，其余先试 UTF-8，不是合法
// UTF-8 再按 GBK。
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
        {"libimetxt", L"LibIME 拼音文本（libimetxt）", L"文本文件 (*.txt)", L"*.txt"},
        {"libimebin", L"LibIME 拼音二进制（libimebin）", L"LibIME 词库 (*.dict)", L"*.dict"},
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
        else if (format == "libimetxt")
            ParseLibimeText(DecodeText(bytes), entries);
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
