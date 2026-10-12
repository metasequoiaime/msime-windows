#include "input_session.h"
#include "data_path.h"
#include "../common/helpcode_utils.h"
#include "../contracts/assets/assets.h"
#include "../user_dictionary/user_dictionary_journal.h"
#include "../local_modes/jianpin_query.h"
#include "../quanpin/quanpin_utils.h"
#include <algorithm>
#include <cctype>

namespace metasequoia
{
void InputSession::enable_fixed_positions()
{
    fixed_positions_enabled_ = true;
    update_mixed_candidates();
}

std::string InputSession::position_context(bool english, bool wubi) const
{
    if (english)
    {
        std::string input = dedicated_english_mode_ ? dedicated_english_preedit_
                                                    : (local_input_mode_ == LocalInputMode::TemporaryEnglish
                                                           ? local_preedit_.substr(1)
                                                           : engine_.get_request().raw_input_with_cases);
        std::transform(input.begin(), input.end(), input.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return "english:" + input;
    }
    if (local_input_mode_ == LocalInputMode::SuperJianpin)
        return local_modes::jianpin_ranking_context(local_preedit_.substr(1), scheme(), shuangpin_profile_);
    if (wubi)
        return engine_.get_request().raw_input;
    std::string context = get_quanpin();
    if (context.empty())
        context = get_pinyin_segmentation();
    if (engine_.get_request().raw_input.size() == 1)
        return context;
    auto plain = context;
    plain.erase(std::remove(plain.begin(), plain.end(), '\''), plain.end());
    // 全拼的上下文 key 跟查询走同一个纠错开关：关掉纠错时 laing 的候选按字面 lai'ng 出，
    // key 若仍被别名层切成 liang，两组固定位置会互相串。双拼词典不受全拼开关管，保持原样。
    const bool correction_aliases =
        scheme() == SchemeType::Shuangpin || quanpin::correction_aliases_enabled(quanpin_autocorrect_types_);
    const auto cuts = quanpin::cut_pinyin_by_mode(plain, "correction", correction_aliases);
    return cuts.empty() ? context : quanpin::join_segments(cuts.front());
}

void InputSession::apply_candidate_positions(std::vector<WordItem> &items)
{
    if (!fixed_positions_enabled_ || items.empty())
        return;
    const auto journal = path_to_utf8(paths_.user(assets::user_journal));
    const bool super_jianpin = local_input_mode_ == LocalInputMode::SuperJianpin;
    const bool regular =
        local_input_mode_ == LocalInputMode::None && !dedicated_english_mode_ && scheme() != SchemeType::JapaneseRomaji;
    if (regular && is_wubi())
    {
        // 混输组合里五笔与拼音候选共存，固定位置必须按候选自己的上下文分别结算：五笔候选
        // 写的是码，拼音候选写的是全拼，混进一个 context 会互相错配。两个分区各自排序后按
        // 「五笔在前、拼音在后」拼回，主方案（五笔）的位置语义原样保留。
        std::vector<WordItem> wubi_items;
        std::vector<WordItem> pinyin_items;
        for (auto &item : items)
        {
            (is_wubi_native_candidate(item) ? wubi_items : pinyin_items).push_back(std::move(item));
        }
        const bool include_missing = engine_.get_request().raw_input.size() == 1;
        if (!wubi_items.empty())
        {
            user_dictionary::apply_fixed_positions(
                journal, position_context(false, true), wubi_items, include_missing,
                [this](const std::string &key, const std::string &word) {
                    return engine_.find_candidate(SchemeType::Wubi, key, word);
                },
                has_active_helpcode());
        }
        if (!pinyin_items.empty())
        {
            user_dictionary::apply_fixed_positions(
                journal, position_context(false, false), pinyin_items, include_missing,
                [this](const std::string &key, const std::string &word) {
                    return engine_.find_candidate(SchemeType::Quanpin, key, word);
                },
                has_active_helpcode());
        }
        // include_missing 会把固定在拼音上下文的词从词库补回来，它可能正是五笔分区已经列出的词。
        // 合并时同词只留五笔那份，和 ImeSession::refresh_candidates 的去重规则一致。
        pinyin_items.erase(std::remove_if(pinyin_items.begin(), pinyin_items.end(),
                                          [&wubi_items](const WordItem &pinyin_item) {
                                              return std::any_of(wubi_items.begin(), wubi_items.end(),
                                                                 [&pinyin_item](const WordItem &wubi_item) {
                                                                     return wubi_item.word == pinyin_item.word;
                                                                 });
                                          }),
                           pinyin_items.end());
        items.clear();
        items.insert(items.end(), std::make_move_iterator(wubi_items.begin()),
                     std::make_move_iterator(wubi_items.end()));
        items.insert(items.end(), std::make_move_iterator(pinyin_items.begin()),
                     std::make_move_iterator(pinyin_items.end()));
    }
    else if (regular)
    {
        user_dictionary::apply_fixed_positions(
            journal, position_context(false, false), items, engine_.get_request().raw_input.size() == 1,
            [this](const std::string &key, const std::string &word) {
                return engine_.find_candidate(scheme(), key, word);
            },
            has_active_helpcode());
    }
    else if (super_jianpin)
    {
        user_dictionary::apply_fixed_positions(journal, position_context(false, false), items, false);
    }
    if (std::any_of(items.begin(), items.end(),
                    [](const auto &item) { return item.source == CandidateSource::EnglishDictionary; }))
        user_dictionary::apply_fixed_positions(journal, position_context(true, false), items, false, {}, true);
}

KeyResult InputSession::set_candidate_position(std::size_t index, int position)
{
    if (position < 0 || position > 5 || index >= candidates().size())
        return {};
    const auto selected = candidates()[index];
    const bool english = selected.source == CandidateSource::EnglishDictionary;
    if (!english &&
        ((selected.source != CandidateSource::Database && selected.source != CandidateSource::UserDatabase) ||
         scheme() == SchemeType::JapaneseRomaji))
        return {};
    const bool wubi = is_wubi_native_candidate(selected) && local_input_mode_ != LocalInputMode::SuperJianpin;
    const auto context = position_context(english, wubi);
    const auto key = english || wubi
                         ? selected.pinyin
                         : (selected.canonical_pinyin.empty() ? selected.pinyin : selected.canonical_pinyin);
    if (context.empty() || key.empty())
        return {};
    const auto journal = path_to_utf8(paths_.user(assets::user_journal));
    const bool ok = position == 0 ? user_dictionary::clear_fixed_position(journal, context, key, selected.word)
                                  : user_dictionary::set_fixed_position(journal, context, key, selected.word, position);
    if (!ok)
        return {true, std::nullopt, "Unable to persist candidate position."};
    reset_cache();
    if (dedicated_english_mode_)
        update_dedicated_english_candidates();
    else if (local_input_mode_ != LocalInputMode::None)
        return {true, std::nullopt, update_local_candidates()};
    else
        recompute_candidates();
    return {true, std::nullopt, std::nullopt};
}

KeyResult InputSession::remove_candidate(std::size_t index)
{
    if (index >= candidates().size())
        return {};
    const auto selected = candidates()[index];
    const bool english = selected.source == CandidateSource::EnglishDictionary;
    if (!english &&
        ((selected.source != CandidateSource::Database && selected.source != CandidateSource::UserDatabase) ||
         scheme() == SchemeType::JapaneseRomaji || HelpcodeUtils::count_utf8_chars(selected.word) <= 1))
        return {};

    const bool wubi = is_wubi_native_candidate(selected) && local_input_mode_ != LocalInputMode::SuperJianpin;
    const auto kind = english
                          ? user_dictionary::DictionaryKind::English
                          : (wubi ? user_dictionary::DictionaryKind::Wubi : user_dictionary::DictionaryKind::Pinyin);
    // A displayed pinyin candidate already carries its exact dictionary key. Re-segmenting it
    // (or expanding shuangpin again) can delete a different pronunciation of the same word.
    const auto &key = english || wubi ? selected.pinyin : selected.canonical_pinyin;
    if (key.empty())
        return {};
    if (!user_dictionary::delete_dictionary_candidate(
            path_to_utf8(paths_.dictionary(english ? assets::english_dictionary : assets::main_dictionary)),
            path_to_utf8(paths_.user(assets::user_journal)), kind, key, selected.word))
        return {true, std::nullopt, "Unable to persist candidate removal."};

    reset_cache();
    if (dedicated_english_mode_)
        update_dedicated_english_candidates();
    else if (local_input_mode_ != LocalInputMode::None)
        return {true, std::nullopt, update_local_candidates()};
    else
        recompute_candidates();
    return {true, std::nullopt, std::nullopt};
}
} // namespace metasequoia
