#pragma once

#include <string>
#include <utility>
#include <vector>

namespace AliyunMt
{
struct Credentials
{
    std::string access_key_id;
    std::string access_key_secret;
};

// Both credentials must be present and non-placeholder before requests are sent.
bool IsUsableCredentials(const Credentials &credentials);

// RFC 3986 percent-encoding: only A-Z a-z 0-9 - _ . ~ stay literal.
std::string PercentEncode(const std::string &value);

// Base64(HMAC-SHA1(secret + "&", "POST&%2F&" + sorted encoded parameters)). Exposed for tests.
std::string BuildSignature(const Credentials &credentials, const std::string &http_method,
                           const std::vector<std::pair<std::string, std::string>> &parameters);

// Maps each entry's "index" back to its position in the batch. Exposed for tests.
std::vector<std::string> ParseBatchResponse(const std::string &response, size_t count);

// SourceText JSON keyed by index within the batch, not within texts. Exposed for tests.
std::string BuildBatchSourceText(const std::vector<std::string> &texts, size_t begin, size_t end);

// Calls GetBatchTranslate once per batch of at most 50 texts. Empty entries mean that item
// failed. When the batch time budget runs out the result is shorter than texts.
std::vector<std::string> TextTranslateBatch(const Credentials &credentials, const std::vector<std::string> &texts,
                                            const std::string &source, const std::string &target);

// Same as above with an explicit endpoint. Production code never calls this; tests use it to point
// at an address that refuses connections and check that the first failing batch stops the rest.
std::vector<std::string> TextTranslateBatch(const Credentials &credentials, const std::vector<std::string> &texts,
                                            const std::string &source, const std::string &target,
                                            const std::string &endpoint);
} // namespace AliyunMt
