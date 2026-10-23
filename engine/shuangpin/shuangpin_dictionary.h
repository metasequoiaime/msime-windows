#pragma once
#include "../common/helpcode_utils.h"
#include "../core/runtime_paths.h"
#include "../core/pinyin_decoder.h"
#include "../core/data_path.h"
#include "../contracts/assets/assets.h"

#include "../common/cache.h"
#include "../core/key_event.h"
#include "../core/word_item.h"
#include "../core/sentence_association_options.h"
#include "../core/syllable_helpcode.h"
#include "../neural/neural_decoder.h"
#include "../quanpin/lattice_rerank.h"
#include "../quanpin/quanpin_query.h"
#include "engine/ngram/language_model.h"
#include "shuangpin_profile.h"
#include <shared_mutex>
#include <array>
#include <vector>
#include <unordered_map>
#include <string>
#include <sqlite3.h>
#include <memory>
#include <optional>
#include <boost/algorithm/string.hpp>

struct QueryRequest;
namespace direct_helpcode
{
class Resolver;
}

class ShuangpinDictionary
{
  public:
    using WordItem = ::WordItem;

    static const int OK = 0;
    static const int ERROR_CODE = -1;

    std::vector<WordItem> generate(             //
        const std::string &pinyin_sequence,     //
        const std::string &pinyin_segmentation, //
        const std::string &cache_key = ""       //
    );
    std::vector<WordItem> generateSeries(       //
        const std::string &pinyin_sequence,     //
        const std::string &pinyin_segmentation, //
        const std::string &cache_key = ""       //
    );
    std::vector<WordItem> generate_with_helpcodes(   //
        const std::string &pure_pinyin,              //
        const std::string &pure_pinyin_segmentation, //
        const std::string &pinyin_sequence,          //
        const std::string &help_codes                //
    );
    bool expand_initial_candidates();
    // 只判断 expand_initial_candidates 的前置条件，不查库。
    static bool can_expand_initial_candidates(const std::string &code, const std::vector<WordItem> &candidates);
    bool expand_initial_candidates(const std::string &code, std::vector<WordItem> &candidates,
                                   const std::string &series_cache_key = {});
    std::optional<WordItem> find_candidate(const std::string &key, const std::string &value);
    int handleVkCode(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch = 0);
    std::vector<WordItem> generate_for_creating_word(const std::string code);
    int create_word(std::string pinyin, std::string word);
    int create_word_from_quanpin(std::string pinyin, std::string word);
    // 一次到顶
    int update_weight_by_word(std::string word);
    // 一次到顶
    int update_weight_by_pinyin_and_word(std::string pinyin, std::string word);
    int delete_by_pinyin_and_word(std::string pinyin, std::string word);

    /*
      Return: list of complete item data of database table
    */
    std::vector<WordItem> generate_tuple(const std::string code);

    std::string search_sentence_from_ime_engine(const std::string &user_pinyin);

    // 设置整句候选来源与去重补位选项。值变化时清缓存，见 QuanpinDictionary::set_sentence_association。
    void set_sentence_association(const SentenceAssociationOptions &options);

    // 神经重排的上下文，见 QuanpinDictionary::set_rescoring_context。
    void set_rescoring_context(const std::string &context);

    // 句中辅助码约束（见 core/syllable_helpcode.h），随每次查询下发，空表示没有。整句来源
    // （词格、Google 解码器、神经重排）在解码时就按它筛字，缓存键也带上它。
    void set_syllable_helpcodes(const SyllableHelpcodes &helpcodes);
    // 候选覆盖到受约束的音节时，那个位置上的字必须满足约束，不满足的去掉。
    void filter_by_syllable_helpcodes(std::vector<WordItem> &candidates) const;

    // 直接辅助码：整句解码选出切分，把请求改写成句中辅助码的形状，见 direct_helpcode/direct_resolver.h。
    bool resolve_direct_helpcode(QueryRequest &request);
    // 直接辅助码开着时，词格的跨度查询与解析器共用一份缓存（见 lattice_lookup）；关着时照旧直查。
    void set_direct_span_cache_enabled(bool enabled)
    {
        direct_span_cache_enabled_ = enabled;
    }
    // 词格跨度查询带的模糊音规则，见 QuanpinDictionary::set_fuzzy_pinyin。变化时清缓存。
    void set_fuzzy_pinyin(metasequoia::FuzzyPinyinOptions options);

    explicit ShuangpinDictionary(const ShuangpinProfile &profile = GetXiaoheShuangpinProfile(),
                                 metasequoia::RuntimePaths paths = metasequoia::RuntimePaths::legacy());
    ~ShuangpinDictionary();

    const ShuangpinProfile &profile() const
    {
        return profile_;
    }

  private:
    const ShuangpinProfile profile_;
    std::string quanpin_db_path_;
    sqlite3 *quanpin_db_ = nullptr;
    sqlite3_int64 data_version_ = -1;
    metasequoia::RuntimePaths paths_;
    metasequoia::PinyinDecoder decoder_;
    // 与全拼共用的词格打分模型，见 QuanpinDictionary::language_model_。
    const ngram::LanguageModel *language_model_ = nullptr;
    // 万象语法模型（octagram .gram），见 QuanpinDictionary::collocation_db_。
    std::shared_ptr<const gram::GramDb> collocation_db_ = nullptr;
    // 神经整句模型，见 QuanpinDictionary::neural_desktop_model_。与全拼共用同一份
    // 进程内缓存（按资源路径），双拼这侧只是各自持一份句柄，开关生效时才载入。
    neural::LazySentenceModel neural_desktop_model_;
    neural::LazySentenceModel neural_keyboard_model_;
    // 整句候选来源与去重补位选项，默认全关；由 set_sentence_association 随请求更新。
    SentenceAssociationOptions sentence_association_;
    // 神经重排的上下文（光标前已上屏的文本）。宿主没给就是空串。
    std::string rescoring_context_;
    HelpcodeUtils::SharedKeymap helpcodes_;
    SyllableHelpcodes syllable_helpcodes_;
    std::string syllable_helpcodes_signature_;
    std::unordered_map<std::string, sqlite3_stmt *> quanpin_statement_cache_;
    // 直接辅助码开着时才建。
    std::unique_ptr<direct_helpcode::Resolver> direct_resolver_;
    // 解析器上一次解出的最优整句及它对应的 generateSeries 缓存键，词格合并时直接用它而不重解。
    std::optional<std::pair<std::string, quanpin::LatticePath>> direct_sentence_;
    // 词库里全部单字行（全拼读音、单字），直接辅助码建辅码索引用。
    std::vector<std::pair<std::string, std::string>> query_single_char_rows();
    // 词格的跨度查询（键与跨度完全相等，至多 limit 行）。直接辅助码开着时套一层按（跨度, limit）的缓存：
    // 解析器选切分时已经查过同样的跨度，词典层整句解码不必再打一遍 SQL。结果与直查逐行相同；词库一变
    // （reset_cache）缓存随之清空。
    quanpin::WordLatticeLookup lattice_lookup(int limit);
    bool direct_span_cache_enabled_ = false;
    metasequoia::FuzzyPinyinOptions fuzzy_pinyin_;
    std::unordered_map<std::string, std::vector<quanpin::LatticeLexeme>> direct_span_cache_;
    // 见 QuanpinDictionary::char_reading_totals_。
    std::shared_ptr<const quanpin::CharReadingTotals> char_reading_totals_;
    void reset_cache_if_database_changed();
    // 这个字能不能落在 helpcode 约束的音节上。
    bool accepts_syllable_char(const SyllableHelpcode &helpcode, const std::string &hanzi) const;
    // 词（从第一个音节起）的每个字都满足落在它位置上的约束；超出词长的约束不管。
    bool satisfies_syllable_helpcodes(const std::string &word) const;
    // 送 Google 解码器出整句。有句中辅助码时约束在解码器内部生效，结果再核一遍字数与约束。
    std::string decode_google_sentence(const std::string &quanpin_segmentation);

    void generate_for_single_char(std::vector<WordItem> &candidate_list, std::string code);
    void filter_with_single_helpcode(                //
        const std::vector<WordItem> &candidate_list, //
        std::vector<WordItem> &filtered_list,        //
        const std::string &help_code,                //
        const std::string &pinyin_sequence           //
    );
    void filter_with_double_helpcodes(               //
        const std::vector<WordItem> &candidate_list, //
        std::vector<WordItem> &filtered_list,        //
        const std::string &help_codes                //
    );
    std::vector<WordItem> select_complete_data(sqlite3 *target_db, const std::string &sql_str);
    int check_data(sqlite3 *target_db, const std::string &sql_str);
    int insert_data(sqlite3 *target_db, const std::string &sql_str);
    int update_data(sqlite3 *target_db, const std::string &sql_str);
    int delete_data(sqlite3 *target_db, const std::string &sql_str);
    std::vector<WordItem> query_from_quanpin_database(const std::string &pinyin_sequence,
                                                      const std::string &pinyin_segmentation);
    std::vector<WordItem> query_initial_from_quanpin_database(const std::string &code, int limit);
    std::string normalize_shuangpin_to_quanpin_segmentation(const std::string &pinyin) const;
    std::string normalize_shuangpin_to_quanpin_input(const std::string &pinyin) const;
    std::string build_quanpin_sql_for_creating_word(const std::string &pinyin) const;
    std::string build_quanpin_sql_for_checking_word(const std::string &key, const std::string &value) const;
    std::string build_quanpin_sql_for_inserting_word(const std::string &key, const std::string &jp,
                                                     const std::string &value) const;
    std::string build_quanpin_sql_for_updating_word(const std::string &word) const;
    std::string build_quanpin_sql_for_updating_word(std::string pinyin, const std::string &word) const;
    // The caller has already resolved the input (raw shuangpin or canonical
    // quanpin) to a canonical quanpin key before reaching this helper.  Do
    // not normalize it as shuangpin again: canonical keys are database keys,
    // not user input.
    std::string build_quanpin_sql_for_deleting_canonical_word(const std::string &canonical_pinyin,
                                                              const std::string &word) const;
    bool do_validate(std::string key, std::string jp, std::string value) const;

  private:
    // Lock
    std::shared_mutex mutex_; // Read-write separation lock

    // Whether in full help mode
    bool _is_full_help_mode = false;
    // Localtion of starting position
    std::string::size_type _help_mode_raw_pos = 0; // Start from pos, e.g. 妮: ninv: 2
    std::string _pinyin_helpcodes = "";            // Help codes
    std::vector<ImeKeyCode> _kb_input_sequence;    // Keyboard input sequence
    std::string _pinyin_sequence = "";             // Pinyin extracted from from keyboard sequence
    std::string _pinyin_sequence_with_cases =
        ""; // Pinyin extracted from from keyboard sequence, but with letters' original cases
    std::string _pure_pinyin_sequence = "";        // Pinyin without help code
    std::array<char, 2> _help_codes_sequence = {}; // Help code extracted from from keyboard sequence
    std::string _pinyin_segmentation = "";         // Segmentation pinyin
    std::string _preedit_pinyin = "";              // Preedit
    /* Current candidate list, computed by current kb_input_sequence */
    std::vector<WordItem> _cur_candidate_list;
    std::vector<WordItem> _cur_page_candidate_list; // Current candidate list
    // boost::circular_buffer<std::pair<std::string, std::vector<WordItem>>> _cached_buffer;
    CircularBuffer<std::string, std::vector<WordItem>> _cached_buffer;              // 缓存纯拼音的结果
    CircularBuffer<std::string, std::vector<WordItem>> _cached_buffer_sgl;          // 缓存单码辅助结果
    CircularBuffer<std::string, std::vector<WordItem>> _cached_buffer_sgl_reversed; // 缓存反向单码辅助结果
    CircularBuffer<std::string, std::vector<WordItem>> _cached_buffer_dbl;          // 缓存双码辅助结果
    CircularBuffer<std::string, std::vector<WordItem>> _cached_buffer_series; // 缓存拼音序列对应的所有结果

  public:
    // Getters and setters
    bool get_full_help_mode()
    {
        return this->_is_full_help_mode;
    }
    void set_full_help_mode(bool is_full_help_mode)
    {
        this->_is_full_help_mode = is_full_help_mode;
    }

    std::string::size_type get_help_mode_raw_pos()
    {
        return this->_help_mode_raw_pos;
    }
    void set_help_mode_raw_pos(std::string::size_type raw_pos)
    {
        this->_help_mode_raw_pos = raw_pos;
    }

    const std::string &get_pinyin_sequence()
    {
        return this->_pinyin_sequence;
    }

    void set_pinyin_sequence(const std::string &pinyin_sequence)
    {
        this->_pinyin_sequence = pinyin_sequence;
    }

    const std::string &get_pinyin_sequence_with_cases()
    {
        return this->_pinyin_sequence_with_cases;
    }

    void set_pinyin_sequence_with_cases(const std::string &pinyin_sequence)
    {
        this->_pinyin_sequence_with_cases = pinyin_sequence;
    }

    const std::string &get_pinyin_segmentation()
    {
        return this->_pinyin_segmentation;
    }

    const std::string &get_pure_pinyin_sequence()
    {
        return this->_pure_pinyin_sequence;
    }

    const std::vector<WordItem> &get_current_candidate_list() const
    {
        return this->_cur_candidate_list;
    }

    const std::vector<WordItem> &get_cur_candiate_list() const
    {
        return get_current_candidate_list();
    }

    int insert_word_to_cached_buffer_series(const std::string &pinyin, const std::string &word, CandidateSource source);
    int insert_word_to_active_helpcode_cache(const std::string &pinyin, const std::string &word, CandidateSource source,
                                             const std::string &double_helpcodes = {});

    bool is_all_complete_pinyin();
    bool is_all_complete_pure_pinyin();
    std::string get_pinyin_segmentation_with_cases();

    std::string get_quanpin() const;
    std::string get_quanpin_seg() const;

    void reset_state();
    void reset_cache();
    // 只清含词格整句排序的缓存，见 QuanpinDictionary::reset_sentence_cache。
    void reset_sentence_cache();
    void set_helpcode_keymap(HelpcodeUtils::SharedKeymap table)
    {
        helpcodes_ = std::move(table);
        reset_cache();
    }
};

using DictionaryUlPb = ShuangpinDictionary;
