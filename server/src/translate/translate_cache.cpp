#include "translate/translate_cache.h"

#include "cloud/translation_gloss.h"
#include "config/ime_config.h"
#include "translate/translate_config.h"
#include "translate/translate_state.h"

namespace Translate
{
namespace
{
constexpr size_t kMaxCacheEntries = 4096;
}

std::string Identity(const Query &query, const std::string &target_language, const std::string &vendor_scope)
{
    return vendor_scope + ":" + target_language + ":" + (query.direction == Direction::EnglishToChinese ? "e:" : "z:") +
           query.key;
}

void RememberPositive(const std::string &identity, std::string gloss)
{
    auto &cache = Internal::Cache();
    std::lock_guard lock(Internal::Mutex());
    if (cache.size() >= kMaxCacheEntries)
        cache.clear();
    cache[identity] = std::move(gloss);
    Internal::Failures().erase(identity);
}

bool IsBackingOff(const std::string &identity)
{
    auto &failures = Internal::Failures();
    const auto found = failures.find(identity);
    return found != failures.end() && std::chrono::steady_clock::now() < found->second.retry_at;
}

void RememberFailure(const std::string &identity)
{
    auto &failures = Internal::Failures();
    std::lock_guard lock(Internal::Mutex());
    if (failures.size() >= kMaxCacheEntries && failures.find(identity) == failures.end())
        failures.clear();
    auto &entry = failures[identity];
    entry.retry_at = std::chrono::steady_clock::now() + CloudTranslation::FailureRetryDelay(++entry.failures);
}

std::string Lookup(const std::string &key, Direction direction)
{
    const auto config = GetConfiguredTranslation();
    const std::string identity = Identity({key, direction}, TargetLanguage(*config), VendorScope(*config));
    auto &cache = Internal::Cache();
    std::lock_guard lock(Internal::Mutex());
    const auto found = cache.find(identity);
    return found == cache.end() ? std::string{} : found->second;
}
} // namespace Translate
