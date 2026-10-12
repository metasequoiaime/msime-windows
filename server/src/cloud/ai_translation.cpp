#include "ai_translation.h"

#include "ai/chat_completions.h"
#include "translation_gloss.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <exception>
#include <string>

namespace
{
std::string Trim(std::string value)
{
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    const auto begin = std::find_if(value.begin(), value.end(), not_space);
    const auto end = std::find_if(value.rbegin(), value.rend(), not_space).base();
    if (begin >= end)
        return {};
    return std::string(begin, end);
}

std::string StripMarkdownFences(std::string text)
{
    text = Trim(std::move(text));
    if (text.rfind("```", 0) != 0)
        return text;
    const size_t newline = text.find('\n');
    if (newline == std::string::npos)
        return text;
    text = text.substr(newline + 1);
    const size_t closing = text.rfind("```");
    if (closing != std::string::npos)
        text = text.substr(0, closing);
    return Trim(std::move(text));
}

std::string LanguageName(const std::string &code)
{
    if (code == "zh")
        return "简体中文";
    if (code == "en")
        return "英语";
    if (code == "fr")
        return "法语";
    if (code == "ja")
        return "日语";
    if (code == "es")
        return "西班牙语";
    if (code == "ru")
        return "俄语";
    if (code == "de")
        return "德语";
    if (code == "ko")
        return "韩语";
    if (code == "th")
        return "泰语";
    if (code == "vi")
        return "越南语";
    if (code == "it")
        return "意大利语";
    return code;
}

std::string BuildSystemPrompt(const std::string &source, const std::string &target)
{
    return "你是一个翻译引擎。输入是一个 JSON 数组，数组元素是需要翻译的词条，源语言是" + LanguageName(source) +
           "，目标语言是" + LanguageName(target) +
           "。把每个词条翻译成目标语言，只输出合法 JSON，不要解释或输出 Markdown：\n"
           "{\"translations\":[\"译文\"]}\n"
           "数组长度和顺序必须与输入完全一致，无法翻译的位置返回空字符串。译文要简短，适合放在输入法候选窗里。";
}

// Picks the translated strings out of a Chat Completions content string. A reply that is
// valid JSON but has none of the expected shapes, or an array whose length differs from the
// request, yields nothing: guessing would show the raw reply in the candidate window and
// persist it into the user's gloss store.
std::vector<std::string> ParseTranslations(const std::string &content, size_t expected)
{
    std::vector<std::string> results(expected);
    if (expected == 0)
        return results;

    const auto from_array = [&](const nlohmann::json &array) {
        if (!array.is_array() || array.size() != expected)
            return false;
        for (size_t i = 0; i < expected; ++i)
        {
            if (array[i].is_string())
                results[i] = Trim(array[i].get<std::string>());
        }
        return true;
    };

    const std::string text = StripMarkdownFences(content);
    if (text.empty())
        return results;
    if (nlohmann::json::accept(text))
    {
        try
        {
            const auto root = nlohmann::json::parse(text);
            if (from_array(root))
                return results;
            if (root.is_object())
            {
                for (const char *key : {"translations", "translation", "results", "data"})
                {
                    const auto found = root.find(key);
                    if (found != root.end() && from_array(*found))
                        return results;
                }
                for (const char *key : {"translation", "text", "result"})
                {
                    const auto found = root.find(key);
                    if (found != root.end() && found->is_string() && expected == 1)
                    {
                        results[0] = Trim(found->get<std::string>());
                        return results;
                    }
                }
            }
        }
        catch (const std::exception &)
        {
        }
        return std::vector<std::string>(expected);
    }

    // Some providers ignore response_format and answer with one line per entry. Text that
    // looks like broken JSON is not a list of translations.
    if (text.front() == '{' || text.front() == '[')
        return results;
    std::vector<std::string> lines;
    std::string current;
    for (const char ch : text)
    {
        if (ch == '\n')
        {
            lines.push_back(Trim(current));
            current.clear();
        }
        else
        {
            current.push_back(ch);
        }
    }
    lines.push_back(Trim(current));
    if (lines.size() == expected)
        return lines;
    return results;
}
} // namespace

namespace AiTranslation
{
std::vector<std::string> ParseTranslations(const std::string &content, size_t expected)
{
    return ::ParseTranslations(content, expected);
}

Config FromConfiguredTranslation(const TranslationConfig &config)
{
    return {config.ai.provider, CloudTranslation::TrimSecret(config.ai.endpoint),
            CloudTranslation::TrimSecret(config.ai.token), CloudTranslation::TrimSecret(config.ai.model)};
}

bool IsUsableConfig(const Config &config)
{
    return CloudTranslation::IsUsableSecret(config.token) && !config.model.empty() &&
           (config.endpoint.rfind("https://", 0) == 0 || config.endpoint.rfind("http://", 0) == 0);
}

std::vector<std::string> TextTranslateBatch(const Config &config, const std::vector<std::string> &texts,
                                            const std::string &source, const std::string &target)
{
    std::vector<std::string> results(texts.size());
    if (texts.empty() || !IsUsableConfig(config) || source.empty() || target.empty())
        return results;

    nlohmann::json body = {{"model", config.model},
                           {"stream", false},
                           {"temperature", 0.0},
                           {"response_format", {{"type", "json_object"}}},
                           {"messages",
                            {{{"role", "system"}, {"content", BuildSystemPrompt(source, target)}},
                             {{"role", "user"}, {"content", nlohmann::json(texts).dump()}}}}};
    if (config.provider == "deepseek")
    {
        // 与 AI 辅助保持一致，避免推理模型的思考内容拖慢候选翻译。
        body["thinking"] = {{"type", "disabled"}};
    }
    const auto content =
        ChatCompletions::FetchContent(config.endpoint, config.token, body.dump(), ChatCompletions::kDefaultTimeoutMs);
    return content ? ParseTranslations(*content, texts.size()) : results;
}
} // namespace AiTranslation
