#pragma once

#include "ai/ai_assistant_request.h"
#include "ai/chat_request_params.h"
#include "voice-input/voice_providers.h"

#include <nlohmann/json.hpp>
#include <string>

namespace ApiCredentialTest::detail
{
// 探测发出去的必须是真实联想请求，而不是另一个「看起来差不多」的请求体。
// 以前这里手写 {"messages":[user "Reply OK"]}，与 BuildRequestBody 各自演化：
// 探测没有 system 提示词、没有 response_format、输出上限 1，而验收却按联想格式卡，
// 于是任何按字面回答 "OK" 的模型都会被判成「回复格式不对」（issue #1265）。
// 复用一个构造器之后，提示词、输出格式策略、token 上限和 thinking 分支都不会再跑偏。
//
// 拼音与上下文用固定样例：探测要的是一次可复现的联想，不是用户的真实输入。
inline AiAssistant::Request BuildChatProbeRequest(const AiAssistantConfig &config)
{
    AiAssistant::Request request;
    request.pinyin_segments = {"shi", "shi"};
    request.context = "方案已经通过，明天开始";
    request.config = config;
    return request;
}

inline nlohmann::json BuildChatProbeRequestBody(const AiAssistantConfig &config)
{
    return AiAssistant::detail::BuildRequestBody(BuildChatProbeRequest(config));
}

// 超时也要跟真实请求一致：推理模型在正式使用时有 30 秒，探测用 15 秒会让它先超时。
inline long ChatProbeTimeoutMs(const AiAssistantConfig &config)
{
    return AiAssistant::detail::RequestTimeoutMs(config);
}

// 润色探测发出去的一句话。真实润色发的是用户的转录文本，探测只需要一句不会改变判定结果的话。
inline constexpr const char *kPolishProbeText = "今天天气不错，我们去公园走走吧。";

// 润色探测的超时。真实润色普通档只有 3 秒、开思考时 30 秒，那 3 秒是「等不到就回退原文」的
// 交互取舍，不是服务能力上限，探测照搬会把正常服务判成超时。这里普通档放宽到 15 秒、
// 思考档跟真实路径一样给 30 秒。
inline long PolishProbeTimeoutMs(const VoiceInputConfig &config)
{
    return VoiceInput::UsesDeepSeekThinking(config.polish_provider, config.polish_endpoint, config.polish_thinking)
               ? ChatCompletions::kReasoningTimeoutMs
               : 15000L;
}

// 与 engine/voice/src/provider_protocol.cpp 的 make_polish_request 同形：只有 system 与
// user 两条消息，不带 response_format；用户文本按真实链路的 WrapAsrUserMessage 包裹。
// 设置进程不链 Voice 库，所以形状在这里复刻。server/tests 的
// credential_test_polish_probe_matches_engine_contract 对着引擎实现比对两份形状。
inline nlohmann::json BuildPolishProbeRequestBody(const std::string &model, const std::string &prompt,
                                                  const std::string &provider, const std::string &endpoint,
                                                  const VoiceInputConfig &config)
{
    nlohmann::json body =
        nlohmann::json{{"model", model},
                       {"stream", false},
                       {"messages",
                        {{{"role", "system"}, {"content", prompt}},
                         {{"role", "user"}, {"content", VoiceInput::WrapAsrUserMessage(kPolishProbeText)}}}}};
    const auto vendor = VoiceInput::VendorForRequest(provider, endpoint);
    if (vendor == VoiceInput::ChatVendor::SiliconFlow)
        body["enable_thinking"] = false;
    else if (VoiceInput::UsesDeepSeekThinking(provider, endpoint, config.polish_thinking))
    {
        body["thinking"] = {{"type", "enabled"}};
        body["reasoning_effort"] = config.polish_thinking;
    }
    else if (vendor == VoiceInput::ChatVendor::DeepSeek)
        body["thinking"] = {{"type", "disabled"}};
    return body;
}

// 与 parse_polished_text 同口径，但入参是 ChatCompletions::FetchContent 已经取出的
// choices[0].message.content，不再解一层 JSON。润色的真实回复是纯文本，不是联想那套
// candidates JSON；剥掉思考段后非空才算可用。
inline std::string ParsePolishedText(const std::string &content)
{
    std::string value = content;
    // 推理模型会把 <think>…</think> 混在 content 里；未闭合的思考段之后没有可靠边界，整段丢弃。
    for (;;)
    {
        const size_t begin = value.find("<think>");
        if (begin == std::string::npos)
            break;
        const size_t end = value.find("</think>", begin);
        if (end == std::string::npos)
        {
            value.erase(begin);
            break;
        }
        value.erase(begin, end - begin + 8);
    }
    const size_t first = value.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : value.substr(first);
}
} // namespace ApiCredentialTest::detail
