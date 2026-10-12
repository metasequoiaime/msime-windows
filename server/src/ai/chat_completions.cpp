#include "chat_completions.h"

#include "utils/network_proxy.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <exception>

namespace
{
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
} // namespace

namespace ChatCompletions
{
std::optional<std::string> FetchContent(const std::string &endpoint, const std::string &token,
                                        const std::string &payload, long timeout_ms)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return std::nullopt;

    // Deliberately exclude the API token and the request body from logs.
    ResponseBuffer response;
    const std::string authorization = "Authorization: Bearer " + token;
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
    headers = curl_slist_append(headers, authorization.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    NetworkProxy::ApplyToCurl(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode performed = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (performed != CURLE_OK || response.overflow || status < 200 || status >= 300)
        return std::nullopt;

    try
    {
        const auto outer = nlohmann::json::parse(response.text);
        return outer.at("choices").at(0).at("message").at("content").get<std::string>();
    }
    catch (const std::exception &)
    {
        return std::nullopt;
    }
}
} // namespace ChatCompletions
