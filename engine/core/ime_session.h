#pragma once

#include "composition_state.h"
#include "input_session_types.h"
#include "scheme_type.h"
#include "sentence_association_options.h"
#include "../providers/provider_registry.h"
#include "../schemes/input_scheme.h"
#include "../schemes/wubi_scheme.h"
#include "../shuangpin/shuangpin_profile.h"
#include <memory>

class ImeSession
{
  public:
    explicit ImeSession(SchemeType scheme_type = SchemeType::Shuangpin,
                        const ShuangpinProfile &shuangpin_profile = GetXiaoheShuangpinProfile(),
                        metasequoia::RuntimePaths paths = metasequoia::RuntimePaths::legacy());

    void handle_key(ImeKeyCode vk, ImeModifierMask modifiers_down = 0, ImeCharacter wch = 0);
    void switch_scheme(SchemeType scheme_type);
    void set_shuangpin_helpcode_enabled(bool enabled);
    void set_mid_sentence_helpcode_enabled(bool enabled);
    // 句中辅助码的大写触发（见 FanyImeMidSentenceHelpcode::StartsUppercaseBlock）。只在句中辅助码开着、
    // 直接辅助码关着时生效。
    void set_mid_sentence_uppercase_trigger_enabled(bool enabled);
    bool mid_sentence_uppercase_trigger_active() const
    {
        return enable_mid_sentence_helpcode_ && enable_mid_sentence_uppercase_trigger_ && !enable_direct_helpcode_;
    }
    // 双拼直接辅助码（万象式，见 engine/direct_helpcode/）。开着时末尾单码/双码辅助和反引号句中
    // 辅助码都让位给它。
    void set_direct_helpcode_enabled(bool enabled);
    bool direct_helpcode_enabled() const
    {
        return enable_direct_helpcode_;
    }
    // 直接辅助码在句中用什么结束四码：补 /、第二位辅码大写，可以都开。
    void set_direct_helpcode_markers(bool slash, bool uppercase);
    void set_quanpin_helpcode_enabled(bool enabled);
    void set_quanpin_autocorrect_types(unsigned autocorrect_types);
    void set_fuzzy_pinyin_options(metasequoia::FuzzyPinyinOptions options)
    {
        fuzzy_pinyin_ = options;
    }
    void set_sentence_association(const SentenceAssociationOptions &options)
    {
        sentence_association_ = options;
    }
    // 本会话最近上屏的文本，随查询下发给神经重排当前文。
    void set_rescoring_context(std::string context)
    {
        rescoring_context_ = std::move(context);
    }
    void set_wubi_input_options(metasequoia::WubiInputOptions options);
    const metasequoia::WubiInputOptions &wubi_input_options() const
    {
        return wubi_options_;
    }
    void replace_shuangpin_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases);
    void replace_quanpin_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases);
    void replace_wubi_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases);
    void replace_japanese_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases);
    // Writes back to whichever scheme is composing. Committing a pinyin answer out of a longer
    // mixed composition has to shorten the live composition, and the pinyin-shaped caller would
    // otherwise address a scheme that is not the active one and be ignored.
    void replace_active_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases);
    // Runs one standalone candidate query for the given raw input without touching the live
    // composition: the active scheme's raw/key strokes and state_'s request/candidates stay put.
    // Query options (helpcode, autocorrect, fuzzy) match refresh_candidates exactly. The caret
    // driven prefix decoding in InputSession uses this to decode a prefix while the composition
    // still owns the full raw string.
    std::vector<WordItem> query_raw_candidates(const std::string &raw_input, const std::string &raw_input_with_cases);
    // 当前请求去掉句中辅助码约束再查一次，组合本身不动。请求里没有约束时直接返回当前候选。
    std::vector<WordItem> query_without_syllable_helpcodes();
    void reset();
    void reset_cache();
    // 只清整句排序所在的缓存层，不重算候选：调用方随后自己 refresh，免得像 reset_cache
    // 那样先冷算一遍、调用方再算一遍。
    void reset_sentence_cache();
    int create_word(std::string pinyin, std::string word);
    // 混输组合里的候选分属五笔码表和拼音词典，写入权重必须显式指明目标方案。
    int update_weight_by_pinyin_and_word(SchemeType scheme, std::string pinyin, std::string word);
    int delete_by_pinyin_and_word(SchemeType scheme, std::string pinyin, std::string word);
    int cache_dynamic_candidate(const std::string &pinyin, const std::string &word, CandidateSource source);
    int cache_dynamic_candidate_for_current_request(const std::string &word, CandidateSource source);
    int apply_dynamic_candidate(const std::string &word, CandidateSource source);
    std::optional<WordItem> find_candidate(SchemeType scheme, const std::string &key, const std::string &value);

    SchemeType current_scheme_type() const;
    const std::string &get_preedit() const;
    const QueryRequest &get_request() const;
    // Forwarded from the live wubi scheme so a session with no wubi scheme answers false. The
    // composition knows whether its raw input is a full four-letter code; only the scheme holds it.
    bool wubi_code_is_complete() const
    {
        return wubi_scheme_ != nullptr && wubi_scheme_->has_complete_code();
    }
    const std::vector<WordItem> &get_candidates() const;
    bool expand_initial_candidates();

    void set_helpcode_keymap(HelpcodeUtils::SharedKeymap table)
    {
        provider_registry_.set_helpcode_keymap(std::move(table));
        refresh_candidates();
    }

  private:
    // Shared option injection for refresh_candidates() and query_raw_candidates(); the two must
    // not drift or a prefix query would answer with different candidates than the live pipeline.
    void apply_request_options(QueryRequest &request) const;
    // 直接辅助码开着时把请求交给解析器改写，见 ProviderRegistry::resolve_direct_helpcode。
    void resolve_direct_helpcode(QueryRequest &request);
    void refresh_candidates();
    void bind_wubi_scheme();
    void bind_shuangpin_scheme();
    // 把会话的直接辅助码、大写触发设置下发给双拼方案（非双拼方案什么也不做）。
    void configure_shuangpin_scheme(IInputScheme *scheme) const;
    // 把会话的纠错掩码下发给全拼方案（非全拼方案什么也不做）。要在 build_request 之前调，
    // 掩码决定切分要不要跑手误别名层。
    void configure_quanpin_scheme(IInputScheme *scheme) const;
    // 换方案或改纠错开关时把掩码绑到当前方案上，和 bind_shuangpin_scheme 同一模式。
    void bind_quanpin_scheme();
    std::unique_ptr<IInputScheme> create_scheme(SchemeType scheme_type) const;

  private:
    ProviderRegistry provider_registry_;
    const ShuangpinProfile shuangpin_profile_;
    std::unique_ptr<IInputScheme> scheme_;
    CompositionState state_;
    bool enable_shuangpin_helpcode_ = false;
    bool enable_mid_sentence_helpcode_ = false;
    bool enable_direct_helpcode_ = false;
    bool direct_helpcode_slash_marker_ = true;
    bool direct_helpcode_uppercase_marker_ = false;
    bool enable_mid_sentence_uppercase_trigger_ = false;
    bool enable_quanpin_helpcode_ = false;
    unsigned quanpin_autocorrect_types_ = 0;
    metasequoia::FuzzyPinyinOptions fuzzy_pinyin_;
    SentenceAssociationOptions sentence_association_;
    std::string rescoring_context_;
    metasequoia::WubiInputOptions wubi_options_;
    // Resolved when the scheme changes rather than on every keystroke.
    WubiScheme *wubi_scheme_ = nullptr;
};
