#pragma once

#include <string>
#include <string_view>

namespace AiAssistant
{
// One user-managed provider entry, in the spirit of HanaAgent's provider registry: an arbitrary
// OpenAI-compatible service with its own name, endpoint and token. Kept dependency-free so both
// the config layer and the store can include it.
struct AiProvider
{
    std::string id;       // stable slug, also the value of ai_assistant.provider
    std::string name;     // shown in the settings UI
    std::string endpoint; // chat completions URL
    std::string token;
    std::string model;
};

// Ollama 的本机服务默认不做鉴权：选它时设置页不显示 API Token，请求不读存下的 Token，也不带 Authorization 头。
inline bool ProviderUsesToken(std::string_view provider)
{
    return provider != "ollama";
}

// 实际随请求发出的 Token。不用 Token 的服务商一律为空，配置里残留的旧值不会被发出去。
inline std::string RequestToken(std::string_view provider, const std::string &token)
{
    return ProviderUsesToken(provider) ? token : std::string();
}

// 鉴权请求头。Token 为空时返回空串，调用方据此不带这个头，而不是发一个空的 `Bearer `。
inline std::string AuthorizationHeader(const std::string &token)
{
    return token.empty() ? std::string() : "Authorization: Bearer " + token;
}
} // namespace AiAssistant