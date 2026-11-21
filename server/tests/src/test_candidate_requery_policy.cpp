#include "ipc/candidate_requery_policy.h"
#include "tests/includes/test_framework.h"

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace
{
std::vector<WordItem> Items(std::initializer_list<const char *> words)
{
    std::vector<WordItem> result;
    result.reserve(words.size());
    for (const char *word : words)
    {
        WordItem item;
        item.word = word;
        result.push_back(std::move(item));
    }
    return result;
}
} // namespace

TEST_CASE(async_requery_ignores_growth_beyond_the_visible_page)
{
    const auto before = Items({"pao", "tong", "pao2", "tong2", "pao3"});
    auto after = before;
    after.push_back(Items({"tong3"}).front());
    REQUIRE(!FanyImeIpc::VisibleCandidatePageChanged(before, after, 5));
}

TEST_CASE(async_requery_detects_a_changed_first_page_word)
{
    const auto before = Items({"pao", "tong", "pao2", "tong2", "pao3"});
    auto after = before;
    after[1].word = "photon";
    REQUIRE(FanyImeIpc::VisibleCandidatePageChanged(before, after, 5));
}

TEST_CASE(async_requery_detects_a_new_row_on_a_short_first_page)
{
    const auto before = Items({"pao", "tong", "pao2"});
    auto after = before;
    after.push_back(Items({"tong2"}).front());
    REQUIRE(FanyImeIpc::VisibleCandidatePageChanged(before, after, 5));
}

TEST_CASE(async_requery_ignores_a_later_page_reorder)
{
    const auto before = Items({"pao", "tong", "pao2", "tong2", "pao3", "tong3", "pao4"});
    auto after = before;
    std::swap(after[5], after[6]);
    REQUIRE(!FanyImeIpc::VisibleCandidatePageChanged(before, after, 5));
}

TEST_CASE(async_requery_compares_up_to_the_page_size)
{
    const auto before = Items({"pao", "tong", "pao2"});
    auto after = Items({"pao", "tong", "pao2", "tong2"});
    REQUIRE(!FanyImeIpc::VisibleCandidatePageChanged(before, after, 3));
}
