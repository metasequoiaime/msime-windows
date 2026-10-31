#include "ime_session.h"
#include "../schemes/quanpin_scheme.h"
#include "../schemes/shuangpin_scheme.h"
#include "../schemes/wubi_scheme.h"
#include "../schemes/japanese_romaji_scheme.h"
#include "../quanpin/quanpin_utils.h"
#include "../shuangpin/shuangpin_query.h"
#include "../contracts/direct_helpcode.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_set>

namespace
{
void ApplyShuangpinHelpcodeSegmentation(QueryRequest &request, const ShuangpinProfile &profile)
{
    if (request.scheme != SchemeType::Shuangpin || !request.enable_shuangpin_helpcode ||
        shuangpin::detect_active_double_helpcode_length(request.raw_input, request.raw_input_with_cases, profile) != 2)
    {
        return;
    }

    const size_t helpcode_length = 2;
    // The detector locates the help codes in delimiter-stripped space, so the split has to be made there too: slicing
    // the last raw bytes would push a pinyin letter into the base and a manual delimiter into the help codes.
    const std::string base_raw_input =
        shuangpin::trim_trailing_letters_preserve_delimiters(request.raw_input, helpcode_length);
    const std::string base_raw_input_with_cases =
        shuangpin::trim_trailing_letters_preserve_delimiters(request.raw_input_with_cases, helpcode_length);
    const std::string base_segmentation = shuangpin::segment_input(base_raw_input, profile);
    const std::string effective_input_with_cases = shuangpin::remove_manual_delimiters(request.raw_input_with_cases);
    const std::string help_codes =
        effective_input_with_cases.substr(effective_input_with_cases.size() - helpcode_length);

    request.raw_segmentation =
        shuangpin::apply_segmentation_cases(base_segmentation, base_raw_input_with_cases) + "'" + help_codes;
    request.normalized_segmentation = shuangpin::to_quanpin_segmentation(base_segmentation, profile) + "'" + help_codes;
    request.segmentation = request.normalized_segmentation;
}
} // namespace

ImeSession::ImeSession(SchemeType scheme_type, const ShuangpinProfile &shuangpin_profile,
                       metasequoia::RuntimePaths paths)
    : provider_registry_(shuangpin_profile, std::move(paths)), shuangpin_profile_(shuangpin_profile),
      scheme_(create_scheme(scheme_type))
{
    bind_wubi_scheme();
    bind_shuangpin_scheme();
    bind_quanpin_scheme();
}

void ImeSession::configure_shuangpin_scheme(IInputScheme *scheme) const
{
    if (auto *shuangpin_scheme = dynamic_cast<ShuangpinScheme *>(scheme))
    {
        shuangpin_scheme->set_direct_helpcode(enable_direct_helpcode_);
        shuangpin_scheme->set_mid_sentence_uppercase_trigger(mid_sentence_uppercase_trigger_active());
        shuangpin_scheme->set_end_helpcode_uppercase_trigger(end_helpcode_uppercase_trigger_active());
    }
}

void ImeSession::configure_quanpin_scheme(IInputScheme *scheme) const
{
    // build_request 在 apply_request_options 之前跑，纠错掩码必须先落到方案上，
    // 否则切分会用方案自己的默认值（关）而不是会话的开关状态。
    if (auto *quanpin_scheme = dynamic_cast<QuanpinScheme *>(scheme))
    {
        quanpin_scheme->set_autocorrect_types(quanpin_autocorrect_types_);
    }
}

void ImeSession::bind_shuangpin_scheme()
{
    configure_shuangpin_scheme(scheme_.get());
}

void ImeSession::bind_quanpin_scheme()
{
    configure_quanpin_scheme(scheme_.get());
}

void ImeSession::set_direct_helpcode_enabled(bool enabled)
{
    enable_direct_helpcode_ = enabled;
    bind_shuangpin_scheme();
}

void ImeSession::set_direct_helpcode_markers(bool slash, bool uppercase)
{
    direct_helpcode_slash_marker_ = slash;
    direct_helpcode_uppercase_marker_ = uppercase;
}

void ImeSession::set_mid_sentence_helpcode_enabled(bool enabled)
{
    enable_mid_sentence_helpcode_ = enabled;
    bind_shuangpin_scheme();
}

void ImeSession::set_mid_sentence_uppercase_trigger_enabled(bool enabled)
{
    enable_mid_sentence_uppercase_trigger_ = enabled;
    bind_shuangpin_scheme();
}

void ImeSession::set_direct_helpcode_end_trigger_enabled(bool enabled)
{
    enable_direct_helpcode_end_trigger_ = enabled;
}

bool ImeSession::direct_helpcode_end_block_active(const QueryRequest &request) const
{
    return enable_direct_helpcode_ && enable_direct_helpcode_end_trigger_ && request.scheme == SchemeType::Shuangpin &&
           request.raw_input_with_syllable_helpcodes.find(FanyImeMidSentenceHelpcode::kEndMarker) != std::string::npos;
}

void ImeSession::resolve_direct_helpcode(QueryRequest &request)
{
    if (enable_direct_helpcode_ && request.valid && request.scheme == SchemeType::Shuangpin &&
        !direct_helpcode_end_block_active(request))
    {
        provider_registry_.resolve_direct_helpcode(request);
    }
}

std::optional<std::string> ImeSession::direct_helpcode_end_trigger_prefix(const std::string &typed)
{
    if (!enable_direct_helpcode_ || !enable_direct_helpcode_end_trigger_ || scheme_->type() != SchemeType::Shuangpin ||
        !FanyImeDirectHelpcode::AcceptsEndTriggerAt(typed.data(), typed.size(), typed.size()))
    {
        return std::nullopt;
    }
    // 和 query_raw_candidates 一样用临时方案建请求：解析器按原串缓存切分，这次查询与当前组合看到的是同一个切分。
    const std::unique_ptr<IInputScheme> query_scheme = create_scheme(SchemeType::Shuangpin);
    configure_shuangpin_scheme(query_scheme.get());
    std::string lowered = typed;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    query_scheme->set_raw_input(lowered, typed);
    QueryRequest request = query_scheme->build_request();
    apply_request_options(request);
    if (!request.valid)
    {
        return std::nullopt;
    }
    if (!provider_registry_.resolve_direct_helpcode(request))
    {
        // 解析器没改写：整串本来就是不带辅码的普通双拼，或者按辅码切不通。前者每一节都得是完整音节。
        if (!shuangpin::is_complete_input(lowered, shuangpin_profile_))
        {
            return std::nullopt;
        }
        return typed;
    }
    // 改写后的 raw_input_with_cases 是去掉辅码与 / 的双拼码，解析器在带辅码的音节、单键声母后面插了 '。
    // 单键声母说明那个音节不完整，这一节就不是一个个完整音节。
    const std::string &clean = request.raw_input_with_cases;
    if (!shuangpin::is_complete_input(clean, shuangpin_profile_) ||
        request.syllable_helpcode_source_index.size() != clean.size() + 1)
    {
        return std::nullopt;
    }
    // 解析器为隔开辅码插的 ' 不要；用户自己敲的 ' 留着（映射回原串正是那个 '）。
    std::string prefix;
    prefix.reserve(clean.size());
    for (std::size_t index = 0; index < clean.size(); ++index)
    {
        if (clean[index] == '\'')
        {
            const std::size_t source = request.syllable_helpcode_source_index[index];
            if (source < typed.size() && typed[source] == '\'' && !prefix.empty() && prefix.back() != '\'')
            {
                prefix.push_back('\'');
            }
            continue;
        }
        prefix.push_back(clean[index]);
    }
    return prefix;
}

void ImeSession::bind_wubi_scheme()
{
    wubi_scheme_ = dynamic_cast<WubiScheme *>(scheme_.get());
    if (wubi_scheme_ != nullptr)
    {
        wubi_scheme_->set_mixed_pinyin_allowed(wubi_options_.mixed_pinyin);
        wubi_scheme_->set_z_wildcard(wubi_options_.z_wildcard);
    }
}

void ImeSession::set_wubi_input_options(metasequoia::WubiInputOptions options)
{
    wubi_options_ = options;
    if (wubi_scheme_ != nullptr)
    {
        wubi_scheme_->set_mixed_pinyin_allowed(wubi_options_.mixed_pinyin);
        wubi_scheme_->set_z_wildcard(wubi_options_.z_wildcard);
    }
}

void ImeSession::handle_key(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch)
{
    scheme_->handle_key(vk, modifiers_down, wch);
    // 查询并更新候选词列表
    refresh_candidates();
}

void ImeSession::switch_scheme(SchemeType scheme_type)
{
    scheme_ = create_scheme(scheme_type);
    bind_wubi_scheme();
    bind_shuangpin_scheme();
    bind_quanpin_scheme();
    state_ = CompositionState{};
}

void ImeSession::set_shuangpin_helpcode_enabled(bool enabled)
{
    enable_shuangpin_helpcode_ = enabled;
    bind_shuangpin_scheme();
}

void ImeSession::set_shuangpin_helpcode_triggers(bool direct, bool uppercase_trigger)
{
    shuangpin_helpcode_direct_ = direct;
    end_helpcode_uppercase_trigger_ = uppercase_trigger;
    bind_shuangpin_scheme();
}

void ImeSession::set_quanpin_helpcode_enabled(bool enabled)
{
    enable_quanpin_helpcode_ = enabled;
}

void ImeSession::set_quanpin_autocorrect_types(unsigned autocorrect_types)
{
    quanpin_autocorrect_types_ = autocorrect_types;
    bind_quanpin_scheme();
}

void ImeSession::replace_shuangpin_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    if (scheme_->type() != SchemeType::Shuangpin)
    {
        return;
    }

    auto *shuangpin_scheme = dynamic_cast<ShuangpinScheme *>(scheme_.get());
    if (!shuangpin_scheme)
    {
        return;
    }

    shuangpin_scheme->set_raw_input(raw_input, raw_input_with_cases);
    refresh_candidates();
}

void ImeSession::replace_quanpin_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    if (scheme_->type() != SchemeType::Quanpin)
    {
        return;
    }

    auto *quanpin_scheme = dynamic_cast<QuanpinScheme *>(scheme_.get());
    if (!quanpin_scheme)
    {
        return;
    }

    quanpin_scheme->set_raw_input(raw_input, raw_input_with_cases);
    refresh_candidates();
}

void ImeSession::replace_wubi_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases,
                                        bool literal_input)
{
    if (scheme_->type() != SchemeType::Wubi)
    {
        return;
    }

    if (wubi_scheme_ == nullptr)
    {
        return;
    }

    // A replacement is host-driven editing of text the composition already holds, so the length it
    // is clipped to comes from the setting rather than from whatever the previous query happened to
    // answer. Without this, moving the caret through a mixed composition silently drops everything
    // past the fourth letter. refresh_candidates puts the query-derived value back.
    wubi_scheme_->set_extended_length_allowed(wubi_options_.mixed_pinyin);
    wubi_scheme_->set_literal_input(literal_input);
    wubi_scheme_->set_raw_input(raw_input, raw_input_with_cases);
    refresh_candidates();
}

void ImeSession::reset()
{
    scheme_->reset();
    state_ = CompositionState{};
}

void ImeSession::reset_cache()
{
    provider_registry_.reset_cache(current_scheme_type());
    // 混输组合同时查过拼音 provider，清缓存必须两边都清，否则选中调频后的重排会读到旧序。
    if (wubi_scheme_ != nullptr && wubi_options_.mixed_pinyin)
    {
        provider_registry_.reset_cache(SchemeType::Quanpin);
    }
    refresh_candidates();
}

void ImeSession::reset_sentence_cache()
{
    provider_registry_.reset_sentence_cache();
}

int ImeSession::create_word(std::string pinyin, std::string word)
{
    return provider_registry_.create_word(current_scheme_type(), std::move(pinyin), std::move(word));
}

int ImeSession::update_weight_by_pinyin_and_word(SchemeType scheme, std::string pinyin, std::string word)
{
    return provider_registry_.update_weight_by_pinyin_and_word(scheme, std::move(pinyin), std::move(word));
}

int ImeSession::delete_by_pinyin_and_word(SchemeType scheme, std::string pinyin, std::string word)
{
    return provider_registry_.delete_by_pinyin_and_word(scheme, std::move(pinyin), std::move(word));
}

int ImeSession::cache_dynamic_candidate(const std::string &pinyin, const std::string &word, CandidateSource source)
{
    return provider_registry_.cache_dynamic_candidate(current_scheme_type(), pinyin, word, source);
}

std::vector<WordItem> ImeSession::query_raw_candidates(const std::string &raw_input,
                                                       const std::string &raw_input_with_cases)
{
    // A throwaway scheme instance: set_raw_input on the live scheme would clobber its key
    // strokes, and a prefix query must leave the composition exactly as it found it.
    const std::unique_ptr<IInputScheme> query_scheme = create_scheme(scheme_->type());
    // 临时方案要按同样的触发规则解析原串（大写段），否则前缀查询与正式查询切得不一样。
    configure_shuangpin_scheme(query_scheme.get());
    configure_quanpin_scheme(query_scheme.get());
    query_scheme->set_raw_input(raw_input, raw_input_with_cases);
    QueryRequest request = query_scheme->build_request();
    apply_request_options(request);
    ApplyShuangpinHelpcodeSegmentation(request, shuangpin_profile_);
    resolve_direct_helpcode(request);
    if (!request.valid)
    {
        return {};
    }
    return provider_registry_.resolve(request.scheme).query(request);
}

std::vector<WordItem> ImeSession::query_without_syllable_helpcodes()
{
    if (!state_.request.valid || state_.request.syllable_helpcodes.empty() ||
        !state_.request.enable_mid_sentence_helpcode)
    {
        return state_.candidates;
    }
    // 反引号段在请求里已换成分隔符，切分与不敲辅助码时一致；关掉开关，词典层就不再按约束筛。
    QueryRequest request = state_.request;
    request.enable_mid_sentence_helpcode = false;
    return provider_registry_.resolve(request.scheme).query(request);
}

void ImeSession::replace_japanese_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    if (scheme_->type() != SchemeType::JapaneseRomaji)
        return;
    auto *japanese_scheme = dynamic_cast<JapaneseRomajiScheme *>(scheme_.get());
    if (!japanese_scheme)
        return;
    japanese_scheme->set_raw_input(raw_input, raw_input_with_cases);
    refresh_candidates();
}

void ImeSession::replace_active_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    switch (scheme_->type())
    {
    case SchemeType::Quanpin:
        replace_quanpin_raw_input(raw_input, raw_input_with_cases);
        return;
    case SchemeType::Shuangpin:
        replace_shuangpin_raw_input(raw_input, raw_input_with_cases);
        return;
    case SchemeType::Wubi:
        replace_wubi_raw_input(raw_input, raw_input_with_cases);
        return;
    case SchemeType::JapaneseRomaji:
        replace_japanese_raw_input(raw_input, raw_input_with_cases);
        return;
    }
}

int ImeSession::cache_dynamic_candidate_for_current_request(const std::string &word, CandidateSource source)
{
    return provider_registry_.cache_dynamic_candidate_for_request(state_.request, word, source);
}

int ImeSession::apply_dynamic_candidate(const std::string &word, CandidateSource source)
{
    const int result = cache_dynamic_candidate_for_current_request(word, source);
    if (result == 0)
    {
        refresh_candidates();
    }
    return result;
}

SchemeType ImeSession::current_scheme_type() const
{
    return scheme_->type();
}

const std::string &ImeSession::get_preedit() const
{
    return state_.preedit;
}

const QueryRequest &ImeSession::get_request() const
{
    return state_.request;
}

const std::vector<WordItem> &ImeSession::get_candidates() const
{
    return state_.candidates;
}

std::optional<WordItem> ImeSession::find_candidate(SchemeType scheme, const std::string &key, const std::string &value)
{
    return provider_registry_.find_candidate(scheme, key, value);
}

bool ImeSession::can_expand_initial_candidates() const
{
    return provider_registry_.can_expand_initial_candidates(state_.request, state_.candidates);
}

bool ImeSession::expand_initial_candidates()
{
    return provider_registry_.expand_initial_candidates(state_.request, state_.candidates);
}

void ImeSession::apply_request_options(QueryRequest &request) const
{
    // 直接辅助码下由引导键进入的末尾段：前面已经整理成纯双拼，末尾段的辅码按引导键式末尾辅助码筛，
    // 与末尾辅助码自己的开关无关。
    const bool direct_end_block = direct_helpcode_end_block_active(request);
    // 末尾辅助码直接触发时按输入形状认辅码；改成引导键触发后，辅码只来自方案层解析出的末尾段。
    request.enable_shuangpin_helpcode = enable_shuangpin_helpcode_ && shuangpin_helpcode_direct_;
    if ((!enable_shuangpin_helpcode_ || shuangpin_helpcode_direct_) && !direct_end_block)
    {
        request.end_helpcodes.clear();
    }
    request.enable_mid_sentence_helpcode = enable_mid_sentence_helpcode_;
    if (direct_end_block)
    {
        // 不按输入形状认辅码，也不交给直接辅助码的解析器与它的词格缓存（resolve_direct_helpcode 同样跳过）。
        request.enable_shuangpin_helpcode = false;
        request.enable_mid_sentence_helpcode = false;
    }
    else if (enable_direct_helpcode_ && request.scheme == SchemeType::Shuangpin)
    {
        // 直接辅助码接管辅码：末尾单码/双码那套判断会把辅码字母当成别的东西，关掉；约束由解析器
        // 改写请求时再打开。
        request.enable_shuangpin_helpcode = false;
        request.end_helpcodes.clear();
        request.enable_mid_sentence_helpcode = false;
        request.enable_direct_helpcode = true;
        request.direct_helpcode_slash_marker = direct_helpcode_slash_marker_;
        request.direct_helpcode_uppercase_marker = direct_helpcode_uppercase_marker_;
    }
    request.enable_quanpin_helpcode = enable_quanpin_helpcode_;
    request.enable_quanpin_autocorrect_transposition =
        (quanpin_autocorrect_types_ & quanpin::kAutocorrectTransposition) != 0;
    request.enable_quanpin_autocorrect_neighbor = (quanpin_autocorrect_types_ & quanpin::kAutocorrectNeighbor) != 0;
    request.enable_quanpin_autocorrect_deletion = (quanpin_autocorrect_types_ & quanpin::kAutocorrectDeletion) != 0;
    request.enable_quanpin_autocorrect_insertion = (quanpin_autocorrect_types_ & quanpin::kAutocorrectInsertion) != 0;
    request.fuzzy_pinyin = fuzzy_pinyin_;
    request.sentence_association = sentence_association_;
    request.rescoring_context = rescoring_context_;
}

void ImeSession::refresh_candidates()
{
    state_.preedit = scheme_->get_preedit();
    state_.request = scheme_->build_request();
    apply_request_options(state_.request);
    ApplyShuangpinHelpcodeSegmentation(state_.request, shuangpin_profile_);
    resolve_direct_helpcode(state_.request);

    // A special mode's literal input on a wubi session gets its candidates from the host (dates,
    // V-mode numbers); a table or pinyin query on the letters it happens to contain would only be
    // thrown away, on every key of the mode.
    if (!state_.request.valid || (wubi_scheme_ != nullptr && wubi_scheme_->literal_input()))
    {
        state_.candidates.clear();
        return;
    }

    state_.candidates = provider_registry_.resolve(state_.request.scheme).query(state_.request);
    if (wubi_scheme_ == nullptr)
    {
        return;
    }

    // The wubi provider also returns longer codes that start with the input (per-key hints), and a
    // one-to-three letter prefix almost always has some. Only a row for exactly this code means the
    // table answered it; hints alone must not keep mixed input from offering pinyin.
    const bool wubi_table_answered =
        std::any_of(state_.candidates.begin(), state_.candidates.end(),
                    [this](const WordItem &item) { return item.pinyin == state_.request.normalized_input; });

    // Once the table has failed the code in hand, mixed input lets the composition grow past four
    // letters so a full spelling can be finished. A code the table answers keeps the four-letter
    // limit, so a fluent wubi typist is not interrupted; the fifth letter is handled by the host's
    // top-commit path instead.
    wubi_scheme_->set_extended_length_allowed(wubi_options_.mixed_pinyin && !wubi_table_answered);

    if (!wubi_options_.mixed_pinyin)
    {
        return;
    }

    // 五笔拼音混输：同一串字母同时交给五笔码表和全拼，五笔候选在前、拼音候选按权重追加在后，
    // 两个表都答得出的词只留五笔那份。这是业界惯例的「候选词优先五笔，兼容拼音」，而不是
    // 只在码表失手时才转拼音的兜底。
    QuanpinScheme pinyin;
    configure_quanpin_scheme(&pinyin);
    pinyin.set_raw_input(state_.request.raw_input, state_.request.raw_input_with_cases);
    QueryRequest pinyin_request = pinyin.build_request();
    apply_request_options(pinyin_request);
    // The same physical keys produced these letters, so the strokes carry over rather than
    // reaching the provider empty.
    pinyin_request.key_strokes = state_.request.key_strokes;
    if (!pinyin_request.valid)
    {
        return;
    }

    std::vector<WordItem> pinyin_candidates = provider_registry_.resolve(pinyin_request.scheme).query(pinyin_request);
    if (pinyin_candidates.empty())
    {
        return;
    }

    // 通配与混输同时开启时，含 z 的编码既可能是通配猜码也可能是拼音拼写（zi、zai、zhong）。
    // 通配查询按权重返回整页无关码行，放在前面会把拼音的正解挤到后几页，所以这种组合里拼音
    // 在前、通配结果在后；同词只留拼音那份。
    std::vector<WordItem> leading = std::move(state_.candidates);
    std::vector<WordItem> trailing = std::move(pinyin_candidates);
    if (state_.request.wubi_z_wildcard)
    {
        std::swap(leading, trailing);
    }

    std::unordered_set<std::string> seen_words;
    seen_words.reserve(leading.size() + trailing.size());
    for (const WordItem &item : leading)
    {
        seen_words.insert(item.word);
    }
    for (WordItem &item : trailing)
    {
        if (seen_words.insert(item.word).second)
        {
            leading.push_back(std::move(item));
        }
    }
    state_.candidates = std::move(leading);
}

std::unique_ptr<IInputScheme> ImeSession::create_scheme(SchemeType scheme_type) const
{
    switch (scheme_type)
    {
    case SchemeType::Shuangpin:
        return std::make_unique<ShuangpinScheme>(shuangpin_profile_);
    case SchemeType::Quanpin:
        return std::make_unique<QuanpinScheme>();
    case SchemeType::Wubi:
        return std::make_unique<WubiScheme>();
    case SchemeType::JapaneseRomaji:
        return std::make_unique<JapaneseRomajiScheme>();
    default:
        throw std::runtime_error("Unknown scheme type.");
    }
}
