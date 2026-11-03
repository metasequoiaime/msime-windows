#pragma once

#include "engine/core/word_item.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace FanyImeIpc
{
// 普通候选的来源：混输各来源（云、AI、英文、emoji、颜文字、符号、日期时间）与快捷短语以外的本地候选。
// NormalizeMixedCandidateOrder 按来源分拣时把快捷短语算作本地候选（它的 default 分支），与这里不同：
// 那里决定谁按异步来源重摆，这里决定字母词与符号能占谁的位置。加新来源时两处都要看。
constexpr bool IsOrdinaryCandidateSource(CandidateSource source) noexcept
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

namespace detail
{
// 写法组：同一底词的繁体写法（variant_rank > 0 的项）归到它前面最近的、来源与 latin_word 都和它相同的组首，
// 没有时归最近的组首；没展开的候选自成一组。异步候选（云、AI、日期时间、英文）按下标插进已展开写法的列表时
// 可能落在组中间，它们的来源与组里的写法不同，不会把组切开。
inline bool IsSpellingGroupHead(const std::vector<WordItem> &items, size_t index)
{
    return index == 0 || items[index].variant_rank == 0;
}

/** 列表切成的写法组，按组首的先后编号。 */
struct SpellingGroups
{
    // 各组成员在列表里的下标，逐组排开；组内组首在前，其余写法按列表先后。
    std::vector<size_t> members;
    // 第 g 组是 members[starts[g], starts[g + 1])。
    std::vector<size_t> starts{0};
    // 从这个下标起，每组都是连续的一段，前面的组也没有成员落在这里或之后。
    size_t contiguous_from = 0;

    size_t count() const
    {
        return starts.size() - 1;
    }
    size_t head(size_t group) const
    {
        return members[starts[group]];
    }
    size_t size(size_t group) const
    {
        return starts[group + 1] - starts[group];
    }
    size_t last_member(size_t group) const
    {
        return members[starts[group + 1] - 1];
    }
};

inline SpellingGroups GroupSpellings(const std::vector<WordItem> &items)
{
    SpellingGroups groups;
    const auto spelling_variant = [](const WordItem &item) { return item.variant_rank != 0; };
    if (items.empty() || std::none_of(items.begin() + 1, items.end(), spelling_variant))
    {
        groups.members.resize(items.size());
        std::iota(groups.members.begin(), groups.members.end(), size_t{0});
        groups.starts.resize(items.size() + 1);
        std::iota(groups.starts.begin(), groups.starts.end(), size_t{0});
        return groups;
    }

    // 每种（来源, latin_word）最近的组首是第几组。
    struct LatestHead
    {
        CandidateSource source;
        bool latin_word;
        size_t group;
    };
    std::vector<LatestHead> latest_heads;
    std::vector<size_t> owner(items.size());
    size_t count = 0;
    for (size_t index = 0; index < items.size(); ++index)
    {
        const WordItem &item = items[index];
        const auto latest = std::find_if(latest_heads.begin(), latest_heads.end(), [&](const LatestHead &head) {
            return head.source == item.source && head.latin_word == item.latin_word;
        });
        if (!IsSpellingGroupHead(items, index))
        {
            owner[index] = latest == latest_heads.end() ? count - 1 : latest->group;
            continue;
        }
        if (latest == latest_heads.end())
            latest_heads.push_back({item.source, item.latin_word, count});
        else
            latest->group = count;
        owner[index] = count++;
    }

    groups.starts.assign(count + 1, 0);
    for (size_t group : owner)
        ++groups.starts[group + 1];
    std::partial_sum(groups.starts.begin(), groups.starts.end(), groups.starts.begin());
    groups.members.resize(items.size());
    std::vector<size_t> next_slot(groups.starts.begin(), groups.starts.end() - 1);
    for (size_t index = 0; index < items.size(); ++index)
        groups.members[next_slot[owner[index]]++] = index;

    // 分界前一项是某个不连续组的成员，所以也没有连续的组跨过分界。
    for (size_t group = 0; group < count; ++group)
    {
        if (groups.last_member(group) + 1 - groups.head(group) != groups.size(group))
            groups.contiguous_from = (std::max)(groups.contiguous_from, groups.last_member(group) + 1);
    }
    return groups;
}

/**
 * 要摆的一个写法组：groups 的第 group 组。成员的来源编号是它在自己列表里的下标加 offset，两张列表合在一起摆时
 * 用 offset 把第二张排在第一张后面。
 */
struct Unit
{
    const SpellingGroups *groups;
    size_t group;
    size_t offset;

    size_t size() const
    {
        return groups->size(group);
    }
};

struct Anchor
{
    // 组首在原列表里的下标，对照项要停在这里。
    size_t index;
    Unit unit;
};

struct LaidOutUnit
{
    Unit unit;
    // 摆好后组首的下标。
    size_t start;
    bool anchored;
    // 对照项是 anchors 里的第几个，其余是 stream 里的第几组。
    size_t ordinal;
};

/**
 * 对照项（按 index 升序）停在 index 上，stream 里的组按先后填进其余位置。一个组会跨过下一个对照项的下标时
 * 照样整组放，那个对照项让到它后面。stream_unit(k) 是 stream 的第 k 组；按摆放先后对每个组调用 visit，
 * visit 返回 false 时停下。
 */
template <typename StreamUnit, typename Visit>
void LayOut(const std::vector<Anchor> &anchors, size_t stream_length, StreamUnit stream_unit, Visit visit)
{
    size_t size = 0;
    size_t next_anchor = 0;
    const auto lay_anchor = [&] {
        const Unit unit = anchors[next_anchor].unit;
        const bool go_on = visit(LaidOutUnit{unit, size, true, next_anchor});
        size += unit.size();
        ++next_anchor;
        return go_on;
    };
    for (size_t ordinal = 0; ordinal < stream_length; ++ordinal)
    {
        while (next_anchor < anchors.size() && anchors[next_anchor].index <= size)
        {
            if (!lay_anchor())
                return;
        }
        const Unit unit = stream_unit(ordinal);
        if (!visit(LaidOutUnit{unit, size, false, ordinal}))
            return;
        size += unit.size();
    }
    while (next_anchor < anchors.size())
    {
        if (!lay_anchor())
            return;
    }
}

inline void AppendSources(std::vector<size_t> &order, const Unit &unit)
{
    const SpellingGroups &groups = *unit.groups;
    for (size_t member = groups.starts[unit.group]; member < groups.starts[unit.group + 1]; ++member)
        order.push_back(groups.members[member] + unit.offset);
}

/** 摆好的新列表：先是 sources 里各项（来源编号），其后接原列表从 settled_from 起的各项，先后不变。 */
struct Arrangement
{
    std::vector<size_t> sources;
    size_t settled_from;
};

/**
 * 按 LayOut 的规则摆好、tail 接在最后。stream 从第 plain_from 组起都是原列表自己的组；原列表从 shift_only_from
 * 起的项都是 stream 或 tail 里的组（不是对照项，也没被取走）。对照项都摆完之后，一摆到组首不在 shift_only_from
 * 之前的原列表组，余下的就是原列表从那个组首起的整段，不再逐组摆。
 */
template <typename StreamUnit>
Arrangement Arrange(const std::vector<Anchor> &anchors, size_t stream_length, StreamUnit stream_unit,
                    const std::vector<Unit> &tail, size_t plain_from, size_t shift_only_from, size_t item_count)
{
    Arrangement arrangement{{}, item_count};
    size_t anchors_laid = 0;
    const auto settles_at = [&](const Unit &unit) {
        return anchors_laid == anchors.size() && unit.groups->head(unit.group) >= shift_only_from;
    };
    LayOut(anchors, stream_length, stream_unit, [&](const LaidOutUnit &laid) {
        if (laid.anchored)
            ++anchors_laid;
        else if (laid.ordinal >= plain_from && settles_at(laid.unit))
        {
            arrangement.settled_from = laid.unit.groups->head(laid.unit.group);
            return false;
        }
        AppendSources(arrangement.sources, laid.unit);
        return true;
    });
    if (arrangement.settled_from < item_count)
        return arrangement;
    if (!tail.empty() && settles_at(tail.front()))
    {
        arrangement.settled_from = tail.front().groups->head(tail.front().group);
        return arrangement;
    }
    for (const Unit &unit : tail)
        AppendSources(arrangement.sources, unit);
    return arrangement;
}

/**
 * 把 items 换成 arrangement 排的新列表：来源编号小于 items.size() 的取 items 里那一项，其余取
 * extra[编号 - items.size()]。每个来源恰好用一次，没被用到的 items 项须已被取走。
 * 新列表开头与原位相同的一段不动，其余每项移动一次；只有项既往前又往后挪、原地搬会覆盖还没搬的来源时，
 * 才经过临时存放。
 */
inline void Rearrange(std::vector<WordItem> &items, std::vector<WordItem> &extra, const Arrangement &arrangement)
{
    const std::vector<size_t> &sources = arrangement.sources;
    const size_t old_size = items.size();
    const size_t new_size = sources.size() + (old_size - arrangement.settled_from);
    // 新下标 index 的项的来源编号；sources 之后的那段来自 items 的 index + old_size - new_size。
    const auto order = [&](size_t index) {
        return index < sources.size() ? sources[index] : index + old_size - new_size;
    };
    const auto source = [&](size_t from) -> WordItem & {
        return from < old_size ? items[from] : extra[from - old_size];
    };
    const auto in_place = [&](size_t index) { return order(index) == index && index < old_size; };
    size_t changed_begin = 0;
    while (changed_begin < new_size && in_place(changed_begin))
        ++changed_begin;

    if (new_size > items.capacity())
    {
        std::vector<WordItem> rebuilt;
        rebuilt.reserve(new_size);
        std::move(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(changed_begin),
                  std::back_inserter(rebuilt));
        for (size_t index = changed_begin; index < new_size; ++index)
            rebuilt.push_back(std::move(source(order(index))));
        items = std::move(rebuilt);
        return;
    }

    // 来自 items 的项都不往后挪（新下标不大于原下标）时从前往后搬，都不往前挪时从后往前搬，来源都不会先被覆盖。
    bool none_move_later = true;
    bool none_move_earlier = true;
    for (size_t index = changed_begin; index < new_size; ++index)
    {
        const size_t from = order(index);
        if (from >= old_size)
            continue;
        none_move_later = none_move_later && from >= index;
        none_move_earlier = none_move_earlier && from <= index;
    }
    if (!none_move_later && !none_move_earlier)
    {
        std::vector<WordItem> moved;
        moved.reserve(new_size - changed_begin);
        for (size_t index = changed_begin; index < new_size; ++index)
            moved.push_back(std::move(source(order(index))));
        items.resize(new_size);
        std::move(moved.begin(), moved.end(), items.begin() + static_cast<std::ptrdiff_t>(changed_begin));
        return;
    }
    if (new_size > old_size)
        items.resize(new_size);
    const auto move_into = [&](size_t index) {
        if (!in_place(index))
            items[index] = std::move(source(order(index)));
    };
    if (none_move_later)
    {
        for (size_t index = changed_begin; index < new_size; ++index)
            move_into(index);
    }
    else
    {
        for (size_t index = new_size; index-- > changed_begin;)
            move_into(index);
    }
    if (new_size < old_size)
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(new_size), items.end());
}

// inserted 整段插在 ordinary 的第 position 组之前时，stream 的第 ordinal 组。
inline Unit UnitWithInsertion(const std::vector<Unit> &ordinary, const std::vector<Unit> &inserted, size_t position,
                              size_t ordinal)
{
    if (ordinal < position)
        return ordinary[ordinal];
    if (ordinal < position + inserted.size())
        return inserted[ordinal - position];
    return ordinary[ordinal - inserted.size()];
}

// inserted 插在 ordinary 的第 position 组之前时首页守不守得住：下标在首页里的对照项（anchors 的前 guarded 个）
// 都在原下标，inserted 的第一组整组在首页。只摆到判得出为止。
inline bool KeepsFirstPage(const std::vector<Anchor> &anchors, size_t guarded, const std::vector<Unit> &ordinary,
                           const std::vector<Unit> &inserted, size_t position, size_t page_end)
{
    bool kept = true;
    bool first_inserted_laid = false;
    size_t anchors_laid = 0;
    LayOut(
        anchors, ordinary.size() + inserted.size(),
        [&](size_t ordinal) { return UnitWithInsertion(ordinary, inserted, position, ordinal); },
        [&](const LaidOutUnit &laid) {
            if (laid.anchored)
            {
                kept = laid.ordinal >= guarded || laid.start == anchors[laid.ordinal].index;
                anchors_laid = laid.ordinal + 1;
            }
            else if (laid.ordinal == position)
            {
                kept = laid.start + laid.unit.size() <= page_end;
                first_inserted_laid = true;
            }
            return kept && !(first_inserted_laid && anchors_laid >= guarded);
        });
    return kept;
}
} // namespace detail

/**
 * 从 items 里取出 take 选中的写法组，按原来的先后返回。take 按列表先后对每个没固定位置的写法组的组首各调用
 * 一次，固定了位置的组首不问 take、不取出；写法组中间插进了别的候选时，整组照样一起取出（分组见
 * detail::IsSpellingGroupHead）。
 * 固定了位置的候选与非普通来源的候选（云、AI、英文、emoji 等）连同写法组留在原下标，其余普通候选往前补位：
 * 对组页时的引擎结果，这等于在不含这些项的列表上做 apply_fixed_positions；对 PlaceInOrdinarySlots 摆过的
 * 列表，取出的正是摆进去的那一段时，这还原出摆之前的列表。什么都没取出时 items 不变。
 */
template <typename Predicate>
std::vector<WordItem> TakeOutKeepingFixedInPlace(std::vector<WordItem> &items, Predicate take)
{
    std::vector<WordItem> taken;
    // 组首只看自己的下标与 variant_rank，第一次选中之前不必分组。
    size_t first_taken_head = 0;
    while (first_taken_head < items.size() &&
           !(items[first_taken_head].fixed_position == 0 && detail::IsSpellingGroupHead(items, first_taken_head) &&
             take(items[first_taken_head])))
        ++first_taken_head;
    if (first_taken_head == items.size())
        return taken;

    const detail::SpellingGroups groups = detail::GroupSpellings(items);
    std::vector<detail::Anchor> anchors;
    std::vector<detail::Unit> stream;
    stream.reserve(groups.count());
    std::vector<size_t> taken_sources;
    // 取走的项与对照项都在这之前。
    size_t shift_only_from = groups.contiguous_from;
    for (size_t group = 0; group < groups.count(); ++group)
    {
        const size_t head_index = groups.head(group);
        const WordItem &head = items[head_index];
        const detail::Unit unit{&groups, group, 0};
        // 第一个选中的组首之前、没固定位置的组首都已问过 take。
        const bool taken_group = head.fixed_position == 0 &&
                                 (head_index == first_taken_head || (head_index > first_taken_head && take(head)));
        const bool anchored = !taken_group && (head.fixed_position > 0 || !IsOrdinaryCandidateSource(head.source));
        if (taken_group)
            detail::AppendSources(taken_sources, unit);
        else if (anchored)
            anchors.push_back({head_index, unit});
        else
            stream.push_back(unit);
        if (taken_group || anchored)
            shift_only_from = (std::max)(shift_only_from, groups.last_member(group) + 1);
    }
    taken.reserve(taken_sources.size());
    for (size_t from : taken_sources)
        taken.push_back(std::move(items[from]));
    const auto arrangement = detail::Arrange(
        anchors, stream.size(), [&](size_t ordinal) { return stream[ordinal]; }, {}, 0, shift_only_from, items.size());
    std::vector<WordItem> no_extra;
    detail::Rearrange(items, no_extra, arrangement);
    return taken;
}

/**
 * 把 group（字母词按引擎给的先后、各带自己的写法，末尾可接首个混输符号）放进 items 里普通候选的位置，只移动
 * group 自己：首选（第一个普通来源的候选）、非普通来源的候选与固定了位置的候选都停在原下标，其余普通候选
 * 保持先后、依次顺延，写法组不拆开。
 * group 的第一组整组放在首页（每页 page_size 个，0 表示整张列表算一页）里能放下它的最靠后的一段连续普通位置：
 * 它从首页上某个普通组（首选除外）的开头起，整组在首页，放下它与后面各组之后首页上的对照项都还在原下标。
 * 第一组只有一项时，这就是首页里除首选外最后一个普通候选的位置。首页上没有这样一段时，整段排在首页上最后
 * 一个普通组之后。其余组紧跟在第一组后面的普通位置上。首页之后的写法组跨过对照项的原下标时，对照项让到组
 * 后面。首页之后、最后一个普通来源的组后面那一串（其余英文、其余 emoji、颜文字、其余日期格式）留在列表末尾，
 * 先后不变。
 */
inline void PlaceInOrdinarySlots(std::vector<WordItem> &items, std::vector<WordItem> group, size_t page_size)
{
    if (group.empty())
        return;
    const size_t page_end = page_size > 0 ? page_size : (std::numeric_limits<size_t>::max)();
    // 列表末尾那一串从这个下标起：首页之后、最后一个普通来源的项之后。
    size_t tail_begin = 0;
    for (size_t index = 0; index < items.size(); ++index)
    {
        if (IsOrdinaryCandidateSource(items[index].source))
            tail_begin = index + 1;
    }
    if (page_size > 0)
        tail_begin = (std::max)(tail_begin, page_end);

    const detail::SpellingGroups groups = detail::GroupSpellings(items);
    std::vector<detail::Anchor> anchors;
    std::vector<detail::Unit> ordinary;
    ordinary.reserve(groups.count());
    std::vector<detail::Unit> tail;
    // 开头落在首页上的普通组有几个（它们排在 ordinary 最前面）。
    size_t first_page_ordinary = 0;
    bool seen_first_choice = false;
    // 对照项都在这之前。
    size_t shift_only_from = groups.contiguous_from;
    for (size_t list_group = 0; list_group < groups.count(); ++list_group)
    {
        const size_t index = groups.head(list_group);
        const WordItem &head = items[index];
        const detail::Unit unit{&groups, list_group, 0};
        const bool ordinary_source = IsOrdinaryCandidateSource(head.source);
        const bool first_choice = ordinary_source && !seen_first_choice;
        seen_first_choice = seen_first_choice || ordinary_source;
        if (index >= tail_begin)
            tail.push_back(unit);
        else if (!ordinary_source || first_choice || head.fixed_position > 0)
        {
            anchors.push_back({index, unit});
            shift_only_from = (std::max)(shift_only_from, groups.last_member(list_group) + 1);
        }
        else
        {
            first_page_ordinary += index < page_end ? 1 : 0;
            ordinary.push_back(unit);
        }
    }
    // group 的项排在 items 后面编号。
    const detail::SpellingGroups placed_groups = detail::GroupSpellings(group);
    std::vector<detail::Unit> inserted;
    for (size_t placed_group = 0; placed_group < placed_groups.count(); ++placed_group)
        inserted.push_back({&placed_groups, placed_group, items.size()});

    // 下标在首页里的对照项排在 anchors 最前面。
    const auto first_page_anchors_end = std::partition_point(
        anchors.begin(), anchors.end(), [&](const detail::Anchor &anchor) { return anchor.index < page_end; });
    const auto guarded = static_cast<size_t>(first_page_anchors_end - anchors.begin());
    // 插得越靠后，第一组的开头越靠后：从首页上最后一个普通组往前试，第一个守得住首页的就是最靠后的一段。
    size_t position = first_page_ordinary;
    for (size_t candidate = first_page_ordinary; candidate-- > 0;)
    {
        if (detail::KeepsFirstPage(anchors, guarded, ordinary, inserted, candidate, page_end))
        {
            position = candidate;
            break;
        }
    }
    const auto arrangement = detail::Arrange(
        anchors, ordinary.size() + inserted.size(),
        [&](size_t ordinal) { return detail::UnitWithInsertion(ordinary, inserted, position, ordinal); }, tail,
        position + inserted.size(), shift_only_from, items.size());
    detail::Rearrange(items, group, arrangement);
}

/**
 * 组页时引擎查出的列表：把字母词（latin_word）放进普通候选的位置（规则见 PlaceInOrdinarySlots），其余候选
 * 连同缓存的云、AI 都停在原下标；没有字母词时列表不动。
 */
inline void PlaceQueriedLatinWords(std::vector<WordItem> &items, size_t page_size)
{
    auto latin = TakeOutKeepingFixedInPlace(items, [](const WordItem &item) { return item.latin_word; });
    PlaceInOrdinarySlots(items, std::move(latin), page_size);
}
} // namespace FanyImeIpc
