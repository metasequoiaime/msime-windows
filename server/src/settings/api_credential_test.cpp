#include "api_credential_test.h"

#include "ai/codex_cli.h"
#include "cloud/custom_translation.h"
#include "cloud/niutrans_translation.h"
#include "cloud/tencent_tmt.h"
#include "cloud/translation_gloss.h"
#include "voice-input/doubao_asr_client.h"

#include "utils/network_proxy.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

namespace
{
constexpr long kConnectTimeoutMs = 5000;
constexpr long kRequestTimeoutMs = 15000;
constexpr std::size_t kMaxResponseBytes = 256 * 1024;
// OpenRouter 等聚合服务的 /models 带描述和价格，能到数 MB。
constexpr std::size_t kMaxModelListBytes = 16 * 1024 * 1024;

struct HttpResponse
{
    CURLcode code = CURLE_FAILED_INIT;
    long status = 0;
    std::string body;
    std::string error;
    std::size_t max_bytes = kMaxResponseBytes;
};

std::string Value(const ApiCredentialTest::Request &request, const char *key)
{
    const auto found = request.config.find(key);
    return found == request.config.end() ? std::string{} : CloudTranslation::TrimSecret(found->second);
}

size_t WriteResponse(char *data, size_t size, size_t count, void *user)
{
    const size_t bytes = size * count;
    auto *response = static_cast<HttpResponse *>(user);
    if (bytes > response->max_bytes - (std::min)(response->body.size(), response->max_bytes))
        return 0;
    response->body.append(data, bytes);
    return bytes;
}

void InitCurl()
{
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

bool IsHttpEndpoint(const std::string &endpoint)
{
    return endpoint.rfind("https://", 0) == 0 || endpoint.rfind("http://", 0) == 0;
}

std::string ErrorDetail(const HttpResponse &response)
{
    if (response.code != CURLE_OK)
        return response.error.empty() ? curl_easy_strerror(response.code) : response.error;
    try
    {
        const auto root = nlohmann::json::parse(response.body);
        if (root.contains("error"))
        {
            const auto &error = root.at("error");
            if (error.is_object() && error.contains("message") && error.at("message").is_string())
                return error.at("message").get<std::string>();
            if (error.is_string())
                return error.get<std::string>();
        }
        if (root.contains("message") && root.at("message").is_string())
            return root.at("message").get<std::string>();
        if (root.contains("errorMsg") && root.at("errorMsg").is_string())
            return root.at("errorMsg").get<std::string>();
    }
    catch (...)
    {
    }
    return "HTTP " + std::to_string(response.status);
}

// payload 为空指针时发 GET，否则以 JSON body 发 POST。
HttpResponse PerformJsonRequest(const std::string &endpoint, const std::string &token, const std::string *payload,
                                std::size_t max_bytes = kMaxResponseBytes)
{
    InitCurl();
    HttpResponse response;
    response.max_bytes = max_bytes;
    CURL *curl = curl_easy_init();
    if (!curl)
        return response;
    char error[CURL_ERROR_SIZE] = {};
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, payload ? "Content-Type: application/json" : "Accept: application/json");
    const std::string authorization = "Authorization: Bearer " + token;
    headers = curl_slist_append(headers, authorization.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    NetworkProxy::ApplyToCurl(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (payload)
    {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload->size()));
    }
    else
    {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kRequestTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    response.code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    response.error = error;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

// 由 Chat Completions 地址或 base URL 推出 OpenAI 兼容的 /models 地址。
// 查询串（如 Azure 的 api-version）原样保留，只改写路径部分。
std::string ModelListEndpoint(const std::string &endpoint)
{
    const std::size_t query_at = endpoint.find_first_of("?#");
    std::string path = endpoint.substr(0, query_at);
    const std::string query = query_at == std::string::npos ? std::string() : endpoint.substr(query_at);
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    const auto ends_with = [&path](std::string_view suffix) {
        return path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    constexpr std::string_view kChatSuffix = "/chat/completions";
    if (ends_with(kChatSuffix))
        path.resize(path.size() - kChatSuffix.size());
    if (!ends_with("/models"))
        path += "/models";
    return path + query;
}

ApiCredentialTest::Result TestChat(const ApiCredentialTest::Request &request)
{
    const std::string token = Value(request, "token");
    const std::string endpoint = Value(request, "endpoint");
    const std::string model = Value(request, "model");
    if (!CloudTranslation::IsUsableSecret(token))
        return {false, "请先填写有效的 API Key。"};
    if (!IsHttpEndpoint(endpoint) || model.empty())
        return {false, "请填写有效的 HTTPS 接口地址和模型名。"};
    nlohmann::json body = {{"model", model},
                           {"stream", false},
                           {"max_tokens", 1},
                           {"messages", {{{"role", "user"}, {"content", "Reply OK"}}}}};
    const std::string provider = Value(request, "provider");
    if (provider == "deepseek")
        body["thinking"] = {{"type", "disabled"}};
    else if (provider == "siliconflow")
        body["enable_thinking"] = false;
    const std::string payload = body.dump();
    const HttpResponse response = PerformJsonRequest(endpoint, token, &payload);
    if (response.code == CURLE_OK && response.status >= 200 && response.status < 300)
        return {true, "连接成功，API Key 和模型配置有效。"};
    return {false, "测试失败：" + ErrorDetail(response)};
}

ApiCredentialTest::Result TestCodex(const ApiCredentialTest::Request &request)
{
    CodexCli::Request probe;
    const std::string executable = Value(request, "codex_executable");
    probe.executable = executable.empty() ? "codex" : executable;
    probe.model = Value(request, "model");
    probe.timeout = std::chrono::milliseconds(kRequestTimeoutMs);
    probe.proxy = NetworkProxy::ProxyForChildProcess();
    probe.prompt =
        R"({"instruction":"This is a synthetic IME connection test. Return exactly one Chinese candidate with text 测试, type chinese and confidence 1.0. Do not use tools.","pinyin":["ce","shi"],"context":"","limit":1})";
    const CodexCli::Result result = CodexCli::Run(probe);
    if (!result.ok)
        return {false, "Codex CLI 测试失败：" + result.error};
    try
    {
        const auto root = nlohmann::json::parse(result.output);
        const auto &candidates = root.at("candidates");
        if (!candidates.is_array() || candidates.empty())
            return {false, "Codex CLI 已响应，但未返回有效候选。"};
        const auto &candidate = candidates.front();
        if (candidate.at("text").get<std::string>() != "测试" || candidate.at("type").get<std::string>() != "chinese" ||
            !candidate.at("confidence").is_number())
            return {false, "Codex CLI 已响应，但测试候选格式无效。"};
        return {true, "连接成功，Codex CLI 登录和模型配置有效。"};
    }
    catch (...)
    {
        return {false, "Codex CLI 已响应，但未返回有效的候选 JSON。"};
    }
}

void AppendLe16(std::vector<unsigned char> &out, std::uint16_t value)
{
    out.push_back(static_cast<unsigned char>(value));
    out.push_back(static_cast<unsigned char>(value >> 8));
}

void AppendLe32(std::vector<unsigned char> &out, std::uint32_t value)
{
    AppendLe16(out, static_cast<std::uint16_t>(value));
    AppendLe16(out, static_cast<std::uint16_t>(value >> 16));
}

std::vector<unsigned char> SilentWav()
{
    constexpr std::uint32_t sample_rate = 16000;
    constexpr std::uint32_t data_size = sample_rate * 2;
    std::vector<unsigned char> wav;
    wav.reserve(44 + data_size);
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    AppendLe32(wav, 36 + data_size);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    AppendLe32(wav, 16);
    AppendLe16(wav, 1);
    AppendLe16(wav, 1);
    AppendLe32(wav, sample_rate);
    AppendLe32(wav, sample_rate * 2);
    AppendLe16(wav, 2);
    AppendLe16(wav, 16);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    AppendLe32(wav, data_size);
    wav.resize(44 + data_size, 0);
    return wav;
}

ApiCredentialTest::Result TestBatchAsr(const ApiCredentialTest::Request &request)
{
    const std::string token = Value(request, "token");
    const std::string endpoint = Value(request, "endpoint");
    const std::string model = Value(request, "model");
    if (!CloudTranslation::IsUsableSecret(token))
        return {false, "请先填写有效的 API Key。"};
    if (!IsHttpEndpoint(endpoint) || model.empty())
        return {false, "请填写有效的 HTTPS 接口地址和模型名。"};

    InitCurl();
    HttpResponse response;
    CURL *curl = curl_easy_init();
    if (!curl)
        return {false, "无法初始化网络请求。"};
    char error[CURL_ERROR_SIZE] = {};
    const auto wav = SilentWav();
    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *file = curl_mime_addpart(mime);
    curl_mime_name(file, "file");
    curl_mime_filename(file, "credential-test.wav");
    curl_mime_type(file, "audio/wav");
    curl_mime_data(file, reinterpret_cast<const char *>(wav.data()), wav.size());
    curl_mimepart *model_part = curl_mime_addpart(mime);
    curl_mime_name(model_part, "model");
    curl_mime_data(model_part, model.c_str(), CURL_ZERO_TERMINATED);
    curl_slist *headers = nullptr;
    const std::string authorization = "Authorization: Bearer " + token;
    headers = curl_slist_append(headers, authorization.c_str());
    headers = curl_slist_append(headers, "Expect:");
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    NetworkProxy::ApplyToCurl(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kRequestTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    response.code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    response.error = error;
    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    if (response.code == CURLE_OK && response.status >= 200 && response.status < 300)
        return {true, "连接成功，API Key 和模型配置有效。"};
    return {false, "测试失败：" + ErrorDetail(response)};
}

ApiCredentialTest::Result TestTranslation(const ApiCredentialTest::Request &request)
{
    std::vector<std::string> translated;
    if (request.service == "translation.tencent")
    {
        const TencentTmt::Credentials credentials{Value(request, "secretId"), Value(request, "secretKey"),
                                                  "ap-guangzhou"};
        if (!CloudTranslation::IsUsableSecret(credentials.secret_id) ||
            !CloudTranslation::IsUsableSecret(credentials.secret_key))
            return {false, "请先填写有效的 SecretId 和 SecretKey。"};
        translated = TencentTmt::TextTranslateBatch(credentials, {"测试"}, "zh", "en");
    }
    else if (request.service == "translation.niutrans")
    {
        const NiuTransTranslation::Config config{Value(request, "appId"), Value(request, "apiKey")};
        if (!NiuTransTranslation::IsUsableConfig(config))
            return {false, "请先填写有效的 APP ID 和 API Key。"};
        translated = NiuTransTranslation::TextTranslateBatch(config, {"测试"}, "zh", "en");
    }
    else
    {
        const CustomTranslation::Config config{Value(request, "endpoint"), Value(request, "apiKey")};
        if (!CustomTranslation::IsSupportedEndpoint(config.endpoint))
            return {false, "请先填写有效的翻译接口地址。"};
        translated = CustomTranslation::TextTranslateBatch(config, {"测试"}, "ZH", "EN");
    }
    if (!translated.empty() && !translated.front().empty())
        return {true, "连接成功，翻译服务配置有效。"};
    return {false, "测试失败：服务未返回有效译文，请检查凭据、接口地址和网络。"};
}
} // namespace

namespace ApiCredentialTest
{
Result Run(const Request &request)
{
    if (request.service.rfind("translation.", 0) == 0)
        return TestTranslation(request);
    if (request.service == "ai.assistant" && Value(request, "provider") == "codex")
        return TestCodex(request);
    if (request.service == "voice.polish" || request.service == "ai.assistant")
        return TestChat(request);
    if (request.service == "voice.asr")
    {
        if (Value(request, "provider") != "doubao")
            return TestBatchAsr(request);
        const bool legacy = Value(request, "authMode") == "legacy";
        const std::string app_id = Value(request, "appId");
        const std::string token = Value(request, "token");
        if (!CloudTranslation::IsUsableSecret(token) || (legacy && !CloudTranslation::IsUsableSecret(app_id)))
            return {false, legacy ? "请先填写有效的 App ID 和 Access Token。" : "请先填写有效的 API Key。"};
        const std::string error = DoubaoAsrClient::TestCredentials(Value(request, "endpoint"), legacy, app_id, token,
                                                                   Value(request, "resourceId"));
        return error.empty() ? Result{true, "连接成功，豆包语音识别凭据有效。"} : Result{false, "测试失败：" + error};
    }
    return {false, "不支持的配置测试类型。"};
}

ModelListResult FetchModels(const Request &request)
{
    if (request.service != "ai.assistant")
        return {false, "不支持的模型列表类型。", {}};
    if (Value(request, "provider") == "codex")
        return {false, "Codex CLI 模型可留空使用 CLI 内置默认模型，或手动填写模型名。", {}};
    const std::string token = Value(request, "token");
    const std::string endpoint = Value(request, "endpoint");
    if (!CloudTranslation::IsUsableSecret(token))
        return {false, "请先填写有效的 API Key。", {}};
    if (!IsHttpEndpoint(endpoint))
        return {false, "请先填写以 http:// 或 https:// 开头的接口地址。", {}};

    const HttpResponse response = PerformJsonRequest(ModelListEndpoint(endpoint), token, nullptr, kMaxModelListBytes);
    if (response.code != CURLE_OK || response.status < 200 || response.status >= 300)
    {
        return {false, "获取模型列表失败：" + ErrorDetail(response), {}};
    }

    try
    {
        const auto root = nlohmann::json::parse(response.body);
        std::vector<std::string> model_ids;
        if (root.contains("data") && root.at("data").is_array())
        {
            for (const auto &item : root.at("data"))
            {
                if (item.is_object() && item.contains("id") && item.at("id").is_string())
                {
                    model_ids.push_back(item.at("id").get<std::string>());
                }
            }
        }
        else if (root.is_array())
        {
            for (const auto &item : root)
            {
                if (item.is_object() && item.contains("id") && item.at("id").is_string())
                {
                    model_ids.push_back(item.at("id").get<std::string>());
                }
                else if (item.is_string())
                {
                    model_ids.push_back(item.get<std::string>());
                }
            }
        }

        if (model_ids.empty())
        {
            return {false, "获取成功但未解析到可用模型。", {}};
        }

        std::sort(model_ids.begin(), model_ids.end());
        model_ids.erase(std::unique(model_ids.begin(), model_ids.end()), model_ids.end());
        return {true, "获取成功", std::move(model_ids)};
    }
    catch (...)
    {
        return {false, "解析服务返回的模型列表失败。", {}};
    }
}
} // namespace ApiCredentialTest
