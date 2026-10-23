#pragma once

#include "translate/translate_config.h"
#include "translate/translate_types.h"
#include <vector>

namespace Translate
{
// 一轮里已经译出来、等着校对的条目。source 决定校对提示词的方向（en→zh / zh→目标）。
struct PendingReview
{
    Query query;
    std::string gloss;
    std::string source;
    bool sentence = false;
};

// 校对线程与翻译线程分开：用户连着打字时，下一句不该排队等校对跑完。
void StartReviewWorker();
void StopReviewWorker();
void SubmitReview(const RuntimeConfig &config, std::vector<PendingReview> items, const std::string &target_language,
                  const std::string &vendor_scope);
} // namespace Translate
