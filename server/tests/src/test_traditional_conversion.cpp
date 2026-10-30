// 链接真实的 chinese_converter.cpp 与 OpenCC。测试程序与 server 输出到同一个目录，读的是 server 构建时拷到
// 那里的 assets\opencc，也就是随产品打包的那一份配置与字典。
// 期望值照施工单〈实跑数据〉表（OpenCC 8cf737a4 用 opencc.exe 实测）抄写。
#include "conversion/chinese_converter.h"
#include "conversion/opencc_character_tables.h"
#include "conversion/traditional_spelling_index.h"
#include "engine/core/word_item.h"
#include "ipc/traditional_region_policy.h"
#include "ipc/traditional_variant_expansion_policy.h"
#include "tests/includes/test_framework.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using Spellings = std::vector<std::string>;

void RequireForms(const std::string &simplified, const std::string &standard, const std::string &taiwan,
                  const std::string &hong_kong, const Spellings &character_table)
{
    const auto forms = ChineseConverter::ToTraditionalForms(simplified);
    REQUIRE_EQ(forms.standard, standard);
    REQUIRE_EQ(forms.taiwan, taiwan);
    REQUIRE_EQ(forms.hong_kong, hong_kong);
    REQUIRE_EQ(ChineseConverter::CharacterTableSpellings(simplified), character_table);
}
} // namespace

TEST_CASE(traditional_conversion_single_characters)
{
    RequireForms("台", "臺", "臺", "台", {"臺", "檯", "颱", "台"});
    RequireForms("为", "爲", "為", "為", {"爲"});
    RequireForms("着", "着", "著", "着", {});
    RequireForms("里", "裏", "裡", "裏", {"裏", "里", "哩"});
    RequireForms("说", "說", "說", "説", {"說"});
    RequireForms("干", "幹", "幹", "幹", {"幹", "乾", "干", "榦"});
    RequireForms("发", "發", "發", "發", {"發", "髮"});
    RequireForms("温", "溫", "溫", "温", {"溫"});
    RequireForms("才", "才", "才", "才", {"才", "纔"});
}

// ---- 地区：读所在地、读区域格式、判定、缓存 ----

namespace
{
// 记下被读了几次的假设置读取函数。
struct FakeSetting
{
    std::string value;
    int reads = 0;

    std::function<std::string()> Reader()
    {
        return [this] {
            ++reads;
            return value;
        };
    }
};

FanyImeIpc::TraditionalRegion RegionFrom(FakeSetting &location, FakeSetting &locale)
{
    ChineseConverter::RegionSettingReaders readers;
    readers.location = location.Reader();
    readers.locale = locale.Reader();
    ChineseConverter::CachedTraditionalRegion region(std::move(readers));
    return region.Get();
}
} // namespace

// 所在地是台湾、区域格式不是港台：按所在地判台湾。两项设置交给判定时若对调，会落到香港。
TEST_CASE(traditional_region_uses_the_location_read_from_the_system)
{
    FakeSetting location{"TW"};
    FakeSetting locale{"en-US"};
    REQUIRE(RegionFrom(location, locale) == FanyImeIpc::TraditionalRegion::Taiwan);
}

// ---- 真数据下的跨底词撞写法 ----

namespace
{
std::vector<WordItem> ExpandWithRealSpellings(std::vector<WordItem> items, FanyImeIpc::TraditionalRegion region)
{
    return FanyImeIpc::ExpandTraditionalVariants(std::move(items), ChineseConverter::OpenCcTraditionalSpellingIndex(),
                                                 region);
}

Spellings OutputTexts(const std::vector<WordItem> &items)
{
    Spellings texts;
    for (const auto &item : items)
        texts.push_back(item.output_text);
    return texts;
}

Spellings Words(const std::vector<WordItem> &items)
{
    Spellings words;
    for (const auto &item : items)
        words.push_back(item.word);
    return words;
}

Spellings Sorted(Spellings texts)
{
    std::sort(texts.begin(), texts.end());
    return texts;
}

// 列表里显示为 spelling 的那些项的底词。
std::vector<std::string> WordsShownAs(const std::vector<WordItem> &items, const std::string &spelling)
{
    std::vector<std::string> words;
    for (const auto &item : items)
        if (item.output_text == spelling)
            words.push_back(item.word);
    return words;
}
} // namespace

// 简体词库「msime.db」在 tai、li 之下也收了繁体字词条，排在「台」「里」后面。真数据下「台」「里」展开出
// 全部写法，这些写法各只出一项、底词是「台」「里」；「臺」「颱」「裏」「裡」词条的写法全被占走，整项不出现，
// 「哩」不在「里」的写法里，自己展开。台湾地区连写法顺序一起比，香港地区只比写法集合。
// 「檯」词条另有一种「台」组里没有的写法，这里不对它下断言。
TEST_CASE(traditional_conversion_keeps_every_spelling_with_tai_ahead_of_traditional_entries)
{
    const std::vector<WordItem> tai = {WordItem("tai", "台", 9), WordItem("tai", "太", 8), WordItem("tai", "臺", 3),
                                       WordItem("tai", "颱", 2), WordItem("tai", "檯", 1)};
    const Spellings tai_spellings = {"臺", "台", "檯", "颱"};
    for (const auto region : {FanyImeIpc::TraditionalRegion::Taiwan, FanyImeIpc::TraditionalRegion::HongKong})
    {
        const auto expanded = ExpandWithRealSpellings(tai, region);
        REQUIRE(expanded.size() >= size_t{5});
        const std::vector<WordItem> group(expanded.begin(), expanded.begin() + 4);
        REQUIRE_EQ(Words(group), (Spellings{"台", "台", "台", "台"}));
        REQUIRE_EQ(Sorted(OutputTexts(group)), Sorted(tai_spellings));
        REQUIRE_EQ(expanded[4].word, std::string("太"));
        for (const auto &spelling : tai_spellings)
            REQUIRE_EQ(WordsShownAs(expanded, spelling), (Spellings{"台"}));
        REQUIRE(std::none_of(expanded.begin(), expanded.end(),
                             [](const WordItem &item) { return item.word == "臺" || item.word == "颱"; }));
        if (region == FanyImeIpc::TraditionalRegion::Taiwan)
            REQUIRE_EQ(OutputTexts(group), tai_spellings);
    }
}

// ---- 繁体写法索引 ----

namespace
{
// 列表里底词为 word 的那些项显示的写法，按列表顺序。
Spellings SpellingsShownFor(const std::vector<WordItem> &items, const std::string &word)
{
    Spellings spellings;
    for (const auto &item : items)
        if (item.word == word)
            spellings.push_back(item.output_text);
    return spellings;
}
} // namespace

// 同一组底词以不同顺序、不同子集展开：「复」组的写法不变，「覆」写法只归底词「覆」；「借」「藉」同理。
TEST_CASE(traditional_conversion_ownership_of_a_spelling_does_not_depend_on_the_list)
{
    const std::vector<std::pair<const char *, const char *>> pairs = {{"复", "覆"}, {"借", "藉"}};
    for (const auto region : {FanyImeIpc::TraditionalRegion::Taiwan, FanyImeIpc::TraditionalRegion::HongKong})
    {
        for (const auto &[table_word, own_word] : pairs)
        {
            const WordItem table_item("x", table_word, 9);
            const WordItem own_item("x", own_word, 8);
            const WordItem other("x", "服", 7);
            const Spellings group = SpellingsShownFor(ExpandWithRealSpellings({table_item}, region), table_word);
            REQUIRE(!group.empty());
            REQUIRE(std::find(group.begin(), group.end(), own_word) == group.end());
            for (const auto &items :
                 {std::vector<WordItem>{table_item, own_item}, std::vector<WordItem>{own_item, table_item},
                  std::vector<WordItem>{table_item, other, own_item},
                  std::vector<WordItem>{own_item, other, table_item}, std::vector<WordItem>{table_item, other}})
            {
                const auto expanded = ExpandWithRealSpellings(items, region);
                REQUIRE_EQ(SpellingsShownFor(expanded, table_word), group);
                const bool own_listed = std::any_of(items.begin(), items.end(),
                                                    [&](const WordItem &item) { return item.word == own_word; });
                REQUIRE_EQ(WordsShownAs(expanded, own_word), own_listed ? Spellings{own_word} : Spellings{});
            }
        }
        REQUIRE_EQ(SpellingsShownFor(ExpandWithRealSpellings({WordItem("fu", "复", 9)}, region), "复"),
                   (Spellings{"復", "複"}));
    }
}

namespace
{
FanyImeIpc::CandidateSpellingSettings RealSettings(FanyImeIpc::TraditionalRegion region,
                                                   bool traditional_character_set = true)
{
    return {ChineseConverter::OpenCcTraditionalSpellingIndex(), region, traditional_character_set, SchemeType::Quanpin,
            /*radical_candidate_list=*/false};
}
} // namespace

// 异步候选插入前的判定，真数据：「复」组没有「覆」，云 / AI 候选「覆」照常插入；列表已有底词「覆」时跳过；
// 「台」组已显示「臺」时，底词「臺」的候选跳过。
TEST_CASE(traditional_conversion_async_candidates_follow_one_item_per_spelling)
{
    const auto region = FanyImeIpc::TraditionalRegion::Taiwan;
    for (const auto source : {CandidateSource::CloudSuggestion, CandidateSource::AiSuggestion})
    {
        const auto fu = ExpandWithRealSpellings({WordItem("fu", "复", 9), WordItem("fu", "服", 8)}, region);
        REQUIRE(FanyImeIpc::ShouldInsertAsyncCandidate(fu, "fu", "覆", source, RealSettings(region)));

        const auto both = ExpandWithRealSpellings({WordItem("fu", "复", 9), WordItem("fu", "覆", 8)}, region);
        REQUIRE(!FanyImeIpc::ShouldInsertAsyncCandidate(both, "fu", "覆", source, RealSettings(region)));

        const auto tai = ExpandWithRealSpellings({WordItem("tai", "台", 9), WordItem("tai", "太", 8)}, region);
        REQUIRE(!FanyImeIpc::ShouldInsertAsyncCandidate(tai, "tai", "臺", source, RealSettings(region)));
    }
}

// 地区一路传到展开与第一写法：同一份候选在台湾、香港两个地区的组内顺序与第一写法逐项比。期望值是
// OpenCC 的转换结果按「本地区、另一地区、标准、字表」排出来的：里 tw 裡、hk 裏、标准 裏，字表 裏 里；
// 着 tw 著、hk 着、标准 着；台 tw 臺、hk 台、标准 臺，字表 臺 檯 颱 台。
TEST_CASE(traditional_conversion_orders_spellings_and_picks_the_first_one_by_region)
{
    using FanyImeIpc::TraditionalRegion;
    REQUIRE_EQ(OutputTexts(ExpandWithRealSpellings({WordItem("li", "里", 9)}, TraditionalRegion::Taiwan)),
               (Spellings{"裡", "裏", "里"}));
    REQUIRE_EQ(OutputTexts(ExpandWithRealSpellings({WordItem("li", "里", 9)}, TraditionalRegion::HongKong)),
               (Spellings{"裏", "裡", "里"}));
    REQUIRE_EQ(OutputTexts(ExpandWithRealSpellings({WordItem("zhe", "着", 9)}, TraditionalRegion::Taiwan)),
               (Spellings{"著", "着"}));
    REQUIRE_EQ(OutputTexts(ExpandWithRealSpellings({WordItem("zhe", "着", 9)}, TraditionalRegion::HongKong)),
               (Spellings{"着", "著"}));
    REQUIRE_EQ(OutputTexts(ExpandWithRealSpellings({WordItem("tai", "台", 9)}, TraditionalRegion::Taiwan)),
               (Spellings{"臺", "台", "檯", "颱"}));
    REQUIRE_EQ(OutputTexts(ExpandWithRealSpellings({WordItem("tai", "台", 9)}, TraditionalRegion::HongKong)),
               (Spellings{"台", "臺", "檯", "颱"}));
    for (const auto source :
         {CandidateSource::Generated, CandidateSource::CloudSuggestion, CandidateSource::AiSuggestion})
    {
        const WordItem li("li", "里", 0, source);
        REQUIRE_EQ(FanyImeIpc::WithFirstTraditionalSpellingOf(li, RealSettings(TraditionalRegion::Taiwan)).output_text,
                   std::string("裡"));
        REQUIRE_EQ(
            FanyImeIpc::WithFirstTraditionalSpellingOf(li, RealSettings(TraditionalRegion::HongKong)).output_text,
            std::string("裏"));
        REQUIRE_EQ(OutputTexts(ExpandWithRealSpellings({li}, TraditionalRegion::HongKong)), Spellings{"裏"});
    }
}

// 三种转换结果不过滤：「么」是独立的简体字，但它是「幺」的台湾写法，台湾地区「幺」组仍列「么」。
// 列表里有「么」时也一样：「么」自己展开成「麼」，不出「么」，没有撞写法。
TEST_CASE(traditional_conversion_keeps_a_regional_form_that_is_an_independent_simplified_character)
{
    const auto &index = ChineseConverter::OpenCcTraditionalSpellingIndex();
    REQUIRE(ChineseConverter::IsIndependentSimplifiedCharacter("么"));
    REQUIRE_EQ(FanyImeIpc::TraditionalSpellingsOf("幺", index, FanyImeIpc::TraditionalRegion::Taiwan),
               (Spellings{"么", "幺"}));
    REQUIRE_EQ(FanyImeIpc::TraditionalSpellingsOf("幺", index, FanyImeIpc::TraditionalRegion::HongKong),
               (Spellings{"幺", "么"}));
    REQUIRE_EQ(OutputTexts(ExpandWithRealSpellings({WordItem("yao", "幺", 9)}, FanyImeIpc::TraditionalRegion::Taiwan)),
               (Spellings{"么", "幺"}));
    const auto with_own = ExpandWithRealSpellings({WordItem("yao", "么", 9), WordItem("yao", "幺", 8)},
                                                  FanyImeIpc::TraditionalRegion::Taiwan);
    REQUIRE_EQ(SpellingsShownFor(with_own, "幺"), (Spellings{"么", "幺"}));
    REQUIRE_EQ(SpellingsShownFor(with_own, "么"), Spellings{"麼"});
}

// 两个简体单字共用一种写法时归首选底词，与先后、地区无关。出厂词库的全部单字都核过：同读音或同首字母、
// 两边都有单字词条、两个简体单字展开同一种写法的有 33 种。这里列的是其中写法不等于任何一方的 18 种，
// 加上写法等于其中一方、由转换结果撞上的 着/著、痹/痺、卫/衞，与只在首字母列表里相遇的 苹/𬞟；
// 首选底词照 TSCharacters.txt 写法条目的第一个值，不在表里时是写法自己。
TEST_CASE(traditional_conversion_gives_a_spelling_shared_by_two_characters_to_its_preferred_word)
{
    struct SharedSpelling
    {
        const char *spelling;
        const char *preferred;
        const char *other;
    };
    const SharedSpelling shared[] = {{"噁", "恶", "𫫇"}, {"穭", "穞", "稆"}, {"篠", "筿", "筱"}, {"線", "线", "缐"},
                                     {"託", "托", "讬"}, {"諮", "咨", "谘"}, {"讎", "雠", "仇"}, {"賙", "赒", "周"},
                                     {"逕", "径", "迳"}, {"鉅", "巨", "钜"}, {"鍊", "炼", "链"}, {"鍾", "钟", "锺"},
                                     {"鏇", "旋", "镟"}, {"顎", "颚", "腭"}, {"餘", "余", "馀"}, {"鯰", "鲶", "鲇"},
                                     {"鹼", "碱", "硷"}, {"麴", "曲", "麹"}, {"著", "著", "着"}, {"痺", "痺", "痹"},
                                     {"衞", "衞", "卫"}, {"蘋", "苹", "𬞟"}};
    for (const auto region : {FanyImeIpc::TraditionalRegion::Taiwan, FanyImeIpc::TraditionalRegion::HongKong})
    {
        for (const auto &row : shared)
        {
            const WordItem preferred("x", row.preferred, 9);
            const WordItem other("x", row.other, 8);
            const WordItem filler("x", "f", 7);
            for (const auto &items : {std::vector<WordItem>{preferred, other}, std::vector<WordItem>{other, preferred},
                                      std::vector<WordItem>{other, filler, preferred}})
            {
                const auto expanded = ExpandWithRealSpellings(items, region);
                if (WordsShownAs(expanded, row.spelling) != Spellings{row.preferred})
                    std::printf("shared spelling %s is not shown under %s\n", row.spelling, row.preferred);
                REQUIRE_EQ(WordsShownAs(expanded, row.spelling), Spellings{row.preferred});
            }
            REQUIRE_EQ(WordsShownAs(ExpandWithRealSpellings({other}, region), row.spelling), Spellings{row.other});
        }
    }
}

namespace
{
std::vector<int> Ranks(const std::vector<WordItem> &items)
{
    std::vector<int> ranks;
    for (const auto &item : items)
        ranks.push_back(item.variant_rank);
    return ranks;
}
} // namespace

// 写法等于一个简体单字底词、而展开这种写法的简体单字底词只有它一个时，这种写法归它，即使前面的繁体字词条
// 也列出了它。「涗」不是简体写法（繁转简成「涚」），展开成 涗 涚；「涚」也展开成 涗 涚。「臺」词条台湾地区
// 展开成 臺 台、香港地区展开成 台 臺。「台」组台湾序列 臺 台 檯 颱，香港序列 台 臺 檯 颱。
// 两个地区下「涚」「台」都留给同名的底词。
TEST_CASE(traditional_conversion_gives_a_spelling_to_the_simplified_word_it_equals_behind_a_traditional_entry)
{
    for (const auto region : {FanyImeIpc::TraditionalRegion::Taiwan, FanyImeIpc::TraditionalRegion::HongKong})
    {
        const auto shui = ExpandWithRealSpellings({WordItem("shui", "涗", 9), WordItem("shui", "涚", 8)}, region);
        REQUIRE_EQ(Words(shui), (Spellings{"涗", "涚"}));
        REQUIRE_EQ(OutputTexts(shui), (Spellings{"涗", "涚"}));
        REQUIRE_EQ(Ranks(shui), (std::vector<int>{0, 0}));

        const auto tai = ExpandWithRealSpellings({WordItem("tai", "臺", 9), WordItem("tai", "台", 8)}, region);
        REQUIRE_EQ(Words(tai), (Spellings{"臺", "台", "台", "台"}));
        REQUIRE_EQ(OutputTexts(tai), (Spellings{"臺", "台", "檯", "颱"}));
        REQUIRE_EQ(Ranks(tai), (std::vector<int>{0, 0, 1, 2}));
    }
}

// 异步判定，真数据：香港地区「台」组是 台 臺 檯 颱，云 / AI 候选「颱」的第一写法是它自己（t2hk(颱)=颱），
// 已显示在组里第 4 位，跳过。
TEST_CASE(traditional_conversion_async_candidate_skips_a_spelling_shown_inside_a_group)
{
    const auto region = FanyImeIpc::TraditionalRegion::HongKong;
    const auto tai = ExpandWithRealSpellings({WordItem("tai", "台", 9), WordItem("tai", "太", 8)}, region);
    REQUIRE_EQ(OutputTexts(tai), (Spellings{"台", "臺", "檯", "颱", "太"}));
    for (const auto source : {CandidateSource::CloudSuggestion, CandidateSource::AiSuggestion})
    {
        REQUIRE_EQ(FanyImeIpc::WithFirstTraditionalSpellingOf(WordItem("tai", "颱", 0, source), RealSettings(region))
                       .output_text,
                   std::string("颱"));
        REQUIRE(!FanyImeIpc::ShouldInsertAsyncCandidate(tai, "tai", "颱", source, RealSettings(region)));
    }
}

// ---- 后台预载 ----

// Join 等到工作函数返回才返回：工作卡在闩锁上，闩锁由另一条线程过一会儿才打开。
// 起过一次之后再 Start 不会再跑；重复 Join 立即返回。
TEST_CASE(traditional_conversion_background_task_join_waits_for_the_work_to_finish)
{
    struct Shared
    {
        std::promise<void> release;
        std::shared_future<void> released = release.get_future().share();
        std::atomic<int> runs{0};
        std::atomic<bool> finished{false};
    };
    // 工作函数按值持有共享状态：Join 若没等就返回，用例结束后线程仍可安全地跑完。
    const auto shared = std::make_shared<Shared>();
    ChineseConverter::BackgroundTask task([shared] {
        ++shared->runs;
        shared->released.wait();
        shared->finished = true;
    });
    task.Start();
    task.Start();
    std::thread releaser([shared] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        shared->release.set_value();
    });
    task.Join();
    const bool finished_when_join_returned = shared->finished;
    releaser.join();
    REQUIRE(finished_when_join_returned);

    task.Join();
    task.Start();
    task.Join();
    REQUIRE_EQ(shared->runs.load(), 1);
}
