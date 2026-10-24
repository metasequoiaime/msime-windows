#pragma once

#include "engine/core/word_item.h"
#include "engine/local_modes/date_time_query.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace FanyImeIpc
{
// Cloud candidates already represent a complete result for the current
// query.  They must not turn a shorter cloud query into word-creation mode.
constexpr bool ShouldEnterCreatingWord(CandidateSource source, bool continues_composition) noexcept
{
    return continues_composition && source != CandidateSource::CloudSuggestion;
}

// 整句落库的长度上限，对标词库里短语自身的上限
// （quanpin::WordLatticeOptions::max_phrase_syllables）。再长的整句只是这一次输入
// 的产物，落库除了撑大用户词库没有别的作用。
constexpr size_t kMaxLearnedSentenceSyllables = 7;

inline size_t CountCanonicalSyllables(const std::string &canonical_pinyin) noexcept
{
    if (canonical_pinyin.empty())
    {
        return 0;
    }
    return 1 + static_cast<size_t>(std::count(canonical_pinyin.begin(), canonical_pinyin.end(), '\''));
}

// 猜出来的整句来源：词格 Generated、Google 解码器 Fallback，神经整句
// NeuralDesktop / NeuralKeyboard，以及 octagram 语法模型重排的 Collocation。
// 它们都不是词库里已有的行，落库/学习判定同类处理。
constexpr bool IsGuessedSentenceSource(CandidateSource source) noexcept
{
    return source == CandidateSource::Generated || source == CandidateSource::Fallback ||
           source == CandidateSource::NeuralDesktop || source == CandidateSource::NeuralKeyboard ||
           source == CandidateSource::Collocation;
}

// 整句候选独立上屏——不接在造词前缀后面、自己就是整条输入——时是否该落库。该落：整句是
// 猜出来的，词库里没有它那一行，调频改的是已有的行，改不到它。不落库的话用户选多少次，
// 下次它仍然要靠猜，也仍然排在词库里那条同音短语后面。
inline bool ShouldStoreStandaloneSentence(CandidateSource source,
                                          const std::string &candidate_canonical_pinyin) noexcept
{
    if (!IsGuessedSentenceSource(source))
    {
        return false;
    }
    const size_t syllables = CountCanonicalSyllables(candidate_canonical_pinyin);
    return syllables > 0 && syllables <= kMaxLearnedSentenceSyllables;
}

// Special candidates commit through an early return in ProcessSelectionKey,
// before the creating-word completion block that normally persists a composed
// phrase.  A lattice whole-sentence candidate therefore has to be stored at
// that early return instead, in both shapes it can take:
//   - 它接在造词前缀后面、结束一段造词时，前后两段都要有 canonical quanpin，否则
//     拼不出完整读音。这一支的长度由用户一段段选出来，维持原样不设上限；
//   - 它自己就是整条输入时，按 ShouldStoreStandaloneSentence 判定。
inline bool ShouldStoreEarlyReturnPhrase(CandidateSource source, bool creating_word_active,
                                         const std::string &prefix_canonical_pinyin,
                                         const std::string &candidate_canonical_pinyin) noexcept
{
    // 与词格 Generated 同一支：神经整句与语法模型重排句同样带 canonical quanpin，可以结束
    // 一段造词或独立上屏。Google Fallback 沿用旧语义仍不走这条造词落库路径。
    const bool early_return_source = source == CandidateSource::Generated || source == CandidateSource::NeuralDesktop ||
                                     source == CandidateSource::NeuralKeyboard ||
                                     source == CandidateSource::Collocation;
    if (!early_return_source || candidate_canonical_pinyin.empty())
    {
        return false;
    }
    if (!creating_word_active)
    {
        return ShouldStoreStandaloneSentence(source, candidate_canonical_pinyin);
    }
    return !prefix_canonical_pinyin.empty();
}

// 异步候选的槽位按普通候选数，快捷短语不占槽位，同一底词的繁体写法（variant_rank > 0 的项）
// 也不另占槽位：越过 count 个普通候选后，先越过最后那个候选剩下的写法，再越过紧跟着的整组
// 快捷短语，所以同一位置上快捷短语排在异步候选前面，写法组与快捷短语组都不会被拆开。
inline size_t IndexAfterLocalCandidates(const std::vector<WordItem> &items, size_t count)
{
    size_t index = 0;
    for (size_t counted = 0; index < items.size() && counted < count; ++index)
    {
        if (items[index].source != CandidateSource::QuickPhrase && items[index].variant_rank == 0)
            ++counted;
    }
    while (index < items.size() && items[index].variant_rank > 0)
        ++index;
    while (index < items.size() && items[index].source == CandidateSource::QuickPhrase)
        ++index;
    return index;
}

// 中英混输里首个英文候选该排在哪。英文词库和中文词库的权重不在一个量纲上，不能拿来比大小
// （曾经英文调频写成「全表最高 + 1000」，选一次 zaire 就永久压过「在」），位置只由这里决定：
//   slot           按输入学到的槽位（user_dictionary::english_slot）：它在列表里的下标，0 是首位。
//                  没学过时用默认位置。
//   require_exact  输入能读成拼音时置位：和输入完全相同的英文词优先占槽位，默认紧跟第一个中文
//                  候选；没有精确匹配时由最前的补全词（zai → zaire）占槽位，默认退到首页末位。
//   input          小写的当前输入，用来认精确匹配。
//   page_size      每页候选数，给补全词的默认位置、槽位上限和首个 emoji 的位置用；0 表示不限。
//   first_page_count 首页最多显示几个英文词，落在首页的固定英文也算在内，但占槽位的英文总留一个；
//                  默认仍为一个，不挤掉英文前面的中文首选、日期、云、AI 候选或首页 emoji。
struct EnglishPlacement
{
    std::optional<size_t> slot;
    bool require_exact = false;
    std::string input;
    size_t page_size = 0;
    size_t first_page_count = 1;
};

inline bool IsExactEnglishMatch(const WordItem &item, const EnglishPlacement &placement)
{
    return item.source == CandidateSource::EnglishDictionary && item.pinyin == placement.input;
}

// 占着英文槽位的那个候选在列表里的下标；没有未固定位置的英文时为空。
// NormalizeMixedCandidateOrder 不让固定了位置的英文占槽位，并把占槽位的英文放在其余未固定的英文前面，
// 所以列表里第一个未固定的英文就是它。
inline std::optional<size_t> SlottedEnglishIndex(const std::vector<WordItem> &items)
{
    const auto english = std::find_if(items.begin(), items.end(), [](const WordItem &item) {
        return item.source == CandidateSource::EnglishDictionary && item.fixed_position == 0;
    });
    if (english == items.end())
        return std::nullopt;
    return static_cast<size_t>(english - items.begin());
}

// Keep asynchronous mixed-input candidates in stable priority slots regardless
// of the order in which their workers finish. English keeps its legacy slotting
// (promoted ahead of AI unless a cloud result forces it behind cloud+AI), and
// emoji/kaomoji are placed at the end of the candidate list so they do not
// displace normal Chinese candidates:
//   no cloud:         Chinese..., English, AI, ..., emoji, kaomoji, symbols
//   cloud:            Chinese..., cloud, AI, English, ..., emoji, kaomoji, symbols
//   cloud only:       Chinese..., cloud, English, ..., emoji, kaomoji, symbols
//   base:             Chinese..., English, ..., emoji, kaomoji, symbols
// 知道每页候选数（page_size）时，最匹配的那个 emoji 单独放在首页末位，其余 emoji 仍在末尾；
// 补全词的默认位置和学到的英文槽位相应让到首页倒数第二位，两者都留在首页。
// 日期时间只在 rq / sj / xq 这类唤醒词上出现，用户打它就是要日期，所以它排在所有异步候选前面，
// 紧跟首个中文候选，展开全部格式的入口紧跟在它后面：Chinese, date/time, 📅日期, cloud, ...
// 符号同理（只在整串等于编码时出现），首个符号排在日期时间之后、云候选之前，其余在末尾。
// The learned English slot (EnglishPlacement) moves the English candidate away
// from that default afterwards, and explicit English ranking choices are
// reapplied last.
inline void NormalizeMixedCandidateOrder(std::vector<WordItem> &items, size_t local_prefix_slots = 1,
                                         const EnglishPlacement &english_placement = {})
{
    std::vector<WordItem> local_candidates;
    std::vector<WordItem> english_candidates;
    std::vector<WordItem> fixed_english_candidates;
    std::vector<WordItem> emoji_candidates;
    std::vector<WordItem> kaomoji_candidates;
    std::vector<WordItem> symbol_candidates;
    std::vector<WordItem> date_time_candidates;
    std::optional<WordItem> date_time_menu;
    std::optional<WordItem> cloud_candidate;
    std::optional<WordItem> ai_candidate;
    local_candidates.reserve(items.size());

    for (auto &item : items)
    {
        switch (item.source)
        {
        case CandidateSource::CloudSuggestion:
            if (!cloud_candidate)
                cloud_candidate = std::move(item);
            break;
        case CandidateSource::AiSuggestion:
            if (!ai_candidate)
                ai_candidate = std::move(item);
            break;
        case CandidateSource::EnglishDictionary:
            // 固定了位置的英文不进可移动英文组，不占槽位，最后按固定位置插回；落在首页的固定词
            // 另外计入首页英文数。它若留在组里，第二次归一化时会挤掉上次显示的另一条补全词，
            // 异步结果回来时首页就会反复跳动。
            if (item.fixed_position > 0)
                fixed_english_candidates.push_back(std::move(item));
            else
                english_candidates.push_back(std::move(item));
            break;
        case CandidateSource::Emoji:
            emoji_candidates.push_back(std::move(item));
            break;
        case CandidateSource::Kaomoji:
            kaomoji_candidates.push_back(std::move(item));
            break;
        case CandidateSource::Symbol:
            symbol_candidates.push_back(std::move(item));
            break;
        case CandidateSource::DateTime:
            if (!metasequoia::local_modes::is_date_time_menu_item(item))
                date_time_candidates.push_back(std::move(item));
            else if (!date_time_menu)
                date_time_menu = std::move(item);
            break;
        default:
            local_candidates.push_back(std::move(item));
            break;
        }
    }

    items = std::move(local_candidates);
    // 同一底词的繁体写法（variant_rank > 0 的项跟在第一写法后面）是一组，任何插入都不落在组中间：
    // 落点在组里时挪到这一组之后。
    const auto outside_spelling_group = [&](size_t index) {
        index = (std::min)(index, items.size());
        while (index < items.size() && items[index].variant_rank > 0)
            ++index;
        return index;
    };
    auto insert_at = [&](size_t index, WordItem candidate) {
        const auto offset = static_cast<std::ptrdiff_t>(outside_spelling_group(index));
        items.insert(items.begin() + offset, std::move(candidate));
    };

    // 能读成拼音的输入优先让精确匹配占槽位：把它挪到英文组最前；没有精确匹配时占槽位的是补全词。
    const bool english_takes_slot = !english_candidates.empty();
    bool completion_takes_slot = false;
    if (english_takes_slot && english_placement.require_exact)
    {
        const auto exact =
            std::find_if(english_candidates.begin(), english_candidates.end(),
                         [&](const WordItem &item) { return IsExactEnglishMatch(item, english_placement); });
        completion_takes_slot = exact == english_candidates.end();
        if (!completion_takes_slot)
            std::rotate(english_candidates.begin(), exact, exact + 1);
    }

    // 没有云候选时 AI 紧跟在英文后面，它和英文前面的候选一样不该被额外的英文挤出首页。
    const bool ai_follows_english = !cloud_candidate && ai_candidate;
    size_t slot = IndexAfterLocalCandidates(items, local_prefix_slots);
    if (!date_time_candidates.empty())
    {
        insert_at(slot++, std::move(date_time_candidates.front()));
        date_time_candidates.erase(date_time_candidates.begin());
        if (date_time_menu)
            insert_at(slot++, std::move(*date_time_menu));
    }
    // 符号只在整串等于某个编码时混进来（alpha、szq、yi），用户打的就是它：首个紧跟首个中文候选
    // （和日期时间一样），其余排在颜文字之后。
    if (!symbol_candidates.empty())
    {
        insert_at(slot++, std::move(symbol_candidates.front()));
        symbol_candidates.erase(symbol_candidates.begin());
    }
    if (cloud_candidate)
    {
        insert_at(slot++, std::move(*cloud_candidate));
        if (ai_candidate)
            insert_at(slot++, std::move(*ai_candidate));
    }
    std::optional<size_t> english_index;
    if (english_takes_slot)
    {
        english_index = outside_spelling_group(slot);
        insert_at(slot++, std::move(english_candidates.front()));
        english_candidates.erase(english_candidates.begin());
    }
    if (!cloud_candidate && ai_candidate)
        insert_at(slot++, std::move(*ai_candidate));

    // 首个 emoji 是查询里最匹配的，占首页末位；每页不足两个时没有可让的位置，和其余 emoji 一起排末尾。
    std::optional<WordItem> first_page_emoji;
    if (english_placement.page_size >= 2 && !emoji_candidates.empty())
    {
        first_page_emoji = std::move(emoji_candidates.front());
        emoji_candidates.erase(emoji_candidates.begin());
    }
    // 落在首页的固定英文占首页英文数，也占首页的位置；排在首页 emoji 前面的那些插回时会把 emoji 往后挤。
    const size_t page_size = english_placement.page_size;
    const auto fixed_within = [&](size_t positions) {
        return static_cast<size_t>(
            std::count_if(fixed_english_candidates.begin(), fixed_english_candidates.end(),
                          [&](const WordItem &item) { return static_cast<size_t>(item.fixed_position) <= positions; }));
    };
    const size_t fixed_on_first_page = fixed_within(page_size);
    const size_t fixed_before_emoji = first_page_emoji ? fixed_within(page_size - 1) : 0;
    const size_t reserved = (first_page_emoji ? 1 : 0) + fixed_on_first_page;

    // 额外英文也从已经排好序的真实词库结果取，不生成词形。默认位置在英文前面的候选（中文首选、日期、
    // 云、AI）、首页 emoji 和首页上的固定英文都留在首页；每页太小时回到单个英文的原有行为。
    // 占槽位的英文总保留一个，与固定英文无关，未知页大小也沿用原行为。
    const size_t page_end = page_size > reserved ? page_size - reserved : 0;
    const size_t protected_prefix = english_index ? *english_index + (ai_follows_english ? 1 : 0) : 1;
    const size_t english_capacity = page_end > protected_prefix ? page_end - protected_prefix : 1;
    const size_t english_budget = (std::max)(english_placement.first_page_count, size_t{1});
    const size_t movable_budget = english_budget > fixed_on_first_page ? english_budget - fixed_on_first_page : 1;
    const size_t requested_count = page_size > 0 ? (std::min)(movable_budget, english_capacity) : 1;
    std::vector<WordItem> first_page_english;
    if (english_index && requested_count > 1)
    {
        std::vector<std::string> selected_keys{items[*english_index].pinyin};
        for (auto candidate = english_candidates.begin();
             candidate != english_candidates.end() && selected_keys.size() < requested_count;)
        {
            if (std::find(selected_keys.begin(), selected_keys.end(), candidate->pinyin) != selected_keys.end())
            {
                ++candidate;
                continue;
            }
            selected_keys.push_back(candidate->pinyin);
            first_page_english.push_back(std::move(*candidate));
            candidate = english_candidates.erase(candidate);
        }
    }
    const size_t english_group_size = first_page_english.size() + 1;
    const size_t english_slot_maximum = page_size > 0
                                            ? (page_end > english_group_size ? page_end - english_group_size : 0)
                                            : (std::numeric_limits<size_t>::max)();

    for (auto &candidate : english_candidates)
        items.push_back(std::move(candidate));
    for (auto &candidate : emoji_candidates)
        items.push_back(std::move(candidate));
    for (auto &candidate : kaomoji_candidates)
        items.push_back(std::move(candidate));
    for (auto &candidate : symbol_candidates)
        items.push_back(std::move(candidate));
    for (auto &candidate : date_time_candidates)
        items.push_back(std::move(candidate));

    // 把英文从默认位置挪到学到的槽位；没学过的补全词挪到首页末位（首页末位给了 emoji 时是倒数第二位）。
    // 槽位 0 排在最前，但仍在首位的快捷短语组后面。后面插入的 AI 候选与末尾追加的 emoji/颜文字都在
    // english_index 之后，下标仍然有效。
    std::optional<size_t> target = english_placement.slot;
    if (!target && completion_takes_slot)
        target = english_placement.page_size > 0 ? english_slot_maximum : items.size();
    if (english_index && target)
    {
        target = (std::min)(*target, english_slot_maximum);
        WordItem candidate = std::move(items[*english_index]);
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(*english_index));
        english_index = outside_spelling_group(*target == 0 ? IndexAfterLocalCandidates(items, 0) : *target);
        insert_at(*english_index, std::move(candidate));
    }

    // 首个英文仍服从原有精确匹配/按输入调频，选中的其余英文紧随其后；所有手工固定位置
    // 在最后重新应用，仍有最高优先级。重复归一化和候选扩展不会改变英文组内部的顺序。
    if (english_index)
    {
        size_t next = *english_index + 1;
        for (auto &candidate : first_page_english)
            insert_at(next++, std::move(candidate));
    }

    // 英文挪完再放 emoji，英文的下标不会被它挤动；列表不满一页时它就落在末尾。用户固定了位置的英文
    // 在下面最后插入：排在它前面的那些插回后 emoji 正好回到首页末位，同位时 emoji 让位。
    if (first_page_emoji)
        insert_at(page_size - 1 - fixed_before_emoji, std::move(*first_page_emoji));

    std::stable_sort(
        fixed_english_candidates.begin(), fixed_english_candidates.end(),
        [](const WordItem &left, const WordItem &right) { return left.fixed_position < right.fixed_position; });
    for (auto &candidate : fixed_english_candidates)
        insert_at(static_cast<size_t>(candidate.fixed_position - 1), std::move(candidate));
}
} // namespace FanyImeIpc
