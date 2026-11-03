#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"
#include "src/ipc/candidate_selection_policy.h"
#include "src/ipc/queried_candidate_arrangement_policy.h"
#include "engine/core/input_session.h"
#include "engine/shuangpin/shuangpin_profile.h"

#include <sqlite3.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::vector<WordItem> MixedEnglishItems()
{
    std::vector<WordItem> items;
    for (int index = 0; index < 12; ++index)
        items.emplace_back("work", "中文" + std::to_string(index), 100 - index);
    for (const char *word : {"work", "works", "worked", "working", "workplace"})
        items.emplace_back(word, word, 0, CandidateSource::EnglishDictionary);
    return items;
}

size_t EnglishOnFirstPage(const std::vector<WordItem> &items, size_t page_size)
{
    return static_cast<size_t>(
        std::count_if(items.begin(), items.begin() + (std::min)(items.size(), page_size),
                      [](const WordItem &item) { return item.source == CandidateSource::EnglishDictionary; }));
}
} // namespace

TEST_CASE(MixedEnglishFirstPageCountPreservesTheExistingSingleCandidateDefault)
{
    auto items = MixedEnglishItems();
    FanyImeIpc::EnglishPlacement placement;
    placement.page_size = 6;
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    REQUIRE_EQ(EnglishOnFirstPage(items, 6), size_t{1});
    REQUIRE_EQ(items.front().word, std::string("中文0"));
    REQUIRE_EQ(items[1].word, std::string("work"));
}

TEST_CASE(MixedEnglishFirstPageCountShowsRealInflectionsWithoutRemovingOtherCandidates)
{
    for (size_t count : {size_t{2}, size_t{3}})
    {
        auto items = MixedEnglishItems();
        const size_t original_size = items.size();
        FanyImeIpc::EnglishPlacement placement;
        placement.page_size = 6;
        placement.first_page_count = count;
        placement.require_exact = true;
        placement.input = "wor"; // 中文可解析的前缀，但没有完全相同的英文词。
        FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
        REQUIRE_EQ(EnglishOnFirstPage(items, 6), count);
        REQUIRE_EQ(items.size(), original_size);
        REQUIRE_EQ(items.front().word, std::string("中文0"));
        REQUIRE_EQ(items[6 - count].word, std::string("work"));
        REQUIRE_EQ(items[7 - count].word, std::string("works"));
        const auto normalized = items;
        FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
        REQUIRE_EQ(items.size(), normalized.size());
        for (size_t index = 0; index < items.size(); ++index)
            REQUIRE_EQ(items[index].word, normalized[index].word);
    }
}

TEST_CASE(MixedEnglishFirstPageCountReservesEmojiAndClampsLearnedSlots)
{
    auto items = MixedEnglishItems();
    items.emplace_back("work", "😀", 0, CandidateSource::Emoji);
    FanyImeIpc::EnglishPlacement placement;
    placement.page_size = 6;
    placement.first_page_count = 3;
    placement.slot = 5;
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    REQUIRE_EQ(items.front().word, std::string("中文0"));
    REQUIRE_EQ(items[2].word, std::string("work"));
    REQUIRE_EQ(items[3].word, std::string("works"));
    REQUIRE_EQ(items[4].word, std::string("worked"));
    REQUIRE_EQ(items[5].source, CandidateSource::Emoji);
}

TEST_CASE(MixedEnglishFirstPageCountHonorsSmallPagesAndExplicitFixedPositions)
{
    for (size_t page_size : {size_t{1}, size_t{2}, size_t{3}})
    {
        auto items = MixedEnglishItems();
        FanyImeIpc::EnglishPlacement placement;
        placement.page_size = page_size;
        placement.first_page_count = 3;
        FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
        REQUIRE(EnglishOnFirstPage(items, page_size) <= (std::max)(size_t{1}, page_size - 1));
        if (page_size > 1)
            REQUIRE_EQ(items.front().word, std::string("中文0"));
    }
    auto items = MixedEnglishItems();
    items.back().fixed_position = 4;
    FanyImeIpc::EnglishPlacement placement;
    placement.page_size = 6;
    placement.first_page_count = 3;
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    REQUIRE_EQ(items[3].word, std::string("workplace"));
    const auto normalized = items;
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    for (size_t index = 0; index < items.size(); ++index)
        REQUIRE_EQ(items[index].word, normalized[index].word);
}

TEST_CASE(MixedEnglishFirstPageCountSharesThePageWithCloudAiAndEmoji)
{
    auto items = MixedEnglishItems();
    items.emplace_back("work", "云", 0, CandidateSource::CloudSuggestion);
    items.emplace_back("work", "AI", 0, CandidateSource::AiSuggestion);
    items.emplace_back("work", "😀", 0, CandidateSource::Emoji);
    FanyImeIpc::EnglishPlacement placement;
    placement.page_size = 6;
    placement.first_page_count = 3;
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    REQUIRE_EQ(items.front().word, std::string("中文0"));
    REQUIRE_EQ(items[1].source, CandidateSource::CloudSuggestion);
    REQUIRE_EQ(EnglishOnFirstPage(items, 6), size_t{2});
    REQUIRE_EQ(items[2].source, CandidateSource::AiSuggestion);
    REQUIRE_EQ(items[5].source, CandidateSource::Emoji);
    REQUIRE_EQ(std::count_if(items.begin(), items.end(),
                             [](const WordItem &item) { return item.source == CandidateSource::AiSuggestion; }),
               1);
    const auto normalized = items;
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    for (size_t index = 0; index < items.size(); ++index)
        REQUIRE_EQ(items[index].word, normalized[index].word);
}

TEST_CASE(MixedEnglishFirstPageCountDoesNotSplitQuickPhraseGroupsOrDisplaceTheChineseHead)
{
    for (size_t phrases : {size_t{3}, size_t{4}})
    {
        auto items = MixedEnglishItems();
        for (size_t index = 0; index < phrases; ++index)
            items.insert(items.begin() + static_cast<std::ptrdiff_t>(index),
                         WordItem("work", "短语" + std::to_string(index), 0, CandidateSource::QuickPhrase));
        FanyImeIpc::EnglishPlacement placement;
        placement.page_size = 6;
        placement.first_page_count = 3;
        FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
        for (size_t index = 0; index < phrases; ++index)
            REQUIRE_EQ(items[index].source, CandidateSource::QuickPhrase);
        REQUIRE_EQ(items[phrases].word, std::string("中文0"));
        REQUIRE_EQ(EnglishOnFirstPage(items, 6), size_t{5} - phrases);
    }
}

TEST_CASE(MixedEnglishFirstPageCountIncludesFixedEnglishAndKeepsTheEmojiSlot)
{
    auto items = MixedEnglishItems();
    items.back().fixed_position = 2;
    items.emplace_back("work", "😀", 0, CandidateSource::Emoji);
    FanyImeIpc::EnglishPlacement placement;
    placement.page_size = 6;
    placement.first_page_count = 3;
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    REQUIRE_EQ(EnglishOnFirstPage(items, 6), size_t{3});
    REQUIRE_EQ(items.front().word, std::string("中文0"));
    REQUIRE_EQ(items[1].word, std::string("workplace"));
    REQUIRE_EQ(items[5].source, CandidateSource::Emoji);
}

TEST_CASE(MixedEnglishFixedFirstWordLeavesTheSlotToTheNextUnfixedWord)
{
    auto items = MixedEnglishItems();
    items[12].fixed_position = 1; // 词库排第一的 work
    FanyImeIpc::EnglishPlacement placement;
    placement.page_size = 6;
    placement.first_page_count = 2;
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    REQUIRE_EQ(items.front().word, std::string("work"));
    REQUIRE_EQ(EnglishOnFirstPage(items, 6), size_t{2});
    const auto slotted = FanyImeIpc::SlottedEnglishIndex(items);
    REQUIRE(slotted.has_value());
    REQUIRE_EQ(items[*slotted].word, std::string("works"));
}

TEST_CASE(MixedEnglishSingleCompletionStillMovesAheadOfAsyncCandidates)
{
    auto items = MixedEnglishItems();
    items.emplace_back("work", "日期", 0, CandidateSource::DateTime);
    items.emplace_back("work", "云", 0, CandidateSource::CloudSuggestion);
    items.emplace_back("work", "AI", 0, CandidateSource::AiSuggestion);
    items.emplace_back("work", "😀", 0, CandidateSource::Emoji);
    FanyImeIpc::EnglishPlacement placement;
    placement.page_size = 5;
    placement.require_exact = true;
    placement.input = "wor";
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    REQUIRE_EQ(items[3].word, std::string("work"));
    REQUIRE_EQ(items[4].source, CandidateSource::Emoji);
}

TEST_CASE(MixedEnglishLearnedSlotDoesNotPushCloudOrAiOffTheFirstPage)
{
    auto items = MixedEnglishItems();
    items.emplace_back("work", "云", 0, CandidateSource::CloudSuggestion);
    items.emplace_back("work", "AI", 0, CandidateSource::AiSuggestion);
    items.emplace_back("work", "😀", 0, CandidateSource::Emoji);
    FanyImeIpc::EnglishPlacement placement;
    placement.page_size = 6;
    placement.first_page_count = 3;
    placement.slot = 1;
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    REQUIRE_EQ(items.front().word, std::string("中文0"));
    REQUIRE_EQ(items[1].word, std::string("work"));
    const auto on_first_page = [&](CandidateSource source) {
        return std::any_of(items.begin(), items.begin() + 6,
                           [&](const WordItem &item) { return item.source == source; });
    };
    REQUIRE(on_first_page(CandidateSource::CloudSuggestion));
    REQUIRE(on_first_page(CandidateSource::AiSuggestion));
    REQUIRE_EQ(items[5].source, CandidateSource::Emoji);
}

TEST_CASE(MixedEnglishFirstPageCountUsesDifferentWordsRatherThanCaseVariants)
{
    auto items = MixedEnglishItems();
    items.insert(items.begin() + 13, WordItem("work", "Work", 0, CandidateSource::EnglishDictionary));
    FanyImeIpc::EnglishPlacement placement;
    placement.page_size = 6;
    placement.first_page_count = 3;
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    REQUIRE_EQ(items[1].word, std::string("work"));
    REQUIRE_EQ(items[2].word, std::string("works"));
    REQUIRE_EQ(items[3].word, std::string("worked"));
    REQUIRE_EQ(std::count_if(items.begin(), items.end(), [](const WordItem &item) { return item.word == "Work"; }), 1);
}

// 字母词（WordItem::latin_word）与混输符号的默认位置：只占普通候选的位置。第一条占首页里除中文首选外
// 最后一个普通候选的位置，其余接在它后面的普通位置上；首选、英文、emoji、日期、云、AI、快捷短语和固定
// 位置的候选都停在没有字母词与符号时的位置。
namespace
{
WordItem Chinese(int index)
{
    return WordItem("zl", "中文" + std::to_string(index), 100 - index);
}

WordItem Latin(const std::string &word, std::int64_t weight)
{
    WordItem item("zl", word, weight);
    item.latin_word = true;
    return item;
}

WordItem MixedSource(const std::string &word, CandidateSource source)
{
    return WordItem("zl", word, 0, source);
}

std::vector<WordItem> ChineseItems(int count)
{
    std::vector<WordItem> items;
    for (int index = 0; index < count; ++index)
        items.push_back(Chinese(index));
    return items;
}

std::vector<std::string> WordsOf(const std::vector<WordItem> &items)
{
    std::vector<std::string> words;
    for (const auto &item : items)
        words.push_back(item.word);
    return words;
}

std::string Listing(const std::vector<std::string> &words)
{
    std::string text;
    for (const auto &word : words)
        text += (text.empty() ? "" : " | ") + word;
    return "[" + text + "]";
}

// 整张列表逐项等于 expected；不等时把两张列表都写进失败信息。
void RequireWords(const std::vector<WordItem> &items, const std::vector<std::string> &expected)
{
    const auto actual = WordsOf(items);
    if (actual != expected)
        throw std::runtime_error("Candidate order " + Listing(actual) + " should be " + Listing(expected));
}

// 列表的前 expected.size() 项逐项等于 expected。
void RequirePrefix(const std::vector<WordItem> &items, const std::vector<std::string> &expected)
{
    const auto actual = WordsOf(items);
    if (actual.size() < expected.size() || !std::equal(expected.begin(), expected.end(), actual.begin()))
        throw std::runtime_error("Candidate order " + Listing(actual) + " should start with " + Listing(expected));
}

std::vector<std::string> ChineseWords(int from, int to)
{
    std::vector<std::string> words;
    for (int index = from; index < to; ++index)
        words.push_back("中文" + std::to_string(index));
    return words;
}

std::vector<std::string> Concat(std::initializer_list<std::vector<std::string>> parts)
{
    std::vector<std::string> words;
    for (const auto &part : parts)
        words.insert(words.end(), part.begin(), part.end());
    return words;
}

FanyImeIpc::EnglishPlacement PageOf(size_t page_size)
{
    FanyImeIpc::EnglishPlacement placement;
    placement.page_size = page_size;
    return placement;
}

// 再归一化一次，每一项都不动（异步结果回来、翻页扩展都会再排一次）。
void RequireStableUnderRenormalization(std::vector<WordItem> items, const FanyImeIpc::EnglishPlacement &placement)
{
    const auto once = WordsOf(items);
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    RequireWords(items, once);
}
} // namespace

TEST_CASE(LatinWordTakesTheLastSlotOfTheFirstPage)
{
    // 引擎把字母词插在覆盖整串的候选之后（这里是下标 2）；server 把它挪到首页末位。
    auto items = ChineseItems(12);
    items.insert(items.begin() + 2, Latin("浙L", 4));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(6));
    RequireWords(items, Concat({ChineseWords(0, 5), {"浙L"}, ChineseWords(5, 12)}));
    RequireStableUnderRenormalization(items, PageOf(6));
}

TEST_CASE(LatinWordAtTheHeadMovesBehindTheChineseCandidates)
{
    // nabla：引擎里没有覆盖整串的普通词，字母词排第一。
    auto items = ChineseItems(8);
    items.insert(items.begin(), Latin("Nabla算子", 3));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(5));
    RequireWords(items, Concat({ChineseWords(0, 4), {"Nabla算子"}, ChineseWords(4, 8)}));
}

TEST_CASE(OtherLatinWordsFollowTheFirstInTheEngineOrder)
{
    // 整串命中的 U盘（权重 5）在前，补全的 U盘锁（权重 20）在后：server 不按权重重排。
    auto items = ChineseItems(10);
    items.insert(items.begin() + 1, Latin("U盘", 5));
    items.insert(items.begin() + 2, Latin("U盘锁", 20));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(6));
    RequireWords(items, Concat({ChineseWords(0, 5), {"U盘", "U盘锁"}, ChineseWords(5, 10)}));
    RequireStableUnderRenormalization(items, PageOf(6));
}

TEST_CASE(FirstSymbolFollowsTheLatinWordsAndTheRestFollowKaomoji)
{
    auto items = ChineseItems(8);
    items.insert(items.begin() + 1, Latin("T恤", 10));
    items.insert(items.begin() + 2, Latin("T恤衫", 9));
    items.push_back(MixedSource("→", CandidateSource::Symbol));
    items.push_back(MixedSource("←", CandidateSource::Symbol));
    items.push_back(MixedSource("(^_^)", CandidateSource::Kaomoji));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(6));
    RequireWords(items, Concat({ChineseWords(0, 5), {"T恤", "T恤衫", "→"}, ChineseWords(5, 8), {"(^_^)", "←"}}));
    RequireStableUnderRenormalization(items, PageOf(6));
}

TEST_CASE(FirstSymbolAloneTakesTheLastSlotOfTheFirstPage)
{
    // jt：首个符号在首页末位，其余符号在颜文字之后。
    auto items = ChineseItems(8);
    items.insert(items.begin() + 1, MixedSource("→", CandidateSource::Symbol));
    items.insert(items.begin() + 2, MixedSource("←", CandidateSource::Symbol));
    items.push_back(MixedSource("(^_^)", CandidateSource::Kaomoji));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(6));
    RequireWords(items, Concat({ChineseWords(0, 5), {"→"}, ChineseWords(5, 8), {"(^_^)", "←"}}));
}

TEST_CASE(LatinWordTakesTheLastOrdinarySlotBeforeEnglishAndEmoji)
{
    // 首页末尾是 work、emoji 时，字母词占它们前面最后一个中文的位置；其余字母词与符号接在 emoji 后面。
    auto items = MixedEnglishItems();
    items.insert(items.begin() + 1, Latin("T恤", 10));
    items.insert(items.begin() + 2, Latin("T恤衫", 9));
    items.push_back(MixedSource("→", CandidateSource::Symbol));
    items.push_back(MixedSource("\xF0\x9F\x98\x80", CandidateSource::Emoji));
    FanyImeIpc::EnglishPlacement placement = PageOf(6);
    placement.require_exact = true;
    placement.input = "wor";
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    RequirePrefix(items,
                  Concat({ChineseWords(0, 3), {"T恤", "work", "\xF0\x9F\x98\x80", "T恤衫", "→"}, ChineseWords(3, 12)}));
    RequireStableUnderRenormalization(items, placement);
}

TEST_CASE(FirstPageTailWithoutEnglishPutsTheLatinWordBeforeTheEmoji)
{
    auto items = ChineseItems(10);
    items.insert(items.begin() + 1, Latin("T恤", 10));
    items.insert(items.begin() + 2, Latin("T恤衫", 9));
    items.push_back(MixedSource("→", CandidateSource::Symbol));
    items.push_back(MixedSource("\xF0\x9F\x98\x80", CandidateSource::Emoji));
    items.push_back(MixedSource("\xF0\x9F\x98\x81", CandidateSource::Emoji));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(6));
    RequireWords(items, Concat({ChineseWords(0, 4),
                                {"T恤", "\xF0\x9F\x98\x80", "T恤衫", "→"},
                                ChineseWords(4, 10),
                                {"\xF0\x9F\x98\x81"}}));
    RequireStableUnderRenormalization(items, PageOf(6));
}

TEST_CASE(ExactEnglishKeepsItsSlotNextToTheLatinWord)
{
    auto items = MixedEnglishItems();
    items.insert(items.begin() + 1, Latin("T恤", 10));
    items.push_back(MixedSource("\xF0\x9F\x98\x80", CandidateSource::Emoji));
    FanyImeIpc::EnglishPlacement placement = PageOf(6);
    placement.require_exact = true;
    placement.input = "work";
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    RequirePrefix(items, {"中文0", "work", "中文1", "中文2", "T恤", "\xF0\x9F\x98\x80", "中文3"});
}

TEST_CASE(LearnedEnglishSlotKeepsItsPlaceNextToTheLatinWord)
{
    // 学到的槽位照旧；字母词占首页里最后一个中文的位置。
    for (const auto &[slot, latin_index] : std::vector<std::pair<size_t, size_t>>{{2, 5}, {5, 4}})
    {
        auto items = MixedEnglishItems();
        items.insert(items.begin() + 1, Latin("T恤", 10));
        FanyImeIpc::EnglishPlacement placement = PageOf(6);
        placement.slot = slot;
        FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
        REQUIRE_EQ(items[slot].word, std::string("work"));
        REQUIRE_EQ(items[latin_index].word, std::string("T恤"));
    }
}

TEST_CASE(LatinWordInAShortListTakesTheLastOrdinarySlot)
{
    // 不满一页：占列表里除首选外最后一个普通候选的位置。
    auto items = ChineseItems(3);
    items.insert(items.begin() + 1, Latin("浙L", 4));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(6));
    RequireWords(items, {"中文0", "中文1", "浙L", "中文2"});

    // 首个 emoji 在它的位置上不动，第二条字母词接在它后面。
    items = ChineseItems(2);
    items.insert(items.begin() + 1, Latin("T恤", 10));
    items.insert(items.begin() + 2, Latin("T恤衫", 9));
    items.push_back(MixedSource("\xF0\x9F\x98\x80", CandidateSource::Emoji));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(6));
    RequireWords(items, {"中文0", "T恤", "\xF0\x9F\x98\x80", "T恤衫", "中文1"});

    // 除首选外没有普通候选：排在末尾。
    items = ChineseItems(1);
    items.insert(items.begin(), Latin("浙L", 4));
    items.push_back(MixedSource("\xF0\x9F\x98\x80", CandidateSource::Emoji));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(6));
    RequireWords(items, {"中文0", "\xF0\x9F\x98\x80", "浙L"});

    // 不知道每页候选数时整张列表当一页。
    items = ChineseItems(3);
    items.insert(items.begin(), Latin("浙L", 4));
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    RequireWords(items, {"中文0", "中文1", "浙L", "中文2"});
}

TEST_CASE(SmallPageWithoutOrdinarySlotsPutsTheLatinWordAfterTheFirstPage)
{
    // 每页 3：首页是 首选、英文补全、emoji，没有可占的普通位置，字母词排到首页之后的第一个位置。
    auto items = MixedEnglishItems();
    items.insert(items.begin() + 1, Latin("T恤", 10));
    items.push_back(MixedSource("\xF0\x9F\x98\x80", CandidateSource::Emoji));
    FanyImeIpc::EnglishPlacement placement = PageOf(3);
    placement.require_exact = true;
    placement.input = "wor";
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    RequirePrefix(items, {"中文0", "work", "\xF0\x9F\x98\x80", "T恤", "中文1"});

    // 英文精确匹配紧跟首选，同样不被挤出首页。
    items = MixedEnglishItems();
    items.insert(items.begin() + 1, Latin("T恤", 10));
    items.push_back(MixedSource("\xF0\x9F\x98\x80", CandidateSource::Emoji));
    placement.input = "work";
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    RequirePrefix(items, {"中文0", "work", "\xF0\x9F\x98\x80", "T恤", "中文1"});
}

// 在 control 里插入 latin：字母词插在其余候选的第 at 个之前，正停在固定位置上的候选留在原下标
// （apply_fixed_positions 在含字母词的列表上放固定候选，结果就是这样）。
std::vector<WordItem> WithLatinInsertedKeepingFixed(std::vector<WordItem> control, size_t at,
                                                    std::vector<WordItem> latin)
{
    std::vector<std::pair<size_t, WordItem>> fixed;
    std::vector<WordItem> others;
    for (size_t index = 0; index < control.size(); ++index)
    {
        if (control[index].fixed_position > 0)
            fixed.emplace_back(index, std::move(control[index]));
        else
            others.push_back(std::move(control[index]));
    }
    others.insert(others.begin() + static_cast<std::ptrdiff_t>((std::min)(at, others.size())),
                  std::make_move_iterator(latin.begin()), std::make_move_iterator(latin.end()));
    std::vector<WordItem> result;
    size_t next_fixed = 0;
    for (auto &item : others)
    {
        while (next_fixed < fixed.size() && fixed[next_fixed].first <= result.size())
            result.push_back(std::move(fixed[next_fixed++].second));
        result.push_back(std::move(item));
    }
    for (; next_fixed < fixed.size(); ++next_fixed)
        result.push_back(std::move(fixed[next_fixed].second));
    return result;
}

TEST_CASE(FixedPositionsKeepTheirPlaceAroundTheLatinWord)
{
    // 固定在首页末位的英文留在那里，字母词占它前面最后一个中文的位置。
    auto items = MixedEnglishItems();
    items.back().fixed_position = 6;
    items.insert(items.begin() + 1, Latin("T恤", 10));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(6));
    RequirePrefix(items, {"中文0", "work", "中文1", "中文2", "T恤", "workplace", "中文3"});
    RequireStableUnderRenormalization(items, PageOf(6));

    // 固定在第 6 位的中文停在那里；字母词在引擎里排在它前面也一样。
    auto control = ChineseItems(10);
    control[5].fixed_position = 6;
    items = WithLatinInsertedKeepingFixed(control, 1, {Latin("T恤", 10)});
    REQUIRE_EQ(items[5].word, std::string("中文5"));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(6));
    RequireWords(items, Concat({ChineseWords(0, 4), {"T恤", "中文5", "中文4"}, ChineseWords(6, 10)}));
    RequireStableUnderRenormalization(items, PageOf(6));

    // zhel 组页：着 固定在第 4 位，引擎把 浙L 放在它前面；组页后 着 仍在第 4 位。
    control = {WordItem("zhel", "这里", 300),
               WordItem("zhel", "这类", 200),
               WordItem("zhel", "这", 500),
               WordItem("zhel", "着", 1),
               Chinese(5),
               Chinese(6),
               Chinese(7)};
    control[3].fixed_position = 4;
    items = WithLatinInsertedKeepingFixed(control, 2, {Latin("浙L", 4)});
    RequireWords(items, {"这里", "这类", "浙L", "着", "这", "中文5", "中文6", "中文7"});
    FanyImeIpc::PlaceQueriedLatinWords(items, 6);
    RequireWords(items, {"这里", "这类", "这", "着", "中文5", "浙L", "中文6", "中文7"});
}

WordItem SpellingOf(WordItem head, const std::string &text)
{
    head.output_text = text;
    head.variant_rank = 1;
    return head;
}

TEST_CASE(LatinWordsMoveAsWholeSpellingGroups)
{
    // 繁体：T恤 有两种写法，整组连续放，占首页末尾 emoji 前的两个普通位置。
    auto items = ChineseItems(8);
    const WordItem tee = Latin("T恤", 10);
    items.insert(items.begin() + 1, tee);
    items.insert(items.begin() + 2, SpellingOf(tee, "T卹"));
    items.push_back(MixedSource("\xF0\x9F\x98\x80", CandidateSource::Emoji));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(6));
    RequireWords(items, Concat({ChineseWords(0, 3), {"T恤", "T恤", "\xF0\x9F\x98\x80"}, ChineseWords(3, 8)}));
    REQUIRE_EQ(items[4].variant_rank, 1);
    RequireStableUnderRenormalization(items, PageOf(6));

    // 中文3 有两种写法、正好排在 emoji 前：字母词放上首页就得拆开它或挪动 emoji，所以排到首页之后。
    items = ChineseItems(8);
    items.insert(items.begin() + 4, SpellingOf(Chinese(3), "中文叁"));
    items.insert(items.begin() + 1, Latin("浙L", 4));
    items.push_back(MixedSource("\xF0\x9F\x98\x80", CandidateSource::Emoji));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(6));
    RequireWords(items, Concat({ChineseWords(0, 4), {"中文3", "\xF0\x9F\x98\x80", "浙L"}, ChineseWords(4, 8)}));
    REQUIRE_EQ(items[4].variant_rank, 1);
}

TEST_CASE(SpellingGroupTakesTheLastRunOfOrdinarySlotsThatHoldsIt)
{
    // 每页 6，英文学到第 5 位：首页的普通位置是下标 1–3 与 5，两种写法的 T恤 整组放进能放下它的最靠后一段，
    // 即下标 2、3。
    const WordItem tee = Latin("T恤", 10);
    auto items = ChineseItems(8);
    items.insert(items.begin() + 1, tee);
    items.insert(items.begin() + 2, SpellingOf(tee, "T卹"));
    items.push_back(WordItem("work", "work", 0, CandidateSource::EnglishDictionary));
    FanyImeIpc::EnglishPlacement placement = PageOf(6);
    placement.slot = 4;
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    RequireWords(items, Concat({ChineseWords(0, 2), {"T恤", "T恤", "work"}, ChineseWords(2, 8)}));
    REQUIRE_EQ(items[3].variant_rank, 1);
    RequireStableUnderRenormalization(items, placement);

    // 中文4 固定在第 5 位：普通位置是下标 1–3 与 5，同样放进下标 2、3，后面还留着首页上的 中文2。
    items = ChineseItems(8);
    items[4].fixed_position = 5;
    items.insert(items.begin() + 1, tee);
    items.insert(items.begin() + 2, SpellingOf(tee, "T卹"));
    std::rotate(items.begin() + 4, items.begin() + 6, items.begin() + 7);
    RequirePrefix(items, Concat({{"中文0", "T恤", "T恤", "中文1", "中文4", "中文2"}}));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(6));
    RequireWords(items, Concat({{"中文0", "中文1", "T恤", "T恤", "中文4", "中文2", "中文3"}, ChineseWords(5, 8)}));
    RequireStableUnderRenormalization(items, PageOf(6));
}

namespace
{
// 组页放一次字母词，异步候选按 at 插进去再归一化；结果等于把同一组候选一起归一化一次，再归一化不变。
std::vector<WordItem> PlacedThenArrived(std::vector<WordItem> engine, const WordItem &async, size_t at,
                                        size_t page_size)
{
    auto items = engine;
    FanyImeIpc::PlaceQueriedLatinWords(items, page_size);
    items.insert(items.begin() + static_cast<std::ptrdiff_t>((std::min)(at, items.size())), async);
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(page_size));
    auto once = engine;
    once.push_back(async);
    FanyImeIpc::NormalizeMixedCandidateOrder(once, 1, PageOf(page_size));
    RequireWords(items, WordsOf(once));
    RequireStableUnderRenormalization(items, PageOf(page_size));
    return items;
}
} // namespace

TEST_CASE(AsyncCandidateInsideALatinSpellingGroupKeepsItWhole)
{
    const WordItem tee = Latin("T恤", 10);
    const WordItem tee_variant = SpellingOf(tee, "T卹");
    const WordItem ai = MixedSource("AI", CandidateSource::AiSuggestion);

    // 不满一页：组页后 T恤 在下标 1、2，AI 回来插在下标 2，落进组里。
    auto items = PlacedThenArrived({Chinese(0), Chinese(1), tee, tee_variant}, ai, 2, 6);
    RequireWords(items, {"中文0", "AI", "T恤", "T恤", "中文1"});
    REQUIRE_EQ(items[3].variant_rank, 1);

    // 每页 3：组页后 T恤 在下标 1、2；AI 回来后首页放不下整组，整组排到首页之后。
    items = PlacedThenArrived({Chinese(0), Chinese(1), tee, tee_variant, Chinese(2), Chinese(3)}, ai, 2, 3);
    RequireWords(items, {"中文0", "AI", "中文1", "T恤", "T恤", "中文2", "中文3"});
    REQUIRE_EQ(items[4].variant_rank, 1);

    // 只有字母词：云、英文回来插在下标 1。
    for (const auto &async : {MixedSource("云", CandidateSource::CloudSuggestion),
                              WordItem("tshirt", "tshirt", 0, CandidateSource::EnglishDictionary)})
    {
        items = PlacedThenArrived({tee, tee_variant}, async, 1, 6);
        RequireWords(items, {async.word, "T恤", "T恤"});
        REQUIRE_EQ(items[2].variant_rank, 1);
    }
}

TEST_CASE(SpellingGroupSplitByOtherCandidatesStaysWholeAfterTakingAndPlacing)
{
    // 中文的第二写法与它隔着几个用户词（来源不同，不归这一组）；取出、放置之后整组相连，其余候选先后不变。
    const auto user_word = [](int index) {
        return WordItem("zl", "用户词" + std::to_string(index), 50 - index, CandidateSource::UserDatabase);
    };

    std::vector<WordItem> items = {Chinese(0),       Chinese(1),   user_word(2),
                                   Latin("T恤", 10), user_word(4), SpellingOf(Chinese(1), "中文壹"),
                                   user_word(6),     user_word(7)};
    const auto taken =
        FanyImeIpc::TakeOutKeepingFixedInPlace(items, [](const WordItem &item) { return item.latin_word; });
    RequireWords(taken, {"T恤"});
    RequireWords(items, {"中文0", "中文1", "中文1", "用户词2", "用户词4", "用户词6", "用户词7"});
    REQUIRE_EQ(items[2].variant_rank, 1);

    items = {Chinese(0),  Chinese(1), Chinese(2), Chinese(3), user_word(4), SpellingOf(Chinese(3), "中文叁"),
             user_word(6)};
    FanyImeIpc::PlaceInOrdinarySlots(items, {Latin("T恤", 10)}, 3);
    RequireWords(items, {"中文0", "中文1", "T恤", "中文2", "中文3", "中文3", "用户词4", "用户词6"});
    REQUIRE_EQ(items[5].variant_rank, 1);
}

TEST_CASE(FixedLatinWordStaysAtItsFixedIndex)
{
    // 固定了位置的字母词不取走、不重新放：留在原下标，那一格显示的仍是它。
    std::vector<WordItem> items = ChineseItems(8);
    WordItem fixed = Latin("T恤", 10);
    fixed.fixed_position = 3;
    items.insert(items.begin() + 3, fixed);
    const auto expected = Concat({ChineseWords(0, 3), {"T恤"}, ChineseWords(3, 8)});

    auto taken_from = items;
    const auto taken =
        FanyImeIpc::TakeOutKeepingFixedInPlace(taken_from, [](const WordItem &item) { return item.latin_word; });
    REQUIRE(taken.empty());
    RequireWords(taken_from, expected);

    auto queried = items;
    FanyImeIpc::PlaceQueriedLatinWords(queried, 6);
    RequireWords(queried, expected);
    REQUIRE_EQ(queried[3].fixed_position, 3);
    REQUIRE(queried[3].latin_word);

    auto normalized = items;
    FanyImeIpc::NormalizeMixedCandidateOrder(normalized, 1, PageOf(6));
    RequireWords(normalized, expected);
    REQUIRE_EQ(normalized[3].fixed_position, 3);
}

TEST_CASE(FixedSymbolIsNotTakenAsTheFirstSymbol)
{
    // 固定了位置的混输符号不取走、也不算首个符号：取走的是它后面那个没固定的符号。
    std::vector<WordItem> items = ChineseItems(8);
    WordItem fixed = MixedSource("→", CandidateSource::Symbol);
    fixed.fixed_position = 3;
    items.insert(items.begin() + 3, fixed);
    items.push_back(MixedSource("←", CandidateSource::Symbol));

    bool first_symbol_taken = false;
    const auto taken = FanyImeIpc::TakeOutKeepingFixedInPlace(items, [&](const WordItem &item) {
        if (item.source != CandidateSource::Symbol || first_symbol_taken)
            return false;
        first_symbol_taken = true;
        return true;
    });
    RequireWords(taken, {"←"});
    RequireWords(items, Concat({ChineseWords(0, 3), {"→"}, ChineseWords(3, 8)}));
    REQUIRE_EQ(items[3].fixed_position, 3);
}

TEST_CASE(FirstSymbolFollowsTheLatinWordsEvenWhenListedBeforeThem)
{
    // 首个符号在列表里排在字母词前面，放的时候仍排在字母词后面。
    std::vector<WordItem> items = ChineseItems(8);
    items.insert(items.begin() + 1, MixedSource("→", CandidateSource::Symbol));
    items.push_back(Latin("T恤", 10));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(6));
    RequireWords(items, Concat({ChineseWords(0, 5), {"T恤", "→"}, ChineseWords(5, 8)}));
}

TEST_CASE(ArrangedQueriedCandidatesExpandSpellingsBeforePlacingLatinWords)
{
    // 单字母输入先截到 24 个候选，再展开写法，最后把字母词放进普通位置：展开出的写法占首页的位置。
    std::vector<WordItem> engine = ChineseItems(30);
    engine.insert(engine.begin() + 1, Latin("T恤", 10));
    size_t expanded_count = 0;
    const auto expand = [&](std::vector<WordItem> items) {
        expanded_count = items.size();
        items.insert(items.begin() + 1, SpellingOf(items.front(), "中文零"));
        return items;
    };
    auto items = FanyImeIpc::ArrangeQueriedCandidates(engine, true, expand, 6);
    REQUIRE_EQ(expanded_count, size_t{24});
    RequireWords(items, Concat({{"中文0", "中文0"}, ChineseWords(1, 4), {"T恤"}, ChineseWords(4, 23)}));

    // 不是单字母输入时不截。
    items = FanyImeIpc::ArrangeQueriedCandidates(engine, false, expand, 6);
    REQUIRE_EQ(expanded_count, size_t{31});
    RequireWords(items, Concat({{"中文0", "中文0"}, ChineseWords(1, 4), {"T恤"}, ChineseWords(4, 30)}));
}

TEST_CASE(GeneratedSentenceWithLettersStaysAnOrdinaryCandidate)
{
    // 整句 买一件T恤、做T台 含字母，但不是字母词那一行：照普通候选排，不跟字母词一起挪。
    std::vector<WordItem> items = {WordItem("x", "买一件T恤", 1, CandidateSource::Generated),
                                   Latin("T恤", 10),
                                   Chinese(0),
                                   Chinese(1),
                                   WordItem("x", "做T台", 1, CandidateSource::Generated),
                                   Chinese(2),
                                   Chinese(3),
                                   Chinese(4),
                                   Chinese(5)};
    const std::vector<std::string> expected = {"买一件T恤", "中文0", "中文1", "做T台", "中文2",
                                               "T恤",       "中文3", "中文4", "中文5"};
    auto queried = items;
    FanyImeIpc::PlaceQueriedLatinWords(queried, 6);
    RequireWords(queried, expected);
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(6));
    RequireWords(items, expected);
}

TEST_CASE(QueriedThenAsyncNormalizationEqualsOneNormalization)
{
    // 组页先放一次字母词，异步英文、emoji、符号、云回来再归一化一次，结果等于一次归一化全部候选。
    std::vector<WordItem> engine = ChineseItems(10);
    engine.insert(engine.begin() + 1, Latin("T恤", 10));
    engine.insert(engine.begin() + 2, Latin("T恤衫", 9));
    engine[6].fixed_position = 7;
    const std::vector<WordItem> async = {WordItem("work", "work", 0, CandidateSource::EnglishDictionary),
                                         MixedSource("\xF0\x9F\x98\x80", CandidateSource::Emoji),
                                         MixedSource("→", CandidateSource::Symbol),
                                         MixedSource("云", CandidateSource::CloudSuggestion)};
    FanyImeIpc::EnglishPlacement placement = PageOf(6);
    placement.require_exact = true;
    placement.input = "wor";

    auto twice = engine;
    FanyImeIpc::PlaceQueriedLatinWords(twice, 6);
    twice.insert(twice.end(), async.begin(), async.end());
    FanyImeIpc::NormalizeMixedCandidateOrder(twice, 1, placement);

    auto once = engine;
    once.insert(once.end(), async.begin(), async.end());
    FanyImeIpc::NormalizeMixedCandidateOrder(once, 1, placement);
    RequireWords(twice, WordsOf(once));
    RequireStableUnderRenormalization(twice, placement);
}

TEST_CASE(MixedOrderWithoutLatinWordsOrSymbolsIsUnchanged)
{
    // 没有字母词、没有符号的列表：期望值按改动前的规则手排。
    auto items = ChineseItems(3);
    items.push_back(MixedSource("\xF0\x9F\x98\x80", CandidateSource::Emoji));
    items.push_back(MixedSource("云", CandidateSource::CloudSuggestion));
    items.push_back(MixedSource("AI", CandidateSource::AiSuggestion));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(6));
    RequireWords(items, {"中文0", "云", "AI", "中文1", "中文2", "\xF0\x9F\x98\x80"});

    items = MixedEnglishItems();
    items.push_back(MixedSource("\xF0\x9F\x98\x80", CandidateSource::Emoji));
    items.push_back(MixedSource("(^_^)", CandidateSource::Kaomoji));
    FanyImeIpc::EnglishPlacement placement = PageOf(6);
    placement.require_exact = true;
    placement.input = "wor";
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    RequireWords(items, Concat({ChineseWords(0, 4),
                                {"work", "\xF0\x9F\x98\x80"},
                                ChineseWords(4, 12),
                                {"works", "worked", "working", "workplace", "(^_^)"}}));

    items = ChineseItems(4);
    items.push_back(MixedSource("2026年11月2日", CandidateSource::DateTime));
    items.push_back(MixedSource("云", CandidateSource::CloudSuggestion));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, PageOf(6));
    RequireWords(items, {"中文0", "2026年11月2日", "云", "中文1", "中文2", "中文3"});
}

TEST_CASE(QueriedLatinWordsMoveAloneAndLeaveCachedCloudAndAiInPlace)
{
    // 组页只挪字母词：没有字母词时列表不动；有字母词时缓存的云、AI 也留在引擎给的位置。
    std::vector<WordItem> items = {Chinese(0), Chinese(1), MixedSource("云", CandidateSource::CloudSuggestion),
                                   Chinese(2), MixedSource("AI", CandidateSource::AiSuggestion)};
    const auto before = WordsOf(items);
    FanyImeIpc::PlaceQueriedLatinWords(items, 6);
    RequireWords(items, before);

    items = ChineseItems(8);
    items.insert(items.begin(), Latin("浙L", 4));
    FanyImeIpc::PlaceQueriedLatinWords(items, 6);
    RequireWords(items, Concat({ChineseWords(0, 5), {"浙L"}, ChineseWords(5, 8)}));

    items = ChineseItems(7);
    items.insert(items.begin() + 1, MixedSource("AI", CandidateSource::AiSuggestion));
    items.insert(items.begin() + 2, Latin("浙L", 4));
    FanyImeIpc::PlaceQueriedLatinWords(items, 6);
    RequireWords(items, {"中文0", "AI", "中文1", "中文2", "中文3", "浙L", "中文4", "中文5", "中文6"});
}

// 真引擎查出的列表经组页的放置函数：夹具与 engine/tests/src/test_latin_words.cpp 同一批词（出厂词库没有
// latin_words 表，字母词只能靠夹具）。
namespace
{
std::filesystem::path CreateLatinWordFixture()
{
    // PID 后缀：并行 ctest 或多工作树同时跑时不删到别人的夹具。
    const auto directory =
        test::TempRoot() / ("msime-latin-word-placement-test-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    std::filesystem::create_directories(directory);
    sqlite3 *database = nullptr;
    if (sqlite3_open(test::Utf8(directory / "msime.db").c_str(), &database) != SQLITE_OK)
    {
        sqlite3_close(database);
        throw std::runtime_error("Failed to create the latin-word placement fixture dictionary.");
    }
    char *error = nullptr;
    const int rc = sqlite3_exec(database,
                                "BEGIN;"
                                "CREATE TABLE tbl_1_z(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                "INSERT INTO tbl_1_z VALUES('zhe','z','这',500);"
                                "CREATE TABLE tbl_1_n(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                "INSERT INTO tbl_1_n VALUES('na','n','那',500);"
                                "CREATE TABLE tbl_2_n(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                "INSERT INTO tbl_2_n VALUES('na''bian','nb','那边',300);"
                                "INSERT INTO tbl_2_n VALUES('na''bian','nb','哪边',200);"
                                "CREATE TABLE tbl_1_j(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                "INSERT INTO tbl_1_j VALUES('ji','j','机',500);"
                                "CREATE TABLE tbl_2_j(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                "INSERT INTO tbl_2_j VALUES('ji''ben','jb','基本',300);"
                                "CREATE TABLE tbl_2_t(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                "INSERT INTO tbl_2_t VALUES('te''xu','tx','特许',100);"
                                "INSERT INTO tbl_2_t VALUES('tong''xue','tx','同学',200);"
                                "CREATE TABLE tbl_3_t(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                "CREATE TABLE tbl_2_z(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                "INSERT INTO tbl_2_z VALUES('zhe''li','zl','这里',300);"
                                "INSERT INTO tbl_2_z VALUES('zhe''lei','zl','这类',200);"
                                "CREATE TABLE latin_words(key TEXT, value TEXT, weight INTEGER, exact_only INTEGER);"
                                "INSERT INTO latin_words VALUES('t''xu','T恤',10,0);"
                                "INSERT INTO latin_words VALUES('t''xu''shan','T恤衫',9,0);"
                                "INSERT INTO latin_words VALUES('zhe''l','浙L',4,1);"
                                "INSERT INTO latin_words VALUES('ji''b','吉B',4,1);"
                                "INSERT INTO latin_words VALUES('n''a''b''l''a''suan''zi','Nabla算子',3,0);"
                                "COMMIT;",
                                nullptr, nullptr, &error);
    const std::string message = error == nullptr ? "SQLite operation failed." : error;
    sqlite3_free(error);
    sqlite3_close(database);
    if (rc != SQLITE_OK)
        throw std::runtime_error(message);
    return directory;
}

metasequoia::RuntimePaths LatinFixturePaths(const std::filesystem::path &directory)
{
    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;
    return paths;
}

std::vector<WordItem> QueriedCandidates(metasequoia::InputSession &session, const std::string &input)
{
    for (const char character : input)
        REQUIRE(session.handle_character(character).handled);
    return session.candidates();
}

std::vector<std::string> WordsWhere(const std::vector<WordItem> &items, bool latin)
{
    std::vector<std::string> words;
    for (const auto &item : items)
    {
        if (item.latin_word == latin)
            words.push_back(item.word);
    }
    return words;
}

// 组页后：普通候选彼此的先后与引擎给的相同；字母词按引擎给的先后连成一段，第一条占首页里除首选外最后
// 一个普通候选的位置（列表不满一页时取整张列表里的，除首选外没有普通候选时排在末尾）；每条字母词只出现
// 一次；leader 非空时首选是它。
void RequireQueriedPlacement(const std::vector<WordItem> &engine_items, const std::string &context,
                             const std::string &leader)
{
    const auto ordinary = WordsWhere(engine_items, false);
    const auto latin = WordsWhere(engine_items, true);
    if (latin.empty())
        throw std::runtime_error("The engine must offer a latin word for " + context);
    for (size_t page_size : {size_t{2}, size_t{3}, size_t{4}, size_t{6}})
    {
        auto items = engine_items;
        FanyImeIpc::PlaceQueriedLatinWords(items, page_size);
        const size_t ordinary_count = ordinary.size();
        const size_t first_latin = ordinary_count >= page_size ? page_size - 1
                                   : ordinary_count >= 2       ? ordinary_count - 1
                                                               : ordinary_count;
        std::vector<std::string> expected(ordinary.begin(), ordinary.begin() + first_latin);
        expected.insert(expected.end(), latin.begin(), latin.end());
        expected.insert(expected.end(), ordinary.begin() + first_latin, ordinary.end());
        const auto actual = WordsOf(items);
        if (actual != expected || (!leader.empty() && items.front().word != leader))
            throw std::runtime_error(context + " with " + std::to_string(page_size) + " per page: " + Listing(actual) +
                                     " should be " + Listing(expected) + ", led by " + leader);
    }
}
} // namespace

TEST_CASE(QueriedLatinWordsMoveToTheFirstPageEndThroughTheRealEngine)
{
    const auto directory = CreateLatinWordFixture();
    const auto paths = LatinFixturePaths(directory);
    const auto quanpin = [&](const std::string &input, bool lattice) {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        if (lattice)
        {
            SentenceAssociationOptions association;
            association.word_lattice = true;
            session.set_sentence_association(association);
        }
        return QueriedCandidates(session, input);
    };
    RequireQueriedPlacement(quanpin("zhel", false), "zhel", "这里");
    RequireQueriedPlacement(quanpin("jib", false), "jib", "基本");
    RequireQueriedPlacement(quanpin("nabla", false), "nabla", "那边");
    // 词格对 t'xu 解出的整句就是 T恤：组页后 T恤 只有一条。
    const auto txu = quanpin("txu", true);
    REQUIRE_EQ(std::count_if(txu.begin(), txu.end(), [](const WordItem &item) { return item.word == "T恤"; }), 1);
    RequireQueriedPlacement(txu, "txu with the lattice", "特许");

    // nab 没打全字母段 nabla：没有字母词，组页不动引擎的列表。
    const auto nab = quanpin("nab", false);
    REQUIRE(WordsWhere(nab, true).empty());
    auto placed = nab;
    FanyImeIpc::PlaceQueriedLatinWords(placed, 6);
    RequireWords(placed, WordsOf(nab));
    REQUIRE_EQ(placed.front().word, std::string("那边"));

    const auto &xiaohe = GetXiaoheShuangpinProfile();
    const auto shuangpin = [&](const std::string &input) {
        metasequoia::InputSession session(SchemeType::Shuangpin, xiaohe, paths);
        session.set_shuangpin_helpcode_enabled(false);
        return QueriedCandidates(session, input);
    };
    RequireQueriedPlacement(shuangpin("vel"), "xiaohe vel", "这里");
    RequireQueriedPlacement(shuangpin("txu"), "xiaohe txu", "特许");
    RequireQueriedPlacement(shuangpin("nabla"), "xiaohe nabla", "");
    REQUIRE(WordsWhere(shuangpin("nab"), true).empty());

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}

// 差分性质：同一组输入，带字母词与符号归一化的结果，与去掉字母词与符号（固定位置的候选留在原下标）
// 后归一化的对照结果相比，字母词与符号只占了普通候选的位置。
namespace
{
// 普通候选的来源：混输各来源与快捷短语以外的本地候选。与实现的 IsOrdinaryCandidateSource 按契约第 1 条各写一份。
bool IsOrdinarySourceForTest(CandidateSource source)
{
    switch (source)
    {
    case CandidateSource::CloudSuggestion:
    case CandidateSource::AiSuggestion:
    case CandidateSource::EnglishDictionary:
    case CandidateSource::QuickPhrase:
    case CandidateSource::Emoji:
    case CandidateSource::Kaomoji:
    case CandidateSource::Symbol:
    case CandidateSource::DateTime:
        return false;
    default:
        return true;
    }
}

std::string Key(const WordItem &item)
{
    return item.word + "#" + std::to_string(item.variant_rank);
}

size_t IndexOfKey(const std::vector<WordItem> &items, const std::string &key)
{
    for (size_t index = 0; index < items.size(); ++index)
    {
        if (Key(items[index]) == key)
            return index;
    }
    return items.size();
}

// 对照结果里的一个写法组：从 start 起 size 项。
struct ControlUnit
{
    size_t start = 0;
    size_t size = 0;
    // 首选（第一个普通来源的组）、非普通来源、固定位置的组是对照项。
    bool anchored = false;
};

std::vector<ControlUnit> UnitsOf(const std::vector<WordItem> &control)
{
    std::vector<ControlUnit> units;
    bool seen_first_choice = false;
    for (size_t index = 0; index < control.size(); ++index)
    {
        const WordItem &item = control[index];
        if (item.variant_rank > 0 && !units.empty())
        {
            ++units.back().size;
            continue;
        }
        const bool ordinary_source = IsOrdinarySourceForTest(item.source);
        const bool first_choice = ordinary_source && !seen_first_choice;
        seen_first_choice = seen_first_choice || ordinary_source;
        units.push_back({index, 1, !ordinary_source || first_choice || item.fixed_position > 0});
    }
    return units;
}

// 按对照的形状算出放字母词与符号之后每一项该在哪，不调被测函数。
//
// 列表末尾那一串（首页之后、最后一个普通来源的项之后）留在末尾，整体后移 group 的项数（裁决 3）。其余对照项
// 停在原下标，普通组按先后填进其余位置；对照项 a 前面的普通位置数等于普通组某个前缀的项数之和时它停得住，
// 否则有一组跨过它，它让到组后面（规格第 7 条），从它起后面的对照项都不再断言下标。
// 字母词与符号插在第 p 个普通组之前：p 取首页上（开头下标小于每页候选数）的普通组里最靠后的一个，使得首页上
// 的对照项都停得住、第一组整组在首页（规格补充第 2 条）；没有这样的 p 时插在首页上最后一个普通组之后。
struct Prediction
{
    size_t head = 0;
    // 每个对照项（按写法组的组首下标）期望的下标；空表示不断言。
    std::vector<std::optional<size_t>> anchor_index;
    bool anchor_yields = false;
};

Prediction Predict(const std::vector<WordItem> &control, const std::vector<size_t> &group_sizes, size_t page_size)
{
    Prediction prediction;
    prediction.anchor_index.assign(control.size(), std::nullopt);
    const size_t page_end = page_size > 0 ? page_size : control.size() + 1;
    size_t tail_begin = page_size;
    for (size_t index = 0; index < control.size(); ++index)
    {
        if (IsOrdinarySourceForTest(control[index].source))
            tail_begin = (std::max)(tail_begin, index + 1);
    }
    size_t group_total = 0;
    for (size_t size : group_sizes)
        group_total += size;

    std::vector<ControlUnit> anchors;
    std::vector<size_t> ordinary_sizes;
    size_t first_page_ordinary = 0;
    for (const auto &unit : UnitsOf(control))
    {
        if (unit.start >= tail_begin)
        {
            prediction.anchor_index[unit.start] = unit.start + group_total;
            continue;
        }
        if (unit.anchored)
        {
            anchors.push_back(unit);
            continue;
        }
        first_page_ordinary += unit.start < page_end ? 1 : 0;
        ordinary_sizes.push_back(unit.size);
    }

    // 普通组插入 group 后，第一个停不住的对照项（anchors 里的序号）；都停得住时为 anchors.size()。
    const auto first_yield = [&](size_t position) {
        std::vector<size_t> stream(ordinary_sizes.begin(), ordinary_sizes.end());
        stream.insert(stream.begin() + static_cast<std::ptrdiff_t>(position), group_sizes.begin(), group_sizes.end());
        std::vector<size_t> prefix_sums{0};
        for (size_t size : stream)
            prefix_sums.push_back(prefix_sums.back() + size);
        size_t anchored_before = 0;
        for (size_t anchor = 0; anchor < anchors.size(); ++anchor)
        {
            const size_t ordinary_before = anchors[anchor].start - anchored_before;
            const bool holds = ordinary_before >= prefix_sums.back() ||
                               std::find(prefix_sums.begin(), prefix_sums.end(), ordinary_before) != prefix_sums.end();
            if (!holds)
                return anchor;
            anchored_before += anchors[anchor].size;
        }
        return anchors.size();
    };
    // group 第一组的开头：它前面的普通组项数，加上在那之前停住的对照项。
    const auto head_of = [&](size_t position) {
        size_t ordinary_before = 0;
        for (size_t index = 0; index < position; ++index)
            ordinary_before += ordinary_sizes[index];
        size_t head = ordinary_before;
        size_t anchored_before = 0;
        for (const auto &anchor : anchors)
        {
            if (anchor.start - anchored_before > ordinary_before)
                break;
            head += anchor.size;
            anchored_before += anchor.size;
        }
        return head;
    };
    const auto anchors_on_first_page = static_cast<size_t>(std::count_if(
        anchors.begin(), anchors.end(), [&](const ControlUnit &anchor) { return anchor.start < page_end; }));

    size_t position = first_page_ordinary;
    for (size_t candidate = first_page_ordinary; candidate-- > 0;)
    {
        if (first_yield(candidate) >= anchors_on_first_page && head_of(candidate) + group_sizes.front() <= page_end)
        {
            position = candidate;
            break;
        }
    }
    prediction.head = head_of(position);
    const size_t yield = first_yield(position);
    prediction.anchor_yields = yield < anchors.size();
    for (size_t anchor = 0; anchor < yield; ++anchor)
        prediction.anchor_index[anchors[anchor].start] = anchors[anchor].start;
    return prediction;
}

// 字母词按写法组切开的各组项数，有首个符号时末尾再加一项。
std::vector<size_t> GroupSizes(const std::vector<WordItem> &latin, bool with_symbol)
{
    std::vector<size_t> sizes;
    for (const auto &item : latin)
    {
        if (item.variant_rank > 0 && !sizes.empty())
            ++sizes.back();
        else
            sizes.push_back(1);
    }
    if (with_symbol)
        sizes.push_back(1);
    return sizes;
}

struct DifferentialCase
{
    // 组页时引擎给的候选（快捷短语、中文与它们的写法，固定位置的中文停在 fixed_position - 1）。
    std::vector<WordItem> engine;
    std::vector<WordItem> cloud;
    std::vector<WordItem> ai;
    // 一次混输结果：日期时间、英文、emoji、颜文字（符号另放 symbols）。
    std::vector<WordItem> mixed;
    std::vector<WordItem> latin;   // 引擎给的先后，含写法
    std::vector<WordItem> symbols; // 第一个是首个符号
    FanyImeIpc::EnglishPlacement placement;
    size_t insert_at = 0;
    // 为真时照产品的顺序走：组页放字母词，云、AI、混输结果按 arrival 的先后回来，各自插进列表再归一化；
    // 为假时全部候选一起归一化一次。
    bool product_flow = false;
    std::vector<int> arrival{0, 1, 2};
};

class CaseGenerator
{
  public:
    explicit CaseGenerator(std::uint32_t seed) : random_(seed)
    {
    }

    DifferentialCase Next()
    {
        DifferentialCase result;
        const size_t page_size = Pick(3, 9);
        result.placement.page_size = page_size;
        result.placement.first_page_count = Pick(1, 3);
        const bool spellings = Chance(30);
        auto &engine = result.engine;
        for (size_t index = 0, count = Pick(0, 2); index < count; ++index)
            engine.push_back(WordItem("x", "短语" + std::to_string(index), 0, CandidateSource::QuickPhrase));
        for (int index = 0, count = static_cast<int>(Pick(1, 14)); index < count; ++index)
        {
            engine.push_back(Chinese(index));
            if (index > 0 && Chance(12))
                engine.back().fixed_position = static_cast<int>(engine.size());
            if (spellings && Chance(25))
                engine.push_back(SpellingOf(engine.back(), "写法" + std::to_string(index)));
        }
        if (Chance(30))
            result.cloud.push_back(MixedSource("云", CandidateSource::CloudSuggestion));
        if (Chance(30))
            result.ai.push_back(MixedSource("AI", CandidateSource::AiSuggestion));
        if (Chance(20))
            result.mixed.push_back(MixedSource("2026年11月2日", CandidateSource::DateTime));
        const size_t english = Chance(60) ? Pick(1, 3) : 0;
        for (size_t index = 0; index < english; ++index)
        {
            const std::string word = "eng" + std::to_string(index);
            result.mixed.push_back(WordItem(word, word, 0, CandidateSource::EnglishDictionary));
            if (Chance(15))
                result.mixed.back().fixed_position = static_cast<int>(Pick(1, page_size + 2));
        }
        if (english > 0)
        {
            result.placement.require_exact = Chance(70);
            result.placement.input = Chance(50) ? "eng0" : "en";
            if (Chance(20))
                result.placement.slot = Pick(0, page_size - 1);
        }
        for (size_t index = 0, count = Pick(0, 2); index < count; ++index)
            result.mixed.push_back(MixedSource("表情" + std::to_string(index), CandidateSource::Emoji));
        if (Chance(30))
            result.mixed.push_back(MixedSource("(^_^)", CandidateSource::Kaomoji));
        for (size_t index = 0, count = Pick(0, 3); index < count; ++index)
        {
            result.latin.push_back(Latin("字母词" + std::to_string(index), 10 - static_cast<int>(index)));
            if (spellings && Chance(30))
                result.latin.push_back(SpellingOf(result.latin.back(), "字母写法" + std::to_string(index)));
        }
        for (size_t index = 0, count = Pick(0, 2); index < count; ++index)
            result.symbols.push_back(MixedSource("符号" + std::to_string(index), CandidateSource::Symbol));
        result.insert_at = Pick(0, 20);
        result.product_flow = Chance(50);
        std::shuffle(result.arrival.begin(), result.arrival.end(), random_);
        return result;
    }

  private:
    size_t Pick(size_t low, size_t high)
    {
        return std::uniform_int_distribution<size_t>(low, high)(random_);
    }
    bool Chance(int percent)
    {
        return static_cast<int>(Pick(0, 99)) < percent;
    }
    std::mt19937 random_;
};

// 把字母词插进 items 的普通候选之间（只插在写法组边界上），固定位置的候选与非普通来源的候选留在原下标
// （组页时 apply_fixed_positions 与 PlaceQuickPhrases 按下标放它们）；留不住（写法组跨过固定下标）时返回空。
std::optional<std::vector<WordItem>> WithLatinWords(const std::vector<WordItem> &items,
                                                    const DifferentialCase &test_case)
{
    std::vector<std::pair<size_t, WordItem>> fixed;
    std::vector<WordItem> others;
    for (size_t index = 0; index < items.size(); ++index)
    {
        const WordItem &item = items[index];
        if (item.fixed_position > 0 || !IsOrdinarySourceForTest(item.source))
            fixed.emplace_back(index, item);
        else
            others.push_back(item);
    }
    size_t at = (std::min)(test_case.insert_at, others.size());
    while (at < others.size() && others[at].variant_rank > 0)
        ++at;
    others.insert(others.begin() + static_cast<std::ptrdiff_t>(at), test_case.latin.begin(), test_case.latin.end());
    std::vector<WordItem> result;
    size_t next_fixed = 0;
    for (auto &item : others)
    {
        while (next_fixed < fixed.size() && fixed[next_fixed].first <= result.size())
        {
            if (fixed[next_fixed].first != result.size() || item.variant_rank > 0)
                return std::nullopt;
            result.push_back(std::move(fixed[next_fixed++].second));
        }
        result.push_back(std::move(item));
    }
    for (; next_fixed < fixed.size(); ++next_fixed)
    {
        if (fixed[next_fixed].first != result.size())
            return std::nullopt;
        result.push_back(std::move(fixed[next_fixed].second));
    }
    return result;
}

void Append(std::vector<WordItem> &items, const std::vector<WordItem> &more)
{
    items.insert(items.end(), more.begin(), more.end());
}

// 照 event_listener_async_candidates.cpp 的插法：云插在下标 1，AI 插在下标 2（列表更短时插在末尾），混输
// 结果里日期时间与英文的第一条插在下标 1、其余与 emoji、颜文字、符号追加在末尾；每次回来都归一化一次。
void Arrive(std::vector<WordItem> &items, const DifferentialCase &test_case, int arrival,
            const std::vector<WordItem> &symbols)
{
    const auto insert_first_and_append_rest = [&](const std::vector<WordItem> &candidates, size_t index) {
        if (candidates.empty())
            return;
        items.insert(items.begin() + static_cast<std::ptrdiff_t>((std::min)(index, items.size())), candidates.front());
        items.insert(items.end(), candidates.begin() + 1, candidates.end());
    };
    if (arrival == 0)
    {
        if (test_case.cloud.empty())
            return;
        insert_first_and_append_rest(test_case.cloud, 1);
    }
    else if (arrival == 1)
    {
        if (test_case.ai.empty())
            return;
        insert_first_and_append_rest(test_case.ai, 2);
    }
    else
    {
        if (test_case.mixed.empty() && symbols.empty())
            return;
        std::vector<WordItem> date_time;
        std::vector<WordItem> english;
        std::vector<WordItem> appended;
        for (const auto &item : test_case.mixed)
        {
            if (item.source == CandidateSource::DateTime)
                date_time.push_back(item);
            else if (item.source == CandidateSource::EnglishDictionary)
                english.push_back(item);
            else
                appended.push_back(item);
        }
        insert_first_and_append_rest(date_time, 1);
        insert_first_and_append_rest(english, 1);
        Append(items, appended);
        Append(items, symbols);
    }
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, test_case.placement);
}

void RequireCase(bool condition, const std::string &what, const std::vector<WordItem> &control,
                 const std::vector<WordItem> &actual, size_t page_size)
{
    if (!condition)
        throw std::runtime_error(what + " (page " + std::to_string(page_size) + "): control " +
                                 Listing(WordsOf(control)) + " actual " + Listing(WordsOf(actual)));
}

struct DifferentialRun
{
    std::vector<WordItem> control;
    std::vector<WordItem> actual;
    // 中途某一次放置按形状算出有对照项让到写法组后面（规格第 7 条），最后的结果不再与对照逐项对得上。
    bool yielded_on_the_way = false;
};

// 同一组输入走两遍：对照不含字母词与符号，实际含。WithLatinWords 造不出时返回空。
std::optional<DifferentialRun> RunCase(const DifferentialCase &test_case)
{
    DifferentialRun run;
    const size_t page_size = test_case.placement.page_size;
    if (!test_case.product_flow)
    {
        auto control_input = test_case.engine;
        Append(control_input, test_case.cloud);
        Append(control_input, test_case.ai);
        Append(control_input, test_case.mixed);
        auto input = WithLatinWords(control_input, test_case);
        if (!input)
            return std::nullopt;
        Append(*input, test_case.symbols);
        run.control = control_input;
        FanyImeIpc::NormalizeMixedCandidateOrder(run.control, 1, test_case.placement);
        run.actual = *input;
        FanyImeIpc::NormalizeMixedCandidateOrder(run.actual, 1, test_case.placement);
        return run;
    }
    auto input = WithLatinWords(test_case.engine, test_case);
    if (!input)
        return std::nullopt;
    run.control = test_case.engine;
    run.actual = *input;
    FanyImeIpc::PlaceQueriedLatinWords(run.actual, page_size);
    bool symbols_arrived = false;
    const auto check_step = [&] {
        if (test_case.latin.empty() && !symbols_arrived)
            return;
        const auto sizes = GroupSizes(test_case.latin, symbols_arrived && !test_case.symbols.empty());
        if (!sizes.empty() && Predict(run.control, sizes, page_size).anchor_yields)
            run.yielded_on_the_way = true;
    };
    check_step();
    for (int arrival : test_case.arrival)
    {
        Arrive(run.control, test_case, arrival, {});
        Arrive(run.actual, test_case, arrival, test_case.symbols);
        symbols_arrived = symbols_arrived || arrival == 2;
        check_step();
    }
    return run;
}

// 样例形状：对照列表（固定了位置的标 @固定位置，非普通来源的标 *）、字母词与写法、首个符号、每页候选数。
std::string ShapeOf(const DifferentialCase &test_case, const std::vector<WordItem> &control)
{
    std::vector<std::string> listed;
    for (const auto &item : control)
    {
        std::string entry = item.word;
        if (item.variant_rank > 0)
            entry += "'" + std::to_string(item.variant_rank);
        if (item.fixed_position > 0)
            entry += "@" + std::to_string(item.fixed_position);
        if (!IsOrdinarySourceForTest(item.source))
            entry += "*";
        listed.push_back(entry);
    }
    std::vector<std::string> latin;
    for (const auto &item : test_case.latin)
        latin.push_back(item.variant_rank > 0 ? item.word + "'" + std::to_string(item.variant_rank) : item.word);
    return "page " + std::to_string(test_case.placement.page_size) + (test_case.product_flow ? " product" : " once") +
           " control " + Listing(listed) + " latin " + Listing(latin) + " symbol " +
           (test_case.symbols.empty() ? std::string("-") : test_case.symbols.front().word);
}
} // namespace

TEST_CASE(LatinWordsAndSymbolsOnlyTakeOrdinarySlots)
{
    CaseGenerator generator(20261102);
    size_t checked = 0;
    size_t product_flow = 0;
    size_t head_on_first_page = 0;
    size_t head_after_first_page = 0;
    size_t multi_spelling_head = 0;
    size_t fixed_after_engine_latin = 0;
    size_t relaxed = 0;
    size_t unstable_control = 0;
    for (int round = 0; round < 6000; ++round)
    {
        const DifferentialCase test_case = generator.Next();
        const auto run = RunCase(test_case);
        if (!run)
            continue;
        const auto &control = run->control;
        const auto &actual = run->actual;
        const size_t page_size = test_case.placement.page_size;
        ++checked;
        product_flow += test_case.product_flow ? 1 : 0;

        std::vector<std::string> group;
        for (const auto &item : test_case.latin)
            group.push_back(Key(item));
        if (!test_case.symbols.empty())
            group.push_back(Key(test_case.symbols.front()));
        RequireCase(actual.size() ==
                        control.size() + group.size() + (test_case.symbols.empty() ? 0 : test_case.symbols.size() - 1),
                    "Every candidate must be kept", control, actual, page_size);

        // 写法组连续。
        for (size_t index = 1; index < actual.size(); ++index)
        {
            if (actual[index].variant_rank > 0)
                RequireCase(actual[index - 1].word == actual[index].word, "Spelling groups stay together", control,
                            actual, page_size);
        }
        // 普通候选彼此的先后、字母词与符号的先后都不变。
        const auto units = UnitsOf(control);
        std::vector<bool> anchored(control.size(), false);
        for (const auto &unit : units)
            std::fill(anchored.begin() + static_cast<std::ptrdiff_t>(unit.start),
                      anchored.begin() + static_cast<std::ptrdiff_t>(unit.start + unit.size), unit.anchored);
        std::vector<std::string> ordinary_control;
        for (size_t index = 0; index < control.size(); ++index)
        {
            if (!anchored[index])
                ordinary_control.push_back(Key(control[index]));
        }
        std::vector<std::string> ordinary_actual;
        std::vector<std::string> group_actual;
        for (const auto &item : actual)
        {
            const std::string key = Key(item);
            if (std::find(group.begin(), group.end(), key) != group.end())
                group_actual.push_back(key);
            else if (std::find(ordinary_control.begin(), ordinary_control.end(), key) != ordinary_control.end())
                ordinary_actual.push_back(key);
        }
        RequireCase(ordinary_actual == ordinary_control, "Ordinary candidates keep their order", control, actual,
                    page_size);
        RequireCase(group_actual == group, "Latin words then the first symbol keep their order", control, actual,
                    page_size);

        auto control_again = control;
        FanyImeIpc::NormalizeMixedCandidateOrder(control_again, 1, test_case.placement);
        const bool control_stable = WordsOf(control_again) == WordsOf(control);
        unstable_control += control_stable ? 0 : 1;
        if (group.empty())
        {
            RequireCase(WordsOf(actual) == WordsOf(control), "Without latin words nothing moves", control, actual,
                        page_size);
            continue;
        }

        // 其余字母词与首个符号紧跟第一条，中间只隔着对照项。
        const size_t head = IndexOfKey(actual, group.front());
        const size_t group_end = IndexOfKey(actual, group.back()) + 1;
        for (size_t index = head; index < group_end; ++index)
        {
            const std::string key = Key(actual[index]);
            const size_t in_control = IndexOfKey(control, key);
            RequireCase(std::find(group.begin(), group.end(), key) != group.end() ||
                            (in_control < control.size() && anchored[in_control]),
                        "The other latin words and the first symbol follow the first one", control, actual, page_size);
        }

        const auto prediction = Predict(control, GroupSizes(test_case.latin, !test_case.symbols.empty()), page_size);
        if (run->yielded_on_the_way || prediction.anchor_yields)
        {
            ++relaxed;
            // 放宽的样例逐条打出来，回归混进这一批时看得见。
            std::printf("[relaxed %s] %s\n", run->yielded_on_the_way ? "skipped" : "partial",
                        ShapeOf(test_case, control).c_str());
        }
        if (run->yielded_on_the_way)
            continue;
        // 第一条在算出的位置；对照项在算出的下标上（让到写法组后面的那些不断言）。
        RequireCase(head == prediction.head,
                    "The first latin word takes the last run of ordinary slots that holds it (expected at " +
                        std::to_string(prediction.head) + ")",
                    control, actual, page_size);
        for (size_t index = 0; index < control.size(); ++index)
        {
            if (!prediction.anchor_index[index])
                continue;
            RequireCase(IndexOfKey(actual, Key(control[index])) == *prediction.anchor_index[index],
                        "Anchored candidate " + control[index].word + " lands at " +
                            std::to_string(*prediction.anchor_index[index]),
                        control, actual, page_size);
        }
        // 再归一化不变；对照自己再归一化就会变时（不满一页时首页 emoji 与其余 emoji 换位）不要求。
        if (control_stable && !prediction.anchor_yields)
            RequireStableUnderRenormalization(actual, test_case.placement);

        head_on_first_page += head < page_size ? 1 : 0;
        head_after_first_page += head >= page_size ? 1 : 0;
        multi_spelling_head += test_case.latin.size() > 1 && test_case.latin[1].variant_rank > 0 ? 1 : 0;
        const auto input = WithLatinWords(test_case.engine, test_case);
        const bool latin_before_fixed =
            input && !test_case.latin.empty() && std::any_of(input->begin(), input->end(), [&](const WordItem &item) {
                return item.fixed_position > 0 &&
                       IndexOfKey(*input, Key(test_case.latin.front())) < IndexOfKey(*input, Key(item));
            });
        fixed_after_engine_latin += latin_before_fixed ? 1 : 0;
    }
    // 抽样没有退化：大多数输入造得出来，两种走法都常见，字母词常落在首页、也常排到首页之后，第一组常有
    // 多种写法，字母词常排在固定候选前面。
    REQUIRE(checked > 5000);
    REQUIRE(product_flow > 2000);
    REQUIRE(head_on_first_page > 1500);
    REQUIRE(head_after_first_page > 200);
    REQUIRE(multi_spelling_head > 200);
    REQUIRE(fixed_after_engine_latin > 300);
    REQUIRE(relaxed < checked / 10);
    REQUIRE(unstable_control < checked / 10);
}

TEST_CASE(AnchorAfterTheFirstPageYieldsToTheSpellingGroupThatCrossesIt)
{
    // 首页之后的写法组跨过固定候选的原下标：固定候选让到组后面，其余普通候选先后不变。
    std::vector<WordItem> items = ChineseItems(7);
    items[4].fixed_position = 5;
    const WordItem shirt = Latin("T恤衫", 9);
    items.push_back(Latin("T恤", 10));
    items.push_back(shirt);
    items.push_back(SpellingOf(shirt, "T卹衫"));
    FanyImeIpc::PlaceQueriedLatinWords(items, 3);
    RequireWords(items, {"中文0", "中文1", "T恤", "T恤衫", "T恤衫", "中文4", "中文2", "中文3", "中文5", "中文6"});
    REQUIRE_EQ(items[4].variant_rank, 1);
    REQUIRE_EQ(items[5].fixed_position, 5);
}
