#include "ai_translation.h"

#include "utils/network_proxy.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <exception>
#include <string>

namespace
{
constexpr long kConnectTimeoutMs = 2500;
constexpr long kTimeoutMs = 5000;
constexpr size_t kMaxResponseBytes = 1024 * 1024;

struct ResponseBuffer
{
    std::string text;
    bool overflow = false;
};

size_t WriteResponse(char *data, size_t size, size_t count, void *user)
{
    const size_t bytes = size * count;
    auto *buffer = static_cast<ResponseBuffer *>(user);
    if (bytes > kMaxResponseBytes - (std::min)(buffer->text.size(), kMaxResponseBytes))
    {
        buffer->overflow = true;
        return 0;
    }
    buffer->text.append(data, bytes);
    return bytes;
}

std::string Trim(std::string value)
{
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    const auto begin = std::find_if(value.begin(), value.end(), not_space);
    const auto end = std::find_if(value.rbegin(), value.rend(), not_space).base();
    if (begin >= end)
        return {};
    return std::string(begin, end);
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

// Picks the translated strings out of a Chat Completions content string.
std::vector<std::string> ParseTranslations(const std::string &content, size_t expected)
{
    std::vector<std::string> results(expected);

    const auto from_array = [&](const nlohmann::json &array) {
        if (!array.is_array())
            return false;
        for (size_t i = 0; i < expected && i < array.size(); ++i)
        {
            if (array[i].is_string())
                results[i] = Trim(array[i].get<std::string>());
        }
        return true;
    };

    try
    {
        const auto root = nlohmann::json::parse(content);
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
                if (found != root.end() && found->is_string() && expected > 0)
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

    // Some providers ignore response_format and answer with one line per entry.
    if (expected > 0)
    {
        std::vector<std::string> lines;
        std::string current;
        for (const char ch : content)
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
        if (expected == 1)
            results[0] = Trim(content);
    }
    return results;
}
} // namespace

namespace AiTranslation
{
std::vector<std::string> ParseTranslations(const std::string &content, size_t expected)
{
    return ::ParseTranslations(content, expected);
}

bool IsUsableConfig(const Config &config)
{
    return !config.endpoint.empty() && !config.token.empty() && !config.model.empty() &&
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
    const std::string payload = body.dump();

    CURL *curl = curl_easy_init();
    if (!curl)
        return results;

    ResponseBuffer response;
    const std::string authorization = "Authorization: Bearer " + config.token;
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
    headers = curl_slist_append(headers, authorization.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, config.endpoint.c_str());
    NetworkProxy::ApplyToCurl(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode performed = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (performed != CURLE_OK || response.overflow || status < 200 || status >= 300)
        return results;

    try
    {
        const auto outer = nlohmann::json::parse(response.text);
        const std::string content = outer.at("choices").at(0).at("message").at("content").get<std::string>();
        return ParseTranslations(content, texts.size());
    }
    catch (const std::exception &)
    {
        return results;
    }
}
} // namespace AiTranslation
