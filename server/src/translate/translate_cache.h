#pragma once

#include "translate/translate_types.h"
#include <string>

namespace Translate
{
// 缓存键：提供方 + 语种 + 方向 + 原文。任何一项变化都不该复用旧译文。
std::string Identity(const Query &query, const std::string &target_language, const std::string &vendor_scope);

void RememberPositive(const std::string &identity, std::string gloss);
bool IsBackingOff(const std::string &identity);
void RememberFailure(const std::string &identity);

// 按当前生效提供方查缓存，供「候选页重建时直接取上次结果」使用。
std::string Lookup(const std::string &key, Direction direction);
} // namespace Translate
