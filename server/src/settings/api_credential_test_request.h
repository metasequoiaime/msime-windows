#pragma once

#include "ai/chat_request_params.h"

#include <nlohmann/json.hpp>
#include <string>

namespace ApiCredentialTest::detail
{
inline nlohmann::json BuildChatRequestBody(const std::string &provider, const std::string &model)
{
    nlohmann::json body = {
        {"model", model}, {"stream", false}, {"messages", {{{"role", "user"}, {"content", "Reply OK"}}}}};
    ChatCompletions::SetTokenLimit(body, provider, 1);
    if (provider == "deepseek")
        body["thinking"] = {{"type", "disabled"}};
    else if (provider == "siliconflow")
        body["enable_thinking"] = false;
    return body;
}
} // namespace ApiCredentialTest::detail
