#pragma once

#include "config/ime_config.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace AiAssistant
{
struct Request
{
    std::vector<std::string> pinyin_segments;
    std::string context;
    std::string identity;
    AiAssistantConfig config;
    // A bounded snapshot of local candidates, used only by choice providers.
    std::vector<std::string> candidates;
};

using ApplyCallback = std::function<void(const std::string &, const std::string &, uint64_t)>;
void Start(ApplyCallback callback);
void Stop();
void OnInputChanged(Request request);
void Clear();
#if defined(MSIME_AI_ASSISTANT_TEST)
namespace testing
{
void SetAfterFetchHook(std::function<void(const std::string &)> hook);
}
#endif
} // namespace AiAssistant
