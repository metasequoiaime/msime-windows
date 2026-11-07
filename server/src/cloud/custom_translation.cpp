#include "custom_translation.h"

#include "utils/network_proxy.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>

namespace
{
// 词条实测 1.1–1.7s，12 字上下的短语会到 2.3s；2.5s 会把它们判成超时而进负缓存，
// 表现就是“这个词永远没翻译”。给到 4s，宁可偶发慢一点，不要诈死。
constexpr long kTimeoutMs = 4000;
// 整句翻译一条就是一段话，模型要多想几拍；2.5s 只够一个词。
constexpr long kSentenceTimeoutMs = 12000;
// 校对比翻译短：只回需要改的行，6 秒够一段短回复；整句校对沿用整句的超时。
constexpr long kReviewTimeoutMs = 6000;
// 最近一次失败的原因，供「测试配置」展示；每次批量调用开始时清空。
thread_local std::string g_last_error;
constexpr size_t kMaxResponseBytes = 1024 * 1024;
constexpr auto kBatchBudget = std::chrono::seconds(6);
// 整句一次请求就够，预算给足，但也不能把窗口卡住。
constexpr auto kSentenceBatchBudget = std::chrono::seconds(20);

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

std::string UppercaseLanguage(std::string language)
{
    std::transform(language.begin(), language.end(), language.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return language;
}

std::string StringFromValue(const nlohmann::json &value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (!value.is_object())
        return {};
    for (const char *key : {"text", "translation", "data"})
    {
        const auto found = value.find(key);
        if (found != value.end() && found->is_string())
            return found->get<std::string>();
    }
    return {};
}

// OpenAI 格式用的语种名。
const char *OpenAiLanguageName(const std::string &code)
{
    // 英文候选要的是中文释义；漏了这一条，中文目标会被当成默认值「英语」。
    if (code == "zh")
        return "中文";
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
    return "英语";
}

std::string ReplaceAll(std::string text, const std::string &token, const std::string &value)
{
    if (token.empty())
        return text;
    size_t at = 0;
    while ((at = text.find(token, at)) != std::string::npos)
    {
        text.replace(at, token.size(), value);
        at += value.size();
    }
    return text;
}

std::string TranslationSystemPrompt(const CustomTranslation::Config &config, const std::string &target)
{
    const std::string language = OpenAiLanguageName(target);
    if (!config.prompt.empty())
    {
        // 自定义提示词同样支持 {to}，方便照着小牛形状自己写。
        return ReplaceAll(ReplaceAll(config.prompt, "{lang}", language), "{to}", target);
    }
    // 一条默认提示词同时管两种输入：候选里既有单词，也有整句联想的句子，
    // 让提示词自己分流，比在代码里按长度切成两套提示词更不容易走岔
    // （自定义提示词只有一份，切两套的话句子必然用错）。
    // 输出沿用小牛翻译 text/translate/v2 的形状，解析层只取 trans_result[0].dst。
    return std::string("你是输入法候选词的翻译引擎。把用户给的内容翻译成") + language +
           "，只输出一个 JSON 对象，不要代码块、不要解释：\n"
           "{\"from\":\"zh\",\"to\":\"" +
           target +
           "\",\"termList\":[{\"src\":\"用户给的内容\",\"tgt\":\"译文\"}]}\n"
           "规则：\n"
           "1. 词或短语：tgt 最多两个义项，用「；」连接，去掉词性标注，尽量短。\n"
           "2. 整句：tgt 给完整译文，保留语气与标点，不缩写、不分条、不用分号。";
}

// 抹平各家模型常见的输出噪声：前后引号、反引号、换行、多余空白。

// 模型按小牛翻译 v2 的形状回 JSON 时，只取 trans_result[0].dst；
// 不是 JSON（老提示词、或模型没守格式）就原样返回，交给 CleanGloss 处理。
std::string ExtractNiutransDst(const std::string &text)
{
    std::string body = text;
    const auto first = body.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return text;
    body.erase(0, first);
    if (body.rfind("```", 0) == 0)
    {
        const auto start = body.find('\n');
        const auto end = body.rfind("```");
        if (start != std::string::npos && end != std::string::npos && end > start)
            body = body.substr(start + 1, end - start - 1);
    }
    if (body.empty() || body.front() != '{')
        return text;
    try
    {
        const auto root = nlohmann::json::parse(body);
        // 两种形状都认：v2 的 termList/tgt（当前提示词）与 v1 的 trans_result/dst（旧提示词）。
        for (const auto &shape : {std::pair<const char *, const char *>{"termList", "tgt"},
                                  std::pair<const char *, const char *>{"trans_result", "dst"}})
        {
            const auto result = root.find(shape.first);
            if (result == root.end() || !result->is_array() || result->empty())
                continue;
            const auto &first_item = result->front();
            if (!first_item.is_object())
                continue;
            const auto value = first_item.find(shape.second);
            if (value == first_item.end() || !value->is_string())
                continue;
            const std::string translated = value->get<std::string>();
            if (!translated.empty())
                return translated;
        }
        return text;
    }
    catch (...)
    {
        return text;
    }
}

std::string CleanGloss(std::string text)
{
    std::string out;
    out.reserve(text.size());
    bool space = false;
    for (const char ch : text)
    {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte == '\r' || byte == '\n' || byte == '\t' || byte == ' ')
        {
            space = !out.empty();
            continue;
        }
        if (space)
        {
            out.push_back(' ');
            space = false;
        }
        out.push_back(ch);
    }
    // 只在整串被同一对引号包住时才剥：模型对词条常回 "apple"，而一句台词
    // 「はい」と言った 也以「开头，逐字剥会把右引号留在句子里。
    static constexpr char kLeftCorner[] = "\xE3\x80\x8C";  // 「
    static constexpr char kRightCorner[] = "\xE3\x80\x8D"; // 」
    const char first = out.empty() ? '\0' : out.front();
    const char last = out.empty() ? '\0' : out.back();
    const bool ascii_pair = out.size() >= 2 && first == last && (first == '"' || first == '`' || first == '\'');
    const bool corner_pair = out.size() >= 6 && out.compare(0, 3, kLeftCorner, 3) == 0 &&
                             out.compare(out.size() - 3, 3, kRightCorner, 3) == 0;
    if (ascii_pair)
    {
        out.erase(out.begin());
        out.pop_back();
    }
    else if (corner_pair)
    {
        out.erase(0, 3);
        out.resize(out.size() - 3);
    }
    return out;
}

// 四种线格式：OpenAI Chat Completions / OpenAI Responses / Anthropic Messages / Google Gemini。
enum class Wire
{
    Chat,
    Responses,
    Anthropic,
    Gemini,
};

Wire WireFromFormat(const std::string &format)
{
    if (format == "responses")
        return Wire::Responses;
    if (format == "anthropic")
        return Wire::Anthropic;
    if (format == "gemini")
        return Wire::Gemini;
    return Wire::Chat;
}

std::string ExpandModel(std::string endpoint, const std::string &model)
{
    const std::string token = "{model}";
    const auto at = endpoint.find(token);
    if (at != std::string::npos)
        endpoint.replace(at, token.size(), model);
    return endpoint;
}

// 只填了主机名或 /v1 的地址，按协议补全默认路径（例如 https://api.deepseek.com → /chat/completions）。
std::string NormalizeEndpoint(std::string endpoint, Wire wire)
{
    while (!endpoint.empty() && endpoint.back() == '/')
        endpoint.pop_back();
    const auto scheme = endpoint.find("://");
    if (scheme == std::string::npos)
        return endpoint;
    const auto path_at = endpoint.find('/', scheme + 3);
    const std::string path = path_at == std::string::npos ? std::string() : endpoint.substr(path_at);
    const bool bare = path.empty();
    if (path.empty())
    {
        switch (wire)
        {
        case Wire::Chat:
            return endpoint + "/v1/chat/completions";
        case Wire::Responses:
            return endpoint + "/v1/responses";
        case Wire::Anthropic:
            return endpoint + "/v1/messages";
        case Wire::Gemini:
            return endpoint + "/v1beta/models/{model}:generateContent";
        }
    }
    if (path == "/v1" || path == "/v1beta" || path == "/v1beta1")
    {
        switch (wire)
        {
        case Wire::Chat:
            return endpoint + "/chat/completions";
        case Wire::Responses:
            return endpoint + "/responses";
        case Wire::Anthropic:
            return endpoint + "/messages";
        case Wire::Gemini:
            return endpoint + "/models/{model}:generateContent";
        }
    }
    (void)bare;
    // 只写了端点、漏了版本前缀，例如 https://api.deepseek.com/chat/completions。
    if (path == "/chat/completions" || path == "/responses" || path == "/messages")
        return endpoint.substr(0, path_at) + "/v1" + path;
    return endpoint;
}

// DeepSeek 的思考 token 也算在 max_tokens 里：不显式关掉，校对这类要求短回复的请求
// 会被思考吃满预算，返回空串（实测 completion_tokens 正好等于上限，正文为空）。
bool IsDeepSeekEndpoint(const std::string &endpoint)
{
    return endpoint.find("deepseek") != std::string::npos;
}

// 思考开关：用户显式选 disabled 就关；没配置时只对 deepseek 官方端点兜底。
// 别的服务（MiniMax 等）不认识 thinking 字段，没配置就不要塞。
bool ShouldDisableThinking(const CustomTranslation::Config &config)
{
    if (config.thinking == "disabled")
        return true;
    return config.thinking.empty() && IsDeepSeekEndpoint(config.endpoint);
}

std::string ReasoningEffortOf(const CustomTranslation::Config &config)
{
    return config.thinking == "low" || config.thinking == "high" || config.thinking == "max" ? config.thinking
                                                                                             : std::string{};
}

nlohmann::json BuildWireBody(Wire wire, const std::string &model, const std::string &system, const std::string &text,
                             int max_tokens, bool disable_thinking = false, const std::string &reasoning_effort = {})
{
    using nlohmann::json;
    switch (wire)
    {
    case Wire::Responses:
        return {{"model", model},   {"instructions", system},          {"input", text},
                {"temperature", 0}, {"max_output_tokens", max_tokens}, {"stream", false}};
    case Wire::Anthropic:
        return {{"model", model},
                {"max_tokens", max_tokens},
                {"temperature", 0},
                {"system", system},
                {"messages", json::array({{{"role", "user"}, {"content", text}}})}};
    case Wire::Gemini:
        return {{"systemInstruction", {{"parts", json::array({{{"text", system}}})}}},
                {"contents", json::array({{{"role", "user"}, {"parts", json::array({{{"text", text}}})}}})},
                {"generationConfig", {{"temperature", 0}, {"maxOutputTokens", max_tokens}}}};
    case Wire::Chat:
    default: {
        json body = {{"model", model},
                     {"temperature", 0},
                     {"max_tokens", max_tokens},
                     {"stream", false},
                     {"messages",
                      json::array({{{"role", "system"}, {"content", system}}, {{"role", "user"}, {"content", text}}})}};
        if (disable_thinking)
            body["thinking"] = {{"type", "disabled"}};
        else if (!reasoning_effort.empty())
        {
            body["thinking"] = {{"type", "enabled"}};
            body["reasoning_effort"] = reasoning_effort;
        }
        return body;
    }
    }
}

std::string JoinPartTexts(const nlohmann::json &parts)
{
    if (!parts.is_array())
        return {};
    std::string text;
    for (const auto &part : parts)
    {
        if (!part.is_object())
            continue;
        const auto value = part.find("text");
        if (value != part.end() && value->is_string())
            text += value->get<std::string>();
    }
    return text;
}

std::string ParseWireResponse(Wire wire, const std::string &response)
{
    try
    {
        const auto root = nlohmann::json::parse(response);
        // 各家都用 error 字段报错，直接当失败处理。
        const auto error = root.find("error");
        if (error != root.end() && !error->is_null())
            return {};
        switch (wire)
        {
        case Wire::Responses: {
            // {output:[{type:"message", content:[{type:"output_text", text}]}]}
            const auto output = root.find("output");
            if (output != root.end() && output->is_array())
            {
                for (const auto &item : *output)
                {
                    if (!item.is_object())
                        continue;
                    const auto content = item.find("content");
                    if (content == item.end())
                        continue;
                    const std::string text = JoinPartTexts(*content);
                    if (!text.empty())
                        return text;
                }
            }
            const auto direct = root.find("output_text");
            return direct != root.end() && direct->is_string() ? direct->get<std::string>() : std::string{};
        }
        case Wire::Anthropic: {
            // {content:[{type:"text", text}]}；也接受 content 直接是字符串的兼容实现。
            const auto content = root.find("content");
            if (content == root.end())
                return {};
            if (content->is_string())
                return content->get<std::string>();
            return JoinPartTexts(*content);
        }
        case Wire::Gemini: {
            // {candidates:[{content:{parts:[{text}]}}]}
            const auto candidates = root.find("candidates");
            if (candidates == root.end() || !candidates->is_array() || candidates->empty())
                return {};
            const auto content = candidates->front().find("content");
            if (content == candidates->front().end())
                return {};
            const auto parts = content->find("parts");
            return parts != content->end() ? JoinPartTexts(*parts) : std::string{};
        }
        case Wire::Chat:
        default: {
            const auto choices = root.find("choices");
            if (choices == root.end() || !choices->is_array() || choices->empty())
                return {};
            // 旧版 completions 直接给 text。
            const auto legacy = choices->front().find("text");
            if (legacy != choices->front().end() && legacy->is_string())
                return legacy->get<std::string>();
            const auto message = choices->front().find("message");
            if (message == choices->front().end())
                return {};
            const auto content = message->find("content");
            if (content == message->end())
                return {};
            // content 可能是字符串，也可能是 [{type:"text",text}] 数组。
            if (content->is_string())
                return content->get<std::string>();
            return JoinPartTexts(*content);
        }
        }
    }
    catch (...)
    {
    }
    return {};
}

// 四种格式共用一个批量入口：一条文本一个请求，词表释义只要一句译文。
std::vector<std::string> TextTranslateBatchOpenAi(const CustomTranslation::Config &config,
                                                  const std::vector<std::string> &texts, const std::string &source,
                                                  const std::string &target)
{
    std::vector<std::string> results(texts.size());
    if (texts.empty() || !CustomTranslation::IsSupportedEndpoint(config.endpoint) || target.empty() ||
        config.model.empty())
        return results;
    // OpenAI 四家都用 system+user 两段，源语种只隐含在提示词里；DeepLX 才需要它。
    (void)source;

    const Wire wire = WireFromFormat(config.format);
    g_last_error.clear();
    const std::string url = ExpandModel(NormalizeEndpoint(config.endpoint, wire), config.model);

    CURL *curl = curl_easy_init();
    if (!curl)
        return results;

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
    std::string auth_header;
    switch (wire)
    {
    case Wire::Anthropic:
        if (!config.api_key.empty())
        {
            auth_header = "x-api-key: " + config.api_key;
            headers = curl_slist_append(headers, auth_header.c_str());
        }
        headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
        break;
    case Wire::Gemini:
        if (!config.api_key.empty())
        {
            auth_header = "x-goog-api-key: " + config.api_key;
            headers = curl_slist_append(headers, auth_header.c_str());
        }
        break;
    case Wire::Chat:
    case Wire::Responses:
    default:
        if (!config.api_key.empty())
        {
            auth_header = "Authorization: Bearer " + config.api_key;
            headers = curl_slist_append(headers, auth_header.c_str());
        }
        break;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    NetworkProxy::ApplyToCurl(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    const long timeout_ms = config.sentence ? kSentenceTimeoutMs : kTimeoutMs;
    const auto batch_budget = config.sentence ? kSentenceBatchBudget : kBatchBudget;
    // 词条一句译文就几十个 token，整句要一整段；上限给小了长句子会被拦腰截断。
    const int max_tokens = config.sentence ? 1024 : 256;
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const std::string system_prompt = TranslationSystemPrompt(config, target);
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < texts.size(); ++i)
    {
        if (std::chrono::steady_clock::now() - started >= batch_budget)
        {
            results.resize(i);
            break;
        }
        const std::string payload = BuildWireBody(wire, config.model, system_prompt, texts[i], max_tokens,
                                                  ShouldDisableThinking(config), ReasoningEffortOf(config))
                                        .dump();
        ResponseBuffer response;
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        const CURLcode performed = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (performed == CURLE_OK && !response.overflow && status >= 200 && status < 300)
        {
            results[i] = CleanGloss(ExtractNiutransDst(ParseWireResponse(wire, response.text)));
            if (results[i].empty() && g_last_error.empty())
                g_last_error = "HTTP " + std::to_string(status) + "，响应里找不到译文：" +
                               CleanGloss(response.text).substr(0, 160);
        }
        else if (g_last_error.empty())
        {
            g_last_error = performed != CURLE_OK
                               ? std::string("网络错误：") + curl_easy_strerror(performed)
                               : "HTTP " + std::to_string(status) + "：" + CleanGloss(response.text).substr(0, 160);
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return results;
}

} // namespace

namespace CustomTranslation
{
bool IsSupportedEndpoint(const std::string &endpoint)
{
    if (endpoint.empty() || endpoint.size() > 2048 || endpoint.find_first_of("\r\n\0", 0, 3) != std::string::npos)
        return false;
    return endpoint.rfind("https://", 0) == 0 || endpoint.rfind("http://", 0) == 0;
}

std::string ParseTranslationResponse(const std::string &response)
{
    try
    {
        const auto root = nlohmann::json::parse(response);
        const auto code = root.find("code");
        if (code != root.end())
        {
            if (code->is_number_integer() && code->get<int>() != 200)
                return {};
            if (code->is_string() && code->get<std::string>() != "200")
                return {};
        }

        for (const char *key : {"data", "translation", "result"})
        {
            const auto found = root.find(key);
            if (found == root.end())
                continue;
            const std::string translated = StringFromValue(*found);
            if (!translated.empty())
                return translated;
            if (found->is_array() && !found->empty())
            {
                const std::string first = StringFromValue(found->front());
                if (!first.empty())
                    return first;
            }
        }

        const auto translations = root.find("translations");
        if (translations != root.end() && translations->is_array() && !translations->empty())
            return StringFromValue(translations->front());
    }
    catch (...)
    {
    }
    return {};
}

std::vector<std::string> TextTranslateBatch(const Config &config, const std::vector<std::string> &texts,
                                            const std::string &source, const std::string &target)
{
    if (config.format != "deeplx")
        return TextTranslateBatchOpenAi(config, texts, source, target);

    std::vector<std::string> results(texts.size());
    if (texts.empty() || !IsSupportedEndpoint(config.endpoint) || source.empty() || target.empty())
        return results;

    CURL *curl = curl_easy_init();
    if (!curl)
        return results;

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
    std::string authorization;
    if (!config.api_key.empty())
    {
        authorization = "Authorization: Bearer " + config.api_key;
        headers = curl_slist_append(headers, authorization.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, config.endpoint.c_str());
    NetworkProxy::ApplyToCurl(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < texts.size(); ++i)
    {
        if (std::chrono::steady_clock::now() - started >= kBatchBudget)
        {
            results.resize(i);
            break;
        }
        const nlohmann::json body = {
            {"text", texts[i]}, {"source_lang", UppercaseLanguage(source)}, {"target_lang", UppercaseLanguage(target)}};
        const std::string payload = body.dump();
        ResponseBuffer response;
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        const CURLcode performed = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (performed == CURLE_OK && !response.overflow && status >= 200 && status < 300)
            results[i] = ParseTranslationResponse(response.text);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return results;
}
std::vector<std::string> ReviewTranslations(const Config &config, const std::vector<std::string> &texts,
                                            const std::vector<std::string> &translations, const std::string &source,
                                            const std::string &target, bool sentence)
{
    std::vector<std::string> fixes(texts.size());
    // deeplx 没有聊天接口，做不了校对；这一条路径直接视为「无需修改」。
    if (texts.empty() || texts.size() != translations.size() || config.format == "deeplx" ||
        !IsSupportedEndpoint(config.endpoint) || config.model.empty())
        return fixes;

    std::string user;
    user.reserve(texts.size() * 32);
    for (size_t i = 0; i < texts.size(); ++i)
    {
        user += std::to_string(i + 1);
        user += "|";
        user += texts[i];
        user += "|";
        user += translations[i];
        user += "\n";
    }
    const std::string system =
        std::string("你是翻译校对员。下面每行是「编号|原文|译文」，译文是从") + OpenAiLanguageName(source) + "翻成" +
        OpenAiLanguageName(target) +
        "的。逐条检查译文是否准确、是否地道、是否只表达原文含义（尤其注意：单词不能拿包含它的复合词释义顶替）。"
        "只输出需要修改的行，格式为「编号|修正后的译文」，不要解释；全部正确就不要输出任何内容。";

    const Wire wire = WireFromFormat(config.format);
    const std::string url = ExpandModel(NormalizeEndpoint(config.endpoint, wire), config.model);
    CURL *curl = curl_easy_init();
    if (!curl)
        return fixes;

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
    std::string auth_header;
    switch (wire)
    {
    case Wire::Anthropic:
        if (!config.api_key.empty())
        {
            auth_header = "x-api-key: " + config.api_key;
            headers = curl_slist_append(headers, auth_header.c_str());
        }
        headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
        break;
    case Wire::Gemini:
        if (!config.api_key.empty())
        {
            auth_header = "x-goog-api-key: " + config.api_key;
            headers = curl_slist_append(headers, auth_header.c_str());
        }
        break;
    case Wire::Chat:
    case Wire::Responses:
    default:
        if (!config.api_key.empty())
        {
            auth_header = "Authorization: Bearer " + config.api_key;
            headers = curl_slist_append(headers, auth_header.c_str());
        }
        break;
    }

    const long timeout_ms = sentence ? kSentenceTimeoutMs : kReviewTimeoutMs;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    NetworkProxy::ApplyToCurl(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const std::string payload =
        BuildWireBody(wire, config.model, system, user, 1024, ShouldDisableThinking(config), ReasoningEffortOf(config))
            .dump();
    ResponseBuffer response;
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    const CURLcode performed = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (performed != CURLE_OK || response.overflow || status < 200 || status >= 300)
        return fixes;

    const std::string reply = ParseWireResponse(wire, response.text);
    const auto trim = [](std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return std::string{};
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    };
    size_t start = 0;
    while (start < reply.size())
    {
        const size_t end = reply.find('\n', start);
        std::string line = trim(reply.substr(start, end == std::string::npos ? std::string::npos : end - start));
        start = end == std::string::npos ? reply.size() : end + 1;
        size_t bar = line.find('|');
        size_t width = 1;
        if (bar == std::string::npos)
        {
            bar = line.find("\xEF\xBD\x9C"); // ｜
            width = 3;
        }
        if (bar == std::string::npos || bar == 0)
            continue;
        int index = 0;
        try
        {
            index = std::stoi(trim(line.substr(0, bar)));
        }
        catch (...)
        {
            continue;
        }
        if (index < 1 || static_cast<size_t>(index) > texts.size())
            continue;
        std::string fixed = trim(line.substr(bar + width));
        if (fixed.size() >= 2 && fixed.front() == '"' && fixed.back() == '"')
            fixed = fixed.substr(1, fixed.size() - 2);
        if (!fixed.empty())
            fixes[static_cast<size_t>(index) - 1] = fixed;
    }
    return fixes;
}

std::string LastError()
{
    return g_last_error;
}
} // namespace CustomTranslation
