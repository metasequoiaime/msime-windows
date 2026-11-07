#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace CloudTranslation
{
// 同一词条连续失败 failures 次后，到下次允许重试前的等待：15 秒起步逐次翻倍，封顶 8 分钟。
std::chrono::seconds FailureRetryDelay(unsigned failures);
// Collapse whitespace for candidate-window gloss. Length is left to layout.
std::string FormatGloss(const std::string &text);
// max_chars 默认 32（词条）；整句译文走调用方传的大上限。
bool ShouldPersistGloss(const std::string &key, const std::string &formatted, size_t max_chars = 32);
size_t Utf8Length(const std::string &text);
bool IsUsableSecret(const std::string &value);
std::string TrimSecret(const std::string &value);
// TMT only: accept the candidate shapes produced by BuildTranslationQuery.
bool IsCloudTranslatableEnglish(const std::string &text);
bool IsCloudTranslatableChinese(const std::string &text);
} // namespace CloudTranslation
