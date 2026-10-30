#include "ai/ai_assistant_reply.h"
#include "ai/chat_completions.h"
#include "tests/includes/test_framework.h"

#include <curl/curl.h>

using ChatCompletions::Failure;

TEST_CASE(ai_reply_parses_the_first_candidate)
{
    const auto reply = AiAssistant::detail::ParseSuggestion(R"({"candidates":[{"text":"你好"},{"text":"拟好"}]})");
    REQUIRE_EQ(reply.candidate, std::string("你好"));
    REQUIRE(reply.failure == Failure::None);
}

TEST_CASE(ai_reply_strips_think_blocks_before_parsing)
{
    // MiniMax 这类强制思考的模型把思考段混进 content，剥掉之后才能拿到 JSON。
    const auto reply =
        AiAssistant::detail::ParseSuggestion("<think>先看拼音 a a a</think>\n{\"candidates\":[{\"text\":\"啊啊啊\"}]}");
    REQUIRE_EQ(reply.candidate, std::string("啊啊啊"));
}

TEST_CASE(ai_reply_reports_thinking_only_replies)
{
    // 整段都是思考段（max_tokens 被截断的典型形态）：要明确指向输出长度，而不是笼统的解析失败。
    const auto reply = AiAssistant::detail::ParseSuggestion("<think>我要先想一想");
    REQUIRE(reply.candidate.empty());
    REQUIRE(reply.failure == Failure::EmptyContent);
    REQUIRE(reply.detail.find("思考") != std::string::npos);
}

TEST_CASE(ai_reply_reports_empty_content)
{
    const auto reply = AiAssistant::detail::ParseSuggestion("");
    REQUIRE(reply.failure == Failure::EmptyContent);
    REQUIRE(!reply.detail.empty());
}

TEST_CASE(ai_reply_reports_non_json_replies_with_a_preview)
{
    // 本地小模型无视 response_format 直接说人话，是最常见的「连得上但没候选」。
    const auto reply = AiAssistant::detail::ParseSuggestion("好的，我认为是「啊啊啊」。");
    REQUIRE(reply.failure == Failure::InvalidJson);
    REQUIRE(reply.detail.find("啊啊啊") != std::string::npos);
}

TEST_CASE(ai_reply_reports_missing_or_malformed_candidates)
{
    for (const char *body : {R"({"text":"你好"})", R"({"candidates":"你好"})", R"({"candidates":[]})",
                             R"({"candidates":[{"t":1}]})", R"([{"text":"你好"}])"})
    {
        const auto reply = AiAssistant::detail::ParseSuggestion(body);
        REQUIRE(reply.candidate.empty());
        REQUIRE(reply.failure == Failure::NoContent);
        REQUIRE(!reply.detail.empty());
    }
}

TEST_CASE(ai_reply_reports_blank_candidate_text)
{
    const auto reply = AiAssistant::detail::ParseSuggestion(R"({"candidates":[{"text":""}]})");
    REQUIRE(reply.candidate.empty());
    REQUIRE(reply.failure == Failure::EmptyContent);
}

TEST_CASE(ai_reply_collapses_newlines_in_the_preview)
{
    const auto reply = AiAssistant::detail::ParseSuggestion("第一行\n第二行\t含制表符");
    REQUIRE(reply.detail.find('\n') == std::string::npos);
    REQUIRE(reply.detail.find('\t') == std::string::npos);
}

TEST_CASE(chat_failure_descriptions_name_the_cause)
{
    // 每种原因都要有一句独立的说法，不能都退化到同一句「失败」。
    const std::vector<Failure> failures{Failure::Request,   Failure::Network,     Failure::Status,
                                        Failure::Timeout,   Failure::TooLarge,    Failure::InvalidJson,
                                        Failure::NoContent, Failure::EmptyContent};
    std::vector<std::string> descriptions;
    for (const auto failure : failures)
    {
        const auto text = ChatCompletions::DescribeFailure(failure, "");
        REQUIRE(!text.empty());
        for (const auto &seen : descriptions)
            REQUIRE(seen != text);
        descriptions.push_back(text);
    }
}

TEST_CASE(chat_failure_description_carries_the_detail)
{
    const auto text = ChatCompletions::DescribeFailure(Failure::Status, "HTTP 400：response_format is not supported");
    REQUIRE(text.find("HTTP 400") != std::string::npos);
    REQUIRE(text.find("response_format") != std::string::npos);
    REQUIRE(ChatCompletions::DescribeFailure(Failure::None, "") == std::string());
}

TEST_CASE(chat_fetch_reports_connection_failures_instead_of_silence)
{
    // 关掉的端口是连接被拒的稳定构造：以前这里返回 nullopt，调用方只能给出「未返回有效译文」。
    // 现在必须带上原因和 curl 的详细错误，用户据此能判断是服务没起还是地址写错。
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const auto result = ChatCompletions::FetchContent("http://127.0.0.1:1/v1/chat/completions", "",
                                                      R"({"model":"m","messages":[]})", 2000);
    REQUIRE(!result.content.has_value());
    REQUIRE(result.failure == Failure::Network);
    REQUIRE(!result.detail.empty());

    const auto described = ChatCompletions::DescribeFailure(result.failure, result.detail);
    REQUIRE(described.find(result.detail) != std::string::npos);
    curl_global_cleanup();
}

TEST_CASE(chat_fetch_reports_a_timeout_apart_from_other_failures)
{
    // 连不通的端口可能立刻被拒也可能先超时，两种结果都要落到明确的分类里，不许回落成空。
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const auto result = ChatCompletions::FetchContent("http://127.0.0.1:1/v1/chat/completions", "", "{}", 1);
    REQUIRE(!result.content.has_value());
    REQUIRE((result.failure == Failure::Network || result.failure == Failure::Timeout));
    REQUIRE(!result.detail.empty());
    curl_global_cleanup();
}
