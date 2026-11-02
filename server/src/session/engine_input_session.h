#pragma once

#include "input_session.h"
#include "engine/common/helpcode_utils.h"
#include "engine/core/input_session.h"

// 模型包的确定性布局解析：<DataDir>/models/<id>/<id>.gram。配置 id 留空 = 未选择任何
// 模型，解析返回空串；文件缺席同样返回空串，调用方按整句加成全关处理。定义在
// engine_input_session.cpp，声明在此供测试钉住激活解析语义。
std::string ResolveCollocationModelPath(const std::string &model_id);

// 神经整句高精度档模型的路径：<DataDir>/models/neural-desktop/ 下已落位的文件，未下载时返回
// 空串，引擎据此静默不重排。声明在此供测试钉住解析语义。
std::string ResolveNeuralDesktopModelPath();

// 词格语言模型 sc.lm 的路径：<DataDir>/models/language-model/ 下已落位的文件，未下载或已删除待重启
// 时返回空串，引擎据此回到资源目录那份（产品安装里没有，退回启发式打分）。声明在此供测试钉住解析语义。
std::string ResolveLatticeLanguageModelPath();

class EngineInputSession : public IInputSession
{
  public:
    explicit EngineInputSession(SchemeType scheme_type = SchemeType::Shuangpin,
                                const ShuangpinProfile &shuangpin_profile = GetXiaoheShuangpinProfile());

    void handle_key(UINT vk, UINT modifiers_down, WCHAR wch) override;
    void recompute_candidates() override;
    void refresh_configuration() override;
    SchemeType current_scheme_type() const override;
    void switch_scheme(SchemeType scheme_type) override;

    void reset_state() override;
    void reset_cache() override;
    void reset_sentence_cache() override;

    const std::vector<WordItem> &get_candidates() const override;
    bool can_expand_initial_candidates() const override;
    bool expand_initial_candidates() override;
    std::string get_helpcode_annotation(const std::string &word, bool uppercase_all) const override;
    std::optional<WordItem> find_candidate(const std::string &key, const std::string &value) override;
    std::vector<WordItem> query_radical(const std::string &code) const override;

    const std::string &get_pinyin_sequence() const override;
    const std::string &get_pinyin_sequence_with_cases() const override;
    const std::string &get_pure_pinyin_sequence() const override;
    const std::string &get_pinyin_segmentation() const override;
    std::string get_pinyin_segmentation_with_cases() const override;
    ShuangpinPreeditForms get_shuangpin_preedit_forms() const override;
    std::vector<std::size_t> segment_raw_boundaries() const override;
    std::string get_quanpin() const override;
    bool is_all_complete_pure_pinyin() const override;
    bool reads_as_pinyin() const override;
    bool wubi_unique_four_code() const override;
    bool wubi_four_code_is_complete() const override;
    bool has_active_helpcode() const override;
    bool accepts_mid_sentence_helpcode_marker(std::size_t caret) const override;
    bool accepts_end_helpcode_marker(std::size_t caret) const override;
    bool accepts_direct_helpcode_slash(std::size_t caret) const override;
    std::optional<std::string> direct_helpcode_end_trigger_prefix(const std::string &typed) override;
    bool has_mid_sentence_helpcode() const override;
    std::vector<WordItem> candidates_without_mid_sentence_helpcode() override;

    void set_rescoring_context(std::string context) override;
    void set_pinyin_sequence(const std::string &pinyin_sequence) override;
    void set_pinyin_sequence_with_cases(const std::string &pinyin_sequence) override;
    void set_wubi_literal_input(bool literal) override;

    void set_caret(std::optional<std::size_t> caret) override;
    std::size_t prefix_end() const override;
    std::string pending_suffix() const override;

    int store_user_phrase(std::string pinyin, std::string word) override;
    int store_user_phrase_from_canonical_pinyin(std::string pinyin, std::string word) override;
    int remove_candidate(std::string pinyin, std::string word, SchemeType scheme) override;
    int cache_dynamic_candidate(const std::string &pinyin, const std::string &word, CandidateSource source) override;
    SelectionTransition advance_composition_after_selection(const std::string &selected_pinyin,
                                                            const std::string &selected_word,
                                                            const std::string &selected_canonical_pinyin,
                                                            SchemeType selected_scheme = SchemeType::Quanpin) override;
    CloudQueryState get_cloud_query_state() const override;
    std::optional<metasequoia::OnlineQuery> online_query() const override;
    bool apply_online_candidate(const metasequoia::OnlineQuery &query, std::string candidate,
                                CandidateSource source) override;
    CreatingWordProgress update_creating_word_progress(const std::string &current_pinyin,
                                                       const std::string &current_word,
                                                       const std::string &selected_word,
                                                       const SelectionTransition &selection_transition) const override;

  private:
    void ApplyConfiguration();
    const metasequoia::RuntimePaths paths_;
    metasequoia::InputSession session_;
    std::string helpcode_schema_;
    HelpcodeUtils::SharedKeymap helpcode_keymap_;
};
