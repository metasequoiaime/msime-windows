#include "utils/dr_dict_brief_policy.h"
#include "tests/includes/test_framework.h"

#include <nlohmann/json.hpp>

TEST_CASE(dr_dict_brief_request_payload_matches_the_dr_dict_protocol)
{
    // DR Dict（app/src/application.cpp 的 on_brief）要求 word 是字符串、reply 是整数；id 原样回传。
    const auto payload = nlohmann::json::parse(DrDictBrief::BuildRequestPayload("apple", 0x1234, 7, 60));
    REQUIRE(payload["word"] == "apple");
    REQUIRE(payload["reply"].is_number_integer());
    REQUIRE(payload["reply"].get<std::int64_t>() == 0x1234);
    REQUIRE(payload["id"].get<std::uint64_t>() == 7);
    REQUIRE(payload["max"].get<std::size_t>() == 60);
    REQUIRE(DrDictBrief::kBriefRequest == 0x46425244);
    REQUIRE(DrDictBrief::kBriefReply == 0x52425244);
}

TEST_CASE(dr_dict_brief_reply_parses_found_and_missing_words)
{
    const auto found = DrDictBrief::ParseReply(
        R"({"id":7,"word":"apple","found":true,"headword":"apple","dictionary":"D","text":"n. 苹果"})");
    REQUIRE(found.has_value());
    REQUIRE(found->id == 7);
    REQUIRE(found->word == "apple");
    REQUIRE(found->found);
    REQUIRE(found->text == "n. 苹果");

    const auto missing = DrDictBrief::ParseReply(R"({"id":8,"word":"zzz","found":false})");
    REQUIRE(missing.has_value());
    REQUIRE(!missing->found);
    REQUIRE(missing->text.empty());

    // found 却没有正文，按没查到处理。
    const auto empty = DrDictBrief::ParseReply(R"({"id":9,"word":"x","found":true,"text":""})");
    REQUIRE(empty.has_value() && !empty->found);
}

TEST_CASE(dr_dict_brief_reply_rejects_malformed_payloads)
{
    REQUIRE(!DrDictBrief::ParseReply("").has_value());
    REQUIRE(!DrDictBrief::ParseReply("not json").has_value());
    REQUIRE(!DrDictBrief::ParseReply(R"({"word":"apple"})").has_value());
    REQUIRE(!DrDictBrief::ParseReply(R"({"id":"7","word":"apple"})").has_value());
    REQUIRE(!DrDictBrief::ParseReply(R"({"id":7,"word":1})").has_value());
    REQUIRE(!DrDictBrief::ParseReply(std::string(DrDictBrief::kMaxReplyBytes + 1, ' ')).has_value());
}
