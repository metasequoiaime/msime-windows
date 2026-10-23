#include "aliyun_mt.h"

#include "translation_gloss.h"
#include <Windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include "utils/network_proxy.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <string_view>
#include <utility>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace
{
constexpr const char *kDefaultUrl = "https://mt.aliyuncs.com";
constexpr const char *kAction = "GetBatchTranslate";
constexpr const char *kVersion = "2018-10-12";
constexpr const char *kSignatureMethod = "HMAC-SHA1";
constexpr const char *kSignatureVersion = "1.0";
// 通用版固定 general + translate_standard；专业版是 translate_ecommerce。
constexpr const char *kScene = "general";
constexpr const char *kApiType = "translate_standard";
constexpr long kTimeoutMs = 2500;
constexpr size_t kMaxResponseBytes = 1024 * 1024;
constexpr auto kBatchBudget = std::chrono::seconds(6);
// 单次最多 50 条，按条数切分；候选词远不会触及 8000 字符上限。
constexpr size_t kMaxBatchItems = 50;

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

std::string HexEncodeLower(const unsigned char *data, size_t size)
{
    static const char *kHex = "0123456789abcdef";
    std::string out;
    out.resize(size * 2);
    for (size_t i = 0; i < size; ++i)
    {
        out[i * 2] = kHex[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[data[i] & 0x0F];
    }
    return out;
}

bool HmacSha1(std::string_view key, std::string_view data, unsigned char out[20])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
        return false;
    const NTSTATUS created =
        BCryptCreateHash(alg, &hash, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(key.data())),
                         static_cast<ULONG>(key.size()), 0);
    const NTSTATUS hashed = created == 0
                                ? BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char *>(data.data())),
                                                 static_cast<ULONG>(data.size()), 0)
                                : created;
    const NTSTATUS finished = hashed == 0 ? BCryptFinishHash(hash, out, 20, 0) : hashed;
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    return finished == 0;
}

std::string Base64Encode(const unsigned char *data, size_t size)
{
    DWORD length = 0;
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(size), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr,
                              &length))
        return {};
    std::string encoded(length, '\0');
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(size), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded.data(),
                              &length))
        return {};
    encoded.resize(length);
    return encoded;
}

std::string RandomNonce()
{
    unsigned char bytes[16]{};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return {};
    return HexEncodeLower(bytes, sizeof(bytes));
}

// ISO 8601 UTC, e.g. 2024-01-01T00:00:00Z.
std::string UtcTimestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &now);
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buf;
}

// POST 表单体，成功时把响应体写进 response。
bool PostForm(CURL *curl, const std::string &url, const std::string &payload, std::string &response)
{
    ResponseBuffer buffer;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    NetworkProxy::ApplyToCurl(curl);
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded; charset=utf-8");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode performed = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    if (performed != CURLE_OK || buffer.overflow || status < 200 || status >= 300)
        return false;
    response = std::move(buffer.text);
    return true;
}
} // namespace

namespace AliyunMt
{
bool IsUsableCredentials(const Credentials &credentials)
{
    return CloudTranslation::IsUsableSecret(credentials.access_key_id) &&
           CloudTranslation::IsUsableSecret(credentials.access_key_secret);
}

std::string PercentEncode(const std::string &value)
{
    static const char *kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (const unsigned char ch : value)
    {
        const bool literal = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                             ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (literal)
        {
            out.push_back(static_cast<char>(ch));
            continue;
        }
        out.push_back('%');
        out.push_back(kHex[(ch >> 4) & 0x0F]);
        out.push_back(kHex[ch & 0x0F]);
    }
    return out;
}

std::string BuildSignature(const Credentials &credentials, const std::string &http_method,
                           const std::vector<std::pair<std::string, std::string>> &parameters)
{
    // std::map 已按参数名排序；Signature 不参与签名。
    std::map<std::string, std::string> sorted;
    for (const auto &[key, value] : parameters)
    {
        if (key == "Signature")
            continue;
        sorted[key] = value;
    }
    std::string canonical;
    for (const auto &[key, value] : sorted)
    {
        if (!canonical.empty())
            canonical.push_back('&');
        canonical += PercentEncode(key) + "=" + PercentEncode(value);
    }
    const std::string string_to_sign = http_method + "&" + PercentEncode("/") + "&" + PercentEncode(canonical);
    const std::string key = credentials.access_key_secret + "&";
    unsigned char digest[20]{};
    if (!HmacSha1(key, string_to_sign, digest))
        return {};
    return Base64Encode(digest, sizeof(digest));
}

std::vector<std::string> ParseBatchResponse(const std::string &response, size_t count)
{
    std::vector<std::string> results(count);
    try
    {
        const auto root = nlohmann::json::parse(response);
        // 整体错误也可能带 TranslatedList，先看顶层 Code，非 200 整批作废。
        if (const auto code = root.find("Code"); code != root.end())
        {
            const bool ok = (code->is_number_integer() && code->get<long long>() == 200) ||
                            (code->is_string() && code->get<std::string>() == "200");
            if (!ok)
                return results;
        }
        // TranslatedList 在顶层。
        const auto found = root.find("TranslatedList");
        if (found == root.end() || !found->is_array())
            return results;
        const nlohmann::json &list = *found;
        for (const auto &item : list)
        {
            if (!item.is_object())
                continue;
            const auto index = item.find("index");
            const auto translated = item.find("translated");
            if (index == item.end() || translated == item.end() || !translated->is_string())
                continue;
            size_t position = count;
            if (index->is_number_integer())
                position = static_cast<size_t>((std::max)(index->get<long long>(), 0LL));
            else if (index->is_string())
            {
                try
                {
                    position = static_cast<size_t>(std::stoull(index->get<std::string>()));
                }
                catch (...)
                {
                    continue;
                }
            }
            if (position >= count)
                continue;
            const auto code = item.find("code");
            if (code != item.end())
            {
                const bool ok = (code->is_number_integer() && code->get<long long>() == 200) ||
                                (code->is_string() && code->get<std::string>() == "200");
                if (!ok)
                    continue;
            }
            results[position] = translated->get<std::string>();
        }
    }
    catch (...)
    {
    }
    return results;
}

std::string BuildBatchSourceText(const std::vector<std::string> &texts, size_t begin, size_t end)
{
    nlohmann::json source_text = nlohmann::json::object();
    for (size_t i = begin; i < end && i < texts.size(); ++i)
        source_text[std::to_string(i - begin)] = texts[i];
    return source_text.dump();
}

std::vector<std::string> TextTranslateBatch(const Credentials &credentials, const std::vector<std::string> &texts,
                                            const std::string &source, const std::string &target)
{
    return TextTranslateBatch(credentials, texts, source, target, kDefaultUrl);
}

std::vector<std::string> TextTranslateBatch(const Credentials &credentials, const std::vector<std::string> &texts,
                                            const std::string &source, const std::string &target,
                                            const std::string &endpoint)
{
    std::vector<std::string> results(texts.size());
    if (texts.empty() || !IsUsableCredentials(credentials) || source.empty() || target.empty())
        return results;

    CURL *curl = curl_easy_init();
    if (!curl)
        return results;

    const auto started = std::chrono::steady_clock::now();
    for (size_t begin = 0; begin < texts.size(); begin += kMaxBatchItems)
    {
        if (std::chrono::steady_clock::now() - started >= kBatchBudget)
        {
            results.resize(begin);
            break;
        }
        const size_t end = (std::min)(begin + kMaxBatchItems, texts.size());
        const std::string source_text = BuildBatchSourceText(texts, begin, end);

        std::vector<std::pair<std::string, std::string>> parameters = {
            {"Action", kAction},
            {"Version", kVersion},
            {"Format", "JSON"},
            {"AccessKeyId", credentials.access_key_id},
            {"SignatureMethod", kSignatureMethod},
            {"SignatureVersion", kSignatureVersion},
            {"SignatureNonce", RandomNonce()},
            {"Timestamp", UtcTimestamp()},
            {"ApiType", kApiType},
            {"Scene", kScene},
            {"FormatType", "text"},
            {"SourceLanguage", source},
            {"TargetLanguage", target},
            {"SourceText", source_text},
        };
        const std::string signature = BuildSignature(credentials, "POST", parameters);
        if (signature.empty())
            break;
        parameters.emplace_back("Signature", signature);

        std::string payload;
        for (const auto &[key, value] : parameters)
        {
            if (!payload.empty())
                payload.push_back('&');
            payload += PercentEncode(key) + "=" + PercentEncode(value);
        }

        std::string response;
        // 一批发不出去，后续批次多半是同一个原因（网络断了、密钥被拒、配额用尽），接着发
        // 只会每个批次再赔上一次超时，把候选词刷新的延迟拉长。剩下的留空交给上层退避。
        if (!PostForm(curl, endpoint, payload, response))
        {
            results.resize(begin);
            break;
        }
        const auto translated = ParseBatchResponse(response, end - begin);
        for (size_t i = begin; i < end; ++i)
            results[i] = translated[i - begin];
    }

    curl_easy_cleanup(curl);
    return results;
}
} // namespace AliyunMt
