// 自定义标点、成对标点、标点吃键在 TSF 一侧的纯逻辑。期望值按施工规格与美式键盘手写，不从产品表抄。
#include "Composition/PunctuationKeyMap.h"
#include "Global/FanyDefines.h"
#include "Key/KeyCharacter.h"
#include "Key/PairedPunctuationPolicy.h"
#include "Key/PunctuationKeyClaim.h"

#include "../../engine/contracts/custom_punctuation.h"
#include "../../engine/contracts/default_punctuation.h"
#include "../../engine/contracts/enumeration_comma.h"
#include "../../engine/contracts/ipc_protocol_limits.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{
int g_failures = 0;

void Check(bool condition, const char *what, int line)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(condition) Check((condition), #condition, __LINE__)

namespace cp = FanyImeCustomPunctuation;
using Table = cp::Table;

constexpr unsigned kBackslashOnly = FanyImeEnumerationComma::kBackslash;
constexpr unsigned kSlashOnly = FanyImeEnumerationComma::kSlash;
constexpr unsigned kBoth = FanyImeEnumerationComma::kBackslash | FanyImeEnumerationComma::kSlash;

bool Maps(const Table &table, wchar_t trigger, const wchar_t *symbol)
{
    const std::wstring *found = table.Find(trigger);
    return found != nullptr && *found == symbol;
}

Table MakeTable(std::initializer_list<std::pair<wchar_t, const wchar_t *>> rows)
{
    Table table;
    for (const auto &row : rows)
        CHECK(table.Add(row.first, row.second));
    return table;
}

// 美式键盘主键盘上那 11 个标点键：不按 Shift 与按 Shift 打出的字符，以及 Win32 的虚拟键码（VK_OEM_*）。
struct MainKey
{
    wchar_t plain;
    wchar_t shifted;
    unsigned vk;
};
constexpr MainKey kMainKeys[] = {
    {L'`', L'~', 0xC0}, {L'-', L'_', 0xBD},  {L'=', L'+', 0xBB}, {L'[', L'{', 0xDB},
    {L']', L'}', 0xDD}, {L'\\', L'|', 0xDC}, {L';', L':', 0xBA}, {L'\'', L'"', 0xDE},
    {L',', L'<', 0xBC}, {L'.', L'>', 0xBE},  {L'/', L'?', 0xBF},
};
// 小键盘与其他来源的虚拟键码（Win32）。
constexpr unsigned kVkMultiply = 0x6A;
constexpr unsigned kVkAdd = 0x6B;
constexpr unsigned kVkSubtract = 0x6D;
constexpr unsigned kVkDecimal = 0x6E;
constexpr unsigned kVkDivide = 0x6F;
constexpr unsigned kVkPacket = 0xE7;
constexpr unsigned kVkOem102 = 0xE2;
constexpr unsigned kVkDigit1 = 0x31;

// 中文标点状态、没有输入时，在主键盘上（美式键盘）打出 ch 的那次按键。不是那 11 个键打出的字符
// （例 ! 是 Shift + 1）记成数字键 1 加 Shift，与自定义标点无关。
PunctuationKeyInput MainKeyboard(wchar_t ch, unsigned commaKeys = FanyImeEnumerationComma::kBackslash)
{
    PunctuationKeyInput input;
    input.character = ch;
    input.punctuationOpen = true;
    input.enumerationCommaKeys = commaKeys;
    input.virtualKey = kVkDigit1;
    input.shiftDown = true;
    for (const MainKey &key : kMainKeys)
    {
        if (key.plain == ch || key.shifted == ch)
        {
            input.virtualKey = key.vk;
            input.shiftDown = key.shifted == ch;
        }
    }
    return input;
}

PunctuationKeyInput Numpad(unsigned vk, wchar_t ch)
{
    PunctuationKeyInput input;
    input.character = ch;
    input.virtualKey = vk;
    input.punctuationOpen = true;
    input.enumerationCommaKeys = FanyImeEnumerationComma::kBackslash;
    return input;
}

// 打 ni 出了候选窗时的状态。
PunctuationKeyInput WithCandidateList(PunctuationKeyInput input)
{
    input.compositionActive = true;
    input.candidateListShown = true;
    input.candidateListRejectsPunctuation = true;
    return input;
}

// 主键盘上打出 ch 时自定义标点给出的结果。
CustomPunctuationPick Pick(const Table &table, wchar_t ch)
{
    return DecidePunctuationKey(MainKeyboard(ch), table).custom;
}

const CustomPunctuationPick kNoCustom{};

// ---- 契约：触发键、配置元素、载荷 ----

void TriggerCharsFollowTheUsLayout()
{
    struct Expected
    {
        char key;
        wchar_t plain;
        wchar_t shifted;
        const char *name;
    };
    // 美式键盘：不按 Shift 与按 Shift 打出的字符，以及 KeyboardEvent.code 的键名。
    const Expected expected[] = {
        {'`', L'`', L'~', "Backquote"},   {'-', L'-', L'_', "Minus"},        {'=', L'=', L'+', "Equal"},
        {'[', L'[', L'{', "BracketLeft"}, {']', L']', L'}', "BracketRight"}, {'\\', L'\\', L'|', "Backslash"},
        {';', L';', L':', "Semicolon"},   {'\'', L'\'', L'"', "Quote"},      {',', L',', L'<', "Comma"},
        {'.', L'.', L'>', "Period"},      {'/', L'/', L'?', "Slash"},
    };
    CHECK(cp::kKeyCount == 11);
    CHECK(cp::kMaxRows == 22);
    for (const Expected &e : expected)
    {
        CHECK(cp::TriggerChar({false, e.key}) == e.plain);
        CHECK(cp::TriggerChar({true, e.key}) == e.shifted);
        CHECK(cp::KeyName(e.key) == e.name);
    }
    CHECK(cp::TriggerChar({false, 'a'}) == 0);
    CHECK(cp::TriggerChar({true, 0}) == 0);
    CHECK(cp::KeyName(' ').empty());
}

void ConfigEntriesRoundTripWithSpecialSymbols()
{
    struct Case
    {
        const char *element;
        bool shift;
        char key;
        const char *symbol;
    };
    const Case cases[] = {
        {"Shift+BracketLeft=「", true, '[', "「"},
        {"Shift+BracketRight=」", true, ']', "」"},
        {"BracketLeft=「", false, '[', "「"},
        // 符号是 =、含 =、是 " 或 #：第一个 = 之后整段都是符号。
        {"Equal==", false, '=', "="},
        {"Shift+Equal===", true, '=', "=="},
        {"Quote=\"", false, '\'', "\""},
        {"Shift+Semicolon=#", true, ';', "#"},
        {"Backslash=\\", false, '\\', "\\"},
        {"Comma=……", false, ',', "……"},
        // 7 个 UTF-16 单元正好在上限内（emoji ZWJ 序列 5 个单元）。
        {"Period=1234567", false, '.', "1234567"},
        {"Slash=🙆‍♂️", false, '/', "🙆‍♂️"},
    };
    for (const Case &c : cases)
    {
        cp::Entry entry;
        CHECK(cp::ParseEntry(c.element, entry));
        CHECK(entry.trigger.shift == c.shift);
        CHECK(entry.trigger.key == c.key);
        CHECK(entry.symbol == c.symbol);
        CHECK(cp::FormatEntry(entry) == c.element);
    }

    const char *invalid[] = {
        "",
        "BracketLeft",
        "BracketLeft=",
        "=「",
        "Ctrl+BracketLeft=「",
        "Shift+Shift+BracketLeft=「",
        "shift+BracketLeft=「",
        "KeyA=「",
        "Space=「",
        "bracketleft=「",
        "Shift+=「",
        "Period=12345678",
        "Period=a\tb",
        "Period=a\nb",
        "Period=\x7f",
        "Period=\xff",
        "Period=\xe3\x80",
    };
    for (const char *element : invalid)
    {
        cp::Entry entry;
        CHECK(!cp::ParseEntry(element, entry));
    }
    CHECK(cp::FormatEntry({{false, 'a'}, "x"}).empty());
    CHECK(cp::FormatEntry({{false, '['}, ""}).empty());
}

void TableRejectsInvalidRows()
{
    Table table;
    CHECK(table.Empty());
    CHECK(table.Add(L'{', L"「"));
    CHECK(!table.Add(L'{', L"『"));
    CHECK(!table.Add(L'a', L"「"));
    CHECK(!table.Add(L'[', L""));
    CHECK(!table.Add(L'[', L"12345678"));
    CHECK(!table.Add(L'[', L"a\tb"));
    CHECK(table.Add(L'[', L"1234567"));
    CHECK(table.Rows().size() == 2);
    CHECK(Maps(table, L'{', L"「"));
    CHECK(table.Find(L'}') == nullptr);
}

void PayloadRoundTripsAndFitsTheWorkerFrame()
{
    Table empty;
    CHECK(cp::FormatPayload(empty).empty());
    Table fromEmpty;
    CHECK(cp::ParsePayload(L"", fromEmpty));
    CHECK(fromEmpty.Empty());

    const Table two = MakeTable({{L'{', L"「"}, {L'=', L"=="}});
    CHECK(cp::FormatPayload(two) == L"{「\t===\t");
    Table back;
    CHECK(cp::ParsePayload(cp::FormatPayload(two), back));
    CHECK(back.Rows().size() == 2);
    CHECK(Maps(back, L'{', L"「"));
    CHECK(Maps(back, L'=', L"=="));

    // 22 行、每行 7 个单元的符号：最长的载荷也放得进一条 Worker 消息。
    Table full;
    for (const auto &key : cp::kKeys)
    {
        CHECK(full.Add(static_cast<wchar_t>(key.key), L"1234567"));
        CHECK(full.Add(key.shifted, L"abcdefg"));
    }
    CHECK(!full.Add(L'a', L"x"));
    const std::wstring payload = cp::FormatPayload(full);
    CHECK(payload.size() == 22 * 9);
    CHECK(payload.size() <= FanyImePipeLimits::CandidateTextMaxLength);
    Table fullBack;
    CHECK(cp::ParsePayload(payload, fullBack));
    CHECK(fullBack.Rows().size() == 22);
    CHECK(Maps(fullBack, L'?', L"abcdefg"));
    CHECK(Maps(fullBack, L'/', L"1234567"));

    // 不合法的载荷整条不收，原表不动。
    const wchar_t *invalid[] = {
        L"{「",        // 缺行尾 \t
        L"{\t",        // 符号为空
        L"a「\t",      // 不是触发字符
        L"{「\t{『\t", // 同一触发字符两行
        L"{12345678\t", L"\t",
    };
    for (const wchar_t *text : invalid)
    {
        Table kept = MakeTable({{L'[', L"「"}});
        CHECK(!cp::ParsePayload(text, kept));
        CHECK(kept.Rows().size() == 1 && Maps(kept, L'[', L"「"));
    }
}

// ---- 默认映射：自定义表为空时与现状逐项相同 ----

void DefaultSimpleTableIsUnchanged()
{
    // 现状的 23 项简单表（\ 走顿号设置，另测）。
    const std::pair<wchar_t, const wchar_t *> expected[] = {
        {L'`', L"·"},  {L'~', L"~"},  {L'!', L"！"}, {L'@', L"@"},  {L'#', L"#"},  {L'$', L"￥"},
        {L'%', L"%"},  {L'^', L"……"}, {L'&', L"&"},  {L'*', L"*"},  {L'(', L"（"}, {L')', L"）"},
        {L'_', L"——"}, {L'[', L"【"}, {L']', L"】"}, {L'{', L"{"},  {L'}', L"}"},  {L';', L"；"},
        {L':', L"："}, {L',', L"，"}, {L'.', L"。"}, {L'?', L"？"},
    };
    for (unsigned keys : {kBackslashOnly, kSlashOnly, kBoth})
    {
        PunctuationKeyMap map;
        for (const auto &e : expected)
        {
            CHECK(PunctuationKeyMap::IsDefaultKey(e.first, keys));
            CHECK(map.Resolve(e.first, keys, kNoCustom) == e.second);
        }
    }
    // 默认映射不认的键。
    for (wchar_t wch : {L'a', L'Z', L'1', L' ', L'-', L'=', L'+', L'|'})
    {
        PunctuationKeyMap map;
        CHECK(!PunctuationKeyMap::IsDefaultKey(wch, kBoth));
        CHECK(map.Resolve(wch, kBoth, kNoCustom).empty());
    }
}

void DefaultOutputsListMatchesWhatTheMapProduces()
{
    // 设置页把 AllOutputs 列进自定义标点的候选：它与默认映射实际能出的符号逐项相同。
    const std::vector<std::wstring> listed = FanyImeDefaultPunctuation::AllOutputs();
    std::vector<std::wstring> produced;
    for (unsigned keys : {kBackslashOnly, kSlashOnly, kBoth})
    {
        PunctuationKeyMap map;
        // 每个键按三次，走遍交替对的两半与嵌套对的两层。
        for (int round = 0; round < 3; ++round)
        {
            for (wchar_t ch = 0x21; ch < 0x7F; ++ch)
            {
                const std::wstring text = map.Resolve(ch, keys, kNoCustom);
                if (!text.empty() && std::find(produced.begin(), produced.end(), text) == produced.end())
                    produced.push_back(text);
            }
            map.Resolve(L'<', keys, kNoCustom);
        }
    }
    CHECK(produced.size() == listed.size());
    for (const std::wstring &text : produced)
        CHECK(std::find(listed.begin(), listed.end(), text) != listed.end());
    // 手写的抽查：【】《》〈〉“”‘’、 都在。
    for (const wchar_t *text : {L"【", L"】", L"《", L"》", L"〈", L"〉", L"“", L"”", L"‘", L"’", L"、", L"……"})
        CHECK(std::find(listed.begin(), listed.end(), std::wstring(text)) != listed.end());
}

void DefaultEnumerationCommaFollowsItsSetting()
{
    PunctuationKeyMap map;
    CHECK(map.Resolve(L'\\', kBackslashOnly, kNoCustom) == L"、");
    CHECK(map.Resolve(L'/', kBackslashOnly, kNoCustom).empty());
    CHECK(PunctuationKeyMap::IsDefaultKey(L'\\', kBackslashOnly));
    CHECK(!PunctuationKeyMap::IsDefaultKey(L'/', kBackslashOnly));
    CHECK(map.Resolve(L'\\', kSlashOnly, kNoCustom).empty());
    CHECK(map.Resolve(L'/', kSlashOnly, kNoCustom) == L"、");
    CHECK(!PunctuationKeyMap::IsDefaultKey(L'\\', kSlashOnly));
    CHECK(PunctuationKeyMap::IsDefaultKey(L'/', kSlashOnly));
    CHECK(map.Resolve(L'\\', kBoth, kNoCustom) == L"、");
    CHECK(map.Resolve(L'/', kBoth, kNoCustom) == L"、");
}

void DefaultQuotesAlternateAndAnglesNest()
{
    PunctuationKeyMap map;
    CHECK(PunctuationKeyMap::IsDefaultKey(L'"', kBoth));
    CHECK(PunctuationKeyMap::IsDefaultKey(L'\'', kBoth));
    CHECK(PunctuationKeyMap::IsDefaultKey(L'<', kBoth));
    CHECK(PunctuationKeyMap::IsDefaultKey(L'>', kBoth));
    CHECK(map.Resolve(L'"', kBoth, kNoCustom) == L"“");
    CHECK(map.Resolve(L'"', kBoth, kNoCustom) == L"”");
    CHECK(map.Resolve(L'"', kBoth, kNoCustom) == L"“");
    CHECK(map.Resolve(L'\'', kBoth, kNoCustom) == L"‘");
    CHECK(map.Resolve(L'\'', kBoth, kNoCustom) == L"’");
    // 两种引号各自轮换，互不影响。
    CHECK(map.Resolve(L'"', kBoth, kNoCustom) == L"”");

    CHECK(map.Resolve(L'<', kBoth, kNoCustom) == L"《");
    CHECK(map.Resolve(L'<', kBoth, kNoCustom) == L"〈");
    CHECK(map.Resolve(L'>', kBoth, kNoCustom) == L"〉");
    CHECK(map.Resolve(L'>', kBoth, kNoCustom) == L"》");
    // 多出来的右键不让深度变负。
    CHECK(map.Resolve(L'>', kBoth, kNoCustom) == L"》");
    CHECK(map.Resolve(L'<', kBoth, kNoCustom) == L"《");
    // 成对补全补出 》 之后退回深度，下一个 < 仍是 《。
    map.BalanceNestPairAfterAutoClose(L'<', kNoCustom);
    CHECK(map.Resolve(L'<', kBoth, kNoCustom) == L"《");
}

// ---- 自定义标点：优先级 ----

void CustomRowsOverrideDefaults()
{
    PunctuationKeyMap map;
    const Table custom = MakeTable({
        {cp::TriggerChar({true, '['}), L"「"},
        {cp::TriggerChar({true, ']'}), L"」"},
    });
    CHECK(map.Resolve(L'{', kBoth, Pick(custom, L'{')) == L"「");
    CHECK(map.Resolve(L'}', kBoth, Pick(custom, L'}')) == L"」");
    // 没定的键照旧。
    CHECK(map.Resolve(L'[', kBoth, Pick(custom, L'[')) == L"【");
    CHECK(map.Resolve(L',', kBoth, Pick(custom, L',')) == L"，");

    // 留空 + [ 压过【。
    const Table plain = MakeTable({{cp::TriggerChar({false, '['}), L"「"}});
    CHECK(map.Resolve(L'[', kBoth, Pick(plain, L'[')) == L"「");
    CHECK(map.Resolve(L'{', kBoth, Pick(plain, L'{')) == L"{");

    // 定了 \ 压过顿号设置；/ 照旧出顿号。
    const Table backslash = MakeTable({{L'\\', L"＼"}});
    CHECK(map.Resolve(L'\\', kBoth, Pick(backslash, L'\\')) == L"＼");
    CHECK(map.Resolve(L'/', kBoth, Pick(backslash, L'/')) == L"、");
    // 顿号设置没开的键也能定。
    const Table slash = MakeTable({{L'/', L"／"}});
    CHECK(map.Resolve(L'/', kBackslashOnly, Pick(slash, L'/')) == L"／");

    // 定了 Shift + , 就不再出《，也不推进嵌套深度。
    const Table comma = MakeTable({{L'<', L"‹"}});
    CHECK(map.Resolve(L'<', kBoth, Pick(comma, L'<')) == L"‹");
    CHECK(map.Resolve(L'<', kBoth, Pick(comma, L'<')) == L"‹");
    map.BalanceNestPairAfterAutoClose(L'<', Pick(comma, L'<'));
    CHECK(map.Resolve(L'<', kBoth, kNoCustom) == L"《");
    CHECK(map.Resolve(L'<', kBoth, kNoCustom) == L"〈");
    // 自定义键补全后不该退回默认 < 的深度。
    map.BalanceNestPairAfterAutoClose(L'<', Pick(comma, L'<'));
    CHECK(map.Resolve(L'>', kBoth, kNoCustom) == L"〉");
    CHECK(map.Resolve(L'>', kBoth, kNoCustom) == L"》");

    // 定了 Shift + ' 不推进双引号的轮换。
    PunctuationKeyMap quotes;
    const Table quote = MakeTable({{L'"', L"「"}});
    CHECK(quotes.Resolve(L'"', kBoth, Pick(quote, L'"')) == L"「");
    CHECK(quotes.Resolve(L'"', kBoth, Pick(quote, L'"')) == L"「");
    CHECK(quotes.Resolve(L'"', kBoth, kNoCustom) == L"“");

    // 只有自定义映射认的键（= + - |）。
    const Table extras = MakeTable({{L'=', L"＝"}, {L'+', L"＋"}, {L'-', L"－"}, {L'|', L"｜"}});
    CHECK(map.Resolve(L'=', kBoth, Pick(extras, L'=')) == L"＝");
    CHECK(map.Resolve(L'+', kBoth, Pick(extras, L'+')) == L"＋");
    CHECK(map.Resolve(L'-', kBoth, Pick(extras, L'-')) == L"－");
    CHECK(map.Resolve(L'|', kBoth, Pick(extras, L'|')) == L"｜");
}

void CustomTableIsLookedUpByCharacterOnly()
{
    // 同一个物理键按不按 Shift 是两行，互不覆盖。
    PunctuationKeyMap map;
    const Table both = MakeTable({{L'[', L"〔"}, {L'{', L"〖"}});
    CHECK(map.Resolve(L'[', kBoth, Pick(both, L'[')) == L"〔");
    CHECK(map.Resolve(L'{', kBoth, Pick(both, L'{')) == L"〖");
}

// ---- 吃键：只在中文标点状态下 ----

void ClaimFollowsPunctuationStateAndInput()
{
    const Table custom = MakeTable({{L'[', L"〔"}, {L'=', L"＝"}});

    // 英文标点状态：默认键、自定义键都不吃，自定义标点也不接管。
    for (wchar_t ch : {L'[', L'=', L','})
    {
        PunctuationKeyInput input = MainKeyboard(ch);
        input.punctuationOpen = false;
        const PunctuationKeyDecision decision = DecidePunctuationKey(input, custom);
        CHECK(!decision.claimed);
        CHECK(!decision.custom.active);
    }

    // 中文标点状态、没有输入：默认键与自定义键（含默认不认的 =）都吃。
    CHECK(DecidePunctuationKey(MainKeyboard(L'='), custom).claimed);
    CHECK(DecidePunctuationKey(MainKeyboard(L'['), custom).claimed);
    CHECK(DecidePunctuationKey(MainKeyboard(L','), custom).claimed);
    CHECK(!DecidePunctuationKey(MainKeyboard(L'='), Table()).claimed);
    CHECK(DecidePunctuationKey(MainKeyboard(L'['), custom).custom.Symbol() == L"〔");
    CHECK(DecidePunctuationKey(MainKeyboard(L'='), custom).custom.Symbol() == L"＝");
    CHECK(!DecidePunctuationKey(MainKeyboard(L','), custom).custom.active);

    // 输入进行中的三种情形：默认键照旧吃，只有自定义认的 = 不吃。自定义标点照样接管，出符号时用。
    PunctuationKeyInput composing = MainKeyboard(L'=');
    composing.compositionActive = true;
    PunctuationKeyInput shown = MainKeyboard(L'=');
    shown.candidateListShown = true;
    PunctuationKeyInput createWord = MainKeyboard(L'=');
    createWord.createWordPending = true;
    for (PunctuationKeyInput input : {composing, shown, createWord})
    {
        CHECK(InputInProgress(input));
        CHECK(!DecidePunctuationKey(input, custom).claimed);
        CHECK(DecidePunctuationKey(input, custom).custom.Symbol() == L"＝");
        input.character = L'[';
        input.virtualKey = 0xDB;
        CHECK(DecidePunctuationKey(input, custom).claimed);
    }
    CHECK(!InputInProgress(MainKeyboard(L'=')));

    // 主键盘上的 /：顿号开着时是默认键，照吃。
    CHECK(DecidePunctuationKey(MainKeyboard(L'/', kBoth), Table()).claimed);
    CHECK(!DecidePunctuationKey(MainKeyboard(L'/', kBackslashOnly), Table()).claimed);
}

void CandidateListConditionKeepsEachPathsDefaultBehaviour()
{
    const Table custom = MakeTable({{L'[', L"〔"}, {L'=', L"＝"}});
    // 同步分类：候选窗挡住标点这一节，默认键、自定义键都不吃。
    PunctuationKeyInput sync = MainKeyboard(L',');
    sync.candidateListShown = true;
    sync.candidateListRejectsPunctuation = true;
    CHECK(!DecidePunctuationKey(sync, Table()).claimed);
    CHECK(!DecidePunctuationKey(sync, custom).claimed);
    sync.character = L'[';
    sync.virtualKey = 0xDB;
    CHECK(!DecidePunctuationKey(sync, custom).claimed);
    sync.character = L'=';
    sync.virtualKey = 0xBB;
    CHECK(!DecidePunctuationKey(sync, custom).claimed);

    // 延迟分类吃默认键时不看候选窗；只有自定义标点认的键看如实的候选窗状态，候选窗开着时不吃。
    PunctuationKeyInput deferred = MainKeyboard(L',');
    deferred.candidateListShown = true;
    deferred.candidateListRejectsPunctuation = true;
    deferred.defaultKeysIgnoreCandidateList = true;
    CHECK(DecidePunctuationKey(deferred, Table()).claimed);
    CHECK(DecidePunctuationKey(deferred, custom).claimed);
    deferred.character = L'[';
    deferred.virtualKey = 0xDB;
    CHECK(DecidePunctuationKey(deferred, custom).claimed);
    CHECK(DecidePunctuationKey(deferred, custom).custom.Symbol() == L"〔");
    deferred.character = L'=';
    deferred.virtualKey = 0xBB;
    CHECK(!DecidePunctuationKey(deferred, custom).claimed);
    // 候选窗只是边打边出的那种：同步分类也照吃默认键。
    PunctuationKeyInput incremental = MainKeyboard(L',');
    incremental.candidateListShown = true;
    CHECK(DecidePunctuationKey(incremental, Table()).claimed);
}

// ---- 小键盘与别的虚拟键 ----

void NumpadKeysIgnoreCustomPunctuation()
{
    // 映射里每个和小键盘同字符的触发键都定了。
    const Table custom = MakeTable({{L'-', L"——"}, {L'+', L"＋"}, {L'.', L"．"}, {L'/', L"／"}, {L'=', L"＝"}});
    const std::pair<unsigned, wchar_t> numpad[] = {
        {kVkSubtract, L'-'}, {kVkAdd, L'+'}, {kVkDecimal, L'.'}, {kVkDivide, L'/'}, {kVkMultiply, L'*'},
    };
    for (const auto &key : numpad)
    {
        for (int state = 0; state < 2; ++state)
        {
            for (unsigned commaKeys : {kBackslashOnly, kBoth})
            {
                PunctuationKeyInput input = Numpad(key.first, key.second);
                input.enumerationCommaKeys = commaKeys;
                if (state == 1)
                    input = WithCandidateList(input);
                const PunctuationKeyDecision withCustom = DecidePunctuationKey(input, custom);
                const PunctuationKeyDecision without = DecidePunctuationKey(input, Table());
                // 与没有自定义标点时逐项相同。
                CHECK(!withCustom.custom.active);
                CHECK(withCustom.claimed == without.claimed);
                // 候选上屏后接小键盘键：照旧是 ASCII。
                if (key.first != kVkMultiply)
                    CHECK(AppendsAsciiAfterCandidate(key.first, key.second, withCustom.custom));
            }
        }
    }
    // 没有组合时小键盘 - + 不吃，交给宿主；小键盘 . 照旧吃（出 ASCII 的 .），小键盘 / 不吃。
    CHECK(!DecidePunctuationKey(Numpad(kVkSubtract, L'-'), custom).claimed);
    CHECK(!DecidePunctuationKey(Numpad(kVkAdd, L'+'), custom).claimed);
    CHECK(DecidePunctuationKey(Numpad(kVkDecimal, L'.'), custom).claimed);
    CHECK(!DecidePunctuationKey(Numpad(kVkDivide, L'/'), custom).claimed);
    // 小键盘 / 留给宿主：顿号设置开着 / 时也不吃，有没有自定义表都一样。
    for (unsigned commaKeys : {kBoth, kSlashOnly})
    {
        PunctuationKeyInput divide = Numpad(kVkDivide, L'/');
        divide.enumerationCommaKeys = commaKeys;
        CHECK(!DecidePunctuationKey(divide, custom).claimed);
        CHECK(!DecidePunctuationKey(divide, Table()).claimed);
    }
    // 按着 Shift 的小键盘 + 也不接管。
    PunctuationKeyInput shiftedAdd = Numpad(kVkAdd, L'+');
    shiftedAdd.shiftDown = true;
    CHECK(!DecidePunctuationKey(shiftedAdd, custom).custom.active);

    // 主键盘上的同一字符照样接管。
    CHECK(DecidePunctuationKey(MainKeyboard(L'-'), custom).claimed);
    CHECK(DecidePunctuationKey(MainKeyboard(L'-'), custom).custom.Symbol() == L"——");
    CHECK(DecidePunctuationKey(MainKeyboard(L'+'), custom).custom.Symbol() == L"＋");
    // 被接管的主键盘 - +：候选后接它出映射后的符号；没被接管的照旧 ASCII。
    CHECK(!AppendsAsciiAfterCandidate(0xBD, L'-', Pick(custom, L'-')));
    CHECK(!AppendsAsciiAfterCandidate(0xBB, L'+', Pick(custom, L'+')));
    CHECK(AppendsAsciiAfterCandidate(0xBD, L'-', kNoCustom));
    CHECK(AppendsAsciiAfterCandidate(0xBB, L'+', kNoCustom));
    CHECK(!AppendsAsciiAfterCandidate(0xBC, L',', kNoCustom));
}

void CustomRowsNeedTheMainKeyAndItsShiftState()
{
    const Table custom = MakeTable({{L'+', L"＋"}, {L'=', L"＝"}, {L'{', L"「"}, {L'\\', L"＼"}});
    // 录了 Shift 的行只在按着 Shift 时触发：不按 Shift 却打出 + 的键（例德语布局的 + 键）不接管。
    PunctuationKeyInput plusWithoutShift = MainKeyboard(L'+');
    plusWithoutShift.shiftDown = false;
    CHECK(!DecidePunctuationKey(plusWithoutShift, custom).custom.active);
    CHECK(!DecidePunctuationKey(plusWithoutShift, custom).claimed);
    CHECK(DecidePunctuationKey(MainKeyboard(L'+'), custom).custom.active);
    // 留空的行按着 Shift 时不触发。
    PunctuationKeyInput equalWithShift = MainKeyboard(L'=');
    equalWithShift.shiftDown = true;
    CHECK(!DecidePunctuationKey(equalWithShift, custom).custom.active);
    // 别的虚拟键送来的同一字符不接管：VK_PACKET、102 键键盘多出的那个键、别的主键盘键。
    for (unsigned vk : {kVkPacket, kVkOem102, 0xBBu})
    {
        PunctuationKeyInput input = MainKeyboard(L'{');
        input.virtualKey = vk;
        CHECK(!DecidePunctuationKey(input, custom).custom.active);
        // { 是默认键，照旧吃，出默认符号。
        CHECK(DecidePunctuationKey(input, custom).claimed);
    }
    PunctuationKeyInput oem102 = MainKeyboard(L'\\');
    oem102.virtualKey = kVkOem102;
    CHECK(!DecidePunctuationKey(oem102, custom).custom.active);
    CHECK(DecidePunctuationKey(MainKeyboard(L'\\'), custom).custom.Symbol() == L"＼");
}

// ---- 字符与 Shift 取自同一份键盘状态 ----

void CharacterAndShiftComeFromOneKeyboardState()
{
    // 换算字符的替身：按传进来的那份状态里的 Shift 打出 { 或 [，并记下拿到的是哪一份。
    const unsigned char *translated = nullptr;
    const auto translate = [&translated](const unsigned char(&keyboard)[256]) {
        translated = keyboard;
        return (keyboard[0x10] & 0x80) != 0 ? L'{' : L'[';
    };
    unsigned char state[256] = {};

    // 这份状态里 Shift 按着：字符是 {，Shift 也是按着，与此刻物理键怎样无关。
    state[0x10] = 0x80;
    KeyCharacter key = ReadKeyCharacter(state, translate);
    CHECK(translated == state);
    CHECK(key.character == L'{');
    CHECK(key.shiftDown);
    // 这份状态里 Shift 松开：两样一起变。
    state[0x10] = 0x00;
    key = ReadKeyCharacter(state, translate);
    CHECK(key.character == L'[');
    CHECK(!key.shiftDown);
    // 只有切换位、没有按下位：不算按着。
    state[0x10] = 0x01;
    CHECK(!ReadKeyCharacter(state, translate).shiftDown);

    // 照这份状态填吃键判定：Shift + [ 那一行接管，留空的 [ 那一行不接管，反之亦然。
    const Table custom = MakeTable({{L'{', L"「"}, {L'[', L"〔"}});
    for (const unsigned char shiftBits : {static_cast<unsigned char>(0x80), static_cast<unsigned char>(0x00)})
    {
        state[0x10] = shiftBits;
        key = ReadKeyCharacter(state, translate);
        PunctuationKeyInput input;
        input.character = key.character;
        input.virtualKey = 0xDB;
        input.shiftDown = key.shiftDown;
        input.punctuationOpen = true;
        const PunctuationKeyDecision decision = DecidePunctuationKey(input, custom);
        CHECK(decision.custom.active);
        CHECK(decision.custom.Symbol() == (shiftBits != 0 ? L"「" : L"〔"));
    }
}

// ---- 吃键时取到的结果自带符号 ----

// pick 是自带符号的值：取它的那份快照用完即弃、店里随后换了表，它照样出取它时那一行的符号。
// 吃键时的 pick 一路带到出符号的接线见 custom_punctuation_wiring.cpp。
void APickKeepsItsSymbolAfterTheStoreIsReplaced()
{
    static_assert(std::is_trivially_copyable_v<CustomPunctuationPick>, "the pick travels inside the key state");
    CustomPunctuationStore store;
    store.Replace(MakeTable({{L'=', L"＝"}, {L'<', L"‹"}}));
    // 每个 pick 取自一份用完即弃的快照。
    const PunctuationKeyDecision equal = DecidePunctuationKey(MainKeyboard(L'='), store.Snapshot());
    const PunctuationKeyDecision angle = DecidePunctuationKey(MainKeyboard(L'<'), store.Snapshot());
    const PunctuationKeyDecision bracket = DecidePunctuationKey(MainKeyboard(L'['), store.Snapshot());
    CHECK(equal.claimed);
    // 店里换了表：pick 照样出它记下的符号。
    store.Replace(MakeTable({{L'[', L"〔"}}));
    PunctuationKeyMap map;
    CHECK(map.Resolve(L'=', kBoth, equal.custom) == L"＝");
    CHECK(map.Resolve(L'<', kBoth, angle.custom) == L"‹");
    // 取的时候没被接管的 pick 照默认映射出符号，不看新表。
    CHECK(map.Resolve(L'[', kBoth, bracket.custom) == L"【");
    // 自定义 < 补全之后不退回默认 < 的深度。
    map.BalanceNestPairAfterAutoClose(L'<', angle.custom);
    CHECK(map.Resolve(L'<', kBoth, kNoCustom) == L"《");
    CHECK(map.Resolve(L'<', kBoth, kNoCustom) == L"〈");
}

// ---- 智能标点 ----

void CustomSymbolsNeverArmTheSpaceConversion()
{
    // 默认映射出的符号能被下一个空格转换。
    CHECK(SmartConversionTail(L"【", false, false) == L'【');
    CHECK(SmartConversionTail(L"你好，", false, false) == L'，');
    // 自定义标点出的符号一律不转换，哪怕它在转换表里。
    CHECK(SmartConversionTail(L"【", false, true) == 0);
    CHECK(SmartConversionTail(L"，", false, true) == 0);
    CHECK(SmartConversionTail(L"你好，", false, true) == 0);
    // 成对补全补出的一对、不在转换表里的符号、空串都不转换。
    CHECK(SmartConversionTail(L"【】", true, false) == 0);
    CHECK(SmartConversionTail(L"「", false, false) == 0);
    CHECK(SmartConversionTail(L"", false, false) == 0);
}

void RevertingTheSameKeyUsesTheMappedSymbol()
{
    // 空格把「。」转成 . 之后再按同一个键转回：转回出的是这次按键的映射，经吃键时的判定拿到。
    const Table custom = MakeTable({{L'.', L"．"}});
    PunctuationKeyMap map;
    CHECK(map.Resolve(L'.', kBoth, DecidePunctuationKey(MainKeyboard(L'.'), custom).custom) == L"．");
    CHECK(map.Resolve(L'.', kBoth, DecidePunctuationKey(MainKeyboard(L'.'), Table()).custom) == L"。");
    // 小键盘 . 不经自定义标点。
    CHECK(map.Resolve(L'.', kBoth, DecidePunctuationKey(Numpad(kVkDecimal, L'.'), custom).custom) == L"。");
}

// ---- 成对：同一个键再按一次 ----

void CustomKeyStepsOverTheClosingItAutoCompleted()
{
    // Shift + ' 定成 “：补出 “” 之后，在 “|” 中间再按 Shift + ' 跨过 ”，与默认 " 键一致。
    const PairedCommit open = ComposePairedCommit(true, L'"', true, L"“");
    CHECK(open.text == L"“”");
    CHECK(PairedStepOverOwnClosing(L'"', true, L"“", L'"', L'”') == L'”');
    // Shift + [ 定成 「：同样跨过它补出的 」。
    CHECK(PairedStepOverOwnClosing(L'{', true, L"「", L'{', L'」') == L'」');
    // 栈顶那一对是别的键补出的：不跨（例两个键都定成 「）。
    CHECK(PairedStepOverOwnClosing(L'[', true, L"「", L'{', L'」') == 0);
    // 栈顶的右半边不是这个符号补出的那个：不跨。
    CHECK(PairedStepOverOwnClosing(L'{', true, L"「", L'{', L'】') == 0);
    // 默认键走它自己的规则，这里不答。
    CHECK(PairedStepOverOwnClosing(L'[', false, L"【", L'[', L'】') == 0);
    // 不是左半边、多字符：不跨。
    CHECK(PairedStepOverOwnClosing(L'{', true, L"」", L'{', L'」') == 0);
    CHECK(PairedStepOverOwnClosing(L'{', true, L"「」", L'{', L'」') == 0);
    CHECK(PairedStepOverOwnClosing(L'{', true, L"", L'{', L'」') == 0);
}

// ---- 自定义符号不套用默认键的成对规则 ----

void StepOverIgnoresMultiCharacterSymbolsStartingWithAClosingHalf()
{
    // 首字符是右半边的多字符符号：不跨越，否则用户要的 」， 被吞掉。
    CHECK(PairedStepOverCandidate(L'}', true, L"」，") == 0);
    CHECK(PairedStepOverCandidate(L'}', true, L"】】") == 0);
}

void CustomQuoteKeysAreNotTurnedIntoOpeningQuotes()
{
    // 自定义 " 定成 ”、' 定成 ’：原样上屏，不改成左引号，也不补全。
    const PairedCommit dq = ComposePairedCommit(true, L'"', true, L"”");
    CHECK(dq.text == L"”");
    CHECK(dq.closing == 0);
    const PairedCommit sq = ComposePairedCommit(true, L'\'', true, L"’");
    CHECK(sq.text == L"’");
    CHECK(sq.closing == 0);
}

void NestDepthDoesNotUnderflow()
{
    // 深度已经是 0 时补全退回不让它回绕：下一个 < 仍是 《。
    PunctuationKeyMap map;
    map.BalanceNestPairAfterAutoClose(L'<', kNoCustom);
    CHECK(map.Resolve(L'<', kBoth, kNoCustom) == L"《");
}

void Utf8AndUtf16ValidationRejectsMalformedSymbols()
{
    cp::Entry entry;
    // 过长编码（. 的两字节写法）、C1 控制字符 NEL。
    CHECK(!cp::ParseEntry("Period=\xc0\xae", entry));
    CHECK(!cp::ParseEntry("Period=\xe0\x80\xae", entry));
    CHECK(!cp::ParseEntry("Period=\xc2\x85", entry));
    CHECK(!cp::ParseEntry("Period=\xc2\x9f", entry));
    CHECK(cp::ParseEntry("Period=\xc2\xa0", entry));
    // 孤立的代理项。
    Table table;
    CHECK(!table.Add(L'[', std::wstring(1, static_cast<wchar_t>(0xD800))));
    CHECK(!table.Add(L'[', std::wstring(1, static_cast<wchar_t>(0xDC00))));
    CHECK(!table.Add(L'[', std::wstring{static_cast<wchar_t>(0xD800), L'a'}));
    CHECK(!table.Add(L'[', std::wstring{L'a', static_cast<wchar_t>(0xDFFF)}));
    CHECK(!table.Add(L'[', std::wstring(1, static_cast<wchar_t>(0x85))));
    CHECK(table.Add(L'[', std::wstring{static_cast<wchar_t>(0xD83D), static_cast<wchar_t>(0xDE46)}));
}

// ---- 成对标点 ----

void PairTableHasTheTwentyNinePairs()
{
    const std::pair<wchar_t, wchar_t> expected[] = {
        {L'{', L'}'},   {L'「', L'」'}, {L'『', L'』'}, {L'“', L'”'},   {L'‘', L'’'},   {L'（', L'）'},
        {L'《', L'》'}, {L'〈', L'〉'}, {L'【', L'】'}, {L'〖', L'〗'}, {L'〔', L'〕'}, {L'［', L'］'},
        {L'｛', L'｝'}, {L'«', L'»'},   {L'‹', L'›'},   {L'⟨', L'⟩'},   {L'〘', L'〙'}, {L'〚', L'〛'},
        {L'〝', L'〞'}, {L'﹁', L'﹂'}, {L'﹃', L'﹄'}, {L'︻', L'︼'}, {L'︗', L'︘'}, {L'︵', L'︶'},
        {L'︷', L'︸'}, {L'︹', L'︺'}, {L'︿', L'﹀'}, {L'︽', L'︾'}, {L'﹇', L'﹈'},
    };
    CHECK(sizeof(expected) / sizeof(expected[0]) == 29);
    CHECK(sizeof(FanyImePairedPunctuation::kPairs) / sizeof(FanyImePairedPunctuation::kPairs[0]) == 29);
    for (const auto &pair : expected)
    {
        CHECK(FanyImePairedPunctuation::ClosingFor(pair.first) == pair.second);
        CHECK(FanyImePairedPunctuation::IsClosing(pair.second));
        CHECK(!FanyImePairedPunctuation::IsClosing(pair.first));
        CHECK(FanyImePairedPunctuation::ClosingFor(pair.second) == 0);

        // 开着时补出右半边；关着时只出左半边。
        const PairedCommit on = ComposePairedCommit(true, L'x', true, std::wstring(1, pair.first));
        CHECK((on.text == std::wstring{pair.first, pair.second}));
        CHECK(on.closing == pair.second);
        const PairedCommit off = ComposePairedCommit(false, L'x', true, std::wstring(1, pair.first));
        CHECK(off.text == std::wstring(1, pair.first));
        CHECK(off.closing == 0);

        // 之后按到输出这个右半边的键时跨过它。
        CHECK(PairedStepOverCandidate(L'x', true, std::wstring(1, pair.second)) == pair.second);
    }
    for (wchar_t ch : {L'。', L'，', L'、', L'a', L'|'})
    {
        CHECK(FanyImePairedPunctuation::ClosingFor(ch) == 0);
        CHECK(!FanyImePairedPunctuation::IsClosing(ch));
    }
}

void BraceStepsOverTheAutoClosedBrace()
{
    // { 补成 {}，在 {|} 中间按 } 要跨过右边那个，而不是得 {}}。
    const PairedCommit open = ComposePairedCommit(true, L'{', false, L"{");
    CHECK(open.text == L"{}");
    CHECK(open.closing == L'}');
    CHECK(PairedStepOverCandidate(L'}', false, L"}") == L'}');
}

void DefaultQuoteKeysStartAFreshPair()
{
    // 默认引号键：解析出右引号时也改成左引号，开一对新的。
    const PairedCommit dq = ComposePairedCommit(true, L'"', false, L"”");
    CHECK(dq.text == L"“”");
    CHECK(dq.closing == L'”');
    const PairedCommit sq = ComposePairedCommit(true, L'\'', false, L"’");
    CHECK(sq.text == L"‘’");
    // 候选上屏连着引号时只改最后一个字符。
    const PairedCommit withCandidate = ComposePairedCommit(true, L'"', false, L"你好”");
    CHECK(withCandidate.text == L"你好“”");
    // 关着时不改。
    CHECK(ComposePairedCommit(false, L'"', false, L"”").text == L"”");
    // 默认引号键由键本身决定跨过哪个右引号。
    CHECK(PairedStepOverCandidate(L'"', false, L"“") == L'”');
    CHECK(PairedStepOverCandidate(L'\'', false, L"‘") == L'’');
}

void CustomKeysAreNotTreatedAsQuoteKeys()
{
    // Shift + ' 定成 」：输出的右半边才是跨过的依据，不是默认引号键的规则。
    CHECK(PairedStepOverCandidate(L'"', true, L"」") == L'」');
    CHECK(PairedStepOverCandidate(L'"', true, L"「") == 0);
    // ' 定成 ”：自定义键不被改成左引号，也不补全（” 不是左半边）。
    const PairedCommit closing = ComposePairedCommit(true, L'\'', true, L"”");
    CHECK(closing.text == L"”");
    CHECK(closing.closing == 0);
    // 多字符符号不触发跨越。
    CHECK(PairedStepOverCandidate(L'_', true, L"——") == 0);
    // 不在成对表里的符号不补全。
    CHECK(ComposePairedCommit(true, L'[', true, L"※").closing == 0);
}

void ExcelIsExcludedFromPairing()
{
    CHECK(Global::IsPairedPunctuationExcludedProcess(L"EXCEL.EXE"));
    CHECK(Global::IsPairedPunctuationExcludedProcess(L"excel.exe"));
    CHECK(!Global::IsPairedPunctuationExcludedProcess(L"notepad.exe"));
    CHECK(!Global::IsPairedPunctuationExcludedProcess(L""));
}

// ---- 智能标点 ----

void SmartConversionIgnoresSymbolsOutsideItsTable()
{
    for (wchar_t ch : {L'「', L'」', L'〔', L'＝', L'‹', L'·', L'{'})
    {
        CHECK(!IsSmartPunctuationChinese(ch));
        CHECK(SmartPunctuationAsciiFor(ch, kBoth) == 0);
    }
    const std::pair<wchar_t, wchar_t> table[] = {
        {L'。', L'.'}, {L'，', L','}, {L'！', L'!'}, {L'？', L'?'}, {L'；', L';'}, {L'：', L':'},
        {L'“', L'"'},  {L'”', L'"'},  {L'‘', L'\''}, {L'’', L'\''}, {L'【', L'['}, {L'】', L']'},
        {L'《', L'<'}, {L'》', L'>'}, {L'（', L'('}, {L'）', L')'},
    };
    for (const auto &e : table)
    {
        CHECK(IsSmartPunctuationChinese(e.first));
        CHECK(SmartPunctuationAsciiFor(e.first, kBoth) == e.second);
    }
    CHECK(IsSmartPunctuationChinese(L'、'));
    CHECK(SmartPunctuationAsciiFor(L'、', kBackslashOnly) == L'\\');
    CHECK(SmartPunctuationAsciiFor(L'、', kSlashOnly) == L'/');
    CHECK(SmartPunctuationAsciiFor(L'、', kBoth) == L'/');
}

// ---- 换表与取快照可以同时进行 ----

void StoreSnapshotsAreNeverTorn()
{
    CustomPunctuationStore store;
    const Table a = MakeTable({{L'{', L"「"}, {L'}', L"」"}});
    const Table b = MakeTable({{L'[', L"〔"}, {L']', L"〕"}, {L'=', L"＝"}});
    store.Replace(a);
    std::atomic<bool> stop{false};
    std::thread writer([&]() {
        for (int i = 0; i < 20000; ++i)
            store.Replace(i % 2 == 0 ? b : a);
        stop = true;
    });
    int torn = 0;
    int reads = 0;
    while (!stop || reads < 1000)
    {
        const Table snapshot = store.Snapshot();
        ++reads;
        const std::wstring payload = cp::FormatPayload(snapshot);
        if (payload != cp::FormatPayload(a) && payload != cp::FormatPayload(b))
            ++torn;
    }
    writer.join();
    CHECK(torn == 0);
    CHECK(cp::FormatPayload(store.Snapshot()) == cp::FormatPayload(a));
}
} // namespace

int main()
{
    TriggerCharsFollowTheUsLayout();
    ConfigEntriesRoundTripWithSpecialSymbols();
    TableRejectsInvalidRows();
    PayloadRoundTripsAndFitsTheWorkerFrame();
    DefaultSimpleTableIsUnchanged();
    DefaultEnumerationCommaFollowsItsSetting();
    DefaultOutputsListMatchesWhatTheMapProduces();
    DefaultQuotesAlternateAndAnglesNest();
    CustomRowsOverrideDefaults();
    CustomTableIsLookedUpByCharacterOnly();
    ClaimFollowsPunctuationStateAndInput();
    CandidateListConditionKeepsEachPathsDefaultBehaviour();
    NumpadKeysIgnoreCustomPunctuation();
    CustomRowsNeedTheMainKeyAndItsShiftState();
    APickKeepsItsSymbolAfterTheStoreIsReplaced();
    CharacterAndShiftComeFromOneKeyboardState();
    PairTableHasTheTwentyNinePairs();
    BraceStepsOverTheAutoClosedBrace();
    DefaultQuoteKeysStartAFreshPair();
    CustomKeysAreNotTreatedAsQuoteKeys();
    ExcelIsExcludedFromPairing();
    SmartConversionIgnoresSymbolsOutsideItsTable();
    CustomSymbolsNeverArmTheSpaceConversion();
    RevertingTheSameKeyUsesTheMappedSymbol();
    CustomKeyStepsOverTheClosingItAutoCompleted();
    StepOverIgnoresMultiCharacterSymbolsStartingWithAClosingHalf();
    CustomQuoteKeysAreNotTurnedIntoOpeningQuotes();
    NestDepthDoesNotUnderflow();
    Utf8AndUtf16ValidationRejectsMalformedSymbols();
    StoreSnapshotsAreNeverTorn();
    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("PASS: custom punctuation, paired punctuation and punctuation claim");
    return 0;
}
