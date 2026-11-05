#pragma once

#include "ipc_protocol_limits.h"
#include "key_chord.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// 自定义标点：中文标点状态下，「是否按 Shift + 哪个标点键」输出用户指定的符号。
//
// 触发键是美式键盘上不按 Shift 能打出标点的 11 个键，每个键分按不按 Shift，共 22 种；同一触发键只能有一行。
// TSF 按字符认键，所以每种触发键对应美式键盘上它打出的那个字符（Shift + [ 是 {）。
//
// 配置里每行是一个字符串元素 "[Shift+]<键名>=<符号>"：键名取 FanyImeKeyChord::NamedKeys 里的名字
// （BracketLeft、Semicolon 等），第一个 = 之后到结尾整段是符号，所以符号本身可以是 = 或含 =。
// 符号要求：合法 UTF-8，不含控制字符，转成 UTF-16 后 1 到 kMaxSymbolUnits 个单元。
//
// Server 把生效的映射经 Worker 消息 CustomPunctuationChanged 发给 TSF，载荷见 FormatPayload。
namespace FanyImeCustomPunctuation
{
struct KeyInfo
{
    // 不按 Shift 打出的字符，也是 FanyImeKeyChord::NamedKeys 里该键的 label。
    char key;
    wchar_t shifted;
};

inline constexpr KeyInfo kKeys[] = {
    {'`', L'~'}, {'-', L'_'},  {'=', L'+'}, {'[', L'{'}, {']', L'}'}, {'\\', L'|'},
    {';', L':'}, {'\'', L'"'}, {',', L'<'}, {'.', L'>'}, {'/', L'?'},
};
inline constexpr std::size_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);
inline constexpr std::size_t kMaxRows = kKeyCount * 2;
inline constexpr std::size_t kMaxSymbolUnits = 7;

struct Trigger
{
    bool shift = false;
    // kKeys 里的 key；0 表示还没选键。
    char key = 0;

    constexpr bool operator==(const Trigger &other) const
    {
        return shift == other.shift && key == other.key;
    }
};

// 美式键盘上这个触发键打出的字符；键不在 kKeys 里时为 0。
inline wchar_t TriggerChar(Trigger trigger);
// 配置元素里的键名（"BracketLeft"）；键不在 kKeys 里时为空。
inline std::string_view KeyName(char key);

struct Entry
{
    Trigger trigger;
    // UTF-8。
    std::string symbol;
};

// 符号是否合法（见文件头），参数是 UTF-8。
inline bool IsValidSymbol(std::string_view symbol);
// 合法时把 UTF-8 符号转成 UTF-16 写进 out 并返回 true。
inline bool SymbolToUtf16(std::string_view symbol, std::wstring &out);

// 解析一个配置元素；格式不对、键名不认识、符号不合法时返回 false，entry 不可用。
inline bool ParseEntry(std::string_view element, Entry &entry);
// entry 的键与符号须合法；不合法时返回空串。
inline std::string FormatEntry(const Entry &entry);
// 一组配置元素的规范化：丢掉不合法的，同一触发键留先出现的那一行，其余按原顺序。
// allValid 非空时写入「每个元素都留下了」。Server 读写配置、组载荷，设置页载入快照都用它。
inline std::vector<Entry> NormalizeEntries(const std::vector<std::string> &elements, bool *allValid = nullptr);

// 发给 TSF 的映射表：触发字符 → UTF-16 符号，至多 kMaxRows 行，触发字符互不相同。
class Table
{
  public:
    struct Row
    {
        wchar_t trigger;
        std::wstring symbol;
    };

    // 触发字符不是 22 种之一、已经有这一行、或符号不合法（空、控制字符、超过 kMaxSymbolUnits）时不加，返回 false。
    inline bool Add(wchar_t trigger, std::wstring_view symbol);
    // 这个字符映射到的符号；没有这一行时为 nullptr。指针在表被改动前有效。
    inline const std::wstring *Find(wchar_t trigger) const;
    const std::vector<Row> &Rows() const
    {
        return rows_;
    }
    bool Empty() const
    {
        return rows_.empty();
    }

  private:
    std::vector<Row> rows_;
};

// 规范化过的行组成发给 TSF 的表：触发字符按美式键盘换算，符号转成 UTF-16。
inline Table BuildTable(const std::vector<Entry> &entries);

// 载荷：每行「触发字符 + 符号 + \t」依次相接，空表是空串。最长 kMaxRows * (kMaxSymbolUnits + 2) 个单元，
// 放得进 Worker 单条载荷。
inline std::wstring FormatPayload(const Table &table);
// 整条载荷合法才写进 out 并返回 true；任何一行不合法都返回 false，out 不动。
inline bool ParsePayload(std::wstring_view payload, Table &out);

// ---- 定义 ----

namespace detail
{
inline constexpr const KeyInfo *FindKey(char key)
{
    for (const KeyInfo &info : kKeys)
    {
        if (info.key == key)
            return &info;
    }
    return nullptr;
}

inline constexpr bool IsTriggerChar(wchar_t ch)
{
    for (const KeyInfo &info : kKeys)
    {
        if (static_cast<wchar_t>(info.key) == ch || info.shifted == ch)
            return true;
    }
    return false;
}

inline constexpr bool IsControl(char32_t ch)
{
    return ch < 0x20 || (ch >= 0x7F && ch < 0xA0);
}

// 一个 UTF-16 符号是否合法：长度在上限内，没有控制字符与孤立的代理项。
inline bool IsValidUtf16Symbol(std::wstring_view symbol)
{
    if (symbol.empty() || symbol.size() > kMaxSymbolUnits)
        return false;
    for (std::size_t i = 0; i < symbol.size(); ++i)
    {
        const wchar_t unit = symbol[i];
        if (unit >= 0xD800 && unit <= 0xDBFF)
        {
            if (i + 1 >= symbol.size() || symbol[i + 1] < 0xDC00 || symbol[i + 1] > 0xDFFF)
                return false;
            ++i;
            continue;
        }
        if ((unit >= 0xDC00 && unit <= 0xDFFF) || IsControl(unit))
            return false;
    }
    return true;
}

// 严格的 UTF-8 解码：拒绝过长编码、代理项与超出 U+10FFFF 的码点。
inline bool DecodeUtf8(std::string_view text, std::wstring &out)
{
    out.clear();
    std::size_t i = 0;
    while (i < text.size())
    {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        char32_t code = 0;
        std::size_t length = 0;
        char32_t minimum = 0;
        if (lead < 0x80)
        {
            code = lead;
            length = 1;
        }
        else if ((lead & 0xE0) == 0xC0)
        {
            code = lead & 0x1F;
            length = 2;
            minimum = 0x80;
        }
        else if ((lead & 0xF0) == 0xE0)
        {
            code = lead & 0x0F;
            length = 3;
            minimum = 0x800;
        }
        else if ((lead & 0xF8) == 0xF0)
        {
            code = lead & 0x07;
            length = 4;
            minimum = 0x10000;
        }
        else
        {
            return false;
        }
        if (i + length > text.size())
            return false;
        for (std::size_t k = 1; k < length; ++k)
        {
            const unsigned char next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80)
                return false;
            code = (code << 6) | (next & 0x3F);
        }
        if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            return false;
        if (code >= 0x10000)
        {
            code -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (code >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (code & 0x3FF)));
        }
        else
        {
            out.push_back(static_cast<wchar_t>(code));
        }
        i += length;
    }
    return true;
}

inline constexpr std::string_view kShiftPrefix = "Shift+";
} // namespace detail

inline wchar_t TriggerChar(Trigger trigger)
{
    const KeyInfo *info = detail::FindKey(trigger.key);
    if (info == nullptr)
        return 0;
    return trigger.shift ? info->shifted : static_cast<wchar_t>(info->key);
}

inline std::string_view KeyName(char key)
{
    if (detail::FindKey(key) == nullptr)
        return {};
    for (const auto &named : FanyImeKeyChord::NamedKeys)
    {
        if (named.label.size() == 1 && named.label[0] == key)
            return named.name;
    }
    return {};
}

inline bool SymbolToUtf16(std::string_view symbol, std::wstring &out)
{
    std::wstring converted;
    if (!detail::DecodeUtf8(symbol, converted) || !detail::IsValidUtf16Symbol(converted))
        return false;
    out = std::move(converted);
    return true;
}

inline bool IsValidSymbol(std::string_view symbol)
{
    std::wstring ignored;
    return SymbolToUtf16(symbol, ignored);
}

inline bool ParseEntry(std::string_view element, Entry &entry)
{
    Entry parsed;
    if (element.substr(0, detail::kShiftPrefix.size()) == detail::kShiftPrefix)
    {
        parsed.trigger.shift = true;
        element.remove_prefix(detail::kShiftPrefix.size());
    }
    const std::size_t separator = element.find('=');
    if (separator == std::string_view::npos)
        return false;
    const std::string_view name = element.substr(0, separator);
    for (const KeyInfo &info : kKeys)
    {
        if (!name.empty() && KeyName(info.key) == name)
            parsed.trigger.key = info.key;
    }
    if (parsed.trigger.key == 0)
        return false;
    const std::string_view symbol = element.substr(separator + 1);
    if (!IsValidSymbol(symbol))
        return false;
    parsed.symbol = std::string(symbol);
    entry = std::move(parsed);
    return true;
}

inline std::string FormatEntry(const Entry &entry)
{
    const std::string_view name = KeyName(entry.trigger.key);
    if (name.empty() || !IsValidSymbol(entry.symbol))
        return {};
    std::string element;
    if (entry.trigger.shift)
        element += detail::kShiftPrefix;
    element += name;
    element += '=';
    element += entry.symbol;
    return element;
}

inline std::vector<Entry> NormalizeEntries(const std::vector<std::string> &elements, bool *allValid)
{
    std::vector<Entry> normalized;
    bool everyElementKept = true;
    for (const std::string &element : elements)
    {
        Entry entry;
        bool kept = ParseEntry(element, entry);
        for (const Entry &earlier : normalized)
            kept = kept && !(earlier.trigger == entry.trigger);
        if (kept)
            normalized.push_back(std::move(entry));
        else
            everyElementKept = false;
    }
    if (allValid != nullptr)
        *allValid = everyElementKept;
    return normalized;
}

inline Table BuildTable(const std::vector<Entry> &entries)
{
    Table table;
    for (const Entry &entry : entries)
    {
        std::wstring symbol;
        if (SymbolToUtf16(entry.symbol, symbol))
            table.Add(TriggerChar(entry.trigger), symbol);
    }
    return table;
}

inline bool Table::Add(wchar_t trigger, std::wstring_view symbol)
{
    if (!detail::IsTriggerChar(trigger) || Find(trigger) != nullptr || !detail::IsValidUtf16Symbol(symbol))
        return false;
    rows_.push_back({trigger, std::wstring(symbol)});
    return true;
}

inline const std::wstring *Table::Find(wchar_t trigger) const
{
    for (const Row &row : rows_)
    {
        if (row.trigger == trigger)
            return &row.symbol;
    }
    return nullptr;
}

inline std::wstring FormatPayload(const Table &table)
{
    std::wstring payload;
    for (const Table::Row &row : table.Rows())
    {
        payload.push_back(row.trigger);
        payload += row.symbol;
        payload.push_back(L'\t');
    }
    return payload;
}

inline bool ParsePayload(std::wstring_view payload, Table &out)
{
    Table parsed;
    while (!payload.empty())
    {
        const std::size_t end = payload.find(L'\t');
        if (end == std::wstring_view::npos || end < 2)
            return false;
        if (!parsed.Add(payload[0], payload.substr(1, end - 1)))
            return false;
        payload.remove_prefix(end + 1);
    }
    out = std::move(parsed);
    return true;
}

static_assert(kMaxRows * (kMaxSymbolUnits + 2) <= FanyImePipeLimits::CandidateTextMaxLength,
              "the full table must fit in one worker frame");
} // namespace FanyImeCustomPunctuation
