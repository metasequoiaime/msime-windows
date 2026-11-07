// 兼容层：旧调用方仍然认 CloudTranslation 这个名字，真正的实现在 translate/。
// 这一层不持有状态、不做判断，只做转发。
#include "cloud_translation.h"

#include "translate/translate_service.h"

namespace CloudTranslation
{
void Start(const std::string &db_path, ApplyCallback apply_callback)
{
    Translate::Start(db_path, std::move(apply_callback));
}

void Stop()
{
    Translate::Stop();
}

void RequestMisses(std::vector<EnglishIme::TranslationQuery> queries, uint64_t generation)
{
    Translate::RequestMisses(std::move(queries), generation);
}

void Clear()
{
    Translate::Clear();
}

std::string LookupCache(const std::string &key, EnglishIme::TranslationDirection direction)
{
    return Translate::LookupCache(key, direction);
}

std::string ActiveProviderScope()
{
    return Translate::ActiveVendorScope();
}
} // namespace CloudTranslation
