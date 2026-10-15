#include "statistics/stats_candidate_capture.h"

#include "statistics/stats_store.h"
#include "statistics/stats_types.h"

#include "config/ime_config.h"
#include "log/candidate_diag_log.h"
#include "utils/common_utils.h"
#include "utils/serial_task_runner.h"

#include <Windows.h>

#include <memory>
#include <mutex>

namespace MsimeStats
{
namespace
{
// Opened lazily on the first valid stats frame or the first Chinese candidate
// commit while recording is enabled. With the switch off no database file is
// ever created, which is the observable half of the privacy contract.
std::unique_ptr<Store> g_stats_store;
// The stats pipe thread and the selection path both reach this accessor; the
// open/close bookkeeping must not race even though every use of the handle is
// serialised by Store's own mutex.
std::mutex g_stats_store_open_mutex;

// Selection counters are written here, off the keystroke path.
SerialTaskRunner &SelectionWriter()
{
    static SerialTaskRunner writer(/*below_normal_priority=*/true);
    return writer;
}
} // namespace

Store *EnsureStatsStoreOpen()
{
    std::lock_guard<std::mutex> lock(g_stats_store_open_mutex);
    if (g_stats_store)
    {
        return g_stats_store.get();
    }
    const std::filesystem::path path = std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"stats.db";
    std::string error;
    g_stats_store = Store::Open(path, error);
    if (!g_stats_store)
    {
        // Deliberately no retry throttle: a transient failure (profile locked
        // during an upgrade, missing parent directory) should recover on the
        // next commit or frame.
        // DiagnosticLog is the only channel that reaches disk in production:
        // InitializeSpdLog() is a no-op here, and the diag switch gates both
        // the formatting and the write.
        DIAG_LOGF(L"stats: unable to open stats.db: {}", string_to_wstring(error));
    }
    return g_stats_store.get();
}

void CloseSharedStatsStore()
{
    // Drain the pending selection writes before the handle goes away; posts
    // that arrive after this are dropped by the stopped runner.
    SelectionWriter().Stop();
    std::lock_guard<std::mutex> lock(g_stats_store_open_mutex);
    g_stats_store.reset();
}

void RecordCandidateSelection(bool is_first, int day_key_override)
{
    // Called on the keystroke/IPC path, often right before the synchronous TSF
    // reply: the upsert can wait up to busy_timeout on a lock held by the stats
    // pipe or the settings process, so it runs on the writer thread, like the
    // dictionary learning writes. The day is taken now so a commit just before
    // midnight is not credited to the next day.
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const int day_key = day_key_override != 0 ? day_key_override : DayKeyOf(now.wYear, now.wMonth, now.wDay);
    SelectionWriter().Post([day_key, is_first] {
        // The switch can be flipped from the settings page at any time; the
        // observable contract is the same as the stats pipe: no store, no
        // counters. ReloadImeConfigIfChanged is a cheap timestamp check that
        // reloads config only after an external edit, matching the per-frame
        // behaviour of the stats pipe.
        ReloadImeConfigIfChanged();
        if (!GetConfiguredStatisticsEnabled())
        {
            return;
        }
        Store *store = EnsureStatsStoreOpen();
        if (store == nullptr)
        {
            return;
        }
        std::string error;
        if (!store->UpsertFirstWordSelection(day_key, is_first, error))
        {
            DIAG_LOGF(L"stats: failed to record the first-word hit: {}", string_to_wstring(error));
        }
    });
}
} // namespace MsimeStats
