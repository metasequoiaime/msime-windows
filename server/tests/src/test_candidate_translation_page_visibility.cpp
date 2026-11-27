#include "ipc/translation_page_visibility.h"
#include "tests/includes/test_framework.h"

#include <string>
#include <vector>

TEST_CASE(translation_page_visibility_only_sees_current_page_keys)
{
    const std::vector<std::string> page{"跑通", "炮筒"};
    REQUIRE(FanyImeIpc::AnyTranslatedKeyOnPage(page, {"跑通"}));
    REQUIRE(FanyImeIpc::AnyTranslatedKeyOnPage(page, {"炮筒", "跑通"}));
    REQUIRE(!FanyImeIpc::AnyTranslatedKeyOnPage(page, {"泡桐"}));
    REQUIRE(!FanyImeIpc::AnyTranslatedKeyOnPage(page, {}));
    REQUIRE(!FanyImeIpc::AnyTranslatedKeyOnPage({}, {"跑通"}));
}

TEST_CASE(translation_page_visibility_matches_simplified_key_under_traditional_display)
{
    struct CandidateRow
    {
        std::string simplified_word;
        std::string displayed_text;
    };
    const std::vector<CandidateRow> items{
        {"等我一下马上好", "等我一下馬上好"},
        {"等我一下", "等我一下"},
        {"繁体候选译文", "繁體候選譯文"},
    };
    const std::vector<std::string> page1_keys = FanyImeIpc::VisiblePageTranslationKeys(
        items, 0, 2, [](const CandidateRow &row) { return row.simplified_word; });
    REQUIRE(FanyImeIpc::AnyTranslatedKeyOnPage(page1_keys, {"等我一下马上好"}));
    REQUIRE(!FanyImeIpc::AnyTranslatedKeyOnPage(page1_keys, {"繁体候选译文"}));

    const std::vector<std::string> page2_keys = FanyImeIpc::VisiblePageTranslationKeys(
        items, 2, 2, [](const CandidateRow &row) { return row.simplified_word; });
    REQUIRE(FanyImeIpc::AnyTranslatedKeyOnPage(page2_keys, {"繁体候选译文"}));
    REQUIRE(!FanyImeIpc::AnyTranslatedKeyOnPage(page2_keys, {"等我一下马上好"}));

    REQUIRE(FanyImeIpc::VisiblePageTranslationKeys(items, -1, 2, [](const CandidateRow &row) {
                return row.simplified_word;
            }).empty());
    REQUIRE(FanyImeIpc::VisiblePageTranslationKeys(items, 10, 2, [](const CandidateRow &row) {
                return row.simplified_word;
            }).empty());
}
