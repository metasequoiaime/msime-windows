#include "chat_completions.h"

#include "config/ai_provider.h"
#include "utils/network_proxy.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <algorithm>
#include <exception>

namespace
{
constexpr size_t kMaxResponseBytes = 1024 * 1024;
// 服务端返回体可能很长（整段堆栈、HTML 错误页）。展示给用户的 detail 只留开头，
// 足够看清错误类型即可，不把几 KB 的响应体塞进设置页。
constexpr size_t kMaxDetailBytes = 200;

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

std::string Narrow(const std::wstring &wide)
{
    if (wide.empty())
        return {};
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(bytes > 0 ? static_cast<size_t>(bytes) : 0, '\0');
    if (bytes > 0)
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), narrow.data(), bytes, nullptr,
                            nullptr);
    return narrow;
}

std::string Truncate(std::string text)
{
    if (text.size() > kMaxDetailBytes)
    {
        // 截断点落在多字节字符中间时退回到完整的字符边界，避免设置页上出现断字。
        text.resize(kMaxDetailBytes);
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80)
            text.pop_back();
        text += "…";
    }
    for (char &ch : text)
    {
        if (ch == '\r' || ch == '\n' || ch == '\t')
            ch = ' ';
    }
    return text;
}

// 服务端的错误体常见三种形状：OpenAI 的 {"error":{"message"}}、Ollama/LM Studio 的
// {"error":"msg"}，以及纯文本 HTML。只取 message，取不到就截取原文，让用户至少看到状态码之外的东西。
std::string ErrorMessageFromBody(const std::string &body)
{
    if (body.empty())
        return {};
    try
    {
        const auto root = nlohmann::json::parse(body);
        if (root.is_object())
        {
            if (root.contains("error"))
            {
                const auto &error = root.at("error");
                if (error.is_object() && error.contains("message") && error.at("message").is_string())
                    return Truncate(error.at("message").get<std::string>());
                if (error.is_string())
                    return Truncate(error.get<std::string>());
            }
            for (const char *key : {"message", "errorMsg", "detail"})
            {
                if (root.contains(key) && root.at(key).is_string())
                    return Truncate(root.at(key).get<std::string>());
            }
        }
    }
    catch (const std::exception &)
    {
    }
    return Truncate(body);
}

// Windows 上的服务端通常返回 UTF-8，但报错页也可能是 GBK。只有整体不是合法 UTF-8 时才按
// 系统 ANSI 代码页再解一次，免得中文错误信息在设置页上变成乱码。
std::string NormalizeBodyEncoding(const std::string &body)
{
    if (body.empty() ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body.data(), static_cast<int>(body.size()), nullptr, 0) > 0)
    {
        return body;
    }
    const int bytes = MultiByteToWideChar(CP_ACP, 0, body.data(), static_cast<int>(body.size()), nullptr, 0);
    if (bytes <= 0)
        return body;
    std::wstring wide(static_cast<size_t>(bytes), L'\0');
    MultiByteToWideChar(CP_ACP, 0, body.data(), static_cast<int>(body.size()), wide.data(), bytes);
    const std::string converted = Narrow(wide);
    return converted.empty() ? body : converted;
}

ChatCompletions::Failure ClassifyCurlError(CURLcode code)
{
    // 超时单独一类：本地模型答得慢是最常见的失败，提示应该直接告诉用户要等更久或换更小的模型。
    if (code == CURLE_OPERATION_TIMEDOUT)
        return ChatCompletions::Failure::Timeout;
    // 其余失败都是「没拿到响应」：连接被拒、DNS、代理、TLS 验证，以及连接建立后对端直接断开。
    return ChatCompletions::Failure::Network;
}

std::string CurlErrorText(CURLcode code, const char *error_buffer)
{
    if (error_buffer != nullptr && error_buffer[0] != '\0')
        return Truncate(error_buffer);
    return curl_easy_strerror(code);
}
} // namespace

namespace ChatCompletions
{
Result FetchContent(const std::string &endpoint, const std::string &token, const std::string &payload, long timeout_ms)
{
    Result result;
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        result.failure = Failure::Request;
        result.detail = "无法初始化网络请求";
        return result;
    }

    // Deliberately exclude the API token and the request body from logs.
    ResponseBuffer response;
    char error_buffer[CURL_ERROR_SIZE] = {};
    const std::string authorization = AiAssistant::AuthorizationHeader(token);
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
    if (!authorization.empty())
        headers = curl_slist_append(headers, authorization.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    NetworkProxy::ApplyToCurl(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode performed = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    // 超时和过大优先于 curl 的返回码判断：中止读取时 curl 报的是写回调失败，
    // 真正的原因是我们主动收的手，不能当成网络故障展示给用户。
    if (performed == CURLE_OPERATION_TIMEDOUT)
    {
        result.failure = Failure::Timeout;
        result.detail = "超过 " + std::to_string(timeout_ms / 1000) + " 秒仍未返回";
        return result;
    }
    if (response.overflow)
    {
        result.failure = Failure::TooLarge;
        result.detail = "响应超过 1 MiB，已中止读取";
        return result;
    }
    if (performed != CURLE_OK)
    {
        result.failure = ClassifyCurlError(performed);
        result.detail = CurlErrorText(performed, error_buffer);
        return result;
    }
    if (status < 200 || status >= 300)
    {
        result.failure = Failure::Status;
        result.detail = "HTTP " + std::to_string(status);
        const std::string message = ErrorMessageFromBody(NormalizeBodyEncoding(response.text));
        if (!message.empty())
            result.detail += "：" + message;
        return result;
    }

    nlohmann::json outer;
    try
    {
        outer = nlohmann::json::parse(response.text);
    }
    catch (const std::exception &)
    {
        result.failure = Failure::InvalidJson;
        result.detail = Truncate(response.text);
        if (result.detail.empty())
            result.detail = "响应为空";
        return result;
    }

    // OpenAI 兼容服务在请求出错时也可能回 200，错误写在 body 的 error 里。
    if (!outer.is_object() || !outer.contains("choices"))
    {
        result.failure = Failure::NoContent;
        result.detail = ErrorMessageFromBody(response.text);
        if (result.detail.empty())
            result.detail = "响应里没有 choices";
        return result;
    }

    try
    {
        result.content = outer.at("choices").at(0).at("message").at("content").get<std::string>();
    }
    catch (const std::exception &)
    {
        result.failure = Failure::NoContent;
        result.detail = "响应里没有 choices[0].message.content";
        return result;
    }

    if (result.content->empty())
    {
        result.failure = Failure::EmptyContent;
        result.detail = "模型返回了空内容";
        return result;
    }
    return result;
}

std::string DescribeFailure(Failure failure, const std::string &detail)
{
    std::string text;
    switch (failure)
    {
    case Failure::None:
        return {};
    case Failure::Request:
        text = "请求构造失败";
        break;
    case Failure::Network:
        text = "无法连接到接口";
        break;
    case Failure::Status:
        text = "服务端拒绝了请求";
        break;
    case Failure::Timeout:
        text = "请求超时";
        break;
    case Failure::TooLarge:
        text = "响应过大";
        break;
    case Failure::InvalidJson:
        text = "响应不是 JSON";
        break;
    case Failure::NoContent:
        text = "响应里没有可用的回复";
        break;
    case Failure::EmptyContent:
        text = "回复为空";
        break;
    }
    return detail.empty() ? text : text + "（" + detail + "）";
}
} // namespace ChatCompletions
