#include "utils/dr_dict_lookup_policy.h"
#include "engine/contracts/dictionary_lookup_keys.h"
#include "tests/includes/test_framework.h"

TEST_CASE(dr_dict_lookup_keys_need_the_exact_modifiers)
{
    using FanyImeDictionaryLookupKeys::IsCandidateLookupKey;
    using FanyImeDictionaryLookupKeys::IsPreeditLookupKey;
    constexpr unsigned kEnter = 0x0D;
    // Shift = 1, Ctrl = 2, Alt = 4: Ctrl+Shift+Enter 查高亮候选，Ctrl+Alt+Enter 查预编辑。
    REQUIRE(IsCandidateLookupKey(kEnter, 0b011));
    REQUIRE(IsPreeditLookupKey(kEnter, 0b110));
    // 与 Ctrl+Enter（译文上屏）、Shift+Enter（上屏全拼）、普通回车互不重叠。
    for (unsigned modifiers = 0; modifiers < 8; ++modifiers)
    {
        REQUIRE(!(IsCandidateLookupKey(kEnter, modifiers) && IsPreeditLookupKey(kEnter, modifiers)));
        if (modifiers != 0b011)
            REQUIRE(!IsCandidateLookupKey(kEnter, modifiers));
        if (modifiers != 0b110)
            REQUIRE(!IsPreeditLookupKey(kEnter, modifiers));
    }
    // 管道里 ModifiersDown 的高位标志不影响判定。
    REQUIRE(IsCandidateLookupKey(kEnter, 0x100u | 0b011));
    REQUIRE(!IsCandidateLookupKey('E', 0b011));
    REQUIRE(!IsPreeditLookupKey('P', 0b110));
}

TEST_CASE(dr_dict_lookup_word_is_trimmed_and_bounded)
{
    using DrDictLookup::NormalizeWord;
    REQUIRE(NormalizeWord(L"  hello\t") == L"hello");
    REQUIRE(NormalizeWord(L"\x3000你好\x3000") == L"你好");
    REQUIRE(NormalizeWord(L"a\nb") == L"a b");
    REQUIRE(NormalizeWord(L" \r\n ").empty());
    std::wstring longWord(DrDictLookup::kMaxWordLength + 10, L'a');
    REQUIRE(NormalizeWord(longWord).size() == DrDictLookup::kMaxWordLength);
    // 截断不拆开代理对：恰好落在高代理项后面时少截一个。
    std::wstring surrogate(DrDictLookup::kMaxWordLength - 1, L'a');
    surrogate += L"\xD83D\xDE00tail";
    REQUIRE(NormalizeWord(surrogate).size() == DrDictLookup::kMaxWordLength - 1);
}

TEST_CASE(dr_dict_preedit_lookup_text_joins_prefix_and_raw)
{
    using DrDictLookup::PreeditLookupText;
    REQUIRE(PreeditLookupText(L"", L"hello", false) == L"hello");
    REQUIRE(PreeditLookupText(L"你好", L"shijie", false) == L"你好shijie");
    // R 模式的 'R' 只是显示前缀。
    REQUIRE(PreeditLookupText(L"", L"Rkonnichiha", true) == L"konnichiha");
    REQUIRE(PreeditLookupText(L"", L"Rust", false) == L"Rust");
    REQUIRE(PreeditLookupText(L"", L"", false).empty());
}

namespace
{
DrDictLookup::Placement At(long x, long y)
{
    DrDictLookup::Placement placement;
    placement.x = x;
    placement.y = y;
    return placement;
}
} // namespace

TEST_CASE(dr_dict_copydata_payload_is_escaped_utf8_json)
{
    using DrDictLookup::BuildCopyDataPayload;
    REQUIRE(BuildCopyDataPayload(L"word", At(10, -20)) == "{\"word\":\"word\",\"x\":10,\"y\":-20}");
    REQUIRE(BuildCopyDataPayload(L"a\"b\\c", At(0, 0)) == "{\"word\":\"a\\\"b\\\\c\",\"x\":0,\"y\":0}");
    REQUIRE(BuildCopyDataPayload(L"\x4F60", At(1, 2)) == "{\"word\":\"\xE4\xBD\xA0\",\"x\":1,\"y\":2}");
    REQUIRE(BuildCopyDataPayload(L"\xD83D\xDE00", At(1, 2)) == "{\"word\":\"\xF0\x9F\x98\x80\",\"x\":1,\"y\":2}");
    // 孤立代理项不能变成非法 UTF-8。
    REQUIRE(BuildCopyDataPayload(L"\xD83D", At(0, 0)) == "{\"word\":\"\xEF\xBF\xBD\",\"x\":0,\"y\":0}");
    REQUIRE(BuildCopyDataPayload(std::wstring(1, L'\x01'), At(0, 0)) == "{\"word\":\"\\u0001\",\"x\":0,\"y\":0}");
}

TEST_CASE(dr_dict_keyboard_payload_carries_the_caret_line)
{
    using DrDictLookup::BuildCopyDataPayload;
    DrDictLookup::Placement placement = At(100, 220);
    placement.keyboard = true;
    // 拿不到光标：退回鼠标位置，但仍标明是键盘唤起，DR Dict 不按鼠标位置自动关闭。
    REQUIRE(BuildCopyDataPayload(L"w", placement) == "{\"word\":\"w\",\"x\":100,\"y\":220,\"keyboard\":true}");
    placement.caret = DrDictLookup::CaretRect{100, 196, 101, 220};
    REQUIRE(BuildCopyDataPayload(L"w", placement) ==
            "{\"word\":\"w\",\"x\":100,\"y\":220,\"caret\":[100,196,101,220],\"keyboard\":true}");
}

TEST_CASE(dr_dict_caret_line_is_estimated_from_the_anchor_in_physical_pixels)
{
    using DrDictLookup::CaretRectFromAnchor;
    // TSF 的锚点是光标所在行的左下角；行高按 24 DIP 估，随显示器缩放。
    const auto at100 = CaretRectFromAnchor(10, 300, 1.0);
    REQUIRE(at100.left == 10 && at100.right == 11 && at100.bottom == 300 && at100.top == 276);
    const auto at150 = CaretRectFromAnchor(10, 300, 1.5);
    REQUIRE(at150.top == 264 && at150.bottom == 300);
    const auto at200 = CaretRectFromAnchor(-1920, 300, 2.0);
    REQUIRE(at200.left == -1920 && at200.top == 252);
    // 缩放读不到（0）时按 100% 算，不能得出零高的行。
    REQUIRE(CaretRectFromAnchor(0, 100, 0.0).top == 76);
}

TEST_CASE(dr_dict_launch_arguments_survive_command_line_parsing)
{
    using DrDictLookup::BuildLaunchArguments;
    REQUIRE(BuildLaunchArguments(L"hello world", At(0, 0)) == L"--lookup \"hello world\"");
    REQUIRE(BuildLaunchArguments(L"say \"hi\"", At(0, 0)) == L"--lookup \"say \\\"hi\\\"\"");
    REQUIRE(BuildLaunchArguments(L"a\\", At(0, 0)) == L"--lookup \"a\\\\\"");
    REQUIRE(BuildLaunchArguments(L"a\\\"b", At(0, 0)) == L"--lookup \"a\\\\\\\"b\"");
    DrDictLookup::Placement keyboard = At(5, 6);
    keyboard.keyboard = true;
    REQUIRE(BuildLaunchArguments(L"w", keyboard) == L"--lookup \"w\" --keyboard");
    keyboard.caret = DrDictLookup::CaretRect{-10, 20, -9, 44};
    REQUIRE(BuildLaunchArguments(L"w", keyboard) == L"--lookup \"w\" --caret -10,20,-9,44 --keyboard");
}
