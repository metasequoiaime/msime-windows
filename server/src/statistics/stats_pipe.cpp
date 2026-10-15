#include "statistics/stats_pipe.h"

#include "config/ime_config.h"
#include "engine/contracts/windows_ipc.h"
#include "ipc/event_listener.h"
#include "ipc/ipc.h"
#include "log/candidate_diag_log.h"
#include "statistics/stats_aggregate.h"
#include "statistics/stats_candidate_capture.h"
#include "statistics/stats_frames.h"
#include "statistics/stats_store.h"
#include "utils/common_utils.h"

#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <memory>
#include <vector>

namespace MsimeStats
{
namespace
{
// A client that connects and never writes must not hold the single listener
// instance forever. The DLL writes immediately after connecting, so a couple of
// seconds is generous; the TSF diagnostic listener uses the same bound.
constexpr auto kStatsPipeReadTimeout = std::chrono::seconds(2);

// Opened lazily via EnsureStatsStoreOpen (stats_candidate_capture.cpp) on the
// first valid frame received while recording is enabled. With the switch off
// no database file is ever created, which is the observable half of the
// privacy contract. The candidate capture path on the selection thread shares
// the same handle.
// The stream is global across host processes: typing continuously in two
// applications counts as continuous activity, matching the standalone tool.
Accumulator g_stats_accumulator;

bool WaitForStatsPipeClient(HANDLE pipe)
{
    const BOOL connected = ConnectNamedPipe(pipe, nullptr);
    return connected || GetLastError() == ERROR_PIPE_CONNECTED;
}

// The config reader already falls back to forever for a missing or invalid
// value; this repeats the fallback so a hand-edited file can never arm a
// cleanup with an unintended policy.
Retention ConfiguredRetention()
{
    Retention retention = Retention::Forever;
    if (!ParseRetention(GetConfiguredStatisticsRetention(), retention))
    {
        retention = Retention::Forever;
    }
    return retention;
}

// Runs at most once per local day, after a successful write. The once-per-day
// marker keeps the cost to one extra meta read per frame and zero deletes on
// every day but the first one; a failure only logs, leaving the marker stale
// so the next frame retries.
void RunRetentionAfterWrite(Store &store)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    RetentionOutcome outcome;
    std::string error;
    if (!RunDailyRetention(store, ConfiguredRetention(), DayKeyOf(now.wYear, now.wMonth, now.wDay), outcome, error))
    {
        DIAG_LOGF(L"stats: retention cleanup failed: {}", string_to_wstring(error));
    }
}

void ProcessStatsFrame(HANDLE pipe, const uint8_t *data, DWORD size)
{
    FanyImeStatsBatchHeader header{};
    std::vector<FanyImeStatsEvent> events;
    FrameError error = DecodeFrame(data, size, header, events);
    ULONG client_process_id = 0;
    if (error == FrameError::None && GetNamedPipeClientProcessId(pipe, &client_process_id))
    {
        error = ValidateFrameSource(header, client_process_id);
    }
    else if (error == FrameError::None)
    {
        // Without the client pid the claimed source cannot be verified, so the
        // strict path rejects the frame instead of trusting the header.
        error = FrameError::SourceProcessMismatch;
    }
    if (error != FrameError::None)
    {
        DIAG_LOGF(L"stats: dropped frame: {}", string_to_wstring(FrameErrorToString(error)));
        return;
    }

    // The switch can be flipped from the settings page at any time. Reloading
    // per frame matches the other pipe threads and bounds the delay to one
    // frame; a discarded frame never reaches the database.
    ReloadImeConfigIfChanged();
    if (!GetConfiguredStatisticsEnabled())
    {
        return;
    }

    if (header.dropped_count != 0)
    {
        // Loss happened inside the DLL and cannot be attributed to a calendar
        // day, so it is only reported. There is no text in this message.
        DIAG_LOGF(L"stats: frame from pid {} reports {} dropped events", header.source_process_id,
                  header.dropped_count);
    }

    if (events.empty())
    {
        return;
    }
    Store *store = EnsureStatsStoreOpen();
    if (store == nullptr)
    {
        return;
    }
    const Batch batch = g_stats_accumulator.Add(events);
    if (batch.Empty())
    {
        return;
    }
    std::string error_message;
    if (!store->Upsert(batch, error_message))
    {
        DIAG_LOGF(L"stats: failed to persist a batch: {}", string_to_wstring(error_message));
        return;
    }
    RunRetentionAfterWrite(*store);
}
} // namespace

uint32_t CurrentStatsSessionId()
{
    DWORD session_id = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id))
    {
        return 0;
    }
    return static_cast<uint32_t>(session_id);
}

void StatsPipeEventListenerLoopThread()
{
    HANDLE listening_pipe = INVALID_HANDLE_VALUE;
    while (pipe_running)
    {
        if (!listening_pipe || listening_pipe == INVALID_HANDLE_VALUE)
        {
            listening_pipe = CreateStatsNamedPipeInstance();
            if (!listening_pipe || listening_pipe == INVALID_HANDLE_VALUE)
            {
                Sleep(50);
                continue;
            }
        }

        if (WaitForStatsPipeClient(listening_pipe) && pipe_running)
        {
            std::vector<uint8_t> frame(FANY_IME_STATS_MAX_FRAME_BYTES);
            DWORD bytes_read = 0;
            BOOL read_result = FALSE;
            DWORD pipe_mode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
            if (SetNamedPipeHandleState(listening_pipe, &pipe_mode, nullptr, nullptr))
            {
                const auto deadline = std::chrono::steady_clock::now() + kStatsPipeReadTimeout;
                while (pipe_running && std::chrono::steady_clock::now() < deadline)
                {
                    read_result =
                        ReadFile(listening_pipe, frame.data(), static_cast<DWORD>(frame.size()), &bytes_read, nullptr);
                    if (read_result || GetLastError() != ERROR_NO_DATA)
                    {
                        break;
                    }
                    Sleep(1);
                }
            }
            if (read_result && bytes_read != 0)
            {
                // A frame that fails decoding is dropped here; the frame is the
                // only thing an exception could damage, so no state escapes.
                ProcessStatsFrame(listening_pipe, frame.data(), bytes_read);
            }
        }

        if (listening_pipe && listening_pipe != INVALID_HANDLE_VALUE)
        {
            DisconnectNamedPipe(listening_pipe);
            CloseHandle(listening_pipe);
            listening_pipe = INVALID_HANDLE_VALUE;
        }
    }

    if (listening_pipe && listening_pipe != INVALID_HANDLE_VALUE)
    {
        DisconnectNamedPipe(listening_pipe);
        CloseHandle(listening_pipe);
    }
}

void ShutdownStatsPipe()
{
    CloseSharedStatsStore();
}
} // namespace MsimeStats
