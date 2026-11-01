#pragma once

#include "translate/translate_config.h"
#include "translate/translate_review.h"
#include "translate/translate_types.h"
#include <string>
#include <vector>

namespace Translate
{
// 把一批译文写进缓存、落库并产出候选结果，同时收集待校对条目。
void ApplyTranslatedGroup(const RuntimeConfig &config, const std::vector<Query> &group, const Outcome &outcome,
                          const std::string &target_language, const std::string &vendor_scope,
                          std::vector<Result> &results, bool sentence, bool review_enabled,
                          std::vector<PendingReview> &pending);

// 发一批、收回、落库。batch 的 source/target 由调用方决定。
void TranslateGroup(const RuntimeConfig &config, const std::vector<Query> &group, const std::string &source,
                    const std::string &target, const std::string &target_language, const std::string &vendor_scope,
                    std::vector<Result> &results, std::vector<PendingReview> &pending, bool sentence = false);

// 后台工作线程：等输入停顿 → 分组 → 分批译 → 分批上屏 → 交校对。
void WorkerLoop();
} // namespace Translate
