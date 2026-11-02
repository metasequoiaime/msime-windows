#include "engine.h"

#include "../quanpin/fuzzy_pinyin.h"
#include "shuangpin_query.h"
#include "shuangpin_utils.h"

#include <optional>
#include <algorithm>

namespace
{
struct HelpcodeQuery
{
    std::string base_raw_input;
    std::string base_pure_input;
    std::string base_segmentation;
    std::string help_codes;
};

bool has_manual_delimiters(const std::string &raw_input)
{
    return raw_input.find('\'') != std::string::npos;
}

bool segmented_parts_are_all_two_chars(const std::string &segmentation)
{
    if (segmentation.empty())
    {
        return false;
    }

    size_t start = 0;
    while (start <= segmentation.size())
    {
        const size_t separator = segmentation.find('\'', start);
        const std::string part =
            separator == std::string::npos ? segmentation.substr(start) : segmentation.substr(start, separator - start);
        if (part.size() != 2)
        {
            return false;
        }
        if (separator == std::string::npos)
        {
            break;
        }
        start = separator + 1;
    }
    return true;
}

std::vector<WordItem> query_normal(ShuangpinDictionary &dictionary, const QueryRequest &request,
                                   const ShuangpinProfile &profile)
{
    const std::string pure_input = shuangpin::remove_manual_delimiters(request.raw_input);
    return dictionary.generateSeries(pure_input, shuangpin::segment_input(request.raw_input, profile),
                                     request.raw_input);
}

std::optional<HelpcodeQuery> build_full_helpcode_query(const std::string &raw_input,
                                                       const std::string &raw_input_with_cases,
                                                       const ShuangpinProfile &profile)
{
    if (shuangpin::detect_active_double_helpcode_length(raw_input, raw_input_with_cases, profile) != 2)
    {
        return std::nullopt;
    }

    HelpcodeQuery query;
    query.base_raw_input = shuangpin::trim_trailing_letters_preserve_delimiters(raw_input, 2);
    query.base_pure_input = shuangpin::remove_manual_delimiters(query.base_raw_input);
    query.base_segmentation = shuangpin::segment_input(query.base_raw_input, profile);
    query.help_codes = ShuangpinUtil::GetFullHelpCodes(shuangpin::remove_manual_delimiters(raw_input_with_cases));
    return query;
}

std::optional<HelpcodeQuery> build_single_helpcode_query(const std::string &raw_input,
                                                         const std::string &pure_input_with_cases,
                                                         const ShuangpinProfile &profile)
{
    if (pure_input_with_cases.size() <= 1 || pure_input_with_cases.size() % 2 == 0)
    {
        return std::nullopt;
    }

    HelpcodeQuery query;
    query.base_raw_input = shuangpin::trim_trailing_letters_preserve_delimiters(raw_input, 1);
    query.base_pure_input = shuangpin::remove_manual_delimiters(query.base_raw_input);
    query.base_segmentation = shuangpin::segment_input(query.base_raw_input, profile);
    if (has_manual_delimiters(raw_input) && !segmented_parts_are_all_two_chars(query.base_segmentation))
    {
        return std::nullopt;
    }
    if (!ShuangpinUtil::is_all_complete_pinyin(query.base_pure_input, query.base_segmentation))
    {
        return std::nullopt;
    }

    query.help_codes = pure_input_with_cases.substr(pure_input_with_cases.size() - 1, 1);
    return query;
}
} // namespace

ShuangpinEngine::ShuangpinEngine(const ShuangpinProfile &profile, metasequoia::RuntimePaths paths)
    : profile_(profile), dictionary_(profile, paths), paths_(std::move(paths))
{
}

std::optional<WordItem> ShuangpinEngine::find_candidate(const std::string &key, const std::string &value)
{
    return dictionary_.find_candidate(key, value);
}

std::vector<WordItem> ShuangpinEngine::query(const QueryRequest &request)
{
    if (!request.valid)
    {
        return {};
    }

    // 整句联想开关随请求下发到词典层，四条整句来源据此各自门控（见 generateSeries）。
    dictionary_.set_sentence_association(request.sentence_association);
    // 前文同理随请求下发；换了前文，按旧前文重排出来的缓存顺序作废。
    dictionary_.set_rescoring_context(request.rescoring_context);
    // 句中辅助码约束也随请求下发；开关关着时不筛（串里本就敲不进反引号，这里只是兜底）。
    dictionary_.set_syllable_helpcodes(request.enable_mid_sentence_helpcode ? request.syllable_helpcodes
                                                                            : SyllableHelpcodes{});
    // 直接辅助码开着时词格的跨度查询走与解析器共用的缓存，见 ShuangpinDictionary::lattice_lookup。
    dictionary_.set_direct_span_cache_enabled(request.enable_direct_helpcode);
    // 整句同样要收模糊读音，见 QuanpinDictionary::set_fuzzy_pinyin。
    dictionary_.set_fuzzy_pinyin(request.fuzzy_pinyin);
    auto candidates = query_unfiltered(request);
    dictionary_.filter_by_syllable_helpcodes(candidates);
    // 放在辅助码各条路径之后：奇数个键的 txu 开着单码辅助时会被读成「tx + 辅码 u」，T恤 照样要有。
    dictionary_.insert_latin_words(candidates, request.raw_input);
    // 辅助码筛过、重排过的列表不补：首选是按辅码挑出来的，补出来的长词不一定满足同样的约束。
    if (request.end_helpcodes.empty() && request.syllable_helpcodes.empty())
        dictionary_.insert_prefix_completion(candidates, request.raw_input, request.prefix_completion);
    return candidates;
}

bool ShuangpinEngine::resolve_direct_helpcode(QueryRequest &request)
{
    if (!request.valid)
    {
        return false;
    }
    // 解码要用的搭配模型跟着整句选项走，先下发，和 query 的顺序一致。
    dictionary_.set_sentence_association(request.sentence_association);
    dictionary_.set_fuzzy_pinyin(request.fuzzy_pinyin);
    return dictionary_.resolve_direct_helpcode(request);
}

std::vector<WordItem> ShuangpinEngine::query_unfiltered(const QueryRequest &request)
{
    const std::string &raw_input = request.raw_input;
    const std::string &raw_input_with_cases =
        request.raw_input_with_cases.empty() ? request.raw_input : request.raw_input_with_cases;
    const std::string pure_input = shuangpin::remove_manual_delimiters(raw_input);
    const std::string pure_input_with_cases = shuangpin::remove_manual_delimiters(raw_input_with_cases);

    if (pure_input.empty())
    {
        return {};
    }

    // 引导键触发的末尾辅助码：辅码由方案层从末尾段里取出，输入串里已经没有它们，不用再猜哪几个键是
    // 辅码，也不会和「再打一个声母」混在一起，所以单码不补原拼音的候选。
    if (!request.end_helpcodes.empty())
    {
        const std::string segmentation = shuangpin::segment_input(raw_input, profile_);
        const auto base = query_normal(dictionary_, request, profile_);
        auto filtered =
            request.end_helpcodes.size() == 2
                ? HelpcodeUtils::filter_candidates_with_double_helpcodes(base, request.end_helpcodes, helpcodes_.get())
                : HelpcodeUtils::reorder_candidates_with_single_helpcode(base, request.end_helpcodes, helpcodes_.get());
        return append_fuzzy(std::move(filtered), segmentation, request.fuzzy_pinyin, request.end_helpcodes);
    }

    if (request.enable_shuangpin_helpcode)
    {
        // 双码辅助
        if (const auto full_helpcode = build_full_helpcode_query(raw_input, raw_input_with_cases, profile_))
        {
            return append_fuzzy(dictionary_.generate_with_helpcodes(full_helpcode->base_pure_input,
                                                                    full_helpcode->base_segmentation, raw_input,
                                                                    full_helpcode->help_codes),
                                full_helpcode->base_segmentation, request.fuzzy_pinyin, full_helpcode->help_codes);
        }

        // 单码辅助
        if (const auto single_helpcode = build_single_helpcode_query(raw_input, pure_input_with_cases, profile_))
        {
            return append_fuzzy(dictionary_.generate_with_helpcodes(single_helpcode->base_pure_input,
                                                                    single_helpcode->base_segmentation, raw_input,
                                                                    single_helpcode->help_codes),
                                single_helpcode->base_segmentation, request.fuzzy_pinyin, single_helpcode->help_codes);
        }

        // 不满足辅助码条件，单独查询，比如，cls -> c'ls，也就直接走下面的 query_normal 了
    }

    return append_fuzzy(query_normal(dictionary_, request, profile_), shuangpin::segment_input(raw_input, profile_),
                        request.fuzzy_pinyin);
}

std::string ShuangpinEngine::expandable_initial_code(const QueryRequest &request) const
{
    if (request.scheme != SchemeType::Shuangpin)
    {
        return {};
    }

    const std::string segmentation = shuangpin::segment_input(request.raw_input, profile_);
    const size_t separator = segmentation.find('\'');
    std::string initial_code = segmentation.substr(0, separator);
    return initial_code.size() == 1 ? initial_code : std::string{};
}

bool ShuangpinEngine::can_expand_initial_candidates(const QueryRequest &request,
                                                    const std::vector<WordItem> &candidates) const
{
    const std::string initial_code = expandable_initial_code(request);
    return !initial_code.empty() && ShuangpinDictionary::can_expand_initial_candidates(initial_code, candidates);
}

bool ShuangpinEngine::expand_initial_candidates(const QueryRequest &request, std::vector<WordItem> &candidates)
{
    const std::string initial_code = expandable_initial_code(request);
    if (initial_code.empty())
    {
        return false;
    }

    return dictionary_.expand_initial_candidates(initial_code, candidates, request.raw_input);
}

int ShuangpinEngine::create_word(std::string pinyin, std::string word)
{
    return dictionary_.create_word(std::move(pinyin), std::move(word));
}

int ShuangpinEngine::update_weight_by_pinyin_and_word(std::string pinyin, std::string word)
{
    return dictionary_.update_weight_by_pinyin_and_word(std::move(pinyin), std::move(word));
}

int ShuangpinEngine::delete_by_pinyin_and_word(std::string pinyin, std::string word)
{
    return dictionary_.delete_by_pinyin_and_word(std::move(pinyin), std::move(word));
}

int ShuangpinEngine::insert_word_to_series_cache(const std::string &pinyin, const std::string &word,
                                                 CandidateSource source)
{
    return dictionary_.insert_word_to_cached_buffer_series(pinyin, word, source);
}

int ShuangpinEngine::insert_word_to_active_helpcode_cache(const std::string &pinyin, const std::string &word,
                                                          CandidateSource source, const std::string &double_helpcodes)
{
    return dictionary_.insert_word_to_active_helpcode_cache(pinyin, word, source, double_helpcodes);
}

std::string ShuangpinEngine::search_sentence_from_ime_engine(const std::string &user_pinyin)
{
    return dictionary_.search_sentence_from_ime_engine(user_pinyin);
}

void ShuangpinEngine::reset_cache()
{
    dictionary_.reset_cache();
    if (fuzzy_dictionary_)
        fuzzy_dictionary_->reset_cache();
}

void ShuangpinEngine::reset_sentence_cache()
{
    dictionary_.reset_sentence_cache();
    if (fuzzy_dictionary_)
        fuzzy_dictionary_->reset_sentence_cache();
}

std::vector<WordItem> ShuangpinEngine::append_fuzzy(std::vector<WordItem> exact, const std::string &raw_segmentation,
                                                    metasequoia::FuzzyPinyinOptions options,
                                                    const std::string &helpcodes)
{
    if (!options.rules)
        return exact;
    if (!fuzzy_dictionary_)
        fuzzy_dictionary_ = std::make_unique<QuanpinDictionary>(std::string{}, paths_);
    const auto typed = quanpin::split_segments(raw_segmentation);
    auto fuzzy =
        fuzzy_dictionary_->fuzzy_candidates(shuangpin::to_quanpin_segmentation(raw_segmentation, profile_), options);
    for (auto &item : fuzzy)
    {
        const auto count = quanpin::split_segments(item.pinyin).size();
        if (count > typed.size())
            continue;
        item.pinyin = quanpin::join_segments(quanpin::Segments(typed.begin(), typed.begin() + count));
    }
    if (helpcodes.size() == 2)
        fuzzy = HelpcodeUtils::filter_candidates_with_double_helpcodes(fuzzy, helpcodes, helpcodes_.get());
    else if (helpcodes.size() == 1)
        fuzzy = HelpcodeUtils::reorder_candidates_with_single_helpcode(fuzzy, helpcodes, helpcodes_.get());
    return quanpin::merge_fuzzy_candidates(std::move(exact), std::move(fuzzy),
                                           [](const WordItem &item) { return item.pinyin.size(); });
}
