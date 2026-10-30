#include "engine_input_session.h"
#include "config/ime_config.h"
#include "engine/common/helpcode_utils.h"
#include "engine/contracts/assets/assets.h"
#include "engine/core/data_path.h"
#include "engine/core/sentence_association_options.h"
#include "engine/local_modes/radical_query.h"
#include "engine/quanpin/quanpin_utils.h"
#include "session/input_cadence.h"
#include "settings/lattice_model.h"
#include "settings/neural_model.h"

#include <filesystem>

// 模型包的确定性布局：<DataDir>/models/<id>/<id>.gram。下载器与手动放置都
// 遵守它，解析因此只是一次 stat，不需要目录扫描，也不需要 model.toml。声明在
// 头文件供测试钉住「激活值留空 = 未选择，解析为空」的语义。
std::string ResolveCollocationModelPath(const std::string &model_id)
{
    // 激活值留空 = 未选择任何模型：解析返回空串，调用方按整句加成全关处理，与模型缺席
    // 同一条降级路径。不回退到推荐包——把默认做进运行时，只会让推荐包变成删不掉的常住包。
    if (model_id.empty())
    {
        return {};
    }
    // 与下载器、状态查询、删除守卫共用同一条基准（data_directory() 每次读环境变量）。
    // 会话其余资源仍走 legacy() 快照；模型路径不能跟着走——legacy() 在进程启动时捕获，
    // 生产环境两者相同，但注入环境变量的测试里会分叉。
    const std::filesystem::path file = metasequoia::data_directory() / "models" / model_id / (model_id + ".gram");
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error) || error)
    {
        return {};
    }
    return file.u8string();
}

std::string ResolveNeuralDesktopModelPath()
{
    const std::filesystem::path file = neural_model::ModelFile();
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error) || error)
    {
        return {};
    }
    return metasequoia::path_to_utf8(file);
}

std::string ResolveLatticeLanguageModelPath()
{
    const std::filesystem::path file = model_download::ResolveInstalledFile(lattice_model::ModelFile());
    return file.empty() ? std::string() : metasequoia::path_to_utf8(file);
}

EngineInputSession::EngineInputSession(SchemeType scheme, const ShuangpinProfile &profile)
    : paths_(metasequoia::RuntimePaths::legacy()), session_(scheme, profile, paths_)
{
    ApplyConfiguration();
}

void EngineInputSession::ApplyConfiguration()
{
    const auto scheme = session_.scheme();
    if (scheme == SchemeType::Quanpin || scheme == SchemeType::Shuangpin)
    {
        const auto &schema = scheme == SchemeType::Quanpin ? GetConfiguredQuanpinHelpcodeSchema()
                                                           : GetConfiguredShuangpinHelpcodeSchema();
        if (schema != helpcode_schema_)
        {
            // Keep filtering and annotations on this session's captured resource layout.
            // Applying unchanged settings on each key must not reload the tables.
            auto keymap = HelpcodeUtils::load_helpcode_keymap(paths_.resources, schema);
            if (session_.set_helpcode_schema(schema))
            {
                helpcode_schema_ = schema;
                helpcode_keymap_ = std::move(keymap);
            }
        }
    }
    session_.set_shuangpin_helpcode_enabled(GetConfiguredShuangpinHelpcodeEnabled());
    // 大写触发给生效值：句中辅助码占着大写时末尾辅助码不收，与发给 TSF 的 EndHelpcodeTriggersChanged 一致。
    session_.set_shuangpin_helpcode_triggers(GetConfiguredShuangpinHelpcodeDirect(),
                                             IsConfiguredEndHelpcodeUppercaseTrigger());
    session_.set_mid_sentence_helpcode_enabled(GetConfiguredShuangpinMidSentenceHelpcodeEnabled());
    session_.set_direct_helpcode_enabled(GetConfiguredShuangpinDirectHelpcodeEnabled());
    session_.set_direct_helpcode_markers(GetConfiguredShuangpinDirectHelpcodeSlash(),
                                         GetConfiguredShuangpinDirectHelpcodeUppercase());
    session_.set_mid_sentence_uppercase_trigger_enabled(GetConfiguredShuangpinMidSentenceHelpcodeUppercase());
    session_.set_quanpin_helpcode_enabled(GetConfiguredQuanpinHelpcodeEnabled());
    // One config key per correction type, no linkage: the server-side session carries the
    // same four-bit mask the engine and the preedit rebuild derive from the same request
    // bools (product-reachable masks are every combination of the four bits). Rule:
    // quanpin_utils.h.
    const unsigned autocorrect_types = quanpin::autocorrect_types_from_switches(
        GetConfiguredQuanpinAutocorrectTransposition(), GetConfiguredQuanpinAutocorrectNeighbor(),
        GetConfiguredQuanpinAutocorrectDeletion(), GetConfiguredQuanpinAutocorrectInsertion());
    session_.set_quanpin_autocorrect_types(autocorrect_types);
    // Fuzzy pinyin applies to both quanpin and shuangpin; the engine fuzzes on the
    // converted quanpin syllables for shuangpin. Re-read on every key so setting
    // changes take effect immediately.
    session_.set_fuzzy_pinyin_options(GetConfiguredFuzzyPinyinOptions());
    // 整句候选来源与去重补位选项，每次击键重读，改设置立即生效。
    SentenceAssociationOptions association;
    association.word_lattice = GetConfiguredAssocSentenceWordLattice();
    association.google = GetConfiguredAssocSentenceGoogle();
    // 连续打字期间不跑效果档：只把模型路径收成空串——没有路径就建不出神经重排器，词格退回
    // 1-best，12-best 解码和下面那次路径 stat 都省掉。开关本身保持配置值：any_sentence_source()
    // 会牵动词格 LM 与切分歧义仲裁，跟着按键节奏翻转会把逐前缀词库缓存一起拖下水（见
    // SentenceAssociationOptions::differs_only_in_effect_tier_model）。停手到点后 IPC 线程
    // 放回路径并排一次重查。速度档和其余整句来源一律不受影响。
    association.neural_desktop = GetConfiguredAssocSentenceNeuralDesktop();
    // 高精度档模型按需下载：效果档生效时才去盘上找（每次击键都会走到这里），没下载时路径为空。
    association.neural_desktop_model = EffectiveNeuralDesktop(association.neural_desktop, TypingInProgress())
                                           ? ResolveNeuralDesktopModelPath()
                                           : std::string();
    association.neural_keyboard = GetConfiguredAssocSentenceNeuralKeyboard();
    association.context_candidate_rerank = GetConfiguredAssocContextCandidateRerank();
    association.show_next_on_duplicate = GetConfiguredAssocSentenceShowNextOnDuplicate();
    // octagram 语法模型总开关：开关关时连模型路径都不解析。引擎侧的加成分由 collocation_model
    // 非空隐含开启，没有独立开关，所以路径空即全部能力关闭；模型缺席时同样置空重排，
    // 免得词典层拿空路径做无谓解析。
    const bool collocation_enabled = GetConfiguredAssocSentenceCollocationEnabled();
    association.collocation_model =
        collocation_enabled ? ResolveCollocationModelPath(GetConfiguredAssocSentenceCollocationModel()) : std::string();
    association.collocation_weight = GetConfiguredAssocSentenceCollocationWeight();
    association.collocation_rerank = collocation_enabled && !association.collocation_model.empty();
    // octagram 六参数（万象现行默认），随配置下发；这些键不进设置页，只由配置文件调。
    association.collocation_grammar.collocation_max_length = GetConfiguredAssocSentenceCollocationMaxLength();
    association.collocation_grammar.collocation_min_length = GetConfiguredAssocSentenceCollocationMinLength();
    association.collocation_grammar.collocation_penalty = GetConfiguredAssocSentenceCollocationPenalty();
    association.collocation_grammar.non_collocation_penalty = GetConfiguredAssocSentenceCollocationNonPenalty();
    association.collocation_grammar.weak_collocation_penalty = GetConfiguredAssocSentenceCollocationWeakPenalty();
    association.collocation_grammar.rear_penalty = GetConfiguredAssocSentenceCollocationRearPenalty();
    // 词格语言模型按需下载：任一整句来源开着时词格才会解码，那时才去盘上找（要在上面的来源都定下来
    // 之后判断）。Trigram 整句和设置页一致，模型不在时不出：设置页那边开关置灰显示为关。开发与测试
    // 环境把 sc.lm 放在资源目录里，那份同样算在。
    if (association.any_sentence_source())
    {
        association.language_model = ResolveLatticeLanguageModelPath();
        if (association.word_lattice && association.language_model.empty())
        {
            std::error_code error;
            association.word_lattice =
                std::filesystem::is_regular_file(paths_.resource(metasequoia::assets::language_model), error);
        }
    }
    session_.set_sentence_association(association);
    session_.set_shuangpin_preedit_uses_raw(GetConfiguredShuangpinPreeditMode() == "shuangpin");
    // 五笔拼音混输与 z 键角色是两个独立设置：混输是「同时给五笔和拼音候选」，z 键角色只管
    // z 是普通字母还是通配。每次击键重读，设置页改动对下一键立即生效。
    metasequoia::WubiInputOptions wubi_options;
    wubi_options.mixed_pinyin = GetConfiguredWubiMixedPinyin();
    wubi_options.z_wildcard = GetConfiguredWubiZMode() == "wildcard";
    session_.set_wubi_input_options(wubi_options);
}

void EngineInputSession::handle_key(UINT vk, UINT modifiers_down, WCHAR wch)
{
    ApplyConfiguration();
    return session_.handle_engine_key(vk, modifiers_down, wch);
}

void EngineInputSession::recompute_candidates()
{
    ApplyConfiguration();
    return session_.recompute_candidates();
}

void EngineInputSession::refresh_configuration()
{
    ApplyConfiguration();
}

SchemeType EngineInputSession::current_scheme_type() const
{
    return session_.current_scheme_type();
}

void EngineInputSession::switch_scheme(SchemeType scheme_type)
{
    session_.switch_scheme(scheme_type);
    ApplyConfiguration();
}

void EngineInputSession::reset_state()
{
    return session_.reset_state();
}

void EngineInputSession::reset_cache()
{
    return session_.reset_cache();
}

void EngineInputSession::reset_sentence_cache()
{
    return session_.reset_sentence_cache();
}

const std::vector<IInputSession::WordItem> &EngineInputSession::get_candidates() const
{
    return session_.get_candidates();
}

bool EngineInputSession::can_expand_initial_candidates() const
{
    return session_.can_expand_initial_candidates();
}

bool EngineInputSession::expand_initial_candidates()
{
    return session_.expand_initial_candidates();
}

std::optional<WordItem> EngineInputSession::find_candidate(const std::string &key, const std::string &value)
{
    return session_.find_candidate(key, value);
}

const std::string &EngineInputSession::get_pinyin_sequence() const
{
    return session_.get_pinyin_sequence();
}

const std::string &EngineInputSession::get_pinyin_sequence_with_cases() const
{
    return session_.get_pinyin_sequence_with_cases();
}

const std::string &EngineInputSession::get_pure_pinyin_sequence() const
{
    return session_.get_pure_pinyin_sequence();
}

const std::string &EngineInputSession::get_pinyin_segmentation() const
{
    return session_.get_pinyin_segmentation();
}

std::string EngineInputSession::get_pinyin_segmentation_with_cases() const
{
    return session_.get_pinyin_segmentation_with_cases();
}

IInputSession::ShuangpinPreeditForms EngineInputSession::get_shuangpin_preedit_forms() const
{
    return session_.get_shuangpin_preedit_forms();
}

std::vector<std::size_t> EngineInputSession::segment_raw_boundaries() const
{
    return session_.segment_raw_boundaries();
}

std::string EngineInputSession::get_quanpin() const
{
    return session_.get_quanpin();
}

bool EngineInputSession::is_all_complete_pure_pinyin() const
{
    return session_.is_all_complete_pure_pinyin();
}

bool EngineInputSession::reads_as_pinyin() const
{
    return session_.reads_as_pinyin();
}

bool EngineInputSession::wubi_unique_four_code() const
{
    return session_.wubi_unique_four_code();
}

bool EngineInputSession::wubi_four_code_is_complete() const
{
    return session_.wubi_four_code_is_complete();
}

bool EngineInputSession::has_active_helpcode() const
{
    return session_.has_active_helpcode();
}

bool EngineInputSession::accepts_mid_sentence_helpcode_marker(std::size_t caret) const
{
    // 开关在 ApplyConfiguration 里随每键重读；这里在吃键之前被问到，那一刻配置可能刚改过，
    // 所以直接按当前配置判断，不等下一次重读。
    return GetConfiguredShuangpinMidSentenceHelpcodeEnabled() && !GetConfiguredShuangpinDirectHelpcodeEnabled() &&
           session_.accepts_mid_sentence_helpcode_marker_at(caret);
}

bool EngineInputSession::accepts_end_helpcode_marker(std::size_t caret) const
{
    // 同上，按当前配置判断。
    return GetConfiguredShuangpinHelpcodeEnabled() && !GetConfiguredShuangpinHelpcodeDirect() &&
           !GetConfiguredShuangpinDirectHelpcodeEnabled() && session_.accepts_end_helpcode_marker_at(caret);
}

bool EngineInputSession::accepts_direct_helpcode_slash(std::size_t caret) const
{
    // 同上，按当前配置判断。
    return GetConfiguredShuangpinDirectHelpcodeEnabled() && GetConfiguredShuangpinDirectHelpcodeSlash() &&
           session_.accepts_direct_helpcode_slash_at(caret);
}

bool EngineInputSession::has_mid_sentence_helpcode() const
{
    return session_.has_mid_sentence_helpcode();
}

std::vector<IInputSession::WordItem> EngineInputSession::candidates_without_mid_sentence_helpcode()
{
    return session_.candidates_without_mid_sentence_helpcode();
}

void EngineInputSession::set_rescoring_context(std::string context)
{
    return session_.set_rescoring_context(std::move(context));
}

void EngineInputSession::set_pinyin_sequence(const std::string &pinyin_sequence)
{
    return session_.set_pinyin_sequence(pinyin_sequence);
}

void EngineInputSession::set_pinyin_sequence_with_cases(const std::string &pinyin_sequence)
{
    return session_.set_pinyin_sequence_with_cases(pinyin_sequence);
}

void EngineInputSession::set_wubi_literal_input(bool literal)
{
    return session_.set_wubi_literal_input(literal);
}

void EngineInputSession::set_caret(std::optional<std::size_t> caret)
{
    return session_.set_caret(caret);
}

std::size_t EngineInputSession::prefix_end() const
{
    return session_.prefix_end();
}

std::string EngineInputSession::pending_suffix() const
{
    return session_.pending_suffix();
}

int EngineInputSession::store_user_phrase(std::string pinyin, std::string word)
{
    return session_.store_user_phrase(pinyin, word);
}

int EngineInputSession::store_user_phrase_from_canonical_pinyin(std::string pinyin, std::string word)
{
    return session_.store_user_phrase_from_canonical_pinyin(pinyin, word);
}

int EngineInputSession::remove_candidate(std::string pinyin, std::string word, SchemeType scheme)
{
    return session_.remove_candidate(pinyin, word, scheme);
}

int EngineInputSession::cache_dynamic_candidate(const std::string &pinyin, const std::string &word,
                                                CandidateSource source)
{
    return session_.cache_dynamic_candidate(pinyin, word, source);
}

IInputSession::SelectionTransition EngineInputSession::advance_composition_after_selection(
    const std::string &selected_pinyin, const std::string &selected_word, const std::string &selected_canonical_pinyin,
    SchemeType selected_scheme)
{
    return session_.advance_composition_after_selection(selected_pinyin, selected_word, selected_canonical_pinyin,
                                                        selected_scheme);
}

IInputSession::CloudQueryState EngineInputSession::get_cloud_query_state() const
{
    return session_.get_cloud_query_state();
}

std::optional<metasequoia::OnlineQuery> EngineInputSession::online_query() const
{
    return session_.online_query();
}

bool EngineInputSession::apply_online_candidate(const metasequoia::OnlineQuery &query, std::string candidate,
                                                CandidateSource source)
{
    return session_.apply_online_candidate(query, std::move(candidate), source);
}

IInputSession::CreatingWordProgress EngineInputSession::update_creating_word_progress(
    const std::string &current_pinyin, const std::string &current_word, const std::string &selected_word,
    const SelectionTransition &selection_transition) const
{
    return session_.update_creating_word_progress(current_pinyin, current_word, selected_word, selection_transition);
}

std::vector<IInputSession::WordItem> EngineInputSession::query_radical(const std::string &code) const
{
    return metasequoia::local_modes::query_radical(paths_.resource(metasequoia::assets::radical_decomposition), code);
}

std::string EngineInputSession::get_helpcode_annotation(const std::string &word, bool uppercase_all) const
{
    const auto scheme = session_.scheme();
    if (!helpcode_keymap_ || (scheme != SchemeType::Quanpin && scheme != SchemeType::Shuangpin))
        return {};
    return HelpcodeUtils::compute_helpcodes(word, uppercase_all, helpcode_keymap_.get());
}
