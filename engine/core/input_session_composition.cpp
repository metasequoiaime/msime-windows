#include "input_session.h"
#include "../common/helpcode_utils.h"
#include "../contracts/mid_sentence_helpcode.h"
#include "../quanpin/quanpin_query.h"
#include "../quanpin/quanpin_utils.h"
#include "../shuangpin/shuangpin_query.h"
#include "../shuangpin/shuangpin_utils.h"
#include "../japanese/romaji_converter.h"
#include <algorithm>
#include <cctype>

namespace metasequoia
{
namespace
{
std::string remove_delimiters(const std::string &segmented)
{
    std::string normalized;
    normalized.reserve(segmented.size());
    for (const char ch : segmented)
    {
        if (ch != '\'')
        {
            normalized.push_back(ch);
        }
    }
    return normalized;
}

void remove_consumed_leading_separators(std::string &raw_input, std::string &raw_input_with_cases)
{
    size_t count = 0;
    while (count < raw_input.size() && raw_input[count] == '\'')
    {
        ++count;
    }
    raw_input.erase(0, count);

    count = 0;
    while (count < raw_input_with_cases.size() && raw_input_with_cases[count] == '\'')
    {
        ++count;
    }
    raw_input_with_cases.erase(0, count);
}

// 整句落库的长度上限。词库里的短语本身就不超过这个音节数（对标
// quanpin::WordLatticeOptions::max_phrase_syllables），再长的整句只是这一次输入
// 的产物，落库除了撑大用户词库没有别的作用。
constexpr size_t kMaxLearnedSentenceSyllables = 7;

std::string normalize_canonical_pinyin_for_word(const std::string &pinyin, const std::string &word)
{
    if (pinyin.empty())
    {
        return {};
    }

    const auto segments = quanpin::split_segments(pinyin);
    if (segments.empty() || segments.size() != HelpcodeUtils::count_han_chars(word))
    {
        return {};
    }
    for (const auto &segment : segments)
    {
        if (segment.empty() || !quanpin::is_complete_pinyin_input(segment))
        {
            return {};
        }
    }
    return quanpin::join_segments(segments);
}

std::string append_canonical_pinyin(const std::string &prefix, const std::string &suffix)
{
    if (prefix.empty())
    {
        return suffix;
    }
    if (suffix.empty())
    {
        return {};
    }
    return prefix + "'" + suffix;
}

// 句中辅助码：选词推进是在去掉反引号段的串上算的，剩下的部分要换回原串，后面音节上的约束才
// 留得住。[clean_begin, clean_end) 是剩余部分在去段串里的范围；开头的分隔符连同挂在已上屏音节上
// 的段一起算进已消耗部分，跟普通推进丢掉开头分隔符的做法一致。
struct MidSentenceRest
{
    std::string consumed;
    std::string rest;
};

MidSentenceRest RemapMidSentenceRest(const QueryRequest &request, const std::string &typed, std::size_t clean_begin,
                                     std::size_t clean_end, const ShuangpinProfile &profile)
{
    // 方案层或直接辅助码的解析器已经把映射填进请求（见 QueryRequest::has_syllable_helpcode_layout），
    // 直接用它；原串的段规则取决于当时的触发设置，这里不再反推。
    shuangpin::MidSentenceHelpcodeInput parsed;
    if (request.has_syllable_helpcode_layout)
    {
        parsed.input = request.raw_input_with_cases;
        parsed.source_index = request.syllable_helpcode_source_index;
    }
    else
    {
        parsed = shuangpin::parse_mid_sentence_helpcodes(typed, profile);
    }
    const std::string &clean = parsed.input;
    clean_end = (std::min)(clean_end, clean.size());
    while (clean_begin < clean_end && clean[clean_begin] == '\'')
    {
        ++clean_begin;
    }
    MidSentenceRest result;
    if (clean_begin >= clean_end)
    {
        result.consumed = typed;
        return result;
    }
    const std::size_t full_begin = parsed.source_index[clean_begin];
    const std::size_t full_end = clean_end >= clean.size() ? typed.size() : parsed.source_index[clean_end];
    result.consumed = typed.substr(0, full_begin);
    result.rest = typed.substr(full_begin, full_end - full_begin);
    return result;
}

std::string LowercaseLetters(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

struct ShuangpinCompositionBase
{
    std::string raw_input;
    std::string raw_input_with_cases;
    std::string effective_raw_input;
    std::string effective_raw_input_with_cases;
    size_t helpcode_length = 0;
};

ShuangpinCompositionBase ResolveShuangpinCompositionBase(const QueryRequest &request, const ShuangpinProfile &profile)
{
    ShuangpinCompositionBase base{
        request.raw_input, request.raw_input_with_cases.empty() ? request.raw_input : request.raw_input_with_cases};
    base.effective_raw_input = shuangpin::remove_manual_delimiters(base.raw_input);
    base.effective_raw_input_with_cases = shuangpin::remove_manual_delimiters(base.raw_input_with_cases);

    if (!request.enable_shuangpin_helpcode || base.effective_raw_input.empty())
    {
        return base;
    }

    const auto has_complete_unseparated_base = [&](size_t helpcode_length) {
        if (base.effective_raw_input.size() <= helpcode_length)
        {
            return false;
        }
        const size_t pure_length = base.effective_raw_input.size() - helpcode_length;
        const size_t raw_prefix_length = shuangpin::raw_length_for_effective_prefix(base.raw_input, pure_length);
        // An apostrophe immediately before the suffix makes that suffix a
        // user-defined pinyin segment, not an auxiliary code.
        if (raw_prefix_length < base.raw_input.size() && base.raw_input[raw_prefix_length] == '\'')
        {
            return false;
        }
        return shuangpin::is_complete_input(base.raw_input.substr(0, raw_prefix_length), profile);
    };

    if (shuangpin::detect_active_double_helpcode_length(base.raw_input, base.raw_input_with_cases, profile) == 2)
    {
        base.helpcode_length = 2;
        return base;
    }

    if (base.effective_raw_input.size() % 2 == 1 && base.effective_raw_input.size() > 1)
    {
        if (has_complete_unseparated_base(1))
        {
            base.helpcode_length = 1;
        }
    }

    return base;
}

bool HasActiveQuanpinHelpcode(const QueryRequest &request)
{
    return request.enable_quanpin_helpcode &&
           quanpin::detect_active_helpcode_length(request.raw_input, request.raw_input_with_cases) > 0;
}

std::string ResolveShuangpinCloudCacheKey(const QueryRequest &request, const ShuangpinProfile &profile)
{
    const auto base = ResolveShuangpinCompositionBase(request, profile);
    if (base.helpcode_length > 0 && base.effective_raw_input.size() >= base.helpcode_length)
    {
        const size_t base_length = base.effective_raw_input.size() - base.helpcode_length;
        return base.raw_input.substr(0, shuangpin::raw_length_for_effective_prefix(base.raw_input, base_length));
    }
    return base.raw_input;
}

std::string ResolveQuanpinCloudCacheKey(const QueryRequest &request)
{
    return quanpin::strip_active_helpcodes(request.raw_input, request.raw_input_with_cases);
}

// Adapter over the request fields: the switch -> mask rule itself lives in
// quanpin::autocorrect_types_from_switches (quanpin_utils.h), shared with
// engine.cpp and the server session so the preedit rebuild always sees the same
// correction space as the query.
unsigned QuanpinAutocorrectTypes(const QueryRequest &request)
{
    return quanpin::autocorrect_types_from_switches(
        request.enable_quanpin_autocorrect_transposition, request.enable_quanpin_autocorrect_neighbor,
        request.enable_quanpin_autocorrect_deletion, request.enable_quanpin_autocorrect_insertion);
}

std::string QuanpinLettersWithoutDelimiters(const std::string &text)
{
    std::string letters;
    letters.reserve(text.size());
    for (const char ch : text)
    {
        if (ch != '\'')
        {
            letters.push_back(ch);
        }
    }
    return letters;
}

// Folds letters for autocorrect display comparisons: lowercases and strips
// manual delimiters, and maps the u-umlaut style 'v' spelling onto 'u'. This
// equivalence is what classifies length-preserving spelling aliases (jv->ju,
// nue->nve) as "explainable by the cut" so the preedit can be rebuilt with
// separators; removing it would drop separators for alias-typed input. It only
// ever compares two internally derived strings, never gates a user-facing
// mark — the dictionary-side corrected_from contract lives in
// quanpin_dictionary.cpp and treats v/u as distinct letters on purpose.
std::string FoldQuanpinAutocorrectLetters(const std::string &text)
{
    std::string folded;
    folded.reserve(text.size());
    for (const char ch : text)
    {
        if (ch == '\'')
        {
            continue;
        }
        const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        folded.push_back(lower == 'v' ? 'u' : lower);
    }
    return folded;
}

// Folded letters of a cut's syllable sequence, for comparing the BFS reading
// against the scheme segmentation reading.
std::string CutSyllableLetters(const quanpin::AutocorrectCut &cut)
{
    std::string letters;
    for (const auto &segment : cut.segments)
    {
        letters += segment.syllable;
    }
    return FoldQuanpinAutocorrectLetters(letters);
}

// Rebuilds the preedit from the cased input letters with a separator after
// each listed letter count (ascending; the final segment end is excluded).
// Letters (case included) are preserved verbatim; manual delimiters are
// replaced by the given boundaries.
std::string RebuildQuanpinDisplayAtBoundaries(const std::string &cased_input, const std::vector<size_t> &boundaries)
{
    std::string display;
    display.reserve(cased_input.size() + boundaries.size());
    size_t letter_index = 0;
    size_t boundary_index = 0;
    for (const char ch : cased_input)
    {
        if (ch == '\'')
        {
            continue;
        }
        display.push_back(ch);
        ++letter_index;
        if (boundary_index < boundaries.size() && letter_index == boundaries[boundary_index])
        {
            display.push_back('\'');
            ++boundary_index;
        }
    }
    return display;
}

// Separators at the raw spans the correction BFS reports.
std::string RebuildQuanpinDisplayFromCut(const std::string &cased_input, const quanpin::AutocorrectCut &cut)
{
    std::vector<size_t> boundaries;
    for (size_t i = 0; i + 1 < cut.segments.size(); ++i)
    {
        boundaries.push_back(cut.segments[i].start + cut.segments[i].raw_text.size());
    }
    return RebuildQuanpinDisplayAtBoundaries(cased_input, boundaries);
}

// Separators for a spelling that passed the jianpin guard, read the way the
// guard reads it: complete syllables plus at most one trailing letter
// ("dongan" -> dong'an, "haoyongg" -> hao'yong'g). Only the typed letters are
// cut, so the result is independent of any alias reading that won the query.
// Returns an empty string when no such reading exists.
std::string RebuildQuanpinDisplayFromLegalSpelling(const std::string &cased_input, const std::string &raw_input)
{
    auto segments = quanpin::cut_one_piece_min_segments(raw_input, true);
    if (segments.empty() && raw_input.size() > 1)
    {
        segments = quanpin::cut_one_piece_min_segments(raw_input.substr(0, raw_input.size() - 1), true);
        if (!segments.empty())
        {
            segments.push_back(raw_input.substr(raw_input.size() - 1));
        }
    }
    if (segments.empty())
    {
        return {};
    }

    std::vector<size_t> boundaries;
    size_t end = 0;
    for (size_t i = 0; i + 1 < segments.size(); ++i)
    {
        end += segments[i].size();
        boundaries.push_back(end);
    }
    return RebuildQuanpinDisplayAtBoundaries(cased_input, boundaries);
}

// The preedit must always show the letters the user actually typed (PRD R5).
// Two layers can rewrite them into canonical pinyin: the scheme alias table
// (sahng -> shang, baked into raw_segmentation) and the dictionary correction
// BFS (shabg -> shang, which only re-separates). Both are rebuilt here from
// the raw letters with separators at the actual cut positions; when the BFS
// cannot explain a rewrite (length-changing aliases such as mihng -> ming) the
// input is shown as typed, with no scheme separators. With the correction
// switches off the scheme alias table no longer rewrites letters, so `base` is
// already the literal cut and this rebuild is a no-op fast path; the rebuild
// matters while correction is on, and AC1 only constrains the candidate list.
std::string BuildQuanpinAutocorrectDisplay(const QueryRequest &request)
{
    const std::string &cased = request.raw_input_with_cases.empty() ? request.raw_input : request.raw_input_with_cases;
    const std::string base = request.raw_segmentation.empty() ? cased : request.raw_segmentation;
    if (request.raw_input.empty() || cased.empty())
    {
        return base;
    }

    const unsigned types = QuanpinAutocorrectTypes(request);
    const std::string folded_input = FoldQuanpinAutocorrectLetters(cased);
    const bool letters_rewritten = FoldQuanpinAutocorrectLetters(QuanpinLettersWithoutDelimiters(base)) != folded_input;

    // Fast path: the scheme kept the typed letters and either no correction
    // type is enabled or the input is already a complete pinyin spelling, so
    // no correction interpretation exists to draw separators from.
    if (!letters_rewritten && (types == 0 || quanpin::is_complete_pinyin_input(request.raw_input)))
    {
        return base;
    }

    // Jianpin guard, mirroring resolve_series_query in the dictionary layer: a
    // leading initial plus complete syllables, or legal syllables plus at most
    // one trailing initial, expresses user intent rather than a typo
    // Corrections like "ywen" -> wen and "zheg" -> zheng would otherwise erase
    // the original syllable boundaries from the preedit
    if (quanpin::looks_like_jianpin_spelling(request.raw_input))
    {
        // The typed spelling is legal, but the scheme alias table may still
        // have re-segmented it into a reading with copied letters (dongan ->
        // dong'gan, haoyonga -> hao'yong'ga). The dictionary ranks those
        // readings against the exact one by frequency, yet the preedit must
        // show the typed letters (PRD R5), so a rewritten base may never
        // reach it verbatim. Separate the typed letters by their own legal
        // reading instead, so one more letter does not drop every separator
        // (hao'yong -> hao'yong'a, not haoyonga).
        if (!letters_rewritten)
        {
            return base;
        }
        const std::string display = RebuildQuanpinDisplayFromLegalSpelling(cased, request.raw_input);
        return display.empty() ? cased : display;
    }

    const auto cut = quanpin::autocorrect_cut_detail(folded_input, types);
    // When the scheme rewrote the letters, the query resolved through the alias
    // layer, so the preedit may only draw separators from the BFS when both
    // layers explain the letters identically ("sahnghao" -> shang'hao).
    // Otherwise the deletion bit would re-separate "sahng" as sa'hng while the
    // candidates actually come from the alias reading shang -- the old code
    // never noticed because the cut happened to be empty without it.
    if (!cut.empty() && (!letters_rewritten || CutSyllableLetters(cut) == FoldQuanpinAutocorrectLetters(
                                                                              QuanpinLettersWithoutDelimiters(base))))
    {
        return RebuildQuanpinDisplayFromCut(cased, cut);
    }
    // The BFS cannot explain the input (e.g. a length-changing alias such as
    // mihng -> ming): fall back to the input exactly as typed when the letters
    // were rewritten, otherwise keep the scheme segmentation untouched. Manual
    // delimiters are part of what was typed, so they stay ("mihng'").
    return letters_rewritten ? cased : base;
}

// 首选是同一串字母的另一种字面切分时（yibananzhao 由 yi'ban'an'zhao 的「一般按照」领衔，
// fangan 由 fang'an 的「方案」领衔），预编辑跟着首选分，而不是停在方案层的最少音节切分上，
// 否则用户看着 yi'ba'nan'zhao 选「一般按照」。只认读音字母与敲的字母逐个相同、盖住整串、
// 不带纠错标记的首选（辅助码、手动分隔符都不满足），而且音节数与方案层切分相同：那正是
// 只差一处 n/ng 边界的等长歧义；音节数不同的读法（nue 的字面整词「怒鹅」nu'e）不在此列，
// 预编辑照旧。否则返回空串。
std::string BuildQuanpinDisplayFromLiteralHead(const QueryRequest &request, const WordItem &head)
{
    const std::string &cased = request.raw_input_with_cases.empty() ? request.raw_input : request.raw_input_with_cases;
    if (!head.corrected_from.empty() || request.raw_input.find('\'') != std::string::npos ||
        cased.size() != request.raw_input.size())
    {
        return {};
    }
    const auto syllables = quanpin::split_segments(head.pinyin);
    const std::string &scheme_segmentation =
        request.normalized_segmentation.empty() ? request.segmentation : request.normalized_segmentation;
    if (syllables.size() < 2 || syllables.size() != quanpin::split_segments(scheme_segmentation).size())
    {
        return {};
    }
    std::vector<size_t> boundaries;
    size_t end = 0;
    for (const auto &syllable : syllables)
    {
        if (request.raw_input.compare(end, syllable.size(), syllable) != 0)
        {
            return {};
        }
        end += syllable.size();
        boundaries.push_back(end);
    }
    if (end != request.raw_input.size())
    {
        return {};
    }
    boundaries.pop_back();
    return RebuildQuanpinDisplayAtBoundaries(cased, boundaries);
}

// 合法输入上的纠错领衔时（ziazheliya 出「在这里呀」、jiuzheeyang 出「就这样」），预编辑
// 按领衔读法分隔成 zia'zhe'li'ya、jiu'zhee'yang，而不是字面切分 zi'a'zhe'li'ya——后者
// 和首选对不上。边界取纠错切分记录的原始区间（多字、漏字纠错和原始字母不等长）；
// 罕见音节替换（lia -> lai）不在 k-best 里，它是换位、字母数不变，按读法音节长度分。
// 切不成合法音节的输入同理：上下文把贵档读法提到首位时（shiideya 出「是的呀」），
// 预编辑要跟着它分成 shii'de'ya，而不是 k-best 首条的 shi'ide'ya。
// 只在首选带纠错标记、读法恰好盖住整串时生效（辅助码、手动分隔符都不满足），否则
// 返回空串。
std::string BuildQuanpinDisplayFromLeadingCorrection(const QueryRequest &request, const WordItem &head)
{
    const std::string &cased = request.raw_input_with_cases.empty() ? request.raw_input : request.raw_input_with_cases;
    if (head.corrected_from.empty() || request.raw_input.find('\'') != std::string::npos ||
        cased.size() != request.raw_input.size())
    {
        return {};
    }
    const auto syllables = quanpin::split_segments(head.pinyin);
    if (syllables.empty())
    {
        return {};
    }
    std::vector<size_t> boundaries;
    size_t end = 0;
    const auto cut =
        quanpin::correction_cut_for_reading(request.raw_input, syllables, QuanpinAutocorrectTypes(request));
    if (cut.has_value() && cut->segments.size() == syllables.size())
    {
        for (const auto &segment : cut->segments)
        {
            end = segment.start + segment.raw_text.size();
            boundaries.push_back(end);
        }
    }
    else
    {
        for (const auto &syllable : syllables)
        {
            end += syllable.size();
            boundaries.push_back(end);
        }
    }
    if (end != request.raw_input.size())
    {
        return {};
    }
    boundaries.pop_back();
    return RebuildQuanpinDisplayAtBoundaries(cased, boundaries);
}
} // namespace

void InputSession::handle_engine_key(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch)
{
    engine_.handle_key(vk, modifiers_down, wch);
    online_requests_.invalidate();
    update_mixed_candidates();
}

void InputSession::recompute_candidates()
{
    if (has_pending_pinyin_sequence_ || has_pending_pinyin_sequence_with_cases_)
    {
        apply_pending_sequence();
        return;
    }
    engine_.handle_key(0, 0, 0);
    update_mixed_candidates();
}

SchemeType InputSession::current_scheme_type() const
{
    return engine_.current_scheme_type();
}

void InputSession::reset_state()
{
    clear_pending_sequence();
    reset_composition();
}

void InputSession::reset_cache()
{
    engine_.reset_cache();
    if (canonical_phrase_engine_)
        canonical_phrase_engine_->reset_cache();
    // 前缀候选是按文本缓存的，不跟着引擎缓存失效：只清缓存键，让下一次
    // refresh_prefix_candidates 按新权重/选项重查；保留当前列表，避免 caret
    // 激活期间出现空候选窗。
    prefix_query_input_.clear();
}

void InputSession::reset_sentence_cache()
{
    engine_.reset_sentence_cache();
    if (canonical_phrase_engine_)
        canonical_phrase_engine_->reset_sentence_cache();
    // 前缀候选同样可能带整句，理由同 reset_cache。
    prefix_query_input_.clear();
}

const std::vector<WordItem> &InputSession::get_candidates() const
{
    return candidates();
}

bool InputSession::can_expand_initial_candidates() const
{
    return engine_.can_expand_initial_candidates();
}

bool InputSession::expand_initial_candidates()
{
    return engine_.expand_initial_candidates();
}

std::optional<WordItem> InputSession::find_candidate(const std::string &key, const std::string &value)
{
    return engine_.find_candidate(scheme(), key, value);
}

const QueryRequest &InputSession::request() const
{
    return engine_.get_request();
}

const std::string &InputSession::get_pinyin_sequence() const
{
    return request().raw_input;
}

const std::string &InputSession::get_pinyin_sequence_with_cases() const
{
    // 句中辅助码的反引号段只在原串里，宿主回读的必须是用户敲的原样。
    if (!request().raw_input_with_syllable_helpcodes.empty())
    {
        return request().raw_input_with_syllable_helpcodes;
    }
    return request().raw_input_with_cases.empty() ? request().raw_input : request().raw_input_with_cases;
}

const std::string &InputSession::get_pure_pinyin_sequence() const
{
    return request().normalized_input;
}

const std::string &InputSession::get_pinyin_segmentation() const
{
    return request().normalized_segmentation.empty() ? request().segmentation : request().normalized_segmentation;
}

std::string InputSession::get_pinyin_segmentation_with_cases() const
{
    return build_pinyin_segmentation_with_cases(shuangpin_preedit_uses_raw_);
}

InputSession::ShuangpinPreeditForms InputSession::get_shuangpin_preedit_forms() const
{
    if (!is_shuangpin())
    {
        return {};
    }
    return {build_pinyin_segmentation_with_cases(true), build_pinyin_segmentation_with_cases(false)};
}

std::string InputSession::build_pinyin_segmentation_with_cases(bool shuangpin_raw) const
{
    if (is_wubi())
    {
        return request().raw_input;
    }
    if (is_japanese())
    {
        return request().raw_input_with_cases.empty() ? request().raw_input : request().raw_input_with_cases;
    }
    // 句中辅助码的反引号段在切分串里只是一个 '，显示时按原样接回对应音节后面；
    // 末尾补 ' 也要看用户敲的原串，否则 ulpb`x 会显示成 ul'pb`x'。
    const std::string &typed = get_pinyin_sequence_with_cases();
    // 段文本由方案层或直接辅助码的解析器随请求给出（见 QueryRequest::has_syllable_helpcode_layout）。
    const auto decorate = [&](const std::string &segmentation) {
        return request().has_syllable_helpcode_layout
                   ? shuangpin::decorate_segmentation(segmentation, request().syllable_helpcode_decorations)
                   : shuangpin::decorate_mid_sentence_segmentation(segmentation, typed, shuangpin_profile_);
    };
    if (is_shuangpin() && shuangpin_raw)
    {
        std::string preedit = request().raw_segmentation.empty() ? request().raw_input : request().raw_segmentation;
        preedit = decorate(preedit);
        if (!typed.empty() && typed.back() == '\'' && (preedit.empty() || preedit.back() != '\''))
        {
            preedit.push_back('\'');
        }
        return preedit;
    }
    if (current_scheme_type() == SchemeType::Quanpin)
    {
        // 容错仲裁选中的读音（句中简拼展开、槽内漏字变体）与敲的字母不逐音节相同，
        // 下面几条按首选重建显示的路径都会比对失败、退回字面切分；那时预编辑会和
        // 首选候选对不上（layout-and-unit-boundaries：切分即编辑单元）。仲裁已经把
        // 分段显示按原串大小写算好，直接用。全拼方案下 shuangpin_raw 也会是 true
        // （宿主默认取原串形态），所以这里只能按方案判断，不能用它当门。
        if (!is_shuangpin())
        {
            const std::string &fault_display = engine_.fault_segmentation_with_cases();
            if (!fault_display.empty())
            {
                return decorate(fault_display);
            }
        }
        if (!candidates().empty())
        {
            const auto &head = candidates().front();
            if (quanpin::literal_mixed_pinyin_match(request().raw_input, head.pinyin, head.canonical_pinyin))
            {
                std::vector<size_t> boundaries;
                size_t consumed = 0;
                for (const auto &segment : quanpin::split_segments(head.pinyin))
                {
                    consumed += segment.size();
                    boundaries.push_back(consumed);
                }
                if (consumed == request().raw_input.size())
                    boundaries.pop_back();
                return RebuildQuanpinDisplayAtBoundaries(typed, boundaries);
            }
        }
        if (!candidates().empty())
        {
            std::string display = BuildQuanpinDisplayFromLiteralHead(request(), candidates().front());
            if (display.empty())
            {
                display = BuildQuanpinDisplayFromLeadingCorrection(request(), candidates().front());
            }
            if (!display.empty())
            {
                return display;
            }
        }
        return BuildQuanpinAutocorrectDisplay(request());
    }
    std::string preedit =
        request().normalized_segmentation.empty() ? request().segmentation : request().normalized_segmentation;
    if (is_shuangpin())
    {
        preedit = decorate(preedit);
    }
    if (!typed.empty() && typed.back() == '\'' && (preedit.empty() || preedit.back() != '\''))
    {
        preedit.push_back('\'');
    }
    return preedit;
}

std::string InputSession::get_quanpin() const
{
    return request().normalized_input;
}

bool InputSession::is_all_complete_pure_pinyin() const
{
    if (is_wubi())
    {
        return request().valid;
    }
    if (is_japanese())
    {
        return japanese::ConvertRomaji(request().raw_input).complete;
    }
    if (is_shuangpin())
    {
        const auto base = ResolveShuangpinCompositionBase(request(), shuangpin_profile_);
        if (base.helpcode_length > 0 && base.effective_raw_input.size() >= base.helpcode_length)
        {
            const size_t base_length = base.effective_raw_input.size() - base.helpcode_length;
            return shuangpin::is_complete_input(
                base.raw_input.substr(0, shuangpin::raw_length_for_effective_prefix(base.raw_input, base_length)),
                shuangpin_profile_);
        }
        return shuangpin::is_complete_input(base.raw_input, shuangpin_profile_);
    }
    const auto &segmentation =
        request().normalized_segmentation.empty() ? request().segmentation : request().normalized_segmentation;
    return !segmentation.empty() && quanpin::is_complete_pinyin_input(segmentation);
}

bool InputSession::reads_as_pinyin() const
{
    if (has_active_helpcode())
        return true;
    std::string raw = remove_delimiters(request().raw_input);
    std::transform(raw.begin(), raw.end(), raw.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (raw.empty())
        return false;
    if (is_shuangpin())
    {
        // 双拼两键一个音节，打到一半时末尾会剩一个声母键。
        return raw.size() == 1 || shuangpin::is_complete_input(raw, shuangpin_profile_) ||
               shuangpin::is_complete_input(raw.substr(0, raw.size() - 1), shuangpin_profile_);
    }
    if (current_scheme_type() == SchemeType::Quanpin)
        return !quanpin::cut_one_piece_min_segments(raw, false).empty();
    return false;
}

bool InputSession::wubi_unique_four_code() const
{
    // Both modes spell words rather than codes; a wubi code is not composed inside them. No host
    // sets either one on a wubi session today, but the guard keeps the pair meaningful if one does.
    if (dedicated_english_mode_ || local_input_mode_ != LocalInputMode::None)
    {
        return false;
    }
    // A code holding z is a wildcard guess, not a code the typist committed to: the "only
    // candidate" behind it is one arbitrary letter away from several others, and committing it
    // silently takes away the one thing the key exists for -- seeing what matched. Same reason
    // mixed input never reaches here (no table row contains z, so its z codes stay empty).
    if (engine_.get_request().raw_input.find('z') != std::string::npos)
    {
        return false;
    }
    // is_wubi() covers "this is a wubi session", and engine_.wubi_code_is_complete() is false for
    // every other scheme.
    if (!is_wubi() || !engine_.wubi_code_is_complete())
    {
        return false;
    }
    // Only the code's own rows count: the pinyin candidates mixed input appends are not the answer
    // the auto-commit looks for. A code the table answered with exactly one row is genuinely unique.
    return wubi_native_candidate_count() == 1;
}

bool InputSession::wubi_four_code_is_complete() const
{
    // Same guards as wubi_unique_four_code minus the candidate count: hosts commit the first
    // candidate on the next key whether or not the code has one candidate or many. The pinyin
    // candidates mixed input appends do not make a code the table never answered complete.
    if (dedicated_english_mode_ || local_input_mode_ != LocalInputMode::None)
    {
        return false;
    }
    // Wildcard codes neither auto-commit nor top-commit: see wubi_unique_four_code.
    if (engine_.get_request().raw_input.find('z') != std::string::npos)
    {
        return false;
    }
    return is_wubi() && engine_.wubi_code_is_complete() && wubi_native_candidate_count() > 0;
}

bool InputSession::has_active_helpcode() const
{
    if (is_wubi() || is_japanese())
    {
        return false;
    }
    if (is_shuangpin())
    {
        return !request().end_helpcodes.empty() ||
               ResolveShuangpinCompositionBase(request(), shuangpin_profile_).helpcode_length > 0;
    }
    return HasActiveQuanpinHelpcode(request());
}

void InputSession::set_pinyin_sequence(const std::string &pinyin_sequence)
{
    pending_pinyin_sequence_ = pinyin_sequence;
    has_pending_pinyin_sequence_ = true;
}

void InputSession::set_pinyin_sequence_with_cases(const std::string &pinyin_sequence)
{
    pending_pinyin_sequence_with_cases_ = pinyin_sequence;
    has_pending_pinyin_sequence_with_cases_ = true;
}

void InputSession::set_wubi_literal_input(bool literal)
{
    pending_wubi_literal_input_ = literal;
}

int InputSession::store_user_phrase(std::string pinyin, std::string word)
{
    return engine_.create_word(std::move(pinyin), std::move(word));
}

int InputSession::store_user_phrase_from_canonical_pinyin(std::string pinyin, std::string word)
{
    // Both quanpin and shuangpin ultimately share the canonical quanpin
    // dictionary.  Do not feed a complete quanpin key back through the active
    // shuangpin profile a second time.
    if (!canonical_phrase_engine_)
        canonical_phrase_engine_ = std::make_unique<QuanpinEngine>(paths_);
    return canonical_phrase_engine_->create_word_from_canonical_pinyin(std::move(pinyin), std::move(word));
}

// 整句候选（词格 CandidateSource::Generated、Google 解码器 CandidateSource::Fallback、
// 神经整句 NeuralDesktop / NeuralKeyboard）
// 是猜出来的，词库里没有它那一行，所以调频对它无效：update_weight_by_pinyin_and_word
// 改的是 SQLite 里已存在的行，找不到行就什么也不做，用户于是发现自己选多少次都提不上来。
// 选中即落库才是对的处理：存成用户词组之后，下次同样的输入它以 UserDatabase 候选出现，
// 按 quanpin::generated_sentence_insert_position 的约定排在两条整句之前，之后再选还能
// 走正常调频。
std::optional<std::string> InputSession::learn_sentence_candidate(const WordItem &selected)
{
    // 本地模式（U/K/E/M/J/Y/R、日期时间）和英文模式也用 Generated 装自己的候选，
    // 那些不是拼音整句，不能往拼音用户词库里塞。五笔码表候选也不进这里：混输追加的整句
    // 候选自带 Quanpin 方案，按候选自己的方案判断，不看会话方案。
    if (local_input_mode_ != LocalInputMode::None || dedicated_english_mode_ || is_japanese() ||
        is_wubi_native_candidate(selected))
    {
        return std::nullopt;
    }

    // 读音必须完整且音节数与字数对得上，否则落库的是个读音残缺的词条。双拼的整句
    // 候选同样带 canonical quanpin，这里统一按全拼键处理。
    const std::string canonical = normalize_canonical_pinyin_for_word(selected.canonical_pinyin, selected.word);
    if (canonical.empty() || quanpin::split_segments(canonical).size() > kMaxLearnedSentenceSyllables)
    {
        return std::nullopt;
    }

    if (store_user_phrase_from_canonical_pinyin(canonical, selected.word) != 0)
    {
        return "Unable to persist the selected sentence.";
    }
    return std::nullopt;
}

int InputSession::remove_candidate(std::string pinyin, std::string word, SchemeType scheme)
{
    if (!is_wubi() && remove_delimiters(request().raw_input).size() == 1)
    {
        return -1;
    }
    return engine_.delete_by_pinyin_and_word(scheme, std::move(pinyin), std::move(word));
}

int InputSession::cache_dynamic_candidate(const std::string &pinyin, const std::string &word, CandidateSource source)
{
    const int cache_result = engine_.cache_dynamic_candidate(pinyin, word, source);
    (void)engine_.cache_dynamic_candidate_for_current_request(word, source);
    return cache_result;
}

InputSession::SelectionTransition InputSession::advance_composition_after_selection(
    const std::string &selected_pinyin, const std::string &selected_word, const std::string &selected_canonical_pinyin,
    SchemeType selected_scheme)
{
    SelectionTransition transition;
    transition.selected_canonical_pinyin = selected_canonical_pinyin;
    transition.wubi_native = selected_scheme == SchemeType::Wubi;
    if (is_japanese())
    {
        transition.full_pure_pinyin = request().raw_input;
        const std::string typed = request().raw_input_with_cases;
        const size_t consumed = selected_pinyin.size();
        std::string rest_raw_input;
        std::string rest_raw_input_with_cases;
        if (consumed > 0 && consumed < typed.size() && typed.compare(0, consumed, selected_pinyin) == 0)
        {
            rest_raw_input = request().raw_input.substr(consumed);
            rest_raw_input_with_cases = typed.substr(consumed);
            // ka'ta 选「か」剩下 'ta：开头的分隔符转不出假名，留着会让剩余输入一个候选都没有。
            remove_consumed_leading_separators(rest_raw_input, rest_raw_input_with_cases);
        }
        transition.continues_composition = !rest_raw_input_with_cases.empty();
        transition.consumed_raw_input_with_cases = typed.substr(0, typed.size() - rest_raw_input_with_cases.size());
        if (transition.continues_composition)
        {
            engine_.replace_japanese_raw_input(rest_raw_input, rest_raw_input_with_cases);
            online_requests_.invalidate();
            // 固定候选位打开时 candidates() 返回的是混排列表，不重建就还是选词前整串的那份。
            update_mixed_candidates();
            if (local_input_mode_ == LocalInputMode::TemporaryJapanese)
            {
                local_preedit_ = "R" + engine_.get_preedit();
                local_candidates_ = engine_.get_candidates();
            }
        }
        transition.current_segmentation = request().segmentation;
        transition.current_segmentation_with_cases = request().raw_input_with_cases;
        return transition;
    }
    if (transition.wubi_native)
    {
        transition.full_pure_pinyin = request().normalized_input;
        transition.current_segmentation = request().normalized_input;
        transition.current_segmentation_with_cases = request().raw_input;
        return transition;
    }
    if (is_shuangpin())
    {
        const auto base = ResolveShuangpinCompositionBase(request(), shuangpin_profile_);
        const size_t word_pinyin_length = HelpcodeUtils::count_han_chars(selected_word) * 2;
        const size_t total_input_length = base.effective_raw_input.size();

        transition.full_pure_pinyin =
            base.helpcode_length > 0 && total_input_length >= base.helpcode_length
                ? base.effective_raw_input.substr(0, total_input_length - base.helpcode_length)
                : base.effective_raw_input;

        size_t consumed_length = remove_delimiters(selected_pinyin).size();
        // 带句中辅助码时 base 是去掉反引号段的串，推进结果要映射回原串，见 RemapMidSentenceRest。
        const std::string typed = request().raw_input_with_syllable_helpcodes;
        const std::string original = typed.empty() ? base.raw_input_with_cases : typed;
        std::string terminal_helpcode;
        if (!prefix_candidates_active_)
        {
            if (!request().end_helpcodes.empty())
            {
                FanyImeMidSentenceHelpcode::ScanBlocks(
                    original.data(), original.size(), engine_.helpcode_uppercase_triggers(),
                    [&](size_t begin, size_t end, size_t, FanyImeMidSentenceHelpcode::BlockKind kind) {
                        if (kind == FanyImeMidSentenceHelpcode::BlockKind::End && end == original.size())
                        {
                            terminal_helpcode = original.substr(begin);
                        }
                    });
            }
            else if (base.helpcode_length > 0)
            {
                const size_t begin = shuangpin::raw_length_for_effective_prefix(
                    base.raw_input_with_cases, total_input_length - base.helpcode_length);
                terminal_helpcode = typed.empty()
                                        ? original.substr(begin)
                                        : RemapMidSentenceRest(request(), typed, begin,
                                                               base.raw_input_with_cases.size(), shuangpin_profile_)
                                              .rest;
            }
        }
        const auto consume_terminal_helpcode = [&](std::string &normalized_rest, std::string &cased_rest) {
            if (terminal_helpcode.empty())
            {
                return;
            }
            // Explicit terminal blocks survive the existing layout remapping;
            // implicit direct codes have already been excluded by rest_end.
            if (!request().end_helpcodes.empty())
            {
                if (cased_rest.size() < terminal_helpcode.size() ||
                    cased_rest.compare(cased_rest.size() - terminal_helpcode.size(), terminal_helpcode.size(),
                                       terminal_helpcode) != 0)
                {
                    return;
                }
                cased_rest.resize(cased_rest.size() - terminal_helpcode.size());
                normalized_rest = LowercaseLetters(cased_rest);
            }
            transition.consumed_terminal_helpcode_with_cases = terminal_helpcode;
            // Include separators removed with the prefix so the consumed prefix,
            // remaining suffix and terminal helpcode reconstruct the exact raw.
            transition.consumed_raw_input_with_cases =
                original.substr(0, original.size() - cased_rest.size() - terminal_helpcode.size());
        };
        if (base.helpcode_length > 0)
        {
            const size_t rest_start =
                shuangpin::raw_length_for_effective_prefix(base.raw_input_with_cases, word_pinyin_length);
            transition.consumed_raw_input_with_cases = base.raw_input_with_cases.substr(0, rest_start);
            const size_t required_length = word_pinyin_length + base.helpcode_length;
            transition.continues_composition =
                required_length < total_input_length && word_pinyin_length < total_input_length;

            if (transition.continues_composition)
            {
                const size_t rest_end = shuangpin::raw_length_for_effective_prefix(
                    base.raw_input_with_cases, total_input_length - base.helpcode_length);
                const std::string rest_pinyin_sequence = base.raw_input.substr(rest_start, rest_end - rest_start);
                std::string normalized_rest = rest_pinyin_sequence;
                std::string cased_rest = base.raw_input_with_cases.substr(rest_start, rest_end - rest_start);
                remove_consumed_leading_separators(normalized_rest, cased_rest);
                if (!typed.empty())
                {
                    auto remapped = RemapMidSentenceRest(request(), typed, rest_start, rest_end, shuangpin_profile_);
                    transition.consumed_raw_input_with_cases = std::move(remapped.consumed);
                    cased_rest = std::move(remapped.rest);
                    normalized_rest = LowercaseLetters(cased_rest);
                }
                consume_terminal_helpcode(normalized_rest, cased_rest);
                engine_.replace_shuangpin_raw_input(normalized_rest, cased_rest);
                online_requests_.invalidate();
                update_mixed_candidates();
            }
        }
        else
        {
            if (consumed_length == 0 || consumed_length > base.effective_raw_input.size())
            {
                consumed_length = (std::min)(word_pinyin_length, base.effective_raw_input.size());
            }

            const size_t consumed_raw_length =
                shuangpin::raw_length_for_effective_prefix(base.raw_input_with_cases, consumed_length);
            transition.consumed_raw_input_with_cases = base.raw_input_with_cases.substr(0, consumed_raw_length);
            transition.continues_composition = consumed_length < transition.full_pure_pinyin.size();

            if (transition.continues_composition)
            {
                const std::string rest_pinyin_sequence =
                    base.raw_input.substr(consumed_raw_length, base.raw_input.size() - consumed_raw_length);
                const std::string rest_pinyin_sequence_with_cases = base.raw_input_with_cases.substr(
                    consumed_raw_length, base.raw_input_with_cases.size() - consumed_raw_length);
                std::string normalized_rest = rest_pinyin_sequence;
                std::string cased_rest = rest_pinyin_sequence_with_cases;
                remove_consumed_leading_separators(normalized_rest, cased_rest);
                if (!typed.empty())
                {
                    auto remapped = RemapMidSentenceRest(request(), typed, consumed_raw_length,
                                                         base.raw_input_with_cases.size(), shuangpin_profile_);
                    transition.consumed_raw_input_with_cases = std::move(remapped.consumed);
                    cased_rest = std::move(remapped.rest);
                    normalized_rest = LowercaseLetters(cased_rest);
                }
                consume_terminal_helpcode(normalized_rest, cased_rest);
                engine_.replace_shuangpin_raw_input(normalized_rest, cased_rest);
                online_requests_.invalidate();
                update_mixed_candidates();
            }
        }

        if (!typed.empty() && !transition.continues_composition)
        {
            // 整串上屏：撤销时要还原的是用户敲的原串，连同反引号段。
            transition.consumed_raw_input_with_cases = typed;
        }

        transition.current_segmentation = get_pinyin_segmentation();
        transition.current_segmentation_with_cases = get_pinyin_segmentation_with_cases();
        return transition;
    }

    transition.full_pure_pinyin = request().normalized_input;
    const std::string current_segmentation =
        request().normalized_segmentation.empty() ? request().segmentation : request().normalized_segmentation;
    const std::string current_segmentation_with_cases = get_pinyin_segmentation_with_cases();
    const std::string selected_pure_pinyin = remove_delimiters(selected_pinyin);
    const std::string raw_input_without_helpcodes =
        quanpin::strip_active_helpcodes(request().raw_input, request().raw_input_with_cases);
    const std::string raw_input_with_cases_without_helpcodes =
        quanpin::strip_active_helpcodes_with_cases(request().raw_input, request().raw_input_with_cases);

    // 纠错读音按它在原始输入里盖住的字母消耗：多字、漏字纠错和原始字母不等长，按读音
    // 长度消耗会剩下或多吞字母（buuhui 选「不会」剩个 i）。
    size_t consumed_letters = selected_pure_pinyin.size();
    const unsigned autocorrect_types = QuanpinAutocorrectTypes(request());
    const bool literal_mixed_prefix =
        current_scheme_type() == SchemeType::Quanpin && !candidates().empty() &&
        quanpin::literal_mixed_pinyin_match(raw_input_without_helpcodes, candidates().front().pinyin,
                                            candidates().front().canonical_pinyin) &&
        raw_input_without_helpcodes.compare(0, consumed_letters, selected_pure_pinyin) == 0;
    if (autocorrect_types != 0 && !literal_mixed_prefix && raw_input_without_helpcodes.find('\'') == std::string::npos)
    {
        if (const auto aligned = quanpin::corrected_reading_raw_length(
                raw_input_without_helpcodes, quanpin::split_segments(selected_pinyin), autocorrect_types))
        {
            consumed_letters = *aligned;
        }
        else if (const auto fault_aligned = quanpin::fault_reading_raw_length(raw_input_without_helpcodes,
                                                                              quanpin::split_segments(selected_pinyin)))
        {
            // 长句容错仲裁的读音（句中简拼展开、槽内漏字变体）不在 k-best 切分里，
            // corrected_reading_raw_length 对不上；按槽位模型对回原始字母。
            consumed_letters = *fault_aligned;
        }
    }

    size_t consumed_raw_length =
        shuangpin::raw_length_for_effective_prefix(raw_input_with_cases_without_helpcodes, consumed_letters);
    transition.consumed_raw_input_with_cases = raw_input_with_cases_without_helpcodes.substr(0, consumed_raw_length);

    transition.continues_composition = !selected_pure_pinyin.empty() &&
                                       consumed_letters < transition.full_pure_pinyin.size() &&
                                       consumed_raw_length < raw_input_without_helpcodes.size();

    if (transition.continues_composition)
    {
        std::string rest_raw_input = raw_input_without_helpcodes.substr(consumed_raw_length);
        std::string rest_raw_input_with_cases = raw_input_with_cases_without_helpcodes.substr(consumed_raw_length);
        remove_consumed_leading_separators(rest_raw_input, rest_raw_input_with_cases);
        engine_.replace_active_raw_input(rest_raw_input, rest_raw_input_with_cases);
        online_requests_.invalidate();
        update_mixed_candidates();
        transition.current_segmentation = get_pinyin_segmentation();
        transition.current_segmentation_with_cases = get_pinyin_segmentation_with_cases();
        return transition;
    }

    transition.current_segmentation = current_segmentation;
    transition.current_segmentation_with_cases = current_segmentation_with_cases;
    return transition;
}

InputSession::CloudQueryState InputSession::get_cloud_query_state() const
{
    CloudQueryState state;

    if (is_japanese())
    {
        state.cache_key = request().raw_input;
        state.committed_pinyin = request().raw_input;
        state.should_query = !request().raw_input.empty();
        state.query_text = state.should_query ? request().raw_input : std::string{};
        return state;
    }

    if (is_wubi())
    {
        state.cache_key = request().normalized_input;
        state.committed_pinyin = request().normalized_input;
        return state;
    }

    if (is_shuangpin())
    {
        const auto base = ResolveShuangpinCompositionBase(request(), shuangpin_profile_);
        state.cache_key = ResolveShuangpinCloudCacheKey(request(), shuangpin_profile_);
        state.committed_pinyin = shuangpin::remove_manual_delimiters(state.cache_key);

        if (has_active_helpcode())
        {
            return state;
        }

        const char last =
            base.effective_raw_input_with_cases.empty() ? '\0' : base.effective_raw_input_with_cases.back();
        const bool ends_with_input_key = (last >= 'a' && last <= 'z') || last == ';';
        state.should_query =
            ends_with_input_key && shuangpin::is_complete_input(base.effective_raw_input, shuangpin_profile_);

        if (state.should_query)
        {
            // 云输入拿的是带撇号的全拼，但 ü 要换成 inputtools 认的写法：它和本地的
            // Google 解码器一样只认 nue/lue，nve'dai'dong'wu 会被它拆成 nv + e，
            // 「虐待动物」于是变成「女蛾黛动物」。committed_pinyin / cache_key 不受
            // 影响，仍是双拼原串。
            const std::string quanpin_segmentation =
                shuangpin::normalize_input_with_delimiters(state.cache_key, shuangpin_profile_);
            state.query_text = quanpin::to_google_spelling(quanpin_segmentation);
        }
        return state;
    }

    state.committed_pinyin = request().normalized_input;
    state.cache_key = ResolveQuanpinCloudCacheKey(request());

    if (has_active_helpcode())
    {
        return state;
    }

    state.should_query = !request().normalized_input.empty();
    // 云端要按用户看到的分词来查：手动把 qi'e'huan 分成三段，就该带着音节边界发给云接口，
    // 否则云端自行贪心断句会当成 qie'huan（切换）。normalized_segmentation 保留了用户的手动
    // 撇号（cut_pinyin_with_corrections 先按撇号切分），既定住了分词，也让 to_google_spelling
    // 能逐音节把 nve / lve 换成云端认的 nue / lue 写法。分词缺失时退回裸串。
    const std::string &segmentation =
        request().normalized_segmentation.empty() ? request().normalized_input : request().normalized_segmentation;
    state.query_text = quanpin::to_google_spelling(segmentation);
    return state;
}

InputSession::CreatingWordProgress InputSession::update_creating_word_progress(
    const std::string &current_pinyin, const std::string &current_word, const std::string &selected_word,
    const SelectionTransition &selection_transition) const
{
    CreatingWordProgress progress;
    if (selection_transition.wubi_native)
    {
        progress.pinyin = current_pinyin.empty() ? selection_transition.full_pure_pinyin : current_pinyin;
        progress.word = current_word + selected_word;
        progress.preedit = progress.word;
        progress.completed = true;
        progress.can_store = false;
        return progress;
    }

    const std::string selected_canonical =
        normalize_canonical_pinyin_for_word(selection_transition.selected_canonical_pinyin, selected_word);
    const bool prior_parts_are_storeable = current_word.empty() || !current_pinyin.empty();
    if (prior_parts_are_storeable && !selected_canonical.empty())
    {
        progress.pinyin = append_canonical_pinyin(current_pinyin, selected_canonical);
    }
    progress.word = current_word + selected_word;
    progress.preedit = progress.word + selection_transition.current_segmentation_with_cases;
    progress.completed = !selection_transition.continues_composition;
    progress.can_store =
        progress.completed && !normalize_canonical_pinyin_for_word(progress.pinyin, progress.word).empty();
    return progress;
}

bool InputSession::is_shuangpin() const
{
    return current_scheme_type() == SchemeType::Shuangpin;
}

bool InputSession::is_wubi() const
{
    return current_scheme_type() == SchemeType::Wubi;
}

bool InputSession::is_wubi_native_candidate(const WordItem &item)
{
    return item.scheme == SchemeType::Wubi;
}

std::size_t InputSession::wubi_native_candidate_count() const
{
    return static_cast<std::size_t>(std::count_if(candidates().begin(), candidates().end(),
                                                  [](const WordItem &item) { return is_wubi_native_candidate(item); }));
}

bool InputSession::is_japanese() const
{
    return current_scheme_type() == SchemeType::JapaneseRomaji;
}

void InputSession::clear_pending_sequence()
{
    pending_pinyin_sequence_.clear();
    pending_pinyin_sequence_with_cases_.clear();
    has_pending_pinyin_sequence_ = false;
    has_pending_pinyin_sequence_with_cases_ = false;
    pending_wubi_literal_input_ = false;
}

void InputSession::apply_pending_sequence()
{
    caret_.reset();
    const std::string raw_input = has_pending_pinyin_sequence_ ? pending_pinyin_sequence_ : request().raw_input;
    const std::string raw_input_with_cases =
        has_pending_pinyin_sequence_with_cases_ ? pending_pinyin_sequence_with_cases_ : raw_input;

    switch (current_scheme_type())
    {
    case SchemeType::Shuangpin:
        engine_.replace_shuangpin_raw_input(raw_input, raw_input_with_cases);
        break;
    case SchemeType::Quanpin:
        engine_.replace_quanpin_raw_input(raw_input, raw_input_with_cases);
        break;
    case SchemeType::Wubi:
        engine_.replace_wubi_raw_input(raw_input, raw_input_with_cases, pending_wubi_literal_input_);
        break;
    case SchemeType::JapaneseRomaji:
        engine_.replace_japanese_raw_input(raw_input, raw_input_with_cases);
        break;
    }
    clear_pending_sequence();
    online_requests_.invalidate();
    update_mixed_candidates();
}
} // namespace metasequoia
