#include "stats_collector.h"
#include "Ipc.h"
#include <Windows.h>
#include <atomic>
#include <cstring>
#include <string>

// from DllMain.cpp; see Globals.h.
HMODULE PinThisModuleForCallback();

namespace
{
// The queue is produced from TSF edit-session threads and consumed by the
// threadpool flush. SRWLOCK plus atomics are used instead of C++ static
// objects on purpose: the DLL loads into host processes with
// /Zc:threadSafeInit-, so nothing here may rely on dynamic initialization.
SRWLOCK g_queueLock = SRWLOCK_INIT;
MsimeStats::StatsEventQueue g_queue;
std::atomic<bool> g_flushInFlight{false};
std::atomic<bool> g_sessionIdResolved{false};
std::atomic<DWORD> g_sessionId{0};

void CALLBACK FlushStatisticsEvents(PTP_CALLBACK_INSTANCE, PVOID);

DWORD CurrentSessionId()
{
    // Named pipes are machine-global; the prefix plus the decimal session id
    // keeps fast user switching and RDP sessions apart. Both ends must compute
    // this the same way (see FANY_IME_STATS_PIPE_NAME_PREFIX).
    if (!g_sessionIdResolved.load(std::memory_order_acquire))
    {
        DWORD sessionId = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
        g_sessionId.store(sessionId, std::memory_order_relaxed);
        g_sessionIdResolved.store(true, std::memory_order_release);
    }
    return g_sessionId.load(std::memory_order_acquire);
}

std::wstring BuildStatsPipeName()
{
    return std::wstring(FANY_IME_STATS_PIPE_NAME_PREFIX) + std::to_wstring(CurrentSessionId());
}

bool SendStatsBatch(const FanyImeStatsBatchHeader &header, const FanyImeStatsEvent *events, size_t eventCount)
{
    const std::wstring pipeName = BuildStatsPipeName();
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        pipe = CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
        {
            break;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_BUSY)
        {
            if (!WaitNamedPipeW(pipeName.c_str(), 20))
            {
                break;
            }
            continue;
        }
        // The Server serves one instance at a time and closes it between
        // connections, so a second host process flushing in that gap sees
        // ERROR_FILE_NOT_FOUND rather than ERROR_PIPE_BUSY: the name exists
        // again a moment later. Anything else means nobody is listening.
        if (error != ERROR_FILE_NOT_FOUND)
        {
            break;
        }
        Sleep(5);
    }
    if (pipe == INVALID_HANDLE_VALUE)
    {
        // The Server is not listening (or it just went away). Statistics are not
        // critical data: drop the batch instead of retrying or buffering on
        // disk. The caller adds the loss to dropped_count.
        return false;
    }

    // The queue capacity bounds a batch, so the whole frame fits on the stack
    // (28 + 256 * 24 = 6172 bytes, far below the 16 KiB frame limit).
    unsigned char frame[sizeof(FanyImeStatsBatchHeader) +
                        MsimeStats::StatsEventQueue::kCapacity * sizeof(FanyImeStatsEvent)] = {};
    const size_t frameBytes = sizeof(header) + eventCount * sizeof(FanyImeStatsEvent);
    std::memcpy(frame, &header, sizeof(header));
    if (eventCount != 0)
    {
        std::memcpy(frame + sizeof(header), events, eventCount * sizeof(FanyImeStatsEvent));
    }

    DWORD bytesWritten = 0;
    const BOOL writeResult = WriteFile(pipe, frame, static_cast<DWORD>(frameBytes), &bytesWritten, nullptr);
    CloseHandle(pipe);
    return writeResult && bytesWritten == frameBytes;
}

void CALLBACK FlushStatisticsEvents(PTP_CALLBACK_INSTANCE instance, PVOID context)
{
    // Released by the thread pool after this function has fully returned.
    FreeLibraryWhenCallbackReturns(instance, static_cast<HMODULE>(context));
    FanyImeStatsEvent batch[MsimeStats::StatsEventQueue::kCapacity] = {};
    for (;;)
    {
        FanyImeStatsBatchHeader header;
        size_t batchCount = 0;
        {
            AcquireSRWLockExclusive(&g_queueLock);
            batchCount = g_queue.PopBatch(batch, MsimeStats::StatsEventQueue::kCapacity);
            header.event_count = static_cast<uint32_t>(batchCount);
            header.payload_bytes = static_cast<uint32_t>(batchCount * sizeof(FanyImeStatsEvent));
            // Only an actually sent frame may consume the counter: an empty
            // batch is never sent, so taking it here would discard a pending
            // loss instead of reporting it with the next frame.
            header.dropped_count = batchCount != 0 ? g_queue.TakeDroppedCount() : 0;
            header.source_process_id = GetCurrentProcessId();
            ReleaseSRWLockExclusive(&g_queueLock);
        }

        if (batchCount != 0 && !SendStatsBatch(header, batch, batchCount))
        {
            AcquireSRWLockExclusive(&g_queueLock);
            // The failed frame carried these events and the drop counter with
            // it; put both back so the next successful frame reports the loss.
            g_queue.AddDroppedCount(header.dropped_count + static_cast<uint32_t>(batchCount));
            ReleaseSRWLockExclusive(&g_queueLock);
        }

        bool queueDrained = false;
        {
            AcquireSRWLockExclusive(&g_queueLock);
            if (g_queue.Size() == 0)
            {
                // Producers that pushed while this flush ran were held back by
                // the in-flight flag; the queue is empty now, so re-open it.
                g_flushInFlight.store(false, std::memory_order_relaxed);
                queueDrained = true;
            }
            ReleaseSRWLockExclusive(&g_queueLock);
        }
        if (queueDrained)
        {
            break;
        }
    }
}
} // namespace

void MsimeStats::QueueStatisticsEvent(const CharClassCounts &counts)
{
    if (counts.Total() == 0)
    {
        return;
    }
    if (!Global::StatisticsEnabled.load(std::memory_order_relaxed))
    {
        // The Server drops frames while the switch is off, but the opt-out has
        // to cost the input path nothing: with it off nothing is queued and the
        // statistics pipe is never opened from a host process.
        return;
    }

    FILETIME now = {};
    GetSystemTimeAsFileTime(&now);
    FanyImeStatsEvent event{};
    event.timestamp_utc_ft = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    event.cjk = counts.cjk;
    event.latin = counts.latin;
    event.digit = counts.digit;
    event.punct = counts.punct;
    event.other = counts.other;

    AcquireSRWLockExclusive(&g_queueLock);
    g_queue.Push(event);
    if (!g_flushInFlight.load(std::memory_order_relaxed))
    {
        // In-memory enqueue plus one threadpool submission: the connection
        // attempt and the write happen off the host's commit path, and a
        // running flush keeps the flag set until it has drained the queue.
        // A loader reference, not DllAddRef: the host may unload this TIP without asking DllCanUnloadNow.
        HMODULE self = PinThisModuleForCallback();
        if (self)
        {
            g_flushInFlight.store(true, std::memory_order_relaxed);
            if (!TrySubmitThreadpoolCallback(FlushStatisticsEvents, self, nullptr))
            {
                g_flushInFlight.store(false, std::memory_order_relaxed);
                FreeLibrary(self);
            }
        }
    }
    ReleaseSRWLockExclusive(&g_queueLock);
}
