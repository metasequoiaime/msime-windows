#include "translate/translate_service.h"

#include "config/ime_config.h"
#include "translate/translate_cache.h"
#include "translate/translate_config.h"
#include "translate/translate_pipeline.h"
#include "translate/translate_review.h"
#include <curl/curl.h>
#include <thread>

namespace Translate
{
namespace
{
std::thread g_worker;
}

void Start(const std::string &db_path, Internal::ApplyCallback apply_callback)
{
    auto &running = Internal::Running();
    if (running.load())
        return;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    Internal::DbPath() = db_path;
    Internal::Callback() = std::move(apply_callback);
    running.store(true);
    g_worker = std::thread(WorkerLoop);
    StartReviewWorker();
}

void Stop()
{
    auto &running = Internal::Running();
    if (!running.load())
        return;
    running.store(false);
    Internal::Cv().notify_all();
    StopReviewWorker();
    if (g_worker.joinable())
        g_worker.join();
    Internal::Callback() = {};
}

void RequestMisses(std::vector<Query> queries, uint64_t generation)
{
    {
        std::lock_guard lock(Internal::Mutex());
        Internal::LatestQueries() = std::move(queries);
        Internal::LatestGeneration() = generation;
        Internal::LastRequest() = std::chrono::steady_clock::now();
        Internal::Job().fetch_add(1);
    }
    Internal::Cv().notify_one();
}

void Clear()
{
    RequestMisses({}, 0);
}

std::string ActiveVendorScope()
{
    return VendorScope(*GetConfiguredTranslation());
}

std::string LookupCache(const std::string &key, Direction direction)
{
    return Lookup(key, direction);
}
} // namespace Translate
