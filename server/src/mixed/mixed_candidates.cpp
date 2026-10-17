#include "mixed/mixed_candidates.h"

#include "mixed/date_time_candidates.h"
#include "engine/core/data_path.h"
#include "engine/english/english_dictionary.h"
#include "engine/local_modes/date_time_query.h"
#include "engine/local_modes/emoji_query.h"
#include "engine/local_modes/kaomoji_query.h"
#include "engine/shuangpin/shuangpin_profile.h"
#include "config/ime_config.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace
{
constexpr std::size_t kEnglishCandidateLimit = 5;
// emoji 和颜文字从第二个字母起才查，单独一个首字母匹配面太大，会把候选刷满。
constexpr std::size_t kExpressiveMinimumPrefixLength = 2;
constexpr int kExpressiveCandidateLimit = 3;
constexpr std::size_t kDateTimeCandidateLimit = 3;

std::mutex g_mutex;
std::condition_variable g_cv;
std::thread g_worker;
std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_generation{0};
MixedCandidates::Request g_latest_request;
std::string g_english_db_path;
std::string g_others_db_path;
MixedCandidates::ApplyCallback g_apply_callback;

// 英文词库只收小写字母；带大写的输入转小写去查，夹着别的字符就不查。
std::string EnglishPrefix(const std::string &input)
{
    std::string prefix;
    prefix.reserve(input.size());
    for (const unsigned char ch : input)
    {
        if (ch >= 'a' && ch <= 'z')
            prefix.push_back(static_cast<char>(ch));
        else if (ch >= 'A' && ch <= 'Z')
            prefix.push_back(static_cast<char>(ch + ('a' - 'A')));
        else
            return {};
    }
    return prefix;
}

bool IsStale(uint64_t generation)
{
    return !g_running || g_generation.load() != generation;
}

void WorkerLoop()
{
    // 英文库的建表由 EnglishIme 负责，这里只读。
    EnglishDictionary english_dictionary(g_english_db_path, false);
    const auto others_db_path = metasequoia::path_from_utf8(g_others_db_path.c_str());
    uint64_t observed_generation = g_generation.load();

    while (g_running)
    {
        std::unique_lock lock(g_mutex);
        g_cv.wait(lock, [&] { return !g_running || g_generation.load() != observed_generation; });
        if (!g_running)
            break;

        observed_generation = g_generation.load();
        const MixedCandidates::Request request = g_latest_request;
        lock.unlock();

        if (request.input.empty())
            continue;

        // 每一段查询之后都看一眼有没有更新的输入：快打时旧输入剩下的查询没必要做完。
        MixedCandidates::Result result;
        if (!request.date_time_keyword.empty())
        {
            // 和 Shift+T 模式同一份顺序，混进来的就是那里排第一的格式。固定位置说的是日期页里的位置，
            // 在普通候选里不作数，所以清掉，免得混输里的日期也挂上固定徽标。
            result.date_time = OrderedDateTimeCandidates(request.date_time_keyword);
            if (result.date_time.size() > kDateTimeCandidateLimit)
                result.date_time.resize(kDateTimeCandidateLimit);
            for (auto &item : result.date_time)
                item.fixed_position = 0;
            if (!result.date_time.empty())
                result.date_time.push_back(metasequoia::local_modes::date_time_menu_item(request.date_time_keyword));
            if (IsStale(observed_generation))
                continue;
        }
        if (request.english)
        {
            const std::string prefix = EnglishPrefix(request.input);
            if (prefix.size() >= (std::max)(std::size_t{1}, request.english_min_prefix))
                result.english = request.english_first_page_count > 1
                                     ? english_dictionary.query_candidates(
                                           request.input, EnglishDictionary::kMixedCandidateLimit, true)
                                     : english_dictionary.query_candidates(request.input, kEnglishCandidateLimit);
            if (IsStale(observed_generation))
                continue;
        }
        const bool expressive_prefix = request.input.size() >= kExpressiveMinimumPrefixLength;
        if (request.emoji && expressive_prefix)
        {
            result.emoji = metasequoia::local_modes::query_emoji(request.input, request.scheme, others_db_path,
                                                                 kExpressiveCandidateLimit,
                                                                 GetShuangpinProfile(GetConfiguredShuangpinSchema()))
                               .candidates;
            if (IsStale(observed_generation))
                continue;
        }
        if (request.kaomoji && expressive_prefix)
        {
            result.kaomoji = metasequoia::local_modes::query_kaomoji(
                                 request.input, request.scheme, others_db_path, kExpressiveCandidateLimit,
                                 GetShuangpinProfile(GetConfiguredShuangpinSchema()))
                                 .candidates;
        }

        // 什么都没查到就不回调：任务线程组页时已经是不带混输候选的列表，没有可以合并的。
        if (result.empty() || IsStale(observed_generation) || !g_apply_callback)
            continue;
        g_apply_callback(std::move(result), request.input, observed_generation);
    }
}
} // namespace

namespace MixedCandidates
{
void Start(const std::string &english_db_path, const std::string &others_db_path, ApplyCallback apply_callback)
{
    if (g_running)
        return;
    g_english_db_path = english_db_path;
    g_others_db_path = others_db_path;
    g_apply_callback = std::move(apply_callback);
    g_running = true;
    g_worker = std::thread(WorkerLoop);
}

void Stop()
{
    if (!g_running)
        return;
    g_running = false;
    g_cv.notify_all();
    if (g_worker.joinable())
        g_worker.join();
    g_apply_callback = {};
}

void OnInputChanged(Request request)
{
    {
        std::lock_guard lock(g_mutex);
        g_latest_request = std::move(request);
        g_generation.fetch_add(1);
    }
    g_cv.notify_one();
}

void Clear()
{
    OnInputChanged({});
}

bool IsCurrent(const std::string &input, uint64_t generation)
{
    std::lock_guard lock(g_mutex);
    return g_running && g_generation.load() == generation && g_latest_request.input == input;
}
} // namespace MixedCandidates
