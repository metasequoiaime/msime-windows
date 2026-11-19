#include "utils/dr_dict_dock_policy.h"
#include "tests/includes/test_framework.h"

#include <nlohmann/json.hpp>

using DrDictDock::BuildPayload;
using DrDictDock::SameState;
using DrDictDock::State;

TEST_CASE(dr_dict_dock_payload_matches_the_dr_dict_protocol)
{
    // DR Dict（app/src/application.cpp 的 parse_dock_payload）要 word 是字符串、rect 是四个整数。
    REQUIRE(DrDictDock::kDockRequest == 0x4B434F44);
    const auto shown = nlohmann::json::parse(BuildPayload(State{true, L"tomato", {100, -20, 400, 180}}));
    REQUIRE(shown["word"] == "tomato");
    REQUIRE(shown["rect"].size() == 4);
    REQUIRE(shown["rect"][0] == 100);
    REQUIRE(shown["rect"][1] == -20);
    REQUIRE(shown["rect"][2] == 400);
    REQUIRE(shown["rect"][3] == 180);
    REQUIRE(!shown.contains("hide"));

    const auto hidden = nlohmann::json::parse(BuildPayload(State{}));
    REQUIRE(hidden["hide"] == true);
    REQUIRE(!hidden.contains("word"));
}

TEST_CASE(dr_dict_dock_payload_escapes_the_word)
{
    // 短语与带撇号的词照样能查；引号、反斜杠转义，控制字符换成空格，结果仍是合法 JSON。
    const auto phrase = nlohmann::json::parse(BuildPayload(State{true, L"rock 'n' roll", {0, 0, 1, 1}}));
    REQUIRE(phrase["word"] == "rock 'n' roll");
    const auto odd = nlohmann::json::parse(BuildPayload(State{true, L"a\"b\\c\td", {0, 0, 1, 1}}));
    REQUIRE(odd["word"] == "a\"b\\c d");
}

TEST_CASE(dr_dict_dock_skips_resending_the_same_state)
{
    const State a{true, L"apple", {1, 2, 3, 4}};
    REQUIRE(SameState(a, a));
    REQUIRE(!SameState(a, State{true, L"apply", {1, 2, 3, 4}}));
    // 候选窗动了（翻页变宽、换行）：同一个词也要重发，让弹窗跟着贴过去。
    REQUIRE(!SameState(a, State{true, L"apple", {1, 2, 5, 4}}));
    REQUIRE(!SameState(a, State{}));
    // 收起状态里残留的词和矩形不算差别。
    REQUIRE(SameState(State{}, State{false, L"x", {9, 9, 9, 9}}));
}
