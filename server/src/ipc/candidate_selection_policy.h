#pragma once

#include "engine/core/word_item.h"
#include "engine/local_modes/date_time_query.h"

#include <algorithm>
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

// 异步候选的槽位按普通候选数，快捷短语不占槽位：越过 count 个非快捷短语候选后，再越过
// 紧跟着的整组快捷短语，所以同一位置上快捷短语排在异步候选前面，组也不会被拆开。
inline size_t IndexAfterLocalCandidates(const std::vector<WordItem> &items, size_t count)
{
    size_t index = 0;
    for (size_t counted = 0; index < items.size() && counted < count; ++index)
    {
        if (items[index].source != CandidateSource::QuickPhrase)
            ++counted;
    }
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
//   page_size      每页候选数，给补全词的默认位置和槽位上限用；0 表示不限。
struct EnglishPlacement
{
    std::optional<size_t> slot;
    bool require_exact = false;
    std::string input;
    size_t page_size = 0;
};

inline bool IsExactEnglishMatch(const WordItem &item, const EnglishPlacement &placement)
{
    return item.source == CandidateSource::EnglishDictionary && item.pinyin == placement.input;
}

// 占着英文槽位的那个候选在列表里的下标；没有英文、或它被用户固定了位置时为空。
// NormalizeMixedCandidateOrder 把占槽位的英文放在其余英文前面，所以列表里第一个英文就是它。
inline std::optional<size_t> SlottedEnglishIndex(const std::vector<WordItem> &items)
{
    const auto english = std::find_if(items.begin(), items.end(), [](const WordItem &item) {
        return item.source == CandidateSource::EnglishDictionary;
    });
    if (english == items.end() || english->fixed_position > 0)
        return std::nullopt;
    return static_cast<size_t>(english - items.begin());
}

// Keep asynchronous mixed-input candidates in stable priority slots regardless
// of the order in which their workers finish. English keeps its legacy slotting
// (promoted ahead of AI unless a cloud result forces it behind cloud+AI), and
// emoji/kaomoji are placed at the end of the candidate list so they do not
// displace normal Chinese candidates:
//   no cloud:         Chinese..., English, AI, ..., emoji, kaomoji
//   cloud:            Chinese..., cloud, AI, English, ..., emoji, kaomoji
//   cloud only:       Chinese..., cloud, English, ..., emoji, kaomoji
//   base:             Chinese..., English, ..., emoji, kaomoji
// 日期时间只在 rq / sj / xq 这类唤醒词上出现，用户打它就是要日期，所以它排在所有异步候选前面，
// 紧跟首个中文候选，展开全部格式的入口紧跟在它后面：Chinese, date/time, 📅日期, cloud, ...
// The learned English slot (EnglishPlacement) moves the English candidate away
// from that default afterwards, and explicit English ranking choices are
// reapplied last.
inline void NormalizeMixedCandidateOrder(std::vector<WordItem> &items, size_t local_prefix_slots = 1,
                                         const EnglishPlacement &english_placement = {})
{
    std::vector<WordItem> local_candidates;
    std::vector<WordItem> english_candidates;
    std::vector<WordItem> emoji_candidates;
    std::vector<WordItem> kaomoji_candidates;
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
            english_candidates.push_back(std::move(item));
            break;
        case CandidateSource::Emoji:
            emoji_candidates.push_back(std::move(item));
            break;
        case CandidateSource::Kaomoji:
            kaomoji_candidates.push_back(std::move(item));
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
    auto insert_at = [&](size_t index, WordItem candidate) {
        const auto offset = static_cast<std::ptrdiff_t>((std::min)(index, items.size()));
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

    size_t slot = IndexAfterLocalCandidates(items, local_prefix_slots);
    if (!date_time_candidates.empty())
    {
        insert_at(slot++, std::move(date_time_candidates.front()));
        date_time_candidates.erase(date_time_candidates.begin());
        if (date_time_menu)
            insert_at(slot++, std::move(*date_time_menu));
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
        english_index = (std::min)(slot, items.size());
        insert_at(slot++, std::move(english_candidates.front()));
        english_candidates.erase(english_candidates.begin());
    }
    if (!cloud_candidate && ai_candidate)
        insert_at(slot++, std::move(*ai_candidate));

    for (auto &candidate : english_candidates)
        items.push_back(std::move(candidate));
    for (auto &candidate : emoji_candidates)
        items.push_back(std::move(candidate));
    for (auto &candidate : kaomoji_candidates)
        items.push_back(std::move(candidate));
    for (auto &candidate : date_time_candidates)
        items.push_back(std::move(candidate));

    // 把英文从默认位置挪到学到的槽位；没学过的补全词挪到首页末位。槽位 0 排在最前，但仍在首位
    // 的快捷短语组后面。后面插入的 AI 候选与末尾追加的 emoji/颜文字都在 english_index 之后，下标仍然有效。
    std::optional<size_t> target = english_placement.slot;
    if (!target && completion_takes_slot)
        target = english_placement.page_size > 0 ? english_placement.page_size - 1 : items.size();
    if (english_index && target && items[*english_index].fixed_position == 0)
    {
        if (english_placement.page_size > 0)
            target = (std::min)(*target, english_placement.page_size - 1);
        WordItem candidate = std::move(items[*english_index]);
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(*english_index));
        insert_at(*target == 0 ? IndexAfterLocalCandidates(items, 0) : *target, std::move(candidate));
    }

    std::vector<WordItem> fixed_english_candidates;
    for (auto candidate = items.begin(); candidate != items.end();)
    {
        if (candidate->source == CandidateSource::EnglishDictionary && candidate->fixed_position > 0)
        {
            fixed_english_candidates.push_back(std::move(*candidate));
            candidate = items.erase(candidate);
        }
        else
        {
            ++candidate;
        }
    }
    std::stable_sort(
        fixed_english_candidates.begin(), fixed_english_candidates.end(),
        [](const WordItem &left, const WordItem &right) { return left.fixed_position < right.fixed_position; });
    for (auto &candidate : fixed_english_candidates)
        insert_at(static_cast<size_t>(candidate.fixed_position - 1), std::move(candidate));
}
} // namespace FanyImeIpc
