#pragma once

#include "config/ime_config.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace AiTranslation
{
struct Config
{
    std::string provider;
    std::string endpoint;
    std::string token;
    std::string model;
};

// 候选词翻译复用 AI 辅助当前服务商的配置。运行时翻译和设置页的凭据测试都从这里取，
// 两边对「配置是否可用」的判断不会分叉。
Config FromConfiguredTranslation(const TranslationConfig &config);

// token 按 CloudTranslation::IsUsableSecret 校验，出厂模板里的占位符不算可用；Ollama 不用 Token。
bool IsUsableConfig(const Config &config);

// 设置页输入框里的原始值：去空白、接口地址没写协议时补 https。凭据测试用它，
// 测的就是页面上填的东西，而不是上一次落盘的配置。
Config FromFields(const std::string &provider, const std::string &endpoint, const std::string &token,
                  const std::string &model);

namespace detail
{
nlohmann::json BuildRequestBody(const Config &config, const std::vector<std::string> &texts, const std::string &source,
                                const std::string &target);
long RequestTimeoutMs(const Config &config);
} // namespace detail

// Translates the whole batch in a single Chat Completions request, reusing the
// provider, endpoint, token and model configured on the AI 辅助 page. Empty
// entries mean that item failed or could not be parsed. failure_detail 非空且最终
// 没有译文时，写入一条可以直接展示给用户的失败原因（网络错误、HTTP 状态码、服务端
// 返回的 error.message，或回复格式不对）。
std::vector<std::string> TextTranslateBatch(const Config &config, const std::vector<std::string> &texts,
                                            const std::string &source, const std::string &target,
                                            std::string *failure_detail = nullptr);

// Parses the translation list out of an LLM response string. Anything that does not
// line up one-to-one with the request yields empty entries rather than a guess.
std::vector<std::string> ParseTranslations(const std::string &content, size_t expected);
} // namespace AiTranslation
