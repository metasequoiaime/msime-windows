#pragma once

#include "composition_state.h"
#include "input_session_types.h"
#include "scheme_type.h"
#include "sentence_association_options.h"
#include "../providers/provider_registry.h"
#include "../schemes/input_scheme.h"
#include "../schemes/wubi_scheme.h"
#include "../contracts/mid_sentence_helpcode.h"
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
    // 双拼末尾辅助码怎么触发：direct 开着时按输入形状认辅码（完整音节后多一个键，或末尾两键带大写）；
    // 关着时改由引导键（输入串里的 kEndMarker 段）或 uppercase_trigger 开着时的大写字母触发。
    void set_shuangpin_helpcode_triggers(bool direct, bool uppercase_trigger);
    bool end_helpcode_uppercase_trigger_active() const
    {
        return enable_shuangpin_helpcode_ && !shuangpin_helpcode_direct_ && end_helpcode_uppercase_trigger_ &&
               !enable_direct_helpcode_ && !mid_sentence_uppercase_trigger_active();
    }
    // 解析原串时哪种大写字母开段，TSF、Server 与引擎按同一组规则数这一节的键。
    FanyImeMidSentenceHelpcode::UppercaseTriggers helpcode_uppercase_triggers() const
    {
        return {mid_sentence_uppercase_trigger_active(), end_helpcode_uppercase_trigger_active()};
    }
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
    // 直接辅助码下用引导键进入末尾辅助码（宿主把 ; 和它前面的输入改写成「纯双拼 + kEndMarker」，见
    // FanyImeDirectHelpcode::AcceptsEndTriggerAt）。开着时输入串里有末尾段就不再交给直接辅助码的解析器，
    // 末尾段的辅码按引导键式末尾辅助码筛候选，与末尾辅助码自己的开关无关。
    void set_direct_helpcode_end_trigger_enabled(bool enabled);
    // typed（光标在末尾）能不能这样进入末尾辅助码：按直接辅助码的切分，每个音节都是完整的两键双拼、
    // 带 0~2 个辅码时，返回去掉辅码后的纯双拼（手动分隔符 ' 原样保留），否则返回空。开关关着、不是双拼、
    // 或 typed 不满足形状规则时同样返回空。只做一次独立查询，不动当前组合。
    std::optional<std::string> direct_helpcode_end_trigger_prefix(const std::string &typed);
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
    // literal_input: see WubiScheme::set_literal_input.
    void replace_wubi_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases,
                                bool literal_input = false);
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
    // 只判断还能不能展开首字母候选，不查库也不改候选；候选窗据此决定满页末页的下一页箭头。
    bool can_expand_initial_candidates() const;
    bool expand_initial_candidates();
    // 容错仲裁选中读音后的分段显示（大小写原样）；空表示当前串没有容错布局。
    const std::string &fault_segmentation_with_cases() const
    {
        return provider_registry_.fault_segmentation_with_cases();
    }

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
    // 直接辅助码与末尾引导都开着、请求的原串里有末尾段：这时按引导键式末尾辅助码查，不再按直接辅助码解析。
    bool direct_helpcode_end_block_active(const QueryRequest &request) const;
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
    bool shuangpin_helpcode_direct_ = true;
    bool end_helpcode_uppercase_trigger_ = false;
    bool enable_mid_sentence_helpcode_ = false;
    bool enable_direct_helpcode_ = false;
    bool direct_helpcode_slash_marker_ = true;
    bool direct_helpcode_uppercase_marker_ = false;
    bool enable_direct_helpcode_end_trigger_ = false;
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
