#include "ipc/candidate_expansion_panel_policy.h"
#include "tests/includes/test_framework.h"

#include <string>
#include <vector>

namespace
{
constexpr const char *kTagTranslation = "\xE8\xAF\x91";       // 译
constexpr const char *kTagEmoji = "\xE8\xA1\xA8\xE6\x83\x85"; // 表情
constexpr const char *kTagKaomoji = "\xE9\xA2\x9C";           // 颜
constexpr const char *kTagSymbols = "\xE7\xAC\xA6";           // 符
constexpr const char *kSun = "\xF0\x9F\x8C\x9E";              // 🌞
constexpr const char *kSmiley = "\xF0\x9F\x98\x80";           // 😀
constexpr const char *kKaomojiBlush = "(^_^)";
constexpr const char *kKaomojiKiss = "(*~▽~)";
constexpr const char *kArrow = "\xE2\xAC\x85";    // ⬅
constexpr const char *kInfinity = "\xE2\x88\x9E"; // ∞
} // namespace

TEST_CASE(candidate_expansion_panel_translation_provider_splits_display_and_commit)
{
    FanyImeIpc::ExpansionContext context;
    context.anchor_gloss = "n. silver medal; v. to medal";

    const auto with_pos = FanyImeIpc::ProduceForCategory(FanyImeIpc::kCategoryTranslation, context);
    REQUIRE(with_pos.size() == 2);
    REQUIRE(with_pos[0].display == "n. silver medal");
    REQUIRE(with_pos[0].commit == "silver medal");
    REQUIRE(with_pos[1].display == "v. to medal");
    REQUIRE(with_pos[1].commit == "to medal");
    for (const auto &item : with_pos)
    {
        REQUIRE(item.category_id == "translation");
        REQUIRE(item.tag == kTagTranslation);
    }

    context.show_pos = false;
    const auto without_pos = FanyImeIpc::ProduceForCategory(FanyImeIpc::kCategoryTranslation, context);
    REQUIRE(without_pos.size() == 2);
    REQUIRE(without_pos[0].display == "silver medal");
    REQUIRE(without_pos[0].commit == "silver medal");
}

TEST_CASE(candidate_expansion_panel_emoji_provider_wraps_prefetched_emoji_with_tag)
{
    FanyImeIpc::ExpansionContext context;
    context.emoji_candidates = {kSun, kSmiley};
    const auto items = FanyImeIpc::ProduceForCategory(FanyImeIpc::kCategoryEmoji, context);
    REQUIRE(items.size() == 2);
    // emoji 即其本身：display 与 commit 相同，顺序沿用预取顺序。
    REQUIRE(items[0].display == kSun);
    REQUIRE(items[0].commit == kSun);
    REQUIRE(items[1].display == kSmiley);
    REQUIRE(items[1].commit == kSmiley);
    for (const auto &item : items)
    {
        REQUIRE(item.category_id == "emoji");
        REQUIRE(item.tag == kTagEmoji);
    }

    // 没有预取 emoji 时产出空；上下文只预取了 emoji、没预取颜文字，颜文字类别同样产出空。
    FanyImeIpc::ExpansionContext empty;
    REQUIRE(FanyImeIpc::ProduceForCategory(FanyImeIpc::kCategoryEmoji, empty).empty());
    REQUIRE(FanyImeIpc::ProduceForCategory(FanyImeIpc::kCategoryKaomoji, context).empty());
}

TEST_CASE(candidate_expansion_panel_kaomoji_provider_wraps_prefetched_kaomoji_with_tag)
{
    FanyImeIpc::ExpansionContext context;
    context.kaomoji_candidates = {kKaomojiBlush, kKaomojiKiss};
    const auto items = FanyImeIpc::ProduceForCategory(FanyImeIpc::kCategoryKaomoji, context);
    REQUIRE(items.size() == 2);
    // 颜文字即其本身：display 与 commit 相同，顺序沿用预取顺序。
    REQUIRE(items[0].display == kKaomojiBlush);
    REQUIRE(items[0].commit == kKaomojiBlush);
    REQUIRE(items[1].display == kKaomojiKiss);
    REQUIRE(items[1].commit == kKaomojiKiss);
    for (const auto &item : items)
    {
        REQUIRE(item.category_id == "kaomoji");
        REQUIRE(item.tag == kTagKaomoji);
    }

    // 没有预取颜文字时产出空。
    FanyImeIpc::ExpansionContext empty;
    REQUIRE(FanyImeIpc::ProduceForCategory(FanyImeIpc::kCategoryKaomoji, empty).empty());
}

TEST_CASE(candidate_expansion_panel_symbols_provider_wraps_prefetched_symbols_with_tag)
{
    FanyImeIpc::ExpansionContext context;
    context.symbols_candidates = {kArrow, kInfinity};
    const auto items = FanyImeIpc::ProduceForCategory(FanyImeIpc::kCategorySymbols, context);
    REQUIRE(items.size() == 2);
    // 符号即其本身：display 与 commit 相同，顺序沿用预取顺序。
    REQUIRE(items[0].display == kArrow);
    REQUIRE(items[0].commit == kArrow);
    REQUIRE(items[1].display == kInfinity);
    REQUIRE(items[1].commit == kInfinity);
    for (const auto &item : items)
    {
        REQUIRE(item.category_id == "symbols");
        REQUIRE(item.tag == kTagSymbols);
    }

    // 没有预取符号时产出空；上下文只预取了符号、没预取 emoji，emoji 类别同样产出空。
    FanyImeIpc::ExpansionContext empty;
    REQUIRE(FanyImeIpc::ProduceForCategory(FanyImeIpc::kCategorySymbols, empty).empty());
    REQUIRE(FanyImeIpc::ProduceForCategory(FanyImeIpc::kCategoryEmoji, context).empty());
}

TEST_CASE(candidate_expansion_panel_flatten_pipeline_concatenates_enabled_categories_in_order)
{
    FanyImeIpc::ExpansionContext context;
    context.anchor_gloss = "n. silver medal";

    const auto only_translation = FanyImeIpc::BuildExpansionItems({"translation"}, context);
    REQUIRE(only_translation.size() == 1);
    REQUIRE(only_translation[0].category_id == "translation");

    // 没有预取 emoji 的类别产不出条目，但 enabled_order 仍决定拼接次序：把 translation 放在后面，
    // 它仍排在启用且有产出的位置。这里 emoji 未预取，结果只有 translation。
    const auto ordered = FanyImeIpc::BuildExpansionItems({"emoji", "translation", "kaomoji"}, context);
    REQUIRE(ordered.size() == 1);
    REQUIRE(ordered[0].category_id == "translation");
}

TEST_CASE(candidate_expansion_panel_flatten_orders_prefetched_emoji_before_translation_when_listed_first)
{
    FanyImeIpc::ExpansionContext context;
    context.emoji_candidates = {kSun};
    context.anchor_gloss = "n. sun";
    const auto items = FanyImeIpc::BuildExpansionItems({"emoji", "translation"}, context);
    REQUIRE(items.size() == 2);
    REQUIRE(items[0].category_id == "emoji");
    REQUIRE(items[0].display == kSun);
    REQUIRE(items[1].category_id == "translation");
    REQUIRE(items[1].commit == "sun");
}

TEST_CASE(candidate_expansion_panel_flatten_truncates_each_category_to_its_own_limit)
{
    FanyImeIpc::ExpansionContext context;
    context.anchor_gloss = "n. one; v. two; adj. three; n. four"; // 4 个译义
    context.emoji_candidates = {kSun, kSmiley};
    context.kaomoji_candidates = {kKaomojiBlush, kKaomojiKiss};
    context.symbols_candidates = {kArrow, kInfinity};

    // 缺省（无上限条目）= 不限制，全部产出。
    const auto unlimited = FanyImeIpc::BuildExpansionItems({"translation", "emoji", "kaomoji", "symbols"}, context);
    REQUIRE(unlimited.size() == 4 + 2 + 2 + 2);

    // 每类各自独立的上限：翻译截到 2、emoji 截到 1、符号截到 1、颜文字未设=不限制。
    context.category_max_count["translation"] = 2;
    context.category_max_count["emoji"] = 1;
    context.category_max_count["symbols"] = 1;
    const auto limited = FanyImeIpc::BuildExpansionItems({"translation", "emoji", "kaomoji", "symbols"}, context);
    REQUIRE(limited.size() == 2 + 1 + 2 + 1);
    // 翻译保留前两条（按 gloss 顺序），display 仍带词性、commit 仍剥词性。
    REQUIRE(limited[0].category_id == "translation");
    REQUIRE(limited[0].display == "n. one");
    REQUIRE(limited[0].commit == "one");
    REQUIRE(limited[1].display == "v. two");
    // emoji 只保留预取的第一条。
    REQUIRE(limited[2].category_id == "emoji");
    REQUIRE(limited[2].display == kSun);
    // 颜文字未设上限，全部保留。
    REQUIRE(limited[3].category_id == "kaomoji");
    REQUIRE(limited[3].display == kKaomojiBlush);
    REQUIRE(limited[4].display == kKaomojiKiss);
    // 符号截到 1，只保留预取的第一条。
    REQUIRE(limited[5].category_id == "symbols");
    REQUIRE(limited[5].display == kArrow);

    // 上限 0 = 不限制条数（不是隐藏该类）：翻译全部保留，emoji 仍按自己的上限 1 截断。
    context.category_max_count["translation"] = 0;
    const auto zero_translation = FanyImeIpc::BuildExpansionItems({"translation", "emoji"}, context);
    REQUIRE(zero_translation.size() == 4 + 1);
    REQUIRE(zero_translation[0].category_id == "translation");
    REQUIRE(zero_translation[0].display == "n. one");
    REQUIRE(zero_translation[3].display == "n. four");
    REQUIRE(zero_translation[4].category_id == "emoji");
    REQUIRE(zero_translation[4].display == kSun);
}

TEST_CASE(candidate_expansion_panel_category_registry_carries_default_order_and_per_category_tags)
{
    const auto categories = FanyImeIpc::KnownExpansionCategories();
    REQUIRE(categories.size() == 4);
    REQUIRE(categories[0].id == "translation");
    REQUIRE(categories[1].id == "emoji");
    REQUIRE(categories[2].id == "kaomoji");
    REQUIRE(categories[3].id == "symbols");
    REQUIRE(categories[0].prefix_tag == kTagTranslation);
    REQUIRE(categories[1].prefix_tag == kTagEmoji);
    REQUIRE(categories[2].prefix_tag == kTagKaomoji);
    REQUIRE(categories[3].prefix_tag == kTagSymbols);
    REQUIRE(FanyImeIpc::FindExpansionCategory("translation") != nullptr);
    REQUIRE(FanyImeIpc::FindExpansionCategory("symbols") != nullptr);
    REQUIRE(FanyImeIpc::FindExpansionCategory("nope") == nullptr);
}
