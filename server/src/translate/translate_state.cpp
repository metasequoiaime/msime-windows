#include "translate/translate_state.h"

namespace Translate::Internal
{
namespace
{
std::mutex g_mutex;
std::condition_variable g_cv;
std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_job{0};
std::vector<Query> g_latest_queries;
uint64_t g_latest_generation = 0;
std::chrono::steady_clock::time_point g_last_request;
std::string g_db_path;
std::unordered_map<std::string, std::string> g_cache;
std::unordered_map<std::string, FailureBackoff> g_failures;
ApplyCallback g_callback;
} // namespace

std::mutex &Mutex()
{
    return g_mutex;
}

std::condition_variable &Cv()
{
    return g_cv;
}

std::atomic<bool> &Running()
{
    return g_running;
}

std::atomic<uint64_t> &Job()
{
    return g_job;
}

std::vector<Query> &LatestQueries()
{
    return g_latest_queries;
}

uint64_t &LatestGeneration()
{
    return g_latest_generation;
}

std::chrono::steady_clock::time_point &LastRequest()
{
    return g_last_request;
}

std::string &DbPath()
{
    return g_db_path;
}

std::unordered_map<std::string, std::string> &Cache()
{
    return g_cache;
}

std::unordered_map<std::string, FailureBackoff> &Failures()
{
    return g_failures;
}

ApplyCallback &Callback()
{
    return g_callback;
}
} // namespace Translate::Internal
