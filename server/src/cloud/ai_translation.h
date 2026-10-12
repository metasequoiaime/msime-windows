#pragma once

#include "config/ime_config.h"

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

// token 按 CloudTranslation::IsUsableSecret 校验，出厂模板里的占位符不算可用。
bool IsUsableConfig(const Config &config);

// Translates the whole batch in a single Chat Completions request, reusing the
// provider, endpoint, token and model configured on the AI 辅助 page. Empty
// entries mean that item failed or could not be parsed.
std::vector<std::string> TextTranslateBatch(const Config &config, const std::vector<std::string> &texts,
                                            const std::string &source, const std::string &target);

// Parses the translation list out of an LLM response string. Anything that does not
// line up one-to-one with the request yields empty entries rather than a guess.
std::vector<std::string> ParseTranslations(const std::string &content, size_t expected);
} // namespace AiTranslation
