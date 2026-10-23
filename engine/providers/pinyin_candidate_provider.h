#pragma once

#include "candidate_provider.h"
#include "../quanpin/engine.h"
#include "../shuangpin/engine.h"

class PinyinCandidateProvider : public ICandidateProvider
{
  public:
    explicit PinyinCandidateProvider(const ShuangpinProfile &shuangpin_profile = GetXiaoheShuangpinProfile(),
                                     metasequoia::RuntimePaths paths = metasequoia::RuntimePaths::legacy());
    std::vector<WordItem> query(const QueryRequest &request) override;
    std::optional<WordItem> find_candidate(SchemeType scheme, const std::string &key,
                                           const std::string &value) override;
    bool can_expand_initial_candidates(const QueryRequest &request, const std::vector<WordItem> &candidates) const;
    bool expand_initial_candidates(const QueryRequest &request, std::vector<WordItem> &candidates);
    // 双拼直接辅助码，见 ShuangpinEngine::resolve_direct_helpcode。
    bool resolve_direct_helpcode(QueryRequest &request);
    void reset_cache() override;
    void reset_sentence_cache();
    int create_word(SchemeType scheme, std::string pinyin, std::string word) override;
    int update_weight_by_pinyin_and_word(SchemeType scheme, std::string pinyin, std::string word) override;
    int delete_by_pinyin_and_word(SchemeType scheme, std::string pinyin, std::string word) override;
    int cache_dynamic_candidate(SchemeType scheme, const std::string &pinyin, const std::string &word,
                                CandidateSource source) override;
    int cache_dynamic_candidate_for_request(const QueryRequest &request, const std::string &word,
                                            CandidateSource source) override;

    void set_helpcode_keymap(HelpcodeUtils::SharedKeymap table)
    {
        quanpin_engine_.set_helpcode_keymap(table);
        shuangpin_engine_.set_helpcode_keymap(std::move(table));
    }
    // 容错仲裁选中读音后的分段显示（大小写原样）；空表示当前串没有容错布局。
    const std::string &fault_segmentation_with_cases() const
    {
        return quanpin_engine_.fault_segmentation_with_cases();
    }

  private:
    const ShuangpinProfile shuangpin_profile_;
    QuanpinEngine quanpin_engine_;
    ShuangpinEngine shuangpin_engine_;
};
