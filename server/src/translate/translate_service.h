#pragma once

#include "translate/translate_state.h"
#include "translate/translate_types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Translate
{
// 翻译子系统的唯一对外入口。上层的 IPC / 引擎只认这几个函数，
// 不关心下面有几家提供方、几条线程。
void Start(const std::string &db_path, Internal::ApplyCallback apply_callback);
void Stop();
void RequestMisses(std::vector<Query> queries, uint64_t generation);
void Clear();
// 当前生效提供方的稳定标识。缓存键与候选页译文失效判断共用，避免两处各写一份优先级。
std::string ActiveVendorScope();
std::string LookupCache(const std::string &key, Direction direction);
} // namespace Translate
