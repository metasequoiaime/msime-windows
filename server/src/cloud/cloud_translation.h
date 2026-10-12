#pragma once

#include "english/english_ime.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace CloudTranslation
{
using ApplyCallback = std::function<void(std::vector<EnglishIme::TranslationResult> results, uint64_t generation)>;

void Start(const std::string &db_path, ApplyCallback apply_callback);
void Stop();
void RequestMisses(std::vector<EnglishIme::TranslationQuery> queries, uint64_t generation);
void Clear();
std::string LookupCache(const std::string &key, EnglishIme::TranslationDirection direction);
// 当前生效提供方的稳定标识。缓存键与候选页译文失效判断共用，避免两处各写一份优先级。
std::string ActiveProviderScope();
} // namespace CloudTranslation
