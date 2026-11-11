#include "PunctuationKeyMap.h"

#include "../../../engine/contracts/default_punctuation.h"
#include "../../../engine/contracts/enumeration_comma.h"

#include <algorithm>

namespace
{
// \ and / produce 、 as configured (engine/contracts/enumeration_comma.h). Returns false for any
// other key, which then goes through the other tables.
bool ResolveEnumerationCommaKey(wchar_t wch, unsigned enumerationCommaKeys, bool *isEnumerationComma)
{
    unsigned bit = 0;
    if (wch == L'\\')
        bit = FanyImeEnumerationComma::kBackslash;
    else if (wch == L'/')
        bit = FanyImeEnumerationComma::kSlash;
    else
        return false;
    *isEnumerationComma = (enumerationCommaKeys & bit) != 0;
    return true;
}

const wchar_t *FindSimple(wchar_t wch)
{
    for (const FanyImeDefaultPunctuation::SimpleMapping &entry : FanyImeDefaultPunctuation::kSimpleTable)
    {
        if (entry.key == wch)
            return entry.text;
    }
    return nullptr;
}

// Chinese -> ASCII mapping for the reversible smart punctuation conversion. Every symbol converts
// only when it reaches the document on its own. The paired marks are listed so a lone " or 【 / 《
// / （ -- what auto-complete off produces -- converts like any other mark; the auto-completed pair
// form never arms a conversion, because rewriting one half would orphan the other.
struct SmartPunctuationMapping
{
    wchar_t chinese;
    wchar_t ascii;
};

constexpr SmartPunctuationMapping kSmartPunctuationMap[] = {
    {L'。', L'.'}, {L'，', L','}, {L'！', L'!'}, {L'？', L'?'}, {L'；', L';'}, {L'：', L':'},
    {L'、', L'/'}, {L'“', L'"'},  {L'”', L'"'},  {L'‘', L'\''}, {L'’', L'\''}, {L'【', L'['},
    {L'】', L']'}, {L'《', L'<'}, {L'》', L'>'}, {L'（', L'('}, {L'）', L')'},
};
} // namespace

bool PunctuationKeyMap::IsDefaultKey(wchar_t wch, unsigned enumerationCommaKeys)
{
    bool isEnumerationComma = false;
    if (ResolveEnumerationCommaKey(wch, enumerationCommaKeys, &isEnumerationComma))
        return isEnumerationComma;
    return FindSimple(wch) != nullptr || wch == L'"' || wch == L'\'' || wch == L'<' || wch == L'>';
}

std::wstring PunctuationKeyMap::Resolve(wchar_t wch, unsigned enumerationCommaKeys, const CustomPunctuationPick &custom)
{
    if (custom.active)
        return std::wstring(custom.Symbol());
    bool isEnumerationComma = false;
    if (ResolveEnumerationCommaKey(wch, enumerationCommaKeys, &isEnumerationComma))
        return isEnumerationComma ? L"、" : L"";
    if (const wchar_t *text = FindSimple(wch))
        return text;

    if (wch == L'"')
    {
        const bool closing = doubleQuoteClosingNext_;
        doubleQuoteClosingNext_ = !closing;
        return closing ? L"”" : L"“";
    }
    if (wch == L'\'')
    {
        const bool closing = singleQuoteClosingNext_;
        singleQuoteClosingNext_ = !closing;
        return closing ? L"’" : L"‘";
    }
    if (wch == L'<')
        return angleDepth_++ == 0 ? L"《" : L"〈";
    if (wch == L'>')
    {
        // An unmatched closing mark must leave the depth at zero. Decrementing past it would make
        // the next opening mark produce the inner 〈 instead of 《, and the count would never
        // recover without an equal number of extra openings.
        if (angleDepth_ == 0)
            return L"》";
        return --angleDepth_ == 0 ? L"》" : L"〉";
    }
    return {};
}

void PunctuationKeyMap::BalanceNestPairAfterAutoClose(wchar_t openingKey, const CustomPunctuationPick &custom)
{
    if (custom.active)
        return;
    if (openingKey == L'<' && angleDepth_ > 0)
        --angleDepth_;
}

bool IsSmartPunctuationChinese(wchar_t ch)
{
    for (const SmartPunctuationMapping &mapping : kSmartPunctuationMap)
    {
        if (mapping.chinese == ch)
            return true;
    }
    return false;
}

wchar_t SmartConversionTail(std::wstring_view committed, bool autoClosedPair, bool customSymbol)
{
    if (committed.empty() || autoClosedPair || customSymbol || !IsSmartPunctuationChinese(committed.back()))
        return 0;
    return committed.back();
}

wchar_t SmartPunctuationAsciiFor(wchar_t chinese, unsigned enumerationCommaKeys)
{
    // 顿号跟随中文标点下出顿号的键位设定：只勾 \ 转 \，勾了 / 转 /；两个都勾时转 /。
    if (chinese == L'、')
        return (enumerationCommaKeys & FanyImeEnumerationComma::kSlash) != 0 ? L'/' : L'\\';
    for (const SmartPunctuationMapping &mapping : kSmartPunctuationMap)
    {
        if (mapping.chinese == chinese)
            return mapping.ascii;
    }
    return 0;
}

void CustomPunctuationStore::Replace(const FanyImeCustomPunctuation::Table &table)
{
    std::lock_guard<std::mutex> lock(mutex_);
    rowCount_ = 0;
    for (const FanyImeCustomPunctuation::Table::Row &row : table.Rows())
    {
        // Table 保证至多 kMaxRows 行、每个符号至多 kMaxSymbolUnits 个单元；这里再截一次，副本不会越界。
        if (rowCount_ == FanyImeCustomPunctuation::kMaxRows)
            break;
        StoredRow &stored = rows_[rowCount_++];
        stored = StoredRow{};
        stored.trigger = row.trigger;
        const std::size_t units = (std::min)(row.symbol.size(), FanyImeCustomPunctuation::kMaxSymbolUnits);
        std::copy_n(row.symbol.data(), units, stored.symbol);
    }
}

FanyImeCustomPunctuation::Table CustomPunctuationStore::Snapshot() const
{
    FanyImeCustomPunctuation::Table table;
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < rowCount_; ++i)
        table.Add(rows_[i].trigger, rows_[i].symbol);
    return table;
}
