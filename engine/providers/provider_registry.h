#pragma once

#include "pinyin_candidate_provider.h"
#include "wubi_candidate_provider.h"
#include "japanese_candidate_provider.h"
#include "../core/scheme_type.h"
#include <string>

class ProviderRegistry
{
  public:
    explicit ProviderRegistry(const ShuangpinProfile &shuangpin_profile = GetXiaoheShuangpinProfile(),
                              metasequoia::RuntimePaths paths = metasequoia::RuntimePaths::legacy());
    ICandidateProvider &resolve(SchemeType scheme_type);
    std::optional<WordItem> find_candidate(SchemeType scheme_type, const std::string &key, const std::string &value);
    // 容错仲裁选中读音后的分段显示（大小写原样）；空表示当前串没有容错布局。
    const std::string &fault_segmentation_with_cases() const
    {
        return pinyin_provider_.fault_segmentation_with_cases();
    }
    bool can_expand_initial_candidates(const QueryRequest &request, const std::vector<WordItem> &candidates) const;
    bool expand_initial_candidates(const QueryRequest &request, std::vector<WordItem> &candidates);
    // 双拼直接辅助码只出在拼音 provider 上，见 PinyinCandidateProvider::resolve_direct_helpcode。
    bool resolve_direct_helpcode(QueryRequest &request)
    {
        return pinyin_provider_.resolve_direct_helpcode(request);
    }
    void reset_cache(SchemeType scheme_type);
    // 整句（词格 + 神经重排）只出在拼音 provider 上，五笔混输查的也是它。
    void reset_sentence_cache()
    {
        pinyin_provider_.reset_sentence_cache();
    }
    int create_word(SchemeType scheme_type, std::string pinyin, std::string word);
    int update_weight_by_pinyin_and_word(SchemeType scheme_type, std::string pinyin, std::string word);
    int delete_by_pinyin_and_word(SchemeType scheme_type, std::string pinyin, std::string word);
    int cache_dynamic_candidate(SchemeType scheme_type, const std::string &pinyin, const std::string &word,
                                CandidateSource source);
    int cache_dynamic_candidate_for_request(const QueryRequest &request, const std::string &word,
                                            CandidateSource source);

    void set_helpcode_keymap(HelpcodeUtils::SharedKeymap table)
    {
        pinyin_provider_.set_helpcode_keymap(std::move(table));
    }

  private:
    PinyinCandidateProvider pinyin_provider_;
    WubiCandidateProvider wubi_provider_;
    JapaneseCandidateProvider japanese_provider_;
};
