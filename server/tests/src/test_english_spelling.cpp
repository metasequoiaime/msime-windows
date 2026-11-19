#include "engine/english/english_spelling.h"
#include "english/windows_spell_checker.h"
#include "tests/includes/test_framework.h"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <string>
#include <vector>

using metasequoia::english::levenshtein;
using metasequoia::english::phonex;
using metasequoia::english::PhonexIndex;

TEST_CASE(phonex_matches_the_talisman_reference_codes)
{
    // 期望值由 hallelujahIM 打包的 talisman phonex.js 原样跑出来。
    REQUIRE(phonex("Peter") == "B36");
    REQUIRE(phonex("Pete") == "B3");
    REQUIRE(phonex("Pedro") == "B36");
    REQUIRE(phonex("Stephen") == "S315");
    REQUIRE(phonex("Steve") == "S31");
    REQUIRE(phonex("Smith") == "S53");
    REQUIRE(phonex("Smythe") == "S53");
    REQUIRE(phonex("Gail") == "G4");
    REQUIRE(phonex("Gayle") == "G4");
    REQUIRE(phonex("Christine") == "C6235");
    REQUIRE(phonex("Kristina") == "C6235");
    REQUIRE(phonex("knight") == "N23");
    REQUIRE(phonex("phone") == "F5");
    REQUIRE(phonex("write") == "R3");
    REQUIRE(phonex("hello") == "A4");
    REQUIRE(phonex("handgun") == "A5");
    REQUIRE(phonex("pending") == "B5");
    REQUIRE(phonex("adcock") == "A2");
}

TEST_CASE(phonex_groups_common_misspellings_with_the_intended_word)
{
    REQUIRE(phonex("conectivity") == phonex("connectivity"));
    REQUIRE(phonex("kerrage") == phonex("courage"));
    REQUIRE(phonex("speling") == phonex("spelling"));
}

TEST_CASE(phonex_of_text_without_letters_is_empty)
{
    REQUIRE(phonex("").empty());
    REQUIRE(phonex("sss").empty());
    REQUIRE(phonex("h").empty());
    REQUIRE(phonex("123").empty());
}

TEST_CASE(levenshtein_counts_edits_and_stops_past_the_limit)
{
    REQUIRE(levenshtein("kitten", "sitting", 10) == 3);
    REQUIRE(levenshtein("speling", "spelling", 3) == 1);
    REQUIRE(levenshtein("", "abc", 3) == 3);
    REQUIRE(levenshtein("same", "same", 3) == 0);
    // 超过上限一律返回 limit + 1。
    REQUIRE(levenshtein("a", "abcdefgh", 3) == 4);
    REQUIRE(levenshtein("abcdef", "uvwxyz", 2) == 3);
}

TEST_CASE(phonex_index_ranks_same_sounding_words_by_distance_then_weight)
{
    PhonexIndex index;
    index.add("connectivity", "connectivity", 100);
    index.add("courage", "courage", 500);
    index.add("carriage", "carriage", 900);
    index.add("spelling", "spelling", 50);
    index.add("cat", "cat", 1000); // 太短，不进索引

    // courage 与 carriage 都和 kerrage 同码、都差 3 步：同距离按权重排。
    const auto courage = index.suggest("kerrage", 4);
    REQUIRE(courage.size() == 2);
    REQUIRE(courage[0].word == "carriage");
    REQUIRE(courage[1].word == "courage");
    REQUIRE(courage[1].distance == 3);

    const auto connectivity = index.suggest("conectivity", 4);
    REQUIRE(connectivity.size() == 1);
    REQUIRE(connectivity.front().display == "connectivity");

    // 输入本身是词时不推荐它自己；三个字母以下不查。
    REQUIRE(index.suggest("spelling", 4).empty());
    REQUIRE(index.suggest("cat", 4).empty());
    REQUIRE(index.size() == 4);
}

TEST_CASE(phonex_index_honours_the_distance_and_result_limits)
{
    PhonexIndex index;
    index.add("courage", "courage", 1);
    index.add("spelling", "spelling", 1);
    // speeeeeling 与 spelling 同码（S145），但差 5 步，不要；limit 为 0 什么都不给。
    REQUIRE(phonex("speeeeeling") == phonex("spelling"));
    REQUIRE(index.suggest("speeeeeling", 4).empty());
    REQUIRE(index.suggest("kerrage", 1).size() == 1);
    REQUIRE(index.suggest("kerrage", 0).empty());
}

TEST_CASE(windows_spell_checker_suggests_plain_words_when_available)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        WindowsSpellChecker checker;
        // 没装英文拼写检查的机器上只要求安静地返回空。
        const auto suggestions = checker.suggest("speling", 8);
        if (checker.available())
        {
            REQUIRE(std::find(suggestions.begin(), suggestions.end(), "spelling") != suggestions.end());
            for (const auto &suggestion : suggestions)
            {
                REQUIRE(suggestion != "speling");
                REQUIRE(suggestion.find(' ') == std::string::npos);
            }
        }
        else
        {
            REQUIRE(suggestions.empty());
        }
        REQUIRE(checker.suggest("", 8).empty());
    }
    if (SUCCEEDED(com))
        CoUninitialize();
}
