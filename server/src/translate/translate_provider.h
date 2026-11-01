#pragma once

#include "translate/translate_config.h"
#include "translate/translate_types.h"
#include <vector>

namespace Translate
{
// 一次批次交给当前提供方；返回的译文可能比请求短（预算到点）。
Outcome TranslateWithProvider(const RuntimeConfig &config, const Batch &batch);
} // namespace Translate
