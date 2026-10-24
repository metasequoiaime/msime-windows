#pragma once

#include "ai/ai_assistant.h"
#include "ai/chat_completions.h"

#include <string>

// AI 联想取回 content 之后还要剥思考段并解析出候选。这一步失败同样只能表现为「没有候选」，
// 和网络失败混在一起用户就分不清了。这里只放纯函数，方便单测和设置页复用。
namespace AiAssistant::detail
{
struct Reply
{
    // 解析出的第一个候选文本。空串表示这次没有可上屏的结果。
    std::string candidate;
    ChatCompletions::Failure failure = ChatCompletions::Failure::None;
    std::string detail;
};

// content 已经是 choices[0].message.content。先剥 <think> 思考段，再按
// {"candidates":[{"text":...}]} 解析；整段都是思考、JSON 不合法或 candidates 为空都算失败，
// 并给出各自的原因。
Reply ParseSuggestion(std::string content);

// 把 ParseSuggestion 的失败原因变成一句中文。
std::string DescribeReplyFailure(const Reply &reply);
} // namespace AiAssistant::detail
