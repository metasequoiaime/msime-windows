#pragma once

#include "translate/translate_types.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Translate::Internal
{
// 翻译与校对共用同一个回调：把结果交给候选层上屏。
using ApplyCallback = std::function<void(std::vector<Result> results, uint64_t generation)>;
// 提供方对超时、限流、凭据错误和「没有译文」都只回空串，分不清哪次值得马上重试，
// 所以失败项按次数退避：网络恢复后很快能补上，持续失败的词也不会每次刷新候选都重发。
struct FailureBackoff
{
    unsigned failures = 0;
    std::chrono::steady_clock::time_point retry_at;
};

// 翻译子系统的全局状态集中在这里，线程与模块只拿引用，不再各写一份 extern。
std::mutex &Mutex();
std::condition_variable &Cv();
std::atomic<bool> &Running();
std::atomic<uint64_t> &Job();
std::vector<Query> &LatestQueries();
uint64_t &LatestGeneration();
std::chrono::steady_clock::time_point &LastRequest();
std::string &DbPath();
std::unordered_map<std::string, std::string> &Cache();
std::unordered_map<std::string, FailureBackoff> &Failures();
ApplyCallback &Callback();
} // namespace Translate::Internal
