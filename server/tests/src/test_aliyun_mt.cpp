#include "tests/includes/test_framework.h"

#include "cloud/aliyun_mt.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

TEST_CASE(AliyunRequiresBothCredentials)
{
    REQUIRE(AliyunMt::IsUsableCredentials({"LTAIrealLookingId", "realLookingSecret"}));
    REQUIRE(!AliyunMt::IsUsableCredentials({"", "realLookingSecret"}));
    REQUIRE(!AliyunMt::IsUsableCredentials({"LTAIrealLookingId", ""}));
    // 出厂占位符不能被当成真凭据。
    REQUIRE(!AliyunMt::IsUsableCredentials({"<YOUR_ALIYUN_ACCESS_KEY_ID>", "<YOUR_ALIYUN_ACCESS_KEY_SECRET>"}));
    REQUIRE(!AliyunMt::IsUsableCredentials({"  <YOUR_ALIYUN_ACCESS_KEY_ID>  ", "FAKESECRET_abc"}));
}

TEST_CASE(AliyunPercentEncodingFollowsRfc3986)
{
    // 只保留 unreserved；空格编成 %20。
    REQUIRE_EQ(AliyunMt::PercentEncode("a b/中~*"), std::string("a%20b%2F%E4%B8%AD~%2A"));
    REQUIRE_EQ(AliyunMt::PercentEncode("A-Z_a.z~9"), std::string("A-Z_a.z~9"));
    REQUIRE_EQ(AliyunMt::PercentEncode("{}:,"), std::string("%7B%7D%3A%2C"));
}

TEST_CASE(AliyunSignatureMatchesReferenceVector)
{
    // 参考值由独立 Python 实现算出，不是被测代码自证。
    const AliyunMt::Credentials credentials{"testid", "testsecret"};
    const std::vector<std::pair<std::string, std::string>> parameters = {
        {"Action", "GetBatchTranslate"},
        {"Version", "2018-10-12"},
        {"Format", "JSON"},
        {"AccessKeyId", "testid"},
        {"SignatureMethod", "HMAC-SHA1"},
        {"SignatureVersion", "1.0"},
        {"SignatureNonce", "3ee8c1b8-83d3-44af-a94f-4e0ad82fd6cf"},
        {"Timestamp", "2024-01-01T00:00:00Z"},
        {"ApiType", "translate_standard"},
        {"Scene", "general"},
        {"FormatType", "text"},
        {"SourceLanguage", "zh"},
        {"TargetLanguage", "en"},
        {"SourceText", R"({"0":"你好"})"},
    };
    REQUIRE_EQ(AliyunMt::BuildSignature(credentials, "POST", parameters), std::string("cqFYpbdsgKT/iLb/JP10CUBamgY="));

    // 参数顺序不影响签名。
    auto reordered = parameters;
    std::reverse(reordered.begin(), reordered.end());
    REQUIRE_EQ(AliyunMt::BuildSignature(credentials, "POST", reordered), std::string("cqFYpbdsgKT/iLb/JP10CUBamgY="));

    // Signature 参数被忽略。
    auto with_signature = parameters;
    with_signature.emplace_back("Signature", "ignored");
    REQUIRE_EQ(AliyunMt::BuildSignature(credentials, "POST", with_signature),
               std::string("cqFYpbdsgKT/iLb/JP10CUBamgY="));
}

TEST_CASE(AliyunParsesBatchResponses)
{
    // TranslatedList 在顶层。
    const std::string top_level = R"({"Code":200,"Message":"success","RequestId":"x","TranslatedList":[
        {"code":"200","index":"1","translated":"go home"},
        {"code":"200","index":"0","translated":"hello boy"}]})";
    const auto top = AliyunMt::ParseBatchResponse(top_level, 3);
    REQUIRE_EQ(top.size(), size_t(3));
    REQUIRE_EQ(top[0], std::string("hello boy"));
    REQUIRE_EQ(top[1], std::string("go home"));
    REQUIRE(top[2].empty());
}

TEST_CASE(AliyunIgnoresFailedItemsAndMalformedResponses)
{
    // 单条失败留空，交给上层退避。
    const std::string partial = R"({"Code":200,"TranslatedList":[
        {"code":"105","index":"0","translated":"","message":"不支持该语言对"},
        {"code":"200","index":"1","translated":"ok"}]})";
    const auto results = AliyunMt::ParseBatchResponse(partial, 2);
    REQUIRE(results[0].empty());
    REQUIRE_EQ(results[1], std::string("ok"));

    // 越界 index、缺 TranslatedList、非 JSON 都得到全空。
    const auto out_of_range =
        AliyunMt::ParseBatchResponse(R"({"TranslatedList":[{"code":"200","index":"9","translated":"x"}]})", 1);
    REQUIRE(out_of_range[0].empty());
    // 返回长度恒等于 count，失败项留空。
    const auto no_list = AliyunMt::ParseBatchResponse(R"({"Code":200,"Message":"success"})", 2);
    REQUIRE_EQ(no_list.size(), size_t(2));
    REQUIRE(no_list[0].empty() && no_list[1].empty());
    const auto not_json = AliyunMt::ParseBatchResponse("not json", 2);
    REQUIRE_EQ(not_json.size(), size_t(2));
    REQUIRE(not_json[0].empty() && not_json[1].empty());

    // 整体失败时即使带 TranslatedList 也整批作废。
    const auto overall_error = AliyunMt::ParseBatchResponse(
        R"({"Code":10001,"Message":"请求超时","TranslatedList":[{"index":"0","translated":"x"}]})", 1);
    REQUIRE(overall_error[0].empty());
    // 字符串错误码同样挡住。
    REQUIRE(AliyunMt::ParseBatchResponse(R"({"Code":"10001","TranslatedList":[{"index":"0","translated":"x"}]})", 1)[0]
                .empty());

    // index 字符串或数字都能对上。
    const auto numeric = AliyunMt::ParseBatchResponse(R"({"TranslatedList":[{"index":0,"translated":"n"}]})", 1);
    REQUIRE_EQ(numeric[0], std::string("n"));
}

// 每批的 SourceText key 必须用批次内下标，否则第二批起全部越界。
TEST_CASE(AliyunBatchIndexIsRelativeToEachBatch)
{
    const std::vector<std::string> texts(51, "w");
    // 第二批的 key 从 "0" 重新计数。
    const auto first = nlohmann::json::parse(AliyunMt::BuildBatchSourceText(texts, 0, 50));
    REQUIRE_EQ(first["0"], std::string("w"));
    REQUIRE_EQ(first["49"], std::string("w"));
    REQUIRE(first.find("50") == first.end());

    const auto second = nlohmann::json::parse(AliyunMt::BuildBatchSourceText(texts, 50, 51));
    REQUIRE_EQ(second.size(), size_t(1));
    REQUIRE_EQ(second["0"], std::string("w"));
    REQUIRE(second.find("50") == second.end());

    // 响应用同一套下标。
    const auto parsed = AliyunMt::ParseBatchResponse(R"({"Code":200,"TranslatedList":[
        {"code":"200","index":"0","translated":"x"}]})",
                                                     1);
    REQUIRE_EQ(parsed[0], std::string("x"));
}

TEST_CASE(AliyunEmptyInputSendsNothing)
{
    const AliyunMt::Credentials credentials{"LTAIrealLookingId", "realLookingSecret"};
    REQUIRE(AliyunMt::TextTranslateBatch(credentials, {}, "zh", "en").empty());
    // 凭据不可用时不发请求。
    REQUIRE_EQ(AliyunMt::TextTranslateBatch({}, {"你好"}, "zh", "en").size(), size_t(1));
}

// 第一批连不上就停：后面的批次不再各赔一次超时，结果截断在第一批之前。
// 上层把「结果比请求短」当成没发出，不计失败退避。
TEST_CASE(AliyunStopsAtTheFirstBatchThatCannotBeSent)
{
    const AliyunMt::Credentials credentials{"LTAIrealLookingId", "realLookingSecret"};
    // 端口 1 上没有监听，连接立即被拒。
    const std::vector<std::string> texts(120, "w");
    const auto results = AliyunMt::TextTranslateBatch(credentials, texts, "zh", "en", "http://127.0.0.1:1");
    REQUIRE(results.empty());
}
