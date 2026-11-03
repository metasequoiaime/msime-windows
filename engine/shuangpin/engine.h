#pragma once
#include "../quanpin/quanpin_dictionary.h"

#include "../core/query_request.h"
#include "../core/word_item.h"
#include "shuangpin_dictionary.h"
#include <string>
#include <vector>

class ShuangpinEngine
{
  public:
    explicit ShuangpinEngine(const ShuangpinProfile &profile = GetXiaoheShuangpinProfile(),
                             metasequoia::RuntimePaths paths = metasequoia::RuntimePaths::legacy());
    std::vector<WordItem> query(const QueryRequest &request);
    // 直接辅助码：在 query 之前把请求改写成句中辅助码的形状，见 ShuangpinDictionary::resolve_direct_helpcode。
    bool resolve_direct_helpcode(QueryRequest &request);
    bool can_expand_initial_candidates(const QueryRequest &request, const std::vector<WordItem> &candidates) const;
    bool expand_initial_candidates(const QueryRequest &request, std::vector<WordItem> &candidates);
    std::optional<WordItem> find_candidate(const std::string &key, const std::string &value);
    int create_word(std::string pinyin, std::string word);
    int update_weight_by_pinyin_and_word(std::string pinyin, std::string word);
    int delete_by_pinyin_and_word(std::string pinyin, std::string word);
    int insert_word_to_series_cache(const std::string &pinyin, const std::string &word, CandidateSource source);
    int insert_word_to_active_helpcode_cache(const std::string &pinyin, const std::string &word, CandidateSource source,
                                             const std::string &double_helpcodes = {});
    std::string search_sentence_from_ime_engine(const std::string &user_pinyin);
    void reset_cache();
    void reset_sentence_cache();

    void set_helpcode_keymap(HelpcodeUtils::SharedKeymap table)
    {
        helpcodes_ = table;
        dictionary_.set_helpcode_keymap(std::move(table));
    }

  private:
    const ShuangpinProfile profile_;
    ShuangpinDictionary dictionary_;
    std::unique_ptr<QuanpinDictionary> fuzzy_dictionary_;
    metasequoia::RuntimePaths paths_;
    HelpcodeUtils::SharedKeymap helpcodes_;
    struct UnfilteredQuery
    {
        std::vector<WordItem> candidates;
        // 输入去掉末尾辅码后的键串，没有辅码时就是整串键。读音盖到它最后一键的候选，选中时连辅码一起
        // 吃掉整串。
        std::string keys_before_helpcodes;
    };
    // query 的主体；query 在它外面下发请求级选项、按句中辅助码收尾筛选。
    UnfilteredQuery query_unfiltered(const QueryRequest &request);
    // 能展开首字母候选时返回那个首字母，否则返回空串。
    std::string expandable_initial_code(const QueryRequest &request) const;
    std::vector<WordItem> append_fuzzy(std::vector<WordItem> exact, const std::string &raw_segmentation,
                                       metasequoia::FuzzyPinyinOptions options, const std::string &helpcodes = "");
};
