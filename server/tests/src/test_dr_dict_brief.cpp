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

TEST_CASE(dr_dict_brief_request_only_carries_flags_that_differ_from_the_default)
{
    // 只要摘要时和旧版请求一字不差；旧版 DR Dict 不认的字段只在要音标或不要摘要时出现。
    const auto plain = nlohmann::json::parse(DrDictBrief::BuildRequestPayload("apple", 1, 2));
    REQUIRE(!plain.contains("brief"));
    REQUIRE(!plain.contains("phonetics"));
    const auto phonetics_only =
        nlohmann::json::parse(DrDictBrief::BuildRequestPayload("apple", 1, 2, DrDictBrief::kMaxChars, false, true));
    REQUIRE(phonetics_only["brief"] == false);
    REQUIRE(phonetics_only["phonetics"] == true);
}

TEST_CASE(dr_dict_brief_reply_reads_british_and_american_phonetics)
{
    const auto both = DrDictBrief::ParseReply(
        R"({"id":1,"word":"tomato","found":false,"phonetics":{"uk":"təˈmɑːtəʊ","us":"təˈmeɪtəʊ","dictionary":"OALD"}})");
    REQUIRE(both.has_value());
    REQUIRE(both->has_phonetics);
    REQUIRE(both->uk == "təˈmɑːtəʊ");
    REQUIRE(both->us == "təˈmeɪtəʊ");
    REQUIRE(!both->found);

    // DR Dict 认这个字段但没查到：has_phonetics 为真、两边为空，调用方不再等它。
    const auto none = DrDictBrief::ParseReply(R"({"id":2,"word":"zzz","found":false,"phonetics":{}})");
    REQUIRE(none.has_value() && none->has_phonetics && none->uk.empty() && none->us.empty());

    // 旧版 DR Dict：回复里没有 phonetics。
    const auto old = DrDictBrief::ParseReply(R"({"id":3,"word":"apple","found":true,"text":"n. 苹果"})");
    REQUIRE(old.has_value() && !old->has_phonetics);

    // 类型不对的字段当作没有。
    const auto bad = DrDictBrief::ParseReply(R"({"id":4,"word":"a","phonetics":{"uk":1,"us":"eɪ"}})");
    REQUIRE(bad.has_value() && bad->has_phonetics && bad->uk.empty() && bad->us == "eɪ");
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
