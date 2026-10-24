#include "ai/jev_candidate_policy.h"
#include "ai/ai_assistant_cache_key.h"
#include "ai/ai_provider_store.h"
#include "cloud/ai_translation.h"
#include "ipc/traditional_variant_expansion_policy.h"
#include "tests/includes/test_framework.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

TEST_CASE(jev_request_uses_choice_api_and_keeps_token_out_of_payload)
{
    const auto body = Jev::BuildRequest(Jev::kModel, "明天开始", {"shi", "shi"}, {"事实", "实施"});
    REQUIRE_EQ(body.at("questions").at("candidate").at("type").get<std::string>(), "choice");
    REQUIRE_EQ(body.at("questions").at("candidate").at("criteria").at("c1").get<std::string>(), "实施");
    REQUIRE(body.at("questions").at("candidate").at("criteria").contains("none"));
    REQUIRE(!body.contains("messages"));
    REQUIRE(!body.contains("token"));
}

TEST_CASE(jev_response_is_mapped_to_the_original_candidate_only)
{
    const auto response = [](const std::string &id) {
        return nlohmann::json{{"answers", {{"candidate", {{"type", "choice"}, {"choice", id}}}}}}.dump();
    };
    const std::vector<std::string> candidates{"事实", "实施"};
    REQUIRE_EQ(Jev::ParseChoice(response("c1"), candidates), "实施");
    for (const auto &id : {"none", "c2", "c01", "实施", "untrusted output"})
        REQUIRE(Jev::ParseChoice(response(id), candidates).empty());
    REQUIRE(Jev::ParseChoice("not json", candidates).empty());
    REQUIRE(Jev::ParseChoice(R"({"answers":{"candidate":{"choice":12}}})", candidates).empty());
}

TEST_CASE(jev_pool_excludes_prefixes_raw_input_mixed_text_and_duplicates)
{
    std::vector<WordItem> items{{"shi'shi", "事实", 10},
                                {"shi", "是", 9},
                                {"shi'shi", "实施", 8},
                                {"shi'shi", "实施", 7},
                                {"shi'shi", "实施abc", 6},
                                {"shi'shi", "shishi", 5, CandidateSource::Fallback},
                                {"shi'shi", "实时", 4, CandidateSource::QuickPhrase}};
    const auto pool = Jev::CandidatePool(items, 2);
    REQUIRE_EQ(pool.size(), size_t{2});
    REQUIRE_EQ(pool[0], "事实");
    REQUIRE_EQ(pool[1], "实施");
    REQUIRE(Jev::CandidatePool(items, 0).empty());
}

TEST_CASE(jev_pool_and_wire_options_are_bounded)
{
    std::vector<std::string> candidates(100, "候选");
    const auto body = Jev::BuildRequest(Jev::kModel, "", {"hou", "xuan"}, candidates);
    REQUIRE_EQ(body["questions"]["candidate"]["criteria"].size(), Jev::kMaxCandidates + 1);
    REQUIRE(Jev::ParseChoice(R"({"answers":{"candidate":{"type":"choice","choice":"c32"}}})", candidates).empty());
}

TEST_CASE(jev_promotion_preserves_local_first_choices_and_dictionary_metadata)
{
    std::vector<WordItem> items{{"shi'shi", "事实", 10},
                                {"shi'shi", "实时", 9},
                                {"shi'shi", "史诗", 8},
                                {"shi'shi", "实施", 7, CandidateSource::UserDatabase, "shi'shi"}};
    REQUIRE(Jev::Promote(items, "实施"));
    REQUIRE_EQ(items.size(), size_t{4});
    REQUIRE_EQ(items[0].word, "事实");
    REQUIRE_EQ(items[1].word, "实时");
    REQUIRE_EQ(items[2].word, "实施");
    REQUIRE_EQ(items[2].canonical_pinyin, "shi'shi");
    REQUIRE(items[2].source == CandidateSource::UserDatabase);
    REQUIRE_EQ(items[2].weight, 7);
    REQUIRE(Jev::Promote(items, "实施"));
    REQUIRE_EQ(items[2].word, "实施");
    REQUIRE(!Jev::Promote(items, "不存在"));
}

TEST_CASE(jev_promotion_does_not_demote_first_choice_or_move_pinned_candidates)
{
    std::vector<WordItem> items{{"a", "甲", 10}, {"b", "乙", 9}, {"c", "丙", 8}, {"d", "丁", 7}};
    REQUIRE(Jev::Promote(items, "甲"));
    REQUIRE_EQ(items.front().word, "甲");
    items[2].fixed_position = 3;
    REQUIRE(Jev::Promote(items, "丁"));
    REQUIRE_EQ(items[2].word, "丙");
    REQUIRE_EQ(items[3].word, "丁");
}

TEST_CASE(jev_promotion_moves_the_pool_entry_and_keeps_async_rows_in_place)
{
    std::vector<WordItem> items{{"shi'shi", "事实", 10},
                                {"shi'shi", "实施", 9, CandidateSource::QuickPhrase},
                                {"shi'shi", "实时", 8},
                                {"shi'shi", "云", 7, CandidateSource::CloudSuggestion},
                                {"shi'shi", "史诗", 6},
                                {"shishi", "shishi", 5, CandidateSource::EnglishDictionary},
                                {"shi'shi", "实施", 4, CandidateSource::UserDatabase}};
    REQUIRE(Jev::Promote(items, "实施"));
    REQUIRE_EQ(items[0].word, "事实");
    REQUIRE(items[1].source == CandidateSource::QuickPhrase);
    REQUIRE_EQ(items[2].word, "实时");
    REQUIRE(items[3].source == CandidateSource::CloudSuggestion);
    REQUIRE_EQ(items[4].word, "实施");
    REQUIRE(items[4].source == CandidateSource::UserDatabase);
    REQUIRE(items[5].source == CandidateSource::EnglishDictionary);
    REQUIRE_EQ(items[6].word, "史诗");
    const auto promoted = items;
    REQUIRE(Jev::Promote(items, "实施"));
    for (size_t i = 0; i < items.size(); ++i)
        REQUIRE_EQ(items[i].word, promoted[i].word);
}

TEST_CASE(jev_promotion_rejects_pinned_or_ineligible_choices)
{
    std::vector<WordItem> items{
        {"a", "甲", 10}, {"b", "乙", 9}, {"c", "丙", 8}, {"d", "丁", 7, CandidateSource::QuickPhrase}, {"e", "戊", 6}};
    REQUIRE(!Jev::Promote(items, "丁"));
    items[4].fixed_position = 5;
    REQUIRE(!Jev::Promote(items, "戊"));
    REQUIRE_EQ(items[2].word, "丙");
    REQUIRE_EQ(items[4].word, "戊");
}

namespace
{
// 繁体模式下展开过的一组写法：同一个底词，variant_rank 从 0 起，紧挨着放。
std::vector<WordItem> SpellingGroup(const std::string &word, std::int64_t weight,
                                    std::initializer_list<const char *> spellings)
{
    std::vector<WordItem> group;
    int rank = 0;
    for (const char *spelling : spellings)
    {
        WordItem item("shi'shi", word, weight);
        item.output_text = spelling;
        item.variant_rank = rank++;
        group.push_back(std::move(item));
    }
    return group;
}

void Append(std::vector<WordItem> &items, std::vector<WordItem> more)
{
    items.insert(items.end(), more.begin(), more.end());
}

std::vector<std::string> DisplayTexts(const std::vector<WordItem> &items)
{
    std::vector<std::string> texts;
    for (const auto &item : items)
        texts.push_back(item.output_text.empty() ? item.word : item.output_text);
    return texts;
}

std::vector<int> Ranks(const std::vector<WordItem> &items)
{
    std::vector<int> ranks;
    for (const auto &item : items)
        ranks.push_back(item.variant_rank);
    return ranks;
}
} // namespace

// 展开过的列表：选中的词整组挪成第三组，前两组（本地第一、第二候选）不动，每组的写法保持相邻、顺序不变。
TEST_CASE(jev_promotion_moves_the_whole_spelling_group_behind_the_first_two_groups)
{
    std::vector<WordItem> items;
    Append(items, SpellingGroup("甲", 10, {"甲一", "甲二"}));
    Append(items, SpellingGroup("乙", 9, {"乙一", "乙二"}));
    Append(items, SpellingGroup("丙", 8, {"丙一"}));
    Append(items, SpellingGroup("丁", 7, {"丁一", "丁二", "丁三"}));
    REQUIRE(Jev::Promote(items, "丁"));
    REQUIRE_EQ(DisplayTexts(items),
               (std::vector<std::string>{"甲一", "甲二", "乙一", "乙二", "丁一", "丁二", "丁三", "丙一"}));
    REQUIRE_EQ(Ranks(items), (std::vector<int>{0, 1, 0, 1, 0, 1, 2, 0}));
    const auto promoted = items;
    REQUIRE(Jev::Promote(items, "丁"));
    REQUIRE_EQ(DisplayTexts(items), DisplayTexts(promoted));
}

// 不参与挪动的行（云、英文）留在它们在各组之间的原有次序上，组不被它们切开。
TEST_CASE(jev_promotion_moves_spelling_groups_around_async_rows)
{
    std::vector<WordItem> items;
    Append(items, SpellingGroup("甲", 10, {"甲一", "甲二"}));
    items.emplace_back("shi'shi", "云", 9, CandidateSource::CloudSuggestion);
    Append(items, SpellingGroup("乙", 8, {"乙一"}));
    Append(items, SpellingGroup("丙", 7, {"丙一", "丙二"}));
    items.emplace_back("shishi", "shishi", 6, CandidateSource::EnglishDictionary);
    Append(items, SpellingGroup("丁", 5, {"丁一", "丁二"}));
    REQUIRE(Jev::Promote(items, "丁"));
    REQUIRE_EQ(DisplayTexts(items),
               (std::vector<std::string>{"甲一", "甲二", "云", "乙一", "丁一", "丁二", "shishi", "丙一", "丙二"}));
    REQUIRE_EQ(Ranks(items), (std::vector<int>{0, 1, 0, 0, 0, 1, 0, 0, 1}));
}

// 组边界与 SpellingGroupStart 同一个判定：variant_rank 大于 0 的项属于它前面最近的第一写法所在的组，
// 不论底词是否相同。这样的项跟着那一组走，不单独占一个名次。
TEST_CASE(jev_promotion_uses_the_spelling_group_boundary_of_the_expansion_policy)
{
    std::vector<WordItem> items;
    Append(items, SpellingGroup("甲", 10, {"甲一"}));
    WordItem follower("shi'shi", "戊", 9);
    follower.output_text = "戊二";
    follower.variant_rank = 1;
    items.push_back(follower);
    Append(items, SpellingGroup("乙", 8, {"乙一"}));
    Append(items, SpellingGroup("丙", 7, {"丙一"}));
    Append(items, SpellingGroup("丁", 6, {"丁一"}));
    REQUIRE_EQ(FanyImeIpc::SpellingGroupStart(items, 1), size_t{0});

    REQUIRE(Jev::Promote(items, "丁"));
    REQUIRE_EQ(DisplayTexts(items), (std::vector<std::string>{"甲一", "戊二", "乙一", "丁一", "丙一"}));
}

TEST_CASE(jev_query_rejects_old_composition_same_text_and_different_session)
{
    metasequoia::OnlineQuery query;
    query.session_id = 10;
    query.generation = 3;
    query.identity = "shi'shi";
    query.ai_eligible = true;
    REQUIRE(Jev::SameQuery(query, query));
    auto changed = query;
    ++changed.generation;
    REQUIRE(!Jev::SameQuery(query, changed));
    changed = query;
    ++changed.session_id;
    REQUIRE(!Jev::SameQuery(query, changed));
    changed = query;
    changed.pinyin_segments = {"shi"};
    REQUIRE(!Jev::SameQuery(query, changed));
}

TEST_CASE(jev_cache_key_tracks_candidate_pool_and_order)
{
    AiAssistant::Request request;
    request.config.provider = "jev";
    request.candidates = {"事实", "实施"};
    const auto before = AiAssistant::detail::BuildSuggestionCacheKey(request);
    std::reverse(request.candidates.begin(), request.candidates.end());
    REQUIRE(before != AiAssistant::detail::BuildSuggestionCacheKey(request));
    request.candidates.clear();
    REQUIRE(before != AiAssistant::detail::BuildSuggestionCacheKey(request));
}

TEST_CASE(jev_accepts_a_configured_endpoint_but_only_over_http)
{
    // 协议是公开的，Laya、OpenJev 这类自建实现照同一套协议就跑得通，所以端点必须可配。
    // 这里验证的是「只发到合法的 http(s) 地址」：给一个连不上的本地端口，请求会失败，
    // 但不能因为地址不是官方端点就被提前拒绝。
    REQUIRE(!Jev::Fetch("http://127.0.0.1:1", "test-only", "{}").has_value());
    REQUIRE(!Jev::Fetch("ftp://127.0.0.1/", "test-only", "{}").has_value());
    REQUIRE(!Jev::Fetch("not-a-url", "test-only", "{}").has_value());
    // 空端点回落到官方地址；取消回调立刻为真时一个请求都不发。
    REQUIRE(!Jev::Fetch("", "test-only", "{}", [] { return true; }).has_value());
    REQUIRE(!Jev::Fetch(Jev::kEndpoint, "test-only", "{}", [] { return true; }).has_value());
    // 官方端点缺 Key 时不发请求；自建端点允许空 Token。
    REQUIRE(!Jev::Fetch("", "", "{}").has_value());
    REQUIRE(!Jev::Fetch(Jev::kEndpoint, "", "{}").has_value());
    REQUIRE(!Jev::Fetch("http://127.0.0.1:1", "", "{}").has_value());
}

TEST_CASE(jev_is_builtin_but_not_a_chat_translation_model)
{
    REQUIRE(AiAssistant::ProviderStore::IsBuiltinProvider("jev"));
    REQUIRE(!AiTranslation::IsUsableConfig({"jev", Jev::kEndpoint, "test-only", Jev::kModel}));
}
