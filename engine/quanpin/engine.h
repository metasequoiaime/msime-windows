#pragma once
#include "../common/helpcode_utils.h"

#include "../core/query_request.h"
#include "../core/word_item.h"
#include "../common/cache.h"
#include "quanpin_dictionary.h"
#include <string>
#include <vector>

class QuanpinEngine
{
  public:
    explicit QuanpinEngine(metasequoia::RuntimePaths paths = metasequoia::RuntimePaths::legacy());
    ~QuanpinEngine();

    std::vector<WordItem> query(const QueryRequest &request);
    bool can_expand_initial_candidates(const QueryRequest &request, const std::vector<WordItem> &candidates) const;
    bool expand_initial_candidates(const QueryRequest &request, std::vector<WordItem> &candidates);
    std::optional<WordItem> find_candidate(const std::string &key, const std::string &value);
    int handleVkCode(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch = 0);
    int create_word(std::string pinyin, std::string word);
    int create_word_from_canonical_pinyin(std::string pinyin, std::string word);
    int update_weight_by_word(std::string word);
    int update_weight_by_pinyin_and_word(std::string pinyin, std::string word);
    int delete_by_pinyin_and_word(std::string pinyin, std::string word);
    int insert_word_to_series_cache(const std::string &pinyin, const std::string &word, CandidateSource source);
    int insert_word_to_series_cache(const QueryRequest &request, const std::string &word, CandidateSource source);
    std::string search_sentence_from_ime_engine(const std::string &user_pinyin);
    void reset_state();
    void reset_cache();
    // 容错仲裁选中读音后的分段显示（大小写原样）；空表示当前串没有容错布局。
    const std::string &fault_segmentation_with_cases() const
    {
        return dictionary_.fault_segmentation_with_cases();
    }
    void reset_sentence_cache();
    void set_helpcode_keymap(HelpcodeUtils::SharedKeymap table)
    {
        helpcodes_ = std::move(table);
        reset_cache();
    }

  private:
    QuanpinDictionary dictionary_;
    HelpcodeUtils::SharedKeymap helpcodes_;
};
