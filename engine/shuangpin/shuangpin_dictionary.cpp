#include "shuangpin_dictionary.h"
#include "../core/query_request.h"
#include "../direct_helpcode/direct_resolver.h"
#include "../user_dictionary/user_dictionary_journal.h"
#include "../common/helpcode_utils.h"
#include "../quanpin/quanpin_query.h"
#include "../quanpin/quanpin_utils.h"
#include "shuangpin_query.h"
#include "shuangpin_utils.h"
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <sqlite3.h>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <cstdlib>
#include <climits>
#include <boost/algorithm/string.hpp>
#include <fmt/xchar.h>
#include <utf8/cpp17.h>

using namespace std;

namespace
{
std::string double_helpcode_cache_key(const std::string &pinyin, const std::string &help_codes)
{
    // The same lowercase input can carry normal or reversed auxiliary codes.
    return pinyin + ":" + help_codes;
}

std::string remove_delimiters(const std::string &segmented)
{
    std::string normalized = segmented;
    normalized.erase(std::remove(normalized.begin(), normalized.end(), '\''), normalized.end());
    return normalized;
}

std::string escape_sql_text(std::string text)
{
    size_t pos = 0;
    while ((pos = text.find('\'', pos)) != std::string::npos)
    {
        text.insert(pos, 1, '\'');
        pos += 2;
    }
    return text;
}

} // namespace

ShuangpinDictionary::ShuangpinDictionary(const ShuangpinProfile &profile, metasequoia::RuntimePaths paths)
    : profile_(profile), paths_(std::move(paths)), decoder_(paths_.resource(metasequoia::assets::pinyin_model),
                                                            paths_.user(metasequoia::assets::pinyin_user_dictionary)),
      language_model_(&ngram::shared_language_model(paths_.resource(metasequoia::assets::language_model))),
      neural_keyboard_model_(metasequoia::path_to_utf8(paths_.resource(metasequoia::assets::neural_model_keyboard))),
      helpcodes_(HelpcodeUtils::load_helpcode_keymap(paths_.resources, HelpcodeUtils::selected_helpcode_schema())),
      _kb_input_sequence(100), _cached_buffer(128), _cached_buffer_sgl(128), _cached_buffer_sgl_reversed(128),
      _cached_buffer_dbl(128), _cached_buffer_series(128)
{
    // 最多可以输出 64 个汉字，拼音最多可以接受 128 个字符

    quanpin_db_path_ = metasequoia::path_to_utf8(paths_.dictionary(metasequoia::assets::main_dictionary));
    int exit = sqlite3_open(quanpin_db_path_.c_str(), &quanpin_db_);
    if (exit != SQLITE_OK)
    {
        (void)0;
    }
    else
    {
        // See QuanpinDictionary's constructor: a query must wait out a commit, not fail.
        sqlite3_busy_timeout(quanpin_db_, quanpin::kDictionaryBusyTimeoutMs);
        quanpin::warm_up(quanpin_db_, quanpin_statement_cache_);
        reset_cache_if_database_changed();
    }
}

/**
 * @brief Generate candidate list when not in help mode
 *
 * @param pinyin_sequence
 * @param pinyin_segmentation
 * @return vector<ShuangpinDictionary::WordItem>
 */
vector<ShuangpinDictionary::WordItem> ShuangpinDictionary::generate( //
    const string &pinyin_sequence,                                   //
    const string &pinyin_segmentation,                               //
    const string &cache_key                                          //
)
{
    // std::shared_lock lock(mutex_);
    vector<ShuangpinDictionary::WordItem> candidate_list;
    if (pinyin_sequence.size() == 0)
    {
        return candidate_list;
    }
    vector<string> code_list;
    if (pinyin_sequence.size() == 1)
    {
        generate_for_single_char(candidate_list, pinyin_sequence);
    }
    else
    {
        const std::string effective_cache_key = cache_key.empty() ? pinyin_sequence : cache_key;
        // Check cache first
        if (_cached_buffer.find(effective_cache_key))
        {
            reset_cache_if_database_changed();
            if (const auto *cached = _cached_buffer.find(effective_cache_key))
            {
                return *cached;
            }
        }

        candidate_list = query_from_quanpin_database(pinyin_sequence, pinyin_segmentation);
        _cached_buffer.insert(effective_cache_key, candidate_list);
    }
    return candidate_list;
}

/**
 * @brief 对于纯粹的拼音，除了完全匹配的汉字串，子串也要全部给出来，子串是为了给接下来可能会进行的造词使用的
 *
 * @param pinyin_sequence
 * @param pinyin_segmentation
 * @return vector<ShuangpinDictionary::WordItem>
 */
vector<ShuangpinDictionary::WordItem> ShuangpinDictionary::generateSeries( //
    const string &pinyin_sequence,                                         //
    const string &pinyin_segmentation,                                     //
    const string &cache_key                                                //
)
{
    vector<ShuangpinDictionary::WordItem> candidate_list;
    if (pinyin_sequence.size() == 0)
    {
        return candidate_list;
    }
    vector<string> code_list;
    if (pinyin_sequence.size() == 1)
    {
        generate_for_single_char(candidate_list, pinyin_sequence);
    }
    else
    {
        // 句中辅助码约束改变整句来源的结果，签名必须进键。
        const std::string effective_cache_key =
            (cache_key.empty() ? pinyin_sequence : cache_key) + syllable_helpcodes_signature_;
        // 先看一下缓存里有没有
        if (_cached_buffer_series.find(effective_cache_key))
        {
            reset_cache_if_database_changed();
            if (const auto *cached = _cached_buffer_series.find(effective_cache_key))
            {
                return *cached;
            }
        }

        // 查询当前的拼音严格对应的数据
        vector<ShuangpinDictionary::WordItem> cur_pinyin_cand =
            generate(pinyin_sequence, pinyin_segmentation, cache_key.empty() ? pinyin_sequence : cache_key);
        if (cur_pinyin_cand.size() > 0)
        {
            candidate_list.insert(candidate_list.end(), cur_pinyin_cand.begin(), cur_pinyin_cand.end());
        }
        else
        { /* 可能数据库查询的结果是空，这时就需要联想，这个只适合在此处联想 */
            if (candidate_list.size() == 0 && sentence_association_.google)
            {
                string quanpin_str =
                    ShuangpinUtil::convert_seg_shuangpin_to_seg_complete_pinyin(pinyin_segmentation, profile_);
                // 只有送进解码器的那一份换 ü 写法，quanpin_str 仍是词库那侧的
                // canonical 拼写，候选要靠它落库。
                string res = decode_google_sentence(quanpin_str);
                if (res.size() > 0)
                {
                    // 整句 fallback 必须带上 canonical quanpin，否则以它结尾的造词无法落库：
                    // update_creating_word_progress 依赖 canonical_pinyin 才能拼出完整读音。
                    candidate_list.emplace_back(_pinyin_sequence, res, 1, CandidateSource::Fallback, quanpin_str);
                    candidate_list.back().sentence_association = true;
                }
            }
        }

        // 查询当前的拼音子串对应的数据
        string pure_pinyin = pinyin_sequence;
        string seg_pinyin = pinyin_segmentation;
        while (true)
        {
            size_t pos = seg_pinyin.rfind('\'');
            if (pos != string::npos)
            {
                seg_pinyin = seg_pinyin.substr(0, pos);
                pure_pinyin = boost::algorithm::replace_all_copy(seg_pinyin, "'", "");
                vector<ShuangpinDictionary::WordItem> sub_pinyin_cand = generate(pure_pinyin, seg_pinyin);
                candidate_list.insert(candidate_list.end(), sub_pinyin_cand.begin(), sub_pinyin_cand.end());
            }
            else
            {
                break;
            }
        }

        const std::string quanpin_segmentation =
            ShuangpinUtil::convert_seg_shuangpin_to_seg_complete_pinyin(pinyin_segmentation, profile_);
        // 次序与全拼一致：词格（kenlm 三元模型打分）在前，Google 解码器在后。两边都
        // 只出一句，免得近似重复的整句把候选页挤满。两条都插在
        // generated_sentence_insert_position 给的位置上，也就是开头那串整串拼音精确
        // 命中词库的候选之后；merge_lattice_candidates 算出的位置就是这条 Fallback
        // 所在的下标，词格随后会落在它上面。
        const auto quanpin_syllables = quanpin::split_segments(quanpin_segmentation);
        if (sentence_association_.google && quanpin_syllables.size() >= 2 &&
            quanpin_segmentation.find('\'') != std::string::npos)
        {
            // 同上：解码器认 nue/lue，词库与下面的 canonical 读音认 nve/lve。
            const std::string google_sentence = decode_google_sentence(quanpin_segmentation);
            const bool duplicate = std::any_of(candidate_list.begin(), candidate_list.end(),
                                               [&](const WordItem &item) { return item.word == google_sentence; });
            if (!google_sentence.empty() && !duplicate)
            {
                // 整句 fallback 必须带上 canonical quanpin，否则以它结尾的造词无法落库。
                const size_t insert_at = quanpin::generated_sentence_insert_position(candidate_list, quanpin_syllables);
                WordItem sentence(_pinyin_sequence, google_sentence, 1, CandidateSource::Fallback,
                                  quanpin_segmentation);
                sentence.sentence_association = true;
                candidate_list.insert(candidate_list.begin() + static_cast<std::ptrdiff_t>(insert_at),
                                      std::move(sentence));
            }
        }
        // 词格给 Trigram 候选和神经模型共用；只开神经时仍在内部解出 n-best，但不显示词格首选。
        // 与全拼同构，见 QuanpinDictionary::query_series。
        std::vector<quanpin::SourcedLatticeReranker> neural_rerankers;
        if (sentence_association_.neural_keyboard && neural_keyboard_model_.get() != nullptr)
        {
            neural_rerankers.push_back({quanpin::make_neural_reranker(neural_keyboard_model_.get(), rescoring_context_),
                                        CandidateSource::NeuralKeyboard});
        }
        if (sentence_association_.neural_desktop && neural_desktop_model_.get() != nullptr)
        {
            neural_rerankers.push_back({quanpin::make_neural_reranker(neural_desktop_model_.get(), rescoring_context_),
                                        CandidateSource::NeuralDesktop});
        }
        // 万象语法模型不再单独重排，与全拼同构，见 QuanpinDictionary::query_series。
        quanpin::WordLatticeOptions lattice_options;
        const bool has_collocation =
            collocation_db_ != nullptr && collocation_db_->valid() && sentence_association_.collocation_weight != 0.0;
        const bool needs_alternatives = !neural_rerankers.empty() || (sentence_association_.word_lattice &&
                                                                      sentence_association_.show_next_on_duplicate);
        lattice_options.nbest = needs_alternatives ? static_cast<int>(neural::RerankOptions{}.max_paths) : 1;
        lattice_options.include_lattice_best = sentence_association_.word_lattice;
        lattice_options.include_collocation_best = sentence_association_.collocation_rerank && has_collocation;
        lattice_options.show_next_on_duplicate = sentence_association_.show_next_on_duplicate;
        lattice_options.language_model = language_model_;
        if (has_collocation)
        {
            lattice_options.collocation_scorer = [db = collocation_db_,
                                                  config = sentence_association_.collocation_grammar](
                                                     std::string_view tail, std::string_view word, bool is_rear) {
                return db->query(std::string(tail), std::string(word), is_rear, config) * quanpin::kLnToLog10;
            };
            lattice_options.collocation_weight = sentence_association_.collocation_weight;
        }
        // 句中辅助码：词格建图时按约束筛边，受约束的跨度不截断地查词库（生僻字才筛得出来）。
        // 音节序号与 quanpin_syllables 一一对应：两者都按同一份双拼切分逐段换算。
        for (const auto &helpcode : syllable_helpcodes_)
        {
            lattice_options.char_constraints.push_back({helpcode.syllable, [this, helpcode](const std::string &hanzi) {
                                                            return accepts_syllable_char(helpcode, hanzi);
                                                        }});
        }
        // 直接辅助码的解析器选切分时已经按同一套打分解出了这串输入的最优整句：只要一条首选（没有重排器要
        // n-best），就直接用它，不再把同一张词格解第二遍。
        if (lattice_options.nbest == 1 && direct_sentence_ && direct_sentence_->first == effective_cache_key)
        {
            lattice_options.precomputed_paths = {direct_sentence_->second};
        }
        if (!lattice_options.char_constraints.empty())
        {
            constexpr int kConstrainedSpanLimit = 4096;
            lattice_options.constrained_lookup = lattice_lookup(kConstrainedSpanLimit);
        }
        if (sentence_association_.word_lattice || !neural_rerankers.empty() || lattice_options.include_collocation_best)
        {
            quanpin::merge_lattice_candidates(candidate_list, quanpin_syllables,
                                              lattice_lookup(lattice_options.span_limit), pinyin_sequence,
                                              lattice_options, neural_rerankers);
        }

        if (sentence_association_.context_candidate_rerank && !rescoring_context_.empty())
        {
            quanpin::rerank_dictionary_candidates(candidate_list, quanpin_segmentation, neural_keyboard_model_.get(),
                                                  rescoring_context_);
        }
        /* 缓存起来 */
        _cached_buffer_series.insert(effective_cache_key, candidate_list);
    }

    return candidate_list;
}

/**
 * @brief Filter with single help code
 *
 * Not only the first Hanzi part, but also the last one that will be considered.
 *   - For single Hanzi, we consider its first and last part
 *   - For Multi Hanzi, we consider first Hanzi's first part and last Hanzi's first part
 *
 * e.g. 阿: 阿's helpcode is ek, when we type aae or aak, 阿 will both be filtered.
 *      阿姨: 阿's helpcode is ek, 姨's helpcode is nr, when we type aayie or aayin, 阿姨 will both be filtered.
 *
 * 此外，单码辅助的情况，需要把原始拼音的候选列表加到辅助码模式的候选列表后面，这里的指的是不将最后一个字符看成是辅助码的情况下得到的候选项的结果
 *
 * @param candidate_list
 * @param filtered_list
 * @param help_code
 * @param pinyin_sequence 原始的拼音序列
 */
void ShuangpinDictionary::filter_with_single_helpcode(           //
    const vector<ShuangpinDictionary::WordItem> &candidate_list, //
    vector<ShuangpinDictionary::WordItem> &result_list,          //
    const string &help_code,                                     //
    const string &pinyin_sequence                                //
)
{
    if (candidate_list.empty() || help_code.size() != 1)
        return;
    const bool prefer_last_helpcode = help_code[0] >= 'A' && help_code[0] <= 'Z';
    const string normalized_help_code(1, static_cast<char>(std::tolower(static_cast<unsigned char>(help_code[0]))));
    vector<ShuangpinDictionary::WordItem> first_helpcode_matched_list;
    vector<ShuangpinDictionary::WordItem> last_helpcode_matched_list;
    vector<ShuangpinDictionary::WordItem> left_helpcode_matched_list; // 被筛完之后剩下的

    for (const auto &cand : candidate_list)
    {
        switch (HelpcodeUtils::match_single_helpcode(cand.word, normalized_help_code, helpcodes_.get()))
        {
        case HelpcodeUtils::SingleHelpcodeMatch::First:
            first_helpcode_matched_list.push_back(cand);
            break;
        case HelpcodeUtils::SingleHelpcodeMatch::Last:
            last_helpcode_matched_list.push_back(cand);
            break;
        case HelpcodeUtils::SingleHelpcodeMatch::Both:
            (prefer_last_helpcode ? last_helpcode_matched_list : first_helpcode_matched_list).push_back(cand);
            break;
        case HelpcodeUtils::SingleHelpcodeMatch::None:
            left_helpcode_matched_list.push_back(cand);
            break;
        }
    }

    /* 辅助码筛出来的候选列表 */
    if (prefer_last_helpcode)
    {
        result_list.insert(result_list.end(), last_helpcode_matched_list.begin(), last_helpcode_matched_list.end());
        result_list.insert(result_list.end(), first_helpcode_matched_list.begin(), first_helpcode_matched_list.end());
    }
    else
    {
        result_list.insert(result_list.end(), first_helpcode_matched_list.begin(), first_helpcode_matched_list.end());
        result_list.insert(result_list.end(), last_helpcode_matched_list.begin(), last_helpcode_matched_list.end());
    }
    /* 把原始拼音的候选列表加到辅助码模式的候选列表后面 */
    const std::string original_segmentation = ShuangpinUtil::pinyin_segmentation(pinyin_sequence, profile_);
    auto original_candidate_list = generateSeries(pinyin_sequence, original_segmentation);
    result_list.insert(result_list.end(), original_candidate_list.begin(), original_candidate_list.end());
    /* 把剩下的候选列表加到辅助码模式的候选列表后面 */
    result_list.insert(result_list.end(), left_helpcode_matched_list.begin(), left_helpcode_matched_list.end());
}

/**
 * @brief Filter with double help codes
 *
 * Rules:
 *   - For single Hanzi, we consider its first and last part
 *   - For Multi Hanzi, we consider first Hanzi's first part and last Hanzi's first part
 *
 * e.g. 阿: 阿's helpcode is ek, when we type aaek, 阿 will be filtered.
 *      阿姨: 阿's helpcode is ek, 姨's helpcode is nr, when we type aayien, 阿姨 will be filtered.
 *
 * @param candidate_list
 * @param result_list
 * @param help_codes
 */
void ShuangpinDictionary::filter_with_double_helpcodes(               //
    const std::vector<ShuangpinDictionary::WordItem> &candidate_list, //
    std::vector<ShuangpinDictionary::WordItem> &result_list,          //
    const std::string &help_codes                                     //
)
{
    if (candidate_list.empty())
        return;

    for (const auto &cand : candidate_list)
    {
        if (HelpcodeUtils::matches_double_helpcodes(cand.word, help_codes, helpcodes_.get()))
        {
            result_list.push_back(cand);
        }
    }
}

/**
 * @brief
 *
 * Note: Use the help code only in standard cases—that is, when the shuangpin part is complete.
 *
 * @param pure_pinyin
 * @param pure_pinyin_segmentation
 * @param pinyin_sequence
 * @param help_codes
 * @return vector<ShuangpinDictionary::WordItem>
 */
vector<ShuangpinDictionary::WordItem> ShuangpinDictionary::generate_with_helpcodes( //
    const string &pure_pinyin,                                                      //
    const string &pure_pinyin_segmentation,                                         //
    const string &pinyin_sequence,                                                  //
    const string &help_codes                                                        //
)
{
    vector<WordItem> candidate_list;
    const bool reversed_single_helpcode = help_codes.size() == 1 && help_codes[0] >= 'A' && help_codes[0] <= 'Z';
    // 句中辅助码约束同样进键，理由见 generateSeries。
    const std::string single_cache_key = pinyin_sequence + syllable_helpcodes_signature_;
    // Check cache first
    if (help_codes.size() == 1)
    {
        auto &single_helpcode_cache = reversed_single_helpcode ? _cached_buffer_sgl_reversed : _cached_buffer_sgl;
        if (single_helpcode_cache.find(single_cache_key))
        {
            reset_cache_if_database_changed();
            if (const auto *refreshed = single_helpcode_cache.find(single_cache_key))
            {
                return *refreshed;
            }
        }
    }
    else if (help_codes.size() == 2)
    {
        const auto cache_key = double_helpcode_cache_key(pinyin_sequence, help_codes) + syllable_helpcodes_signature_;
        if (_cached_buffer_dbl.find(cache_key))
        {
            reset_cache_if_database_changed();
            if (const auto *cached = _cached_buffer_dbl.find(cache_key))
            {
                return *cached;
            }
        }
    }

    candidate_list = generateSeries(pure_pinyin, pure_pinyin_segmentation);
    vector<WordItem> result_list;
    // Filter with help codes
    if (help_codes.size() == 1)
    {
        filter_with_single_helpcode( //
            candidate_list,          //
            result_list,             //
            help_codes,              //
            pinyin_sequence          //
        );
        auto &single_helpcode_cache = reversed_single_helpcode ? _cached_buffer_sgl_reversed : _cached_buffer_sgl;
        single_helpcode_cache.insert(single_cache_key, result_list);
    }
    else if (help_codes.size() == 2)
    {
        filter_with_double_helpcodes( //
            candidate_list,           //
            result_list,              //
            help_codes                //
        );
        _cached_buffer_dbl.insert(
            double_helpcode_cache_key(pinyin_sequence, help_codes) + syllable_helpcodes_signature_, result_list);
    }
    return result_list;
}

std::string VkCodeToChar(ImeKeyCode vk)
{
    if (vk >= 'A' && vk <= 'Z')
    {
        return std::string(1, char(vk + ('a' - 'A')));
    }
    if (vk >= '0' && vk <= '9')
    {
        return std::string(1, char(vk));
    }
    switch (vk)
    {
    case ImeKey::Space:
        return " ";
    case ImeKey::Tab:
        return "\t";
    case ImeKey::Return:
        return "\n";
    default:
        return "";
    }
}

std::string VkSequenceToString(const ImeKeyCode *vk_codes, size_t count)
{
    std::string result;
    for (size_t i = 0; i < count; ++i)
    {
        result += VkCodeToChar(vk_codes[i]);
    }
    return result;
}

void ShuangpinDictionary::generate_for_single_char(vector<ShuangpinDictionary::WordItem> &candidate_list, string code)
{
    constexpr int kInitialCandidateLimit = 24;
    candidate_list = query_initial_from_quanpin_database(code, kInitialCandidateLimit);
}

bool ShuangpinDictionary::expand_initial_candidates()
{
    return expand_initial_candidates(_pinyin_sequence, _cur_candidate_list);
}

bool ShuangpinDictionary::can_expand_initial_candidates(const std::string &code,
                                                        const std::vector<WordItem> &candidates)
{
    if (code.size() != 1)
    {
        return false;
    }
    // 与 generate_for_single_char 的截断条数一致：正好截满才可能还有，展开过之后数目就不再是 24。
    constexpr size_t kInitialCandidateLimit = 24;
    return static_cast<size_t>(std::count_if(candidates.begin(), candidates.end(), [&](const WordItem &item) {
               return item.source == CandidateSource::Database && item.pinyin == code;
           })) == kInitialCandidateLimit;
}

bool ShuangpinDictionary::expand_initial_candidates(const std::string &code, std::vector<WordItem> &candidates,
                                                    const std::string &series_cache_key)
{
    if (!can_expand_initial_candidates(code, candidates))
    {
        return false;
    }

    const auto is_limited_initial = [&](const WordItem &item) {
        return item.source == CandidateSource::Database && item.pinyin == code;
    };
    const size_t limited_count =
        static_cast<size_t>(std::count_if(candidates.begin(), candidates.end(), is_limited_initial));

    auto expanded = query_initial_from_quanpin_database(code, INT_MAX);
    if (expanded.size() <= limited_count)
    {
        return false;
    }

    std::vector<WordItem> merged;
    merged.reserve(candidates.size() - limited_count + expanded.size());
    bool inserted = false;
    for (auto &item : candidates)
    {
        if (is_limited_initial(item))
        {
            if (!inserted)
            {
                merged.insert(merged.end(), expanded.begin(), expanded.end());
                inserted = true;
            }
            continue;
        }
        merged.push_back(std::move(item));
    }

    candidates = std::move(merged);
    if (!series_cache_key.empty())
    {
        _cached_buffer_series.insert(series_cache_key, candidates);
    }
    return true;
}

/**
 * @brief
 *
 * @param vk
 * @return int
 */
int ShuangpinDictionary::handleVkCode(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch)
{
    if (vk != 0)
    { /* 0 是造词过程中的 dummy code */
        _kb_input_sequence.push_back(vk);
        if (vk >= 'A' && vk <= 'Z')
        {
            const char lowerAlpha = static_cast<char>(vk + ('a' - 'A'));
            _pinyin_sequence += lowerAlpha;

            // Prefer the real typed character from TSF side so CapsLock/Shift combinations are preserved.
            if (wch >= u'A' && wch <= u'Z')
            {
                _pinyin_sequence_with_cases += static_cast<char>(wch);
            }
            else if (wch >= u'a' && wch <= u'z')
            {
                _pinyin_sequence_with_cases += static_cast<char>(wch);
            }
            else if (modifiers_down >> 0 & 1u)
            {
                // Fallback for callers that don't provide wch.
                _pinyin_sequence_with_cases += static_cast<char>(vk);
            }
            else
            {
                _pinyin_sequence_with_cases += lowerAlpha;
            }
        }
        else if (ShuangpinProfileUsesSemicolonFinal(profile_) && vk == ImeKey::Semicolon && wch == u';' &&
                 _pinyin_sequence.size() % 2 == 1)
        {
            _pinyin_sequence += ';';
            _pinyin_sequence_with_cases += ';';
        }
        else if (vk == ImeKey::Space || (vk >= '0' && vk <= '9') || vk == ImeKey::Return || vk == ImeKey::Shift ||
                 vk == ImeKey::Escape)
        {
            if (vk == ImeKey::Return || vk == ImeKey::Shift || vk == ImeKey::Escape)
            { /* 空格键和数字键不要清理状态，因为可能会触发造词 */
                // Clear state
                reset_state();
            }
            return 0;
        }
        else if (vk == ImeKey::Tab)
        {
            return 0;
        }
        else if (vk == ImeKey::Backspace)
        {
            if (_pinyin_sequence.size() > 0)
            {
                _pinyin_sequence = _pinyin_sequence.substr(0, _pinyin_sequence.size() - 1);
                _pinyin_sequence_with_cases =
                    _pinyin_sequence_with_cases.substr(0, _pinyin_sequence_with_cases.size() - 1);
            }
        }
    }

    //
    // We do not handle other keys currently
    //

    /* 初始状态 */
    _pure_pinyin_sequence = _pinyin_sequence;

    /* Whether in full help mode */
    _is_full_help_mode = ShuangpinUtil::IsFullHelpMode(_pinyin_sequence_with_cases, profile_);
    if (_is_full_help_mode)
    {
        _help_mode_raw_pos = _pinyin_sequence.size() - 2;
    }
    else
    {
        _help_mode_raw_pos = 0;
    }

    /* Generate candidate list */
    if (_is_full_help_mode)
    { // 全码辅助，结果只包含根据辅助码筛出来的候选词部分
        _pure_pinyin_sequence = _pinyin_sequence.substr(0, _help_mode_raw_pos);
        _pinyin_segmentation = ShuangpinUtil::pinyin_segmentation(_pure_pinyin_sequence, profile_);
        _pinyin_helpcodes = ShuangpinUtil::GetFullHelpCodes(_pinyin_sequence_with_cases);
        _cur_candidate_list = generate_with_helpcodes( //
            _pure_pinyin_sequence,                     //
            _pinyin_segmentation,                      //
            _pinyin_sequence,                          //
            _pinyin_helpcodes                          //
        );
    }
    else
    {
        // 不是全码辅助的情况：
        //   1. 奇数长度拼音序列，且双拼部分是完整的拼音，需要触发辅助码
        //   2. 偶数长度拼音序列，不需要触发辅助码
        if (_pinyin_sequence.size() % 2 == 1 && _pinyin_sequence.size() > 1)
        { /* 1. 奇数长度拼音序列 */
            _pure_pinyin_sequence = _pinyin_sequence.substr(0, _pinyin_sequence.size() - 1);
            _pinyin_segmentation = ShuangpinUtil::pinyin_segmentation(_pure_pinyin_sequence, profile_);
            if (ShuangpinUtil::is_all_complete_pinyin(_pure_pinyin_sequence, _pinyin_segmentation))
            { /* 双拼部分是完整的拼音，需要触发辅助码 */
                _pure_pinyin_sequence = _pinyin_sequence.substr(0, _pinyin_sequence.size() - 1);
                _pinyin_segmentation = ShuangpinUtil::pinyin_segmentation(_pure_pinyin_sequence, profile_);
                _pinyin_helpcodes = _pinyin_sequence_with_cases.substr(_pinyin_sequence_with_cases.size() - 1, 1);
                _cur_candidate_list = generate_with_helpcodes( //
                    _pure_pinyin_sequence,                     //
                    _pinyin_segmentation,                      //
                    _pinyin_sequence,                          //
                    _pinyin_helpcodes                          //
                );
            }
            else
            { /* 依然使用纯拼音，不触发辅助码模式 */
                _pinyin_segmentation = ShuangpinUtil::pinyin_segmentation(_pinyin_sequence, profile_);
                _cur_candidate_list = generateSeries(_pinyin_sequence, _pinyin_segmentation);
            }
        }
        else
        { /* 偶数长度拼音序列，不需要触发辅助码 */
            _pinyin_segmentation = ShuangpinUtil::pinyin_segmentation(_pinyin_sequence, profile_);
            _cur_candidate_list = generateSeries(_pinyin_sequence, _pinyin_segmentation);
        }
    }

    _pinyin_segmentation = ShuangpinUtil::pinyin_segmentation(_pinyin_sequence, profile_);

    return 0;
}

std::string ShuangpinDictionary::get_quanpin() const
{

    string quanpin_str = ShuangpinUtil::convert_seg_shuangpin_to_seg_complete_pinyin(_pinyin_segmentation, profile_);
    quanpin_str.erase(std::remove(quanpin_str.begin(), quanpin_str.end(), '\''), quanpin_str.end());
    return quanpin_str;
}

std::string ShuangpinDictionary::get_quanpin_seg() const
{
    string quanpin_str = ShuangpinUtil::convert_seg_shuangpin_to_seg_complete_pinyin(_pinyin_segmentation, profile_);
    return quanpin_str;
}

vector<ShuangpinDictionary::WordItem> ShuangpinDictionary::generate_for_creating_word(const string code)
{
    return select_complete_data(quanpin_db_, build_quanpin_sql_for_creating_word(code));
}

int ShuangpinDictionary::create_word(string pinyin, string word)
{
    return create_word_from_quanpin(shuangpin::normalize_input_with_delimiters(pinyin, profile_), std::move(word));
}

int ShuangpinDictionary::create_word_from_quanpin(string pinyin, string word)
{
    const auto segments = quanpin::split_segments(pinyin);
    const size_t han_count = HelpcodeUtils::count_han_chars(word);
    if (segments.empty() || segments.size() != han_count ||
        std::any_of(segments.begin(), segments.end(), [](const std::string &segment) {
            return segment.empty() || !quanpin::is_complete_pinyin_input(segment);
        }))
    {
        return ERROR_CODE;
    }

    pinyin = quanpin::join_segments(segments);
    const string jp = quanpin::segments_to_jianpin(segments);
    if (!do_validate(pinyin, jp, word))
    {
        return ERROR_CODE;
    }
    if (check_data(quanpin_db_, build_quanpin_sql_for_checking_word(pinyin, word)))
    {
        return OK;
    }
    if (insert_data(quanpin_db_, build_quanpin_sql_for_inserting_word(pinyin, jp, word)) != OK)
    {
        return ERROR_CODE;
    }
    (void)user_dictionary::record_user_insert(metasequoia::path_to_utf8(paths_.user(metasequoia::assets::user_journal)),
                                              user_dictionary::DictionaryKind::Pinyin, pinyin, word, 10000);
    /* 插入新词之后要清理缓存 */
    reset_cache();
    return OK;
}

int ShuangpinDictionary::update_data(sqlite3 *target_db, const std::string &sql_str)
{
    if (target_db == nullptr)
    {
        return ERROR_CODE;
    }
    char *errmsg = nullptr;
    int exit = sqlite3_exec(target_db, sql_str.c_str(), nullptr, nullptr, &errmsg);
    if (exit != SQLITE_OK)
    {
        (void)0;
        sqlite3_free(errmsg);
        return ERROR_CODE;
    }
    return OK;
}

int ShuangpinDictionary::delete_data(sqlite3 *target_db, const std::string &sql_str)
{
    if (target_db == nullptr)
    {
        return ERROR_CODE;
    }
    char *errmsg = nullptr;
    int exit = sqlite3_exec(target_db, sql_str.c_str(), nullptr, nullptr, &errmsg);
    if (exit != SQLITE_OK)
    {
        (void)0;
        sqlite3_free(errmsg);
        return ERROR_CODE;
    }
    return OK;
}

int ShuangpinDictionary::update_weight_by_word(string word)
{
    return update_weight_by_pinyin_and_word(get_quanpin(), std::move(word));
}

int ShuangpinDictionary::update_weight_by_pinyin_and_word(string pinyin, string word)
{
    const auto direct_cuts = quanpin::cut_pinyin_by_mode(remove_delimiters(pinyin), "correction", true);
    // 只有“完整合法拼读”的直切才可信：correction 模式的 greedy 后备允许单声母
    // 等不完整段，会把双拼原始串（如 qbtmuo → q'b't'mu'o）切成恰好能拼回原串
    // 的碎片，导致归一化被跳过、置顶更新落在错误 key 上静默失败（0 行更新
    // 仍返回 OK）。完整拼读检查与 delete 路径的段数-字数比对语义一致。
    if (direct_cuts.empty() || !quanpin::has_only_complete_pinyin_segments(direct_cuts.front()) ||
        remove_delimiters(quanpin::join_segments(direct_cuts.front())) != remove_delimiters(pinyin))
    {
        pinyin = normalize_shuangpin_to_quanpin_input(pinyin);
    }
    const auto cuts = quanpin::cut_pinyin_by_mode(remove_delimiters(pinyin), "correction", true);
    if (cuts.empty())
        return ERROR_CODE;
    auto segments = cuts.front();
    const size_t han_count = HelpcodeUtils::count_han_chars(word);
    if (segments.size() > han_count)
        segments.resize(han_count);
    const std::string normalized = quanpin::join_segments(segments);
    if (update_data(quanpin_db_, build_quanpin_sql_for_updating_word(normalized, word)) != OK)
    {
        return ERROR_CODE;
    }
    (void)user_dictionary::record_pinyin_upsert_from_database(
        quanpin_db_path_, normalized, word, metasequoia::path_to_utf8(paths_.user(metasequoia::assets::user_journal)));
    reset_cache();
    return OK;
}

int ShuangpinDictionary::delete_by_pinyin_and_word(string pinyin, string word)
{
    const auto direct_cuts = quanpin::cut_pinyin_by_mode(remove_delimiters(pinyin), "correction", true);
    const auto normalized_shuangpin = normalize_shuangpin_to_quanpin_input(pinyin);
    const auto shuangpin_cuts = quanpin::cut_pinyin_by_mode(normalized_shuangpin, "correction", true);
    const size_t han_count = HelpcodeUtils::count_han_chars(word);
    const bool direct_key_matches_word = !direct_cuts.empty() && direct_cuts.front().size() == han_count;
    const bool shuangpin_key_matches_word = !shuangpin_cuts.empty() && shuangpin_cuts.front().size() == han_count;
    if ((!direct_key_matches_word && shuangpin_key_matches_word) ||
        (direct_cuts.empty() ||
         remove_delimiters(quanpin::join_segments(direct_cuts.front())) != remove_delimiters(pinyin)))
    {
        pinyin = normalized_shuangpin;
    }
    const auto cuts = quanpin::cut_pinyin_by_mode(remove_delimiters(pinyin), "correction", true);
    if (cuts.empty())
        return ERROR_CODE;
    const std::string normalized = quanpin::join_segments(cuts.front());
    if (!user_dictionary::delete_dictionary_candidate(
            quanpin_db_path_, metasequoia::path_to_utf8(paths_.user(metasequoia::assets::user_journal)),
            user_dictionary::DictionaryKind::Pinyin, normalized, word))
        return ERROR_CODE;
    reset_cache();
    return OK;
}

// generate_with_seg_pinyin

ShuangpinDictionary::~ShuangpinDictionary()
{
    for (auto &[sql, stmt] : quanpin_statement_cache_)
    {
        if (stmt != nullptr)
        {
            sqlite3_finalize(stmt);
        }
    }
    if (quanpin_db_)
    {
        sqlite3_close(quanpin_db_);
    }
}

vector<ShuangpinDictionary::WordItem> ShuangpinDictionary::query_from_quanpin_database(
    const std::string &pinyin_sequence, const std::string &pinyin_segmentation)
{
    if (quanpin_db_ == nullptr || pinyin_segmentation.empty())
    {
        return {};
    }

    const std::string quanpin_segmentation =
        ShuangpinUtil::convert_seg_shuangpin_to_seg_complete_pinyin(pinyin_segmentation, profile_);
    const auto segments = quanpin::split_segments(quanpin_segmentation);
    if (segments.empty())
    {
        return {};
    }

    std::vector<WordItem> candidate_list;
    try
    {
        const auto flat_items = quanpin::query_segments_keyed_flat(segments, quanpin_db_, quanpin_statement_cache_,
                                                                   INT_MAX, quanpin::QuerySource::Shuangpin);
        candidate_list.reserve(flat_items.size());
        for (const auto &item : flat_items)
        {
            candidate_list.emplace_back(pinyin_sequence, item.value, item.weight, CandidateSource::Database, item.key);
        }
    }
    catch (const std::exception &)
    {
        (void)0;
    }

    return candidate_list;
}

std::optional<WordItem> ShuangpinDictionary::find_candidate(const std::string &key, const std::string &value)
{
    const std::string table = quanpin::build_table_name(quanpin::split_segments(key));
    if (!quanpin_db_ || table.empty())
        return std::nullopt;
    sqlite3_stmt *stmt = nullptr;
    const std::string sql = "SELECT weight FROM \"" + table + "\" WHERE key=?1 AND value=?2 LIMIT 1";
    if (sqlite3_prepare_v2(quanpin_db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, sqlite3_finalize);
    if (sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_ROW)
        return std::nullopt;
    return WordItem(key, value, sqlite3_column_int64(stmt, 0), CandidateSource::Database, key);
}

vector<ShuangpinDictionary::WordItem> ShuangpinDictionary::query_initial_from_quanpin_database(const std::string &code,
                                                                                               int limit)
{
    if (quanpin_db_ == nullptr || code.size() != 1 || code.front() < 'a' || code.front() > 'z')
    {
        return {};
    }

    const std::string initial = ShuangpinUtil::convert_seg_shuangpin_to_seg_complete_pinyin(code, profile_);
    const auto rows = quanpin::query_initial(quanpin_db_, initial, limit);

    vector<WordItem> candidate_list;
    candidate_list.reserve(rows.size());
    for (const auto &item : rows)
    {
        candidate_list.emplace_back(code, item.value, item.weight, CandidateSource::Database, item.key);
    }
    return candidate_list;
}

vector<ShuangpinDictionary::WordItem> ShuangpinDictionary::select_complete_data(sqlite3 *target_db,
                                                                                const std::string &sql_str)
{
    vector<ShuangpinDictionary::WordItem> candidateList;
    if (target_db == nullptr)
    {
        return candidateList;
    }
    sqlite3_stmt *stmt;
    int exit = sqlite3_prepare_v2(target_db, sql_str.c_str(), -1, &stmt, 0);
    if (exit != SQLITE_OK)
    {
        (void)0;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        candidateList.emplace_back(                                               //
            string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0))), // key
            string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2))), // value
            sqlite3_column_int64(stmt, 3), CandidateSource::Database,
            string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)))); // canonical key
    }
    sqlite3_finalize(stmt);
    return candidateList;
}

int ShuangpinDictionary::check_data(sqlite3 *target_db, const std::string &sql_str)
{
    if (target_db == nullptr)
    {
        return false;
    }
    sqlite3_stmt *stmt;
    int exit = sqlite3_prepare_v2(target_db, sql_str.c_str(), -1, &stmt, 0);
    if (exit != SQLITE_OK)
    {
        (void)0;
    }
    bool exists = false;
    exit = sqlite3_step(stmt);
    if (exit == SQLITE_ROW)
    {
        exists = true;
    }
    sqlite3_finalize(stmt);
    return exists;
}

int ShuangpinDictionary::insert_data(sqlite3 *target_db, const std::string &sql_str)
{
    if (target_db == nullptr)
    {
        return ERROR_CODE;
    }
    char *errmsg = nullptr;
    int exit = sqlite3_exec(target_db, sql_str.c_str(), nullptr, nullptr, &errmsg);
    if (exit != SQLITE_OK)
    {
        (void)0;
        sqlite3_free(errmsg);
        return ERROR_CODE;
    }
    return OK;
}

std::string ShuangpinDictionary::normalize_shuangpin_to_quanpin_segmentation(const std::string &pinyin) const
{
    if (pinyin.empty())
    {
        return {};
    }

    return shuangpin::normalize_input_with_delimiters(pinyin, profile_);
}

std::string ShuangpinDictionary::normalize_shuangpin_to_quanpin_input(const std::string &pinyin) const
{
    return remove_delimiters(normalize_shuangpin_to_quanpin_segmentation(pinyin));
}

std::string ShuangpinDictionary::build_quanpin_sql_for_creating_word(const std::string &pinyin) const
{
    const std::string normalized = normalize_shuangpin_to_quanpin_input(pinyin);
    const auto cuts = quanpin::cut_pinyin_by_mode(normalized, "correction", true);
    if (cuts.empty())
    {
        return "";
    }

    std::string sql;
    for (size_t i = 1; i <= cuts.front().size(); ++i)
    {
        std::vector<std::string> partial(cuts.front().begin(), cuts.front().begin() + i);
        const std::string key = quanpin::join_segments(partial);
        const std::string table = quanpin::build_table_name(partial);
        const std::string each =
            fmt::format("select * from(select * from {} where key = '{}' order by weight desc)", table, key);
        sql = sql.empty() ? each : each + " union all " + sql;
    }
    return sql;
}

std::string ShuangpinDictionary::build_quanpin_sql_for_checking_word(const std::string &key,
                                                                     const std::string &value) const
{
    const auto cuts = quanpin::cut_pinyin_by_mode(key, "correction", true);
    if (cuts.empty())
    {
        return "";
    }
    const std::string table = quanpin::build_table_name(cuts.front());
    return fmt::format("select 1 from {} where key = '{}' and value = '{}';", table, escape_sql_text(key),
                       escape_sql_text(value));
}

std::string ShuangpinDictionary::build_quanpin_sql_for_inserting_word(const std::string &key, const std::string &jp,
                                                                      const std::string &value) const
{
    const auto cuts = quanpin::cut_pinyin_by_mode(key, "correction", true);
    if (cuts.empty())
    {
        return "";
    }
    const std::string table = quanpin::build_table_name(cuts.front());
    return fmt::format("insert into {} (key, jp, value, weight) values ('{}', '{}', '{}', '{}');", table,
                       escape_sql_text(key), escape_sql_text(jp), escape_sql_text(value), 10000);
}

std::string ShuangpinDictionary::build_quanpin_sql_for_updating_word(const std::string &word) const
{
    return build_quanpin_sql_for_updating_word(get_quanpin(), word);
}

std::string ShuangpinDictionary::build_quanpin_sql_for_updating_word(std::string pinyin, const std::string &word) const
{
    // 调用方约定传入 canonical 全拼（update_weight 已归一化；get_quanpin 本身就
    // 是全拼）。这里不能再过 normalize_shuangpin_to_quanpin_input：双拼 profile
    // 会把它当作双拼码重切，qin'tian'shuo 被搧成 qi'n'ti'an'sang'shuo，UPDATE
    // 永远落在错误 key 上静默失败（0 行更新仍返回 OK）。
    const auto cuts = quanpin::cut_pinyin_by_mode(remove_delimiters(pinyin), "correction", true);
    if (cuts.empty())
    {
        return "";
    }

    size_t han_cnt = HelpcodeUtils::count_han_chars(word);
    auto segments = cuts.front();
    if (segments.size() > han_cnt)
    {
        segments.resize(han_cnt);
    }

    pinyin = quanpin::join_segments(segments);
    const std::string jp = quanpin::segments_to_jianpin(segments);
    if (!do_validate(pinyin, jp, word))
    {
        return "";
    }

    const std::string table = quanpin::build_table_name(segments);
    return fmt::format("update {0} set weight = ( select MAX(weight) + 1 from {0} AS sub where sub.key = '{1}') "
                       "where key = '{1}' and value = '{2}';",
                       table, escape_sql_text(pinyin), escape_sql_text(word));
}

std::string ShuangpinDictionary::build_quanpin_sql_for_deleting_canonical_word(const std::string &canonical_pinyin,
                                                                               const std::string &word) const
{
    const auto cuts = quanpin::cut_pinyin_by_mode(canonical_pinyin, "correction", true);
    if (cuts.empty())
    {
        return "";
    }

    const std::string normalized = quanpin::join_segments(cuts.front());
    const std::string jp = quanpin::segments_to_jianpin(cuts.front());
    if (!do_validate(normalized, jp, word))
    {
        return "";
    }

    return fmt::format("delete from {} where key = '{}' and value = '{}';", quanpin::build_table_name(cuts.front()),
                       escape_sql_text(normalized), escape_sql_text(word));
}

bool ShuangpinDictionary::do_validate(string key, string jp, string value) const
{
    const std::string pure_key = remove_delimiters(key);
    if (pure_key.empty())
    {
        return false;
    }

    const size_t han_count = HelpcodeUtils::count_han_chars(value);
    if (jp.size() != han_count)
    {
        return false;
    }

    const auto cuts = quanpin::cut_pinyin_by_mode(pure_key, "correction", true);
    if (!cuts.empty())
    {
        return cuts.front().size() == han_count;
    }

    return pure_key.size() % 2 == 0 && pure_key.size() == han_count * 2;
}

string ShuangpinDictionary::search_sentence_from_ime_engine(const string &user_pinyin)
{
    return decoder_.sentence(user_pinyin);
}

void ShuangpinDictionary::reset_state()
{
    _is_full_help_mode = false;
    _help_mode_raw_pos = 0;
    _kb_input_sequence.clear();
    _pinyin_sequence = "";
    _pinyin_sequence_with_cases = "";
    _pure_pinyin_sequence = "";
    _pinyin_segmentation = "";
    _help_codes_sequence.fill(0);
    _cur_candidate_list.clear();
    _cur_page_candidate_list.clear();
}

void ShuangpinDictionary::reset_cache()
{
    _cached_buffer.clear();
    _cached_buffer_sgl.clear();
    _cached_buffer_sgl_reversed.clear();
    _cached_buffer_dbl.clear();
    _cached_buffer_series.clear();
    direct_span_cache_.clear();
    if (direct_resolver_)
    {
        direct_resolver_->reset_cache();
    }
}

void ShuangpinDictionary::set_fuzzy_pinyin(metasequoia::FuzzyPinyinOptions options)
{
    if (fuzzy_pinyin_.rules == options.rules)
    {
        return;
    }
    fuzzy_pinyin_ = options;
    reset_cache();
}

quanpin::WordLatticeLookup ShuangpinDictionary::lattice_lookup(int limit)
{
    if (!char_reading_totals_)
    {
        char_reading_totals_ = quanpin::load_char_reading_totals(quanpin_db_);
    }
    auto lookup = quanpin::make_lattice_db_lookup(quanpin_db_, quanpin_statement_cache_, limit, fuzzy_pinyin_,
                                                  char_reading_totals_);
    if (!direct_span_cache_enabled_)
    {
        return lookup;
    }
    return [this, lookup = std::move(lookup), limit](const quanpin::Segments &span) {
        // 超过上限整体清掉，免得一次长时间输入把内存撑大。
        constexpr std::size_t kDirectSpanCacheLimit = 8192;
        std::string key = quanpin::join_segments(span);
        key.push_back('#');
        key += std::to_string(limit);
        auto found = direct_span_cache_.find(key);
        if (found == direct_span_cache_.end())
        {
            if (direct_span_cache_.size() >= kDirectSpanCacheLimit)
            {
                direct_span_cache_.clear();
            }
            found = direct_span_cache_.emplace(std::move(key), lookup(span)).first;
        }
        return found->second;
    };
}

bool ShuangpinDictionary::resolve_direct_helpcode(QueryRequest &request)
{
    reset_cache_if_database_changed();
    if (!direct_resolver_)
    {
        direct_resolver_ = std::make_unique<direct_helpcode::Resolver>(profile_);
    }
    // 与 generateSeries 的词格同一套查询：普通跨度在 SQL 里截到 32 行，带辅码约束的跨度不截，
    // 筛完约束再截（生僻字才筛得出来）。
    constexpr int kSpanLimit = 32;
    constexpr int kConstrainedSpanLimit = 4096;
    // 解析器只在直接辅助码开着时跑，查到的行进共用缓存，随后的 generateSeries 直接复用。
    direct_span_cache_enabled_ = true;
    auto lookup = lattice_lookup(kSpanLimit);
    auto constrained_lookup = lattice_lookup(kConstrainedSpanLimit);
    direct_helpcode::ResolveContext context;
    context.lookup = [lookup, constrained_lookup](const quanpin::Segments &span, bool constrained) {
        return constrained ? constrained_lookup(span) : lookup(span);
    };
    context.single_char_rows = [this]() { return query_single_char_rows(); };
    context.keymap = helpcodes_ ? helpcodes_.get() : &HelpcodeUtils::helpcode_keymap();
    context.options.language_model = language_model_;
    if (collocation_db_ != nullptr && collocation_db_->valid() && sentence_association_.collocation_weight != 0.0)
    {
        context.options.collocation_scorer = [db = collocation_db_, config = sentence_association_.collocation_grammar](
                                                 std::string_view tail, std::string_view word, bool is_rear) {
            return db->query(std::string(tail), std::string(word), is_rear, config) * quanpin::kLnToLog10;
        };
        context.options.collocation_weight = sentence_association_.collocation_weight;
    }
    const bool rewritten = direct_resolver_->resolve(request, context);
    // 记下解出的整句，键与 generateSeries 的缓存键同义（请求最终的 raw_input + 约束签名），见那边的词格块。
    direct_sentence_.reset();
    if (const auto *sentence = direct_resolver_->resolved_sentence())
    {
        quanpin::LatticePath path;
        path.sentence = sentence->sentence;
        path.key = sentence->key;
        path.log_prob = sentence->log_prob;
        path.words = sentence->words;
        direct_sentence_.emplace(request.raw_input + syllable_helpcodes_signature(request.syllable_helpcodes),
                                 std::move(path));
    }
    return rewritten;
}

std::vector<std::pair<std::string, std::string>> ShuangpinDictionary::query_single_char_rows()
{
    // 单字都在 tbl_1_<首字母> 里（表名规则见 contracts/dictionary/format.json），逐表全扫一遍。
    std::vector<std::pair<std::string, std::string>> rows;
    if (quanpin_db_ == nullptr)
    {
        return rows;
    }
    for (char letter = 'a'; letter <= 'z'; ++letter)
    {
        const std::string sql = "SELECT key, value FROM " + quanpin::build_table_name({std::string(1, letter)});
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(quanpin_db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
        {
            sqlite3_finalize(statement);
            continue;
        }
        while (sqlite3_step(statement) == SQLITE_ROW)
        {
            const auto *key = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
            const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement, 1));
            if (key != nullptr && value != nullptr)
            {
                rows.emplace_back(key, value);
            }
        }
        sqlite3_finalize(statement);
    }
    return rows;
}

void ShuangpinDictionary::reset_sentence_cache()
{
    // 整句和上文词库重排在 generateSeries 里合入，辅助码的三个缓存从它的结果筛出来，一起清；
    // _cached_buffer 只是当前拼音的词库行，不含整句，留着。
    _cached_buffer_sgl.clear();
    _cached_buffer_sgl_reversed.clear();
    _cached_buffer_dbl.clear();
    _cached_buffer_series.clear();
}

void ShuangpinDictionary::set_sentence_association(const SentenceAssociationOptions &options)
{
    if (sentence_association_ == options)
    {
        return;
    }
    // 打字闸门每段输入只翻效果档的模型路径：那种改动只影响 series_cache_，
    // 见 QuanpinDictionary::set_sentence_association。
    const bool series_only = sentence_association_.differs_only_in_effect_tier_model(options);
    sentence_association_ = options;
    // 模型路径随开关一起变更时才重新解析，见 QuanpinDictionary::set_sentence_association。
    collocation_db_ =
        sentence_association_.collocation_model.empty()
            ? nullptr
            : gram::shared_gram_db(metasequoia::path_from_utf8(sentence_association_.collocation_model.c_str()));
    neural_desktop_model_ = neural::LazySentenceModel(sentence_association_.neural_desktop_model);
    // 用户下载的 sc.lm，见 QuanpinDictionary::set_sentence_association。
    language_model_ =
        &ngram::shared_language_model(sentence_association_.language_model.empty()
                                          ? paths_.resource(metasequoia::assets::language_model)
                                          : metasequoia::path_from_utf8(sentence_association_.language_model.c_str()));
    // 开关变了就清缓存，否则打开/关闭要等缓存过期才见效。见 generateSeries 的整句块。
    if (series_only)
    {
        reset_sentence_cache();
    }
    else
    {
        reset_cache();
    }
}

void ShuangpinDictionary::set_rescoring_context(const std::string &context)
{
    if (rescoring_context_ == context)
    {
        return;
    }
    rescoring_context_ = context;
    // 只有神经重排生效时上文才影响候选，理由见 QuanpinDictionary::set_rescoring_context。
    const bool rescoring_active =
        ((sentence_association_.neural_keyboard || sentence_association_.context_candidate_rerank) &&
         neural_keyboard_model_.get() != nullptr) ||
        (sentence_association_.neural_desktop && neural_desktop_model_.get() != nullptr);
    if (rescoring_active)
    {
        reset_sentence_cache();
    }
}

void ShuangpinDictionary::set_syllable_helpcodes(const SyllableHelpcodes &helpcodes)
{
    // 不清缓存：约束签名进了缓存键，换约束等于换键。
    syllable_helpcodes_ = helpcodes;
    syllable_helpcodes_signature_ = syllable_helpcodes_signature(helpcodes);
}

bool ShuangpinDictionary::accepts_syllable_char(const SyllableHelpcode &helpcode, const std::string &hanzi) const
{
    const auto &keymap = helpcodes_ ? *helpcodes_ : HelpcodeUtils::helpcode_keymap();
    const auto found = keymap.find(hanzi);
    return found != keymap.end() && syllable_helpcode_matches(helpcode, found->second);
}

bool ShuangpinDictionary::satisfies_syllable_helpcodes(const std::string &word) const
{
    if (syllable_helpcodes_.empty())
    {
        return true;
    }
    std::vector<std::string> chars;
    for (std::size_t pos = 0; pos < word.size();)
    {
        const std::size_t size = ShuangpinUtil::get_first_char_size(word.substr(pos, 4));
        chars.push_back(word.substr(pos, size));
        pos += size;
    }
    return std::all_of(syllable_helpcodes_.begin(), syllable_helpcodes_.end(), [&](const SyllableHelpcode &helpcode) {
        return helpcode.syllable >= chars.size() || accepts_syllable_char(helpcode, chars[helpcode.syllable]);
    });
}

void ShuangpinDictionary::filter_by_syllable_helpcodes(std::vector<WordItem> &candidates) const
{
    if (syllable_helpcodes_.empty())
    {
        return;
    }
    // 只筛按拼音出的汉字候选；英文、快捷短语之类的不是逐音节对应的，留着。
    const auto is_pinyin_hanzi = [](const WordItem &item) {
        switch (item.source)
        {
        case CandidateSource::Database:
        case CandidateSource::UserDatabase:
        case CandidateSource::Generated:
        case CandidateSource::Fallback:
        case CandidateSource::NeuralDesktop:
        case CandidateSource::NeuralKeyboard:
        // 云和 AI 拿到的是去掉反引号段的拼音，不知道约束，结果同样要核。
        case CandidateSource::CloudSuggestion:
        case CandidateSource::AiSuggestion:
            return HelpcodeUtils::count_han_chars(item.word) == HelpcodeUtils::count_utf8_chars(item.word);
        default:
            return false;
        }
    };
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&](const WordItem &item) {
                                        return is_pinyin_hanzi(item) && !satisfies_syllable_helpcodes(item.word);
                                    }),
                     candidates.end());
}

std::string ShuangpinDictionary::decode_google_sentence(const std::string &quanpin_segmentation)
{
    // 只有送进解码器的那一份换 ü 写法，调用方手里的 canonical 拼写不动。按音节整体替换，
    // 音节数与起点都不变，约束的位置可以直接在替换后的串上算。
    const std::string google_input = quanpin::to_google_spelling(quanpin_segmentation);
    if (syllable_helpcodes_.empty())
    {
        return search_sentence_from_ime_engine(google_input);
    }

    std::vector<std::size_t> syllable_starts{0};
    for (std::size_t i = 0; i < google_input.size(); ++i)
    {
        if (google_input[i] == '\'')
            syllable_starts.push_back(i + 1);
    }
    std::vector<metasequoia::DecoderCharConstraint> constraints;
    for (const auto &helpcode : syllable_helpcodes_)
    {
        if (helpcode.syllable >= syllable_starts.size())
            continue;
        constraints.push_back({syllable_starts[helpcode.syllable], [this, helpcode](const std::string &hanzi) {
                                   return accepts_syllable_char(helpcode, hanzi);
                               }});
    }
    std::string sentence = decoder_.sentence(google_input, constraints);
    // 约束让某一段走不通时解码器可能吐出残句，字数对不上或约束没满足就不出这一句。
    if (sentence.empty() || HelpcodeUtils::count_utf8_chars(sentence) != syllable_starts.size() ||
        !satisfies_syllable_helpcodes(sentence))
    {
        return {};
    }
    return sentence;
}

void ShuangpinDictionary::reset_cache_if_database_changed()
{
    if (quanpin_db_ == nullptr)
    {
        return;
    }
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(quanpin_db_, "PRAGMA data_version", -1, &statement, nullptr) != SQLITE_OK)
    {
        return;
    }
    if (sqlite3_step(statement) != SQLITE_ROW)
    {
        sqlite3_finalize(statement);
        return;
    }
    const sqlite3_int64 current_version = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    if (data_version_ >= 0 && current_version != data_version_)
    {
        reset_cache();
    }
    data_version_ = current_version;
}

int ShuangpinDictionary::insert_word_to_cached_buffer_series(const std::string &pinyin, const std::string &word,
                                                             CandidateSource source)
{
    (void)0;
    if (pinyin.empty() || word.empty())
    {
        return -1;
    }

    // 键与 generateSeries 一致：带上当前请求的句中辅助码签名。
    const std::string cache_key = pinyin + syllable_helpcodes_signature_;
    auto list = _cached_buffer_series.get(cache_key).value_or(std::vector<WordItem>{});

    // Keep at most one cloud/AI suggestion in the series cache for this key.
    if (source == CandidateSource::AiSuggestion || source == CandidateSource::CloudSuggestion)
    {
        list.erase(
            std::remove_if(list.begin(), list.end(), [source](const WordItem &item) { return item.source == source; }),
            list.end());
    }

    const auto exists = std::find_if(list.begin(), list.end(), [&](const WordItem &item) { return item.word == word; });
    if (exists == list.end())
    {
        if (list.empty())
        {
            list.emplace_back(pinyin, word, 1, source);
        }
        else
        {
            const size_t index = source == CandidateSource::AiSuggestion ? std::min<size_t>(2, list.size()) : 1;
            list.insert(list.begin() + index, WordItem(pinyin, word, 1, source));
        }
    }

    if (source == CandidateSource::CloudSuggestion)
    {
        const auto ai = std::find_if(list.begin(), list.end(),
                                     [](const WordItem &item) { return item.source == CandidateSource::AiSuggestion; });
        if (ai != list.end())
        {
            WordItem ai_item = std::move(*ai);
            list.erase(ai);
            list.insert(list.begin() + std::min<size_t>(2, list.size()), std::move(ai_item));
        }
    }

    _cached_buffer_series.insert(cache_key, list);
    return 0;
}

int ShuangpinDictionary::insert_word_to_active_helpcode_cache(const std::string &pinyin, const std::string &word,
                                                              CandidateSource source,
                                                              const std::string &double_helpcodes)
{
    auto insert_into_cache = [&](auto &cache, const std::string &cache_key) {
        if (auto opt = cache.get(cache_key))
        {
            auto list = opt.value();
            if (source == CandidateSource::AiSuggestion || source == CandidateSource::CloudSuggestion)
            {
                list.erase(std::remove_if(list.begin(), list.end(),
                                          [source](const WordItem &item) { return item.source == source; }),
                           list.end());
            }
            const auto exists =
                std::find_if(list.begin(), list.end(), [&](const WordItem &item) { return item.word == word; });
            if (exists == list.end())
            {
                if (list.size() >= 1)
                {
                    const size_t index = source == CandidateSource::AiSuggestion ? std::min<size_t>(2, list.size()) : 1;
                    list.insert(list.begin() + index, WordItem(pinyin, word, 1, source));
                }
                else
                {
                    list.emplace_back(pinyin, word, 1, source);
                }
            }
            cache.insert(cache_key, list);
            return true;
        }
        return false;
    };

    if (!double_helpcodes.empty())
    {
        return insert_into_cache(_cached_buffer_dbl,
                                 double_helpcode_cache_key(pinyin, double_helpcodes) + syllable_helpcodes_signature_)
                   ? 0
                   : -1;
    }
    const bool updated_single = insert_into_cache(_cached_buffer_sgl, pinyin + syllable_helpcodes_signature_);
    const bool updated_reversed_single =
        insert_into_cache(_cached_buffer_sgl_reversed, pinyin + syllable_helpcodes_signature_);
    return updated_single || updated_reversed_single ? 0 : -1;
}

bool ShuangpinDictionary::is_all_complete_pinyin()
{
    bool res = ShuangpinUtil::is_all_complete_pinyin(_pinyin_sequence, _pinyin_segmentation);
    return res;
}

bool ShuangpinDictionary::is_all_complete_pure_pinyin()
{
    bool res = ShuangpinUtil::is_all_complete_pinyin( //
        _pure_pinyin_sequence,                        //
        ShuangpinUtil::pinyin_segmentation(_pure_pinyin_sequence, profile_));
    return res;
}

std::string ShuangpinDictionary::get_pinyin_segmentation_with_cases()
{
    string res;
    int index = 0;

    if (_pinyin_segmentation.empty() || _pinyin_sequence_with_cases.empty())
        return res;

    string extracted_pinyin = "";
    for (size_t i = 0; i < _pinyin_segmentation.size(); ++i)
    {
        if (_pinyin_segmentation[i] == '\'')
        {
            continue;
        }
        else
        {
            extracted_pinyin += _pinyin_segmentation[i];
        }
    }

    if (extracted_pinyin != boost::algorithm::to_lower_copy(_pinyin_sequence_with_cases))
    {
        return res;
    }

    for (size_t i = 0; i < _pinyin_segmentation.size(); ++i)
    {
        if (_pinyin_segmentation[i] == '\'')
        {
            res += _pinyin_segmentation[i];
            continue;
        }
        else
        {
            if (_pinyin_segmentation[i] == _pinyin_sequence_with_cases[index])
            {
                res += _pinyin_segmentation[i];
            }
            else if (_pinyin_segmentation[i] == _pinyin_sequence_with_cases[index] + ('a' - 'A'))
            {
                res += _pinyin_sequence_with_cases[index];
            }
        }
        index += 1;
    }

    return res;
}
