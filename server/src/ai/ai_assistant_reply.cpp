#include "ai_assistant_reply.h"

#include <nlohmann/json.hpp>
#include <exception>
#include <string_view>

namespace
{
// 和引擎里 strip_reasoning 同一套规则：剥掉 <think>…</think>，没有闭合标签时从 <think> 起
// 全部丢弃（max_tokens 截断的半截思考比错误答案更危险），最后清掉开头的空白。
// 这里不直接引用引擎的 provider_protocol.h：设置进程不应该为了剥标签去链 Voice 库。
std::string StripReasoning(std::string_view text)
{
    std::string result(text);
    constexpr std::string_view open_tag = "<think>";
    constexpr std::string_view close_tag = "</think>";
    for (;;)
    {
        const size_t begin = result.find(open_tag);
        if (begin == std::string::npos)
            break;
        const size_t end = result.find(close_tag, begin);
        if (end == std::string::npos)
        {
            result.erase(begin);
            break;
        }
        result.erase(begin, end - begin + close_tag.size());
    }
    const size_t first = result.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    result.erase(0, first);
    return result;
}

// 展示给用户的截断内容只留开头，足够看出模型答成了什么格式。
std::string Preview(std::string text)
{
    constexpr size_t kMaxPreviewBytes = 160;
    for (char &ch : text)
    {
        if (ch == '\r' || ch == '\n' || ch == '\t')
            ch = ' ';
    }
    if (text.size() <= kMaxPreviewBytes)
        return text;
    text.resize(kMaxPreviewBytes);
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80)
        text.pop_back();
    return text + "…";
}
} // namespace

namespace AiAssistant::detail
{
Reply ParseSuggestion(std::string content)
{
    Reply reply;
    if (content.empty())
    {
        reply.failure = ChatCompletions::Failure::EmptyContent;
        reply.detail = "模型返回了空内容";
        return reply;
    }

    const std::string stripped = StripReasoning(content);
    if (stripped.empty())
    {
        // 整段都是思考段，或者是被 max_tokens 截断的半截思考。这一种在对本地推理模型时最常见，
        // 提示要明确指向输出长度，而不是笼统说「解析失败」。
        reply.failure = ChatCompletions::Failure::EmptyContent;
        reply.detail = "回复只有思考内容，没有给出候选；可尝试关闭思考或换更短的回答";
        return reply;
    }

    nlohmann::json parsed;
    try
    {
        parsed = nlohmann::json::parse(stripped);
    }
    catch (const std::exception &)
    {
        reply.failure = ChatCompletions::Failure::InvalidJson;
        reply.detail = "模型没有按要求返回 JSON：" + Preview(stripped);
        return reply;
    }

    if (!parsed.is_object() || !parsed.contains("candidates") || !parsed.at("candidates").is_array())
    {
        reply.failure = ChatCompletions::Failure::NoContent;
        reply.detail = "回复里没有 candidates 数组：" + Preview(stripped);
        return reply;
    }
    if (parsed.at("candidates").empty())
    {
        reply.failure = ChatCompletions::Failure::NoContent;
        reply.detail = "模型返回了空候选列表";
        return reply;
    }

    const auto &first = parsed.at("candidates").at(0);
    if (!first.is_object() || !first.contains("text") || !first.at("text").is_string())
    {
        reply.failure = ChatCompletions::Failure::NoContent;
        reply.detail = "候选格式不对：" + Preview(stripped);
        return reply;
    }
    reply.candidate = first.at("text").get<std::string>();
    if (reply.candidate.empty())
    {
        reply.failure = ChatCompletions::Failure::EmptyContent;
        reply.detail = "候选文本为空：" + Preview(stripped);
    }
    return reply;
}

std::string DescribeReplyFailure(const Reply &reply)
{
    return ChatCompletions::DescribeFailure(reply.failure, reply.detail);
}
} // namespace AiAssistant::detail
