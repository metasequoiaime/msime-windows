#pragma once

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

bool IsUsableConfig(const Config &config);

// Translates the whole batch in a single Chat Completions request, reusing the
// provider, endpoint, token and model configured on the AI 辅助 page. Empty
// entries mean that item failed or could not be parsed.
std::vector<std::string> TextTranslateBatch(const Config &config, const std::vector<std::string> &texts,
                                            const std::string &source, const std::string &target);
} // namespace AiTranslation
