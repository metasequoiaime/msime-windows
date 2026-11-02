#include "Ipc.h"
#include "../../../engine/contracts/ipc_negotiation.h"
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <debugapi.h>
#include <deque>
#include <handleapi.h>
#include <map>
#include <mutex>
#include <minwindef.h>
#include <utility>
#include <vector>
#include <winnt.h>
#include <winuser.h>
#include "Globals.h"
#include "FanyDefines.h"
#include "MetasequoiaIME.h"
#include <fmt/xchar.h>
#include "../Utils/PerfTimer.h"

static thread_local HANDLE hMapFile = nullptr;
static thread_local void *pBuf = nullptr;
static thread_local FanyImeSharedMemoryData *sharedData = nullptr;
static thread_local bool canUseSharedMemory = false;

static thread_local HANDLE hPipe = nullptr;
static thread_local uint32_t negotiatedServerCapabilities = 0;
static thread_local HANDLE hFromServerPipe = nullptr;
static thread_local HANDLE hToTsfWorkerThreadPipe = nullptr;

static thread_local FanyImeNamedpipeData namedpipeData = {};
static thread_local FanyImeNamedpipeDataToTsf namedpipeDataFromServer = {};
static thread_local FanyImeNamedpipeDataToTsf transportUnavailableReply = {};
static thread_local std::map<uint64_t, FanyImeNamedpipeDataToTsf> pendingReplies;
static thread_local std::deque<FanyImeNamedpipeDataToTsf> unsolicitedReplies;
static thread_local uint64_t nextRequestId = 0;
static thread_local uint64_t nextFocusSessionToken = 0;
static thread_local CMetasequoiaIME *boundFocusStateOwner = nullptr;
static thread_local bool *boundFocusResetPending = nullptr;
static thread_local bool *boundActivationRequired = nullptr;
static thread_local std::atomic<uint64_t> *boundExpectedWorkerFocusToken = nullptr;
static thread_local std::atomic<bool> *boundLocalSessionResetPending = nullptr;
static thread_local std::atomic<UINT> *boundLocalSessionResetToken = nullptr;
static thread_local std::atomic<bool> *boundWorkerCommitReady = nullptr;
static thread_local std::atomic<uint64_t> *boundAcknowledgedWorkerFocusToken = nullptr;
static thread_local std::atomic<HANDLE> *boundWorkerPipeHandle = nullptr;
static thread_local std::atomic<FanyImeTemporaryJapanese::Policy> *boundTemporaryJapanesePolicy = nullptr;
static thread_local std::atomic<UINT> *boundWorkerPipeGeneration = nullptr;
static thread_local HANDLE boundWorkerAckEvent = nullptr;
static thread_local HANDLE boundWorkerPipePublishedEvent = nullptr;

/* Data size transfered from Server process */
static const int ServerDtPipeDataSize = 512;

namespace
{
constexpr size_t MaxPendingReplyCount = 64;
constexpr size_t MaxDiagnosticRecordCount = 256;
constexpr size_t MaxDiagnosticQueuedUnits = 32 * 1024;
constexpr DWORD DiagnosticFlushDelayMs = 250;
std::mutex diagnosticLogMutex;
std::deque<std::wstring> diagnosticLogRecords;
size_t diagnosticLogQueuedUnits = 0;
uint32_t diagnosticLogDroppedCount = 0;
bool diagnosticFlushScheduled = false;
std::atomic<uint64_t> issue47Sequence{0};

void CALLBACK FlushTsfDiagnosticLogs(PTP_CALLBACK_INSTANCE, PVOID);

void ScheduleDiagnosticFlushLocked()
{
    if (diagnosticFlushScheduled)
    {
        return;
    }
    // A loader reference, not DllAddRef: the host can unload this TIP without asking DllCanUnloadNow, and a callback
    // sleeping in here would then return into whatever was mapped at this address next (CS2 crashed in SDL3 that way).
    HMODULE self = PinThisModuleForCallback();
    if (!self)
    {
        return;
    }
    diagnosticFlushScheduled = true;
    if (!TrySubmitThreadpoolCallback(FlushTsfDiagnosticLogs, self, nullptr))
    {
        diagnosticFlushScheduled = false;
        FreeLibrary(self);
    }
}

bool SendDiagnosticBatch(const FanyImeTsfDiagnosticBatchHeader &header, const std::wstring &payload)
{
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        pipe = CreateFileW(FANY_IME_TSF_DIAGNOSTIC_NAMED_PIPE, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
        {
            break;
        }
        if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(FANY_IME_TSF_DIAGNOSTIC_NAMED_PIPE, 20))
        {
            break;
        }
    }
    if (pipe == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    const size_t payloadBytes = payload.size() * sizeof(wchar_t);
    std::vector<unsigned char> frame(sizeof(header) + payloadBytes);
    memcpy(frame.data(), &header, sizeof(header));
    if (payloadBytes != 0)
    {
        memcpy(frame.data() + sizeof(header), payload.data(), payloadBytes);
    }
    DWORD bytesWritten = 0;
    const BOOL writeResult = WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &bytesWritten, nullptr);
    CloseHandle(pipe);
    return writeResult && bytesWritten == frame.size();
}

void CALLBACK FlushTsfDiagnosticLogs(PTP_CALLBACK_INSTANCE instance, PVOID context)
{
    // Released by the thread pool after this function has fully returned.
    FreeLibraryWhenCallbackReturns(instance, static_cast<HMODULE>(context));
    Sleep(DiagnosticFlushDelayMs);

    FanyImeTsfDiagnosticBatchHeader header;
    std::vector<std::wstring> batchRecords;
    batchRecords.reserve(MaxDiagnosticRecordCount);
    size_t batchUnits = 0;
    std::wstring payload;
    bool loggingDisabled = false;
    {
        std::lock_guard lock(diagnosticLogMutex);
        if (!Global::TsfDiagnosticLogEnabled.load(std::memory_order_relaxed))
        {
            diagnosticLogRecords.clear();
            diagnosticLogQueuedUnits = 0;
            diagnosticLogDroppedCount = 0;
            diagnosticFlushScheduled = false;
            loggingDisabled = true;
        }
        else
        {
            const size_t maxPayloadUnits =
                (FANY_IME_TSF_DIAGNOSTIC_MAX_FRAME_BYTES - sizeof(FanyImeTsfDiagnosticBatchHeader)) / sizeof(wchar_t);
            while (!diagnosticLogRecords.empty())
            {
                const std::wstring &record = diagnosticLogRecords.front();
                if (!batchRecords.empty() && batchUnits + record.size() > maxPayloadUnits)
                {
                    break;
                }
                batchUnits += (std::min)(record.size(), maxPayloadUnits - batchUnits);
                diagnosticLogQueuedUnits -= record.size();
                batchRecords.push_back(std::move(diagnosticLogRecords.front()));
                diagnosticLogRecords.pop_front();
                ++header.record_count;
                if (batchUnits == maxPayloadUnits)
                {
                    break;
                }
            }
            header.dropped_count = diagnosticLogDroppedCount;
            header.source_process_id = GetCurrentProcessId();
            diagnosticLogDroppedCount = 0;
        }
    }
    if (loggingDisabled)
    {
        return;
    }

    payload.reserve(batchUnits);
    for (const std::wstring &record : batchRecords)
    {
        payload.append(record.data(), (std::min)(record.size(), batchUnits - payload.size()));
    }
    header.payload_bytes = static_cast<uint32_t>(payload.size() * sizeof(wchar_t));

    const bool sent = header.record_count != 0 && SendDiagnosticBatch(header, payload);
    {
        std::lock_guard lock(diagnosticLogMutex);
        if (!sent)
        {
            diagnosticLogDroppedCount += header.record_count;
        }
        diagnosticFlushScheduled = false;
        if (!diagnosticLogRecords.empty())
        {
            ScheduleDiagnosticFlushLocked();
        }
    }
}
// These tokens fence messages that can outlive a CMetasequoiaIME instance.
// Per-instance counters can collide after HWND/HANDLE reuse during a rapid
// deactivate/reactivate cycle.
std::atomic<UINT> nextLocalSessionResetToken{0};
std::atomic<UINT> nextWorkerPipeGeneration{0};

uint64_t NextProtocolId(uint64_t &counter)
{
    do
    {
        ++counter;
    } while (counter == FANY_IME_UNSOLICITED_REQUEST_ID || counter == FANY_IME_NO_REQUEST_ID);
    return counter;
}

UINT NextAtomicNonzeroToken(std::atomic<UINT> &counter)
{
    UINT token = 0;
    do
    {
        token = counter.fetch_add(1, std::memory_order_acq_rel) + 1;
    } while (token == 0);
    return token;
}

bool IsLocalSessionResetPending()
{
    return boundLocalSessionResetPending && boundLocalSessionResetPending->load(std::memory_order_acquire);
}

bool IsExpectedWorkerFocusAcknowledged()
{
    if (!boundWorkerCommitReady)
    {
        return true;
    }
    if (!boundExpectedWorkerFocusToken || !boundAcknowledgedWorkerFocusToken)
    {
        return boundWorkerCommitReady->load(std::memory_order_acquire);
    }
    const uint64_t expected = boundExpectedWorkerFocusToken->load(std::memory_order_acquire);
    return expected != 0 && boundWorkerCommitReady->load(std::memory_order_acquire) &&
           boundAcknowledgedWorkerFocusToken->load(std::memory_order_acquire) == expected;
}

void PublishWorkerPipeHandleIfChanged(HANDLE workerPipe)
{
    if (!boundWorkerPipeHandle || boundWorkerPipeHandle->load(std::memory_order_acquire) == workerPipe)
    {
        return;
    }
    if (boundWorkerPipeGeneration)
    {
        boundWorkerPipeGeneration->store(NextAtomicNonzeroToken(nextWorkerPipeGeneration), std::memory_order_release);
    }
    boundWorkerPipeHandle->store(workerPipe, std::memory_order_release);
    // The IPC worker thread sleeps while it has no pipe; without this it only
    // noticed the new handle on its next 50 ms timeout, and the FocusSessionReady
    // echo the first key waits for sat unread in the pipe until then.
    if (workerPipe && boundWorkerPipePublishedEvent)
    {
        SetEvent(boundWorkerPipePublishedEvent);
    }
}

inline bool IsValidPipeHandle(HANDLE hPipeHandle)
{
    return hPipeHandle != nullptr && hPipeHandle != INVALID_HANDLE_VALUE;
}

double GetElapsedMilliseconds(const LARGE_INTEGER &startCounter, const LARGE_INTEGER &endCounter,
                              const LARGE_INTEGER &frequency)
{
    return static_cast<double>(endCounter.QuadPart - startCounter.QuadPart) * 1000.0 /
           static_cast<double>(frequency.QuadPart);
}

uint64_t GetPipeClientId()
{
    thread_local const uint64_t clientId =
        (static_cast<uint64_t>(GetCurrentProcessId()) << 32) | static_cast<uint64_t>(GetCurrentThreadId());
    return clientId;
}

void ClosePipeHandleIfValid(HANDLE &hPipeHandle)
{
    if (IsValidPipeHandle(hPipeHandle))
    {
        CloseHandle(hPipeHandle);
    }
    hPipeHandle = nullptr;
}

void RequestNamedpipeReconnect()
{
    MarkNamedpipeSessionDirty();
    if (Global::msgWndHandle && IsWindow(Global::msgWndHandle))
    {
        PostMessage(Global::msgWndHandle, WM_IpcReconnect, 0, 0);
    }
}

bool IsValidServerReply(const FanyImeNamedpipeDataToTsf &reply)
{
    if (reply.msg_type > Global::DataFromServerMsgType::MaxKnown)
    {
        return false;
    }

    for (const wchar_t ch : reply.candidate_string)
    {
        if (ch == L'\0')
        {
            return true;
        }
    }
    return false;
}

// Keeps the cache bounded by evicting the oldest request ids, but never the
// entry just stored: that is the reply a waiter is about to look up.
void StorePendingReply(uint64_t requestId, const FanyImeNamedpipeDataToTsf &reply)
{
    pendingReplies[requestId] = reply;
    auto oldest = pendingReplies.begin();
    while (pendingReplies.size() > MaxPendingReplyCount)
    {
        if (oldest->first == requestId)
        {
            ++oldest;
        }
        oldest = pendingReplies.erase(oldest);
    }
}

void CachePendingReply(const FanyImeNamedpipeDataToTsf &reply)
{
    if (reply.msg_type == Global::DataFromServerMsgType::PipeReady ||
        FanyImeProtocol::IsNegotiationReply(reply.msg_type))
    {
        return;
    }
    if (reply.request_id == FANY_IME_UNSOLICITED_REQUEST_ID)
    {
        unsolicitedReplies.push_back(reply);
        while (unsolicitedReplies.size() > MaxPendingReplyCount)
        {
            unsolicitedReplies.pop_front();
        }
        return;
    }
    StorePendingReply(reply.request_id, reply);
}

bool WriteOverlappedWithTimeout(HANDLE hPipeHandle, const void *data, DWORD size, DWORD &bytesWritten)
{
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent)
    {
        return false;
    }

    BOOL writeResult = WriteFile(hPipeHandle, data, size, &bytesWritten, &overlapped);
    if (!writeResult && GetLastError() == ERROR_IO_PENDING)
    {
        const DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 50);
        if (waitResult == WAIT_OBJECT_0)
        {
            writeResult = GetOverlappedResult(hPipeHandle, &overlapped, &bytesWritten, FALSE);
        }
        else
        {
            CancelIoEx(hPipeHandle, &overlapped);
            // Cancellation can lose a race with normal completion. Preserve a
            // successfully completed hello instead of closing a healthy pipe.
            writeResult = GetOverlappedResult(hPipeHandle, &overlapped, &bytesWritten, TRUE);
        }
    }

    CloseHandle(overlapped.hEvent);
    return writeResult && bytesWritten == size;
}

enum class OverlappedReadResult
{
    Completed,
    TimedOut,
    Failed,
};

OverlappedReadResult ReadOverlappedWithTimeout(HANDLE hPipeHandle, void *data, DWORD size, DWORD timeoutMs,
                                               DWORD &bytesRead)
{
    bytesRead = 0;
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent)
    {
        return OverlappedReadResult::Failed;
    }

    BOOL readResult = ReadFile(hPipeHandle, data, size, &bytesRead, &overlapped);
    if (!readResult && GetLastError() == ERROR_IO_PENDING)
    {
        const DWORD waitResult = WaitForSingleObject(overlapped.hEvent, timeoutMs);
        if (waitResult == WAIT_OBJECT_0)
        {
            readResult = GetOverlappedResult(hPipeHandle, &overlapped, &bytesRead, FALSE);
        }
        else
        {
            CancelIoEx(hPipeHandle, &overlapped);
            const BOOL completed = GetOverlappedResult(hPipeHandle, &overlapped, &bytesRead, TRUE);
            const DWORD completionError = completed ? ERROR_SUCCESS : GetLastError();
            CloseHandle(overlapped.hEvent);
            if (completed)
            {
                return OverlappedReadResult::Completed;
            }
            return waitResult == WAIT_TIMEOUT && completionError == ERROR_OPERATION_ABORTED
                       ? OverlappedReadResult::TimedOut
                       : OverlappedReadResult::Failed;
        }
    }

    CloseHandle(overlapped.hEvent);
    return readResult ? OverlappedReadResult::Completed : OverlappedReadResult::Failed;
}

bool WaitForReplyPipeReady(HANDLE hPipeHandle)
{
    constexpr DWORD readyTimeoutMs = 250;
    const ULONGLONG deadline = GetTickCount64() + readyTimeoutMs;

    while (true)
    {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
        {
            return false;
        }

        FanyImeNamedpipeDataToTsf reply = {};
        DWORD bytesRead = 0;
        const DWORD remainingMs = static_cast<DWORD>(deadline - now);
        const OverlappedReadResult result =
            ReadOverlappedWithTimeout(hPipeHandle, &reply, sizeof(reply), remainingMs, bytesRead);
        if (result != OverlappedReadResult::Completed || bytesRead != sizeof(reply) || !IsValidServerReply(reply))
        {
            return false;
        }

        if (reply.msg_type == Global::DataFromServerMsgType::PipeReady)
        {
            return reply.request_id == FANY_IME_UNSOLICITED_REQUEST_ID;
        }

        // A reconnect can race with a reply already queued by the Server.
        // Preserve it for its owning edit session and continue waiting for the
        // explicit endpoint-registration acknowledgement.
        CachePendingReply(reply);
    }
}

bool WaitForWorkerPipeReady(HANDLE hPipeHandle)
{
    constexpr DWORD readyTimeoutMs = 250;
    FanyImeNamedpipeDataToTsfWorkerThread reply = {};
    DWORD bytesRead = 0;
    const OverlappedReadResult result =
        ReadOverlappedWithTimeout(hPipeHandle, &reply, sizeof(reply), readyTimeoutMs, bytesRead);
    return result == OverlappedReadResult::Completed && bytesRead == sizeof(reply) &&
           reply.msg_type == Global::DataToTsfWorkerThreadMsgType::PipeReady && reply.data[0] == L'\0';
}

bool WaitForProtocolReady(uint64_t requestId)
{
    negotiatedServerCapabilities = 0;
    // Do not clear the temporary R-mode policy here: the worker pipe registers
    // before Main and may already hold this connection's policy. CloseNamedpipe
    // and the worker's disconnect path clear it for a replaced Server.
    const ULONGLONG deadline = GetTickCount64() + 250;
    while (GetTickCount64() < deadline)
    {
        FanyImeNamedpipeDataToTsf reply{};
        DWORD bytesRead = 0;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
            return false;
        const auto result = ReadOverlappedWithTimeout(hFromServerPipe, &reply, sizeof(reply),
                                                      static_cast<DWORD>(deadline - now), bytesRead);
        if (result != OverlappedReadResult::Completed || bytesRead != sizeof(reply) || !IsValidServerReply(reply))
            return false;
        if (FanyImeProtocol::IsNegotiationReply(reply.msg_type) && reply.request_id == requestId)
        {
            const bool accepted = FanyImeProtocol::AcceptReply(reply, requestId);
            if (accepted)
                negotiatedServerCapabilities = FanyImeProtocol::ReplyCapabilities(reply);
            return accepted;
        }
        CachePendingReply(reply);
    }
    return false;
}

bool WritePipeHello(HANDLE hPipeHandle, UINT pipeRole)
{
    DWORD bytesWritten = 0;
    if (pipeRole == FanyImePipeRole::Main)
    {
        const auto hello =
            FanyImeProtocol::Hello(GetPipeClientId(), NextProtocolId(nextRequestId),
                                   FanyImeProtocol::Capabilities | FanyImeProtocol::CharacterSetShortcut |
                                       FanyImeProtocol::CompositionRestore | FanyImeProtocol::CaretStateIndicator |
                                       FanyImeProtocol::TrilingualCycle | FanyImeProtocol::InputStateShortcuts |
                                       FanyImeProtocol::TemporaryJapaneseLongVowel);
        BOOL ret = WriteFile(hPipeHandle, &hello, sizeof(hello), &bytesWritten, NULL);
        // Never authorize keys from merely writing a hello. An old Server
        // without negotiation times out into the existing raw-input fallback.
        return ret && bytesWritten == sizeof(hello) && WaitForProtocolReady(hello.request_id);
    }

    FanyImePipeHello hello = {};
    hello.client_id = GetPipeClientId();
    hello.pipe_role = pipeRole;
    return WriteOverlappedWithTimeout(hPipeHandle, &hello, sizeof(hello), bytesWritten);
}

bool TryOpenClientPipe(HANDLE &hPipeHandle, const wchar_t *pipeName, UINT pipeRole)
{
    if (IsValidPipeHandle(hPipeHandle))
    {
        if (pipeRole == FanyImePipeRole::ToTsfWorkerThread && boundWorkerPipeHandle &&
            boundWorkerPipeHandle->load(std::memory_order_acquire) != hPipeHandle)
        {
            // IpcWorkerThread clears the published atomic only after its
            // OVERLAPPED read has completed. A still-nonnull TLS handle is
            // therefore a stale endpoint and is now safe to close/re-register;
            // do not publish it again before the queued disconnect message.
            MarkNamedpipeSessionDirty();
            ClosePipeHandleIfValid(hPipeHandle);
        }
        else
        {
            if (pipeRole != FanyImePipeRole::ToTsf)
            {
                // Main is synchronous and is validated by its NOWAIT write. The
                // worker pipe can have a read outstanding on another thread, which
                // is responsible for reporting disconnection.
                return true;
            }

            // The reply pipe is overlapped and has no outstanding operation here,
            // so PeekNamedPipe is a nonblocking liveness probe. A stale non-null
            // HANDLE after Server restart must not authorize the first new key.
            DWORD bytesAvailable = 0;
            if (PeekNamedPipe(hPipeHandle, nullptr, 0, nullptr, &bytesAvailable, nullptr))
            {
                return true;
            }

            MarkNamedpipeSessionDirty();
            ClosePipeHandleIfValid(hPipeHandle);
            ResetNamedpipeReplyState();
        }
    }

    // The pipe name lives in the machine-global namespace, so whichever
    // process answers is not necessarily the Server, nor even the same
    // account. SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION lets it check
    // who connected but never act as this thread, which matters most for the
    // app container and low integrity hosts the TIP is loaded into.
    const DWORD flags = SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION |
                        (pipeRole == FanyImePipeRole::Main ? 0 : FILE_FLAG_OVERLAPPED);
    HANDLE openedPipe = CreateFile(   //
        pipeName,                     //
        GENERIC_READ | GENERIC_WRITE, //
        0,                            //
        nullptr,                      //
        OPEN_EXISTING,                //
        flags,                        //
        nullptr                       //
    );

    if (openedPipe == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeW(pipeName, 10))
        {
            openedPipe = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, flags, nullptr);
        }
    }

    if (openedPipe == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    hPipeHandle = openedPipe;
    // Main writes are NOWAIT. Both receive pipes use true overlapped I/O with
    // explicit deadlines, so neither can stall the host UI thread.
    DWORD readMode = PIPE_READMODE_MESSAGE | (pipeRole == FanyImePipeRole::Main ? PIPE_NOWAIT : PIPE_WAIT);
    if (!SetNamedPipeHandleState(hPipeHandle, &readMode, nullptr, nullptr))
    {
        ClosePipeHandleIfValid(hPipeHandle);
        return false;
    }
    if (!WritePipeHello(hPipeHandle, pipeRole))
    {
        ClosePipeHandleIfValid(hPipeHandle);
        return false;
    }
    if (pipeRole == FanyImePipeRole::ToTsf && !WaitForReplyPipeReady(hPipeHandle))
    {
        // A successful CreateFile/hello write does not mean the Server has
        // registered this reverse endpoint yet. Do not allow a key request to
        // overtake registration and lose its reply.
        ClosePipeHandleIfValid(hPipeHandle);
        return false;
    }
    if (pipeRole == FanyImePipeRole::ToTsfWorkerThread && !WaitForWorkerPipeReady(hPipeHandle))
    {
        // Do not publish a worker handle until the Server has installed the
        // exact endpoint. This prevents Main/Activated from overtaking the
        // worker hello on cold start or under scheduler pressure.
        ClosePipeHandleIfValid(hPipeHandle);
        return false;
    }
    if (pipeRole == FanyImePipeRole::ToTsfWorkerThread)
    {
        // Every physical worker endpoint needs a new activation token.  A
        // registration ACK only proves that the endpoint exists; it does not
        // authorize commits from the preceding endpoint/focus generation.
        RequireNamedpipeFocusActivation();
    }
    return true;
}
} // namespace

void BindNamedpipeFocusState(CMetasequoiaIME *owner, bool *focusResetPending, bool *activationRequired,
                             std::atomic<uint64_t> *expectedWorkerFocusToken,
                             std::atomic<bool> *localSessionResetPending, std::atomic<UINT> *localSessionResetToken,
                             std::atomic<bool> *workerCommitReady, std::atomic<uint64_t> *acknowledgedWorkerFocusToken,
                             std::atomic<HANDLE> *workerPipeHandle, std::atomic<UINT> *workerPipeGeneration,
                             std::atomic<FanyImeTemporaryJapanese::Policy> *temporaryJapanesePolicy)
{
    boundFocusStateOwner = owner;
    boundFocusResetPending = focusResetPending;
    boundActivationRequired = activationRequired;
    boundExpectedWorkerFocusToken = expectedWorkerFocusToken;
    boundLocalSessionResetPending = localSessionResetPending;
    boundLocalSessionResetToken = localSessionResetToken;
    boundWorkerCommitReady = workerCommitReady;
    boundAcknowledgedWorkerFocusToken = acknowledgedWorkerFocusToken;
    boundWorkerPipeHandle = workerPipeHandle;
    boundWorkerPipeGeneration = workerPipeGeneration;
    boundTemporaryJapanesePolicy = temporaryJapanesePolicy;
}

FanyImeTemporaryJapanese::Policy GetTemporaryJapanesePolicy()
{
    return boundTemporaryJapanesePolicy ? boundTemporaryJapanesePolicy->load(std::memory_order_relaxed) : 0;
}

void BindNamedpipeWakeEvents(const void *owner, HANDLE workerAckEvent, HANDLE workerPipePublishedEvent)
{
    if (owner == nullptr || boundFocusStateOwner != owner)
    {
        return;
    }
    boundWorkerAckEvent = workerAckEvent;
    boundWorkerPipePublishedEvent = workerPipePublishedEvent;
}

void UnbindNamedpipeFocusState(const void *owner)
{
    if (owner == nullptr || boundFocusStateOwner != owner)
    {
        return;
    }
    boundFocusStateOwner = nullptr;
    boundFocusResetPending = nullptr;
    boundActivationRequired = nullptr;
    boundExpectedWorkerFocusToken = nullptr;
    boundLocalSessionResetPending = nullptr;
    boundLocalSessionResetToken = nullptr;
    boundWorkerCommitReady = nullptr;
    boundAcknowledgedWorkerFocusToken = nullptr;
    boundWorkerPipeHandle = nullptr;
    boundWorkerPipeGeneration = nullptr;
    boundTemporaryJapanesePolicy = nullptr;
    boundWorkerAckEvent = nullptr;
    boundWorkerPipePublishedEvent = nullptr;
}

bool IsNamedpipeFocusStateOwner(const void *owner)
{
    return owner != nullptr && boundFocusStateOwner == owner;
}

// Same test as the Server's IsForegroundInputThreadClient: the foreground
// window's thread, or the thread owning its keyboard focus (a child window
// hosted across threads), is this one.
bool IsCurrentThreadForegroundInput()
{
    const HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    if (foregroundThread == 0)
    {
        return false;
    }
    const DWORD currentThread = GetCurrentThreadId();
    if (foregroundThread == currentThread)
    {
        return true;
    }
    GUITHREADINFO info{sizeof(info)};
    return GetGUIThreadInfo(foregroundThread, &info) && info.hwndFocus &&
           GetWindowThreadProcessId(info.hwndFocus, nullptr) == currentThread;
}

bool IsNamedpipeThreadFocused()
{
    ITfThreadMgr *threadMgr = boundFocusStateOwner ? boundFocusStateOwner->_GetThreadMgr() : nullptr;
    if (!threadMgr)
    {
        return false;
    }
    BOOL focused = FALSE;
    if (SUCCEEDED(threadMgr->IsThreadFocus(&focused)))
    {
        return focused != FALSE;
    }
    // A host that cannot report TSF focus must neither stay offline forever
    // nor let a windowless process (headless Chromium started in the
    // background) take over the Server. Ask Win32 instead.
    return IsCurrentThreadForegroundInput();
}

UINT BeginNamedpipeLocalSessionReset()
{
    if (!boundLocalSessionResetPending || !boundLocalSessionResetToken)
    {
        return 0;
    }
    const UINT token = NextAtomicNonzeroToken(nextLocalSessionResetToken);
    boundLocalSessionResetToken->store(token, std::memory_order_release);
    boundLocalSessionResetPending->store(true, std::memory_order_release);
    return token;
}

void InvalidateNamedpipeWorkerGeneration()
{
    if (boundWorkerPipeGeneration)
    {
        boundWorkerPipeGeneration->store(NextAtomicNonzeroToken(nextWorkerPipeGeneration), std::memory_order_release);
    }
}

void RequireNamedpipeFocusActivation()
{
    if (boundActivationRequired)
    {
        *boundActivationRequired = true;
    }
    const uint64_t token = NextProtocolId(nextFocusSessionToken);
    if (boundExpectedWorkerFocusToken)
    {
        boundExpectedWorkerFocusToken->store(token, std::memory_order_release);
    }
    if (boundWorkerCommitReady)
    {
        boundWorkerCommitReady->store(false, std::memory_order_release);
    }
    if (boundAcknowledgedWorkerFocusToken)
    {
        boundAcknowledgedWorkerFocusToken->store(0, std::memory_order_release);
    }
}

void MarkNamedpipeFocusLost()
{
    if (boundFocusResetPending)
    {
        *boundFocusResetPending = true;
    }
    if (boundActivationRequired)
    {
        *boundActivationRequired = false;
    }
    if (boundExpectedWorkerFocusToken)
    {
        boundExpectedWorkerFocusToken->store(0, std::memory_order_release);
    }
    if (boundWorkerCommitReady)
    {
        boundWorkerCommitReady->store(false, std::memory_order_release);
    }
    if (boundAcknowledgedWorkerFocusToken)
    {
        boundAcknowledgedWorkerFocusToken->store(0, std::memory_order_release);
    }
    // Frames from the focus epoch that just ended must not survive into the
    // next explicit activation. This handle has no outstanding worker read.
    ClosePipeHandleIfValid(hFromServerPipe);
    ResetNamedpipeReplyState();
}

void MarkNamedpipeSessionDirty()
{
    // A transport failure must not manufacture a focus transition. Preserve
    // a real OnKillThreadFocus suspension that is already pending, but leave
    // a currently focused session's flag false.
    RequireNamedpipeFocusActivation();
    ClosePipeHandleIfValid(hFromServerPipe);
    ResetNamedpipeReplyState();

    bool shouldPostReset = false;
    UINT resetToken = 0;
    if (boundLocalSessionResetPending && !boundLocalSessionResetPending->exchange(true, std::memory_order_acq_rel))
    {
        resetToken = BeginNamedpipeLocalSessionReset();
        shouldPostReset = resetToken != 0;
    }
    bool resetDelivered = !shouldPostReset;
    const HWND ownerWindow = Global::msgWndHandle;
    if (shouldPostReset && ownerWindow && IsWindow(ownerWindow))
    {
        resetDelivered = PostMessage(ownerWindow, WM_IpcSessionDirty, static_cast<WPARAM>(resetToken), 0) != FALSE;
        if (!resetDelivered && GetWindowThreadProcessId(ownerWindow, nullptr) == GetCurrentThreadId())
        {
            SendMessage(ownerWindow, WM_IpcSessionDirty, static_cast<WPARAM>(resetToken), 0);
            resetDelivered = true;
        }
    }
    if (shouldPostReset && !resetDelivered && boundLocalSessionResetPending && boundLocalSessionResetToken &&
        boundLocalSessionResetToken->load(std::memory_order_acquire) == resetToken)
    {
        // No message can run the local cancel.  Release only the exact token;
        // the activation/transport dirty flags remain and the next key will
        // retry the lifecycle handshake.
        boundLocalSessionResetPending->store(false, std::memory_order_release);
    }
}

bool MarkNamedpipeSessionDirtyForOwner(const void *owner)
{
    if (!IsNamedpipeFocusStateOwner(owner))
    {
        return false;
    }
    MarkNamedpipeSessionDirty();
    return true;
}

int InitIpc()
{
    // The live protocol is named-pipe-only. Keep the legacy mapping open for
    // ABI compatibility, but never advertise or select it for new traffic.
    canUseSharedMemory = false;
    sharedData = nullptr;
    pBuf = nullptr;

    //
    // Shared memory, open here
    //
    hMapFile = OpenFileMappingW( //
        FILE_MAP_ALL_ACCESS,     //
        FALSE,                   //
        FANY_IME_SHARED_MEMORY   //
    );

    //
    // Shared memory is not available, try to use namedpipe
    //
    InitNamedpipe();

    if (!hMapFile)
    {
        // Error handling
        canUseSharedMemory = false;

        // TODO: Log error

        return 0;
    }

    pBuf = MapViewOfFile(    //
        hMapFile,            //
        FILE_MAP_ALL_ACCESS, //
        0,                   //
        0,                   //
        BUFFER_SIZE          //
    );                       //

    if (!pBuf)
    {
        CloseHandle(hMapFile);
        hMapFile = nullptr;
        sharedData = nullptr;
        canUseSharedMemory = false;
        return 0;
    }

    sharedData = static_cast<FanyImeSharedMemoryData *>(pBuf);

    return 0;
}

bool TryReadCandidatePageFromSharedMemory(std::wstring *candidatePage)
{
    if (candidatePage == nullptr || sharedData == nullptr)
    {
        return false;
    }
    candidatePage->assign(sharedData->candidate_string);
    return !candidatePage->empty();
}

int InitNamedpipe()
{
    return ConnectToAllNamedpipe();
}

int ConnectToAllNamedpipe()
{
    if (!TryOpenClientPipe(hFromServerPipe, FANY_IME_TO_TSF_NAMED_PIPE, FanyImePipeRole::ToTsf))
    {
        return 0;
    }
    if (!TryOpenClientPipe(hToTsfWorkerThreadPipe, FANY_IME_TO_TSF_WORKER_THREAD_NAMED_PIPE,
                           FanyImePipeRole::ToTsfWorkerThread))
    {
        return 0;
    }

    // Publish the ACKed worker endpoint before attempting Main. If Main is
    // unavailable, the next retry must keep this healthy endpoint instead of
    // mistaking its still-private TLS handle for a stale registration.
    PublishWorkerPipeHandleIfChanged(hToTsfWorkerThreadPipe);
    if (IsLocalSessionResetPending())
    {
        return 0;
    }

    // Open Main last. Once it exists, a key may be sent immediately, so both
    // reverse channels must already have completed their hello handshakes.
    return TryOpenClientPipe(hPipe, FANY_IME_NAMED_PIPE, FanyImePipeRole::Main) ? 1 : 0;
}

int ConnectToTsfNamedpipe()
{
    return TryOpenClientPipe(hFromServerPipe, FANY_IME_TO_TSF_NAMED_PIPE, FanyImePipeRole::ToTsf) ? 1 : 0;
}

int CloseIpc()
{
    //
    // Namedpipe
    //
    CloseNamedpipe();
    const int result = canUseSharedMemory ? 0 : -1;

    //
    // Shared memory
    //
    if (pBuf)
    {
        UnmapViewOfFile(pBuf);
        pBuf = nullptr;
    }
    sharedData = nullptr;

    if (hMapFile)
    {
        CloseHandle(hMapFile);
        hMapFile = nullptr;
    }
    canUseSharedMemory = false;

    //
    // Events
    //
    for (const auto &eventName : FANY_IME_EVENT_ARRAY)
    {
        HANDLE hEvent = OpenEventW( //
            EVENT_ALL_ACCESS,       //
            FALSE,                  //
            eventName.c_str()       //
        );                          //
        if (hEvent)
        {
            CloseHandle(hEvent);
        }
    }

    return result;
}

int CloseNamedpipe()
{
    negotiatedServerCapabilities = 0;
    if (boundTemporaryJapanesePolicy)
        boundTemporaryJapanesePolicy->store(0, std::memory_order_relaxed);
    ClosePipeHandleIfValid(hPipe);
    ClosePipeHandleIfValid(hFromServerPipe);
    ClosePipeHandleIfValid(hToTsfWorkerThreadPipe);
    ResetNamedpipeReplyState();
    return 0;
}

void ResetNamedpipeReplyState()
{
    pendingReplies.clear();
    unsolicitedReplies.clear();
}

bool SupportsCharacterSetShortcut()
{
    return hPipe && hPipe != INVALID_HANDLE_VALUE &&
           (negotiatedServerCapabilities & FanyImeProtocol::CharacterSetShortcut) != 0;
}

bool SupportsCompositionRestore()
{
    return hPipe && hPipe != INVALID_HANDLE_VALUE &&
           (negotiatedServerCapabilities & FanyImeProtocol::CompositionRestore) != 0;
}

bool UsesHorizontalArrowSelection()
{
    return Global::HorizontalArrowSelectionEnabled.load(std::memory_order_relaxed) && !Global::IsUiLessMode() &&
           SupportsCompositionRestore();
}

bool UsesDirectHelpcodeEndTrigger()
{
    return Global::DirectHelpcodeEndTriggerKeys.load(std::memory_order_relaxed) != 0 && !Global::IsUiLessMode() &&
           SupportsCompositionRestore();
}

bool SupportsCaretStateIndicator()
{
    return hPipe && hPipe != INVALID_HANDLE_VALUE &&
           (negotiatedServerCapabilities & FanyImeProtocol::CaretStateIndicator) != 0;
}

bool SupportsTrilingualCycle()
{
    return hPipe && hPipe != INVALID_HANDLE_VALUE &&
           (negotiatedServerCapabilities & FanyImeProtocol::TrilingualCycle) != 0;
}

bool SupportsInputStateShortcuts()
{
    return hPipe && hPipe != INVALID_HANDLE_VALUE &&
           (negotiatedServerCapabilities & FanyImeProtocol::InputStateShortcuts) != 0;
}

HANDLE GetToTsfWorkerThreadNamedpipe()
{
    return hToTsfWorkerThreadPipe;
}

int WriteDataToSharedMemory(           //
    UINT keycode,                      //
    WCHAR wch,                         //
    UINT modifiers_down,               //
    const int point[2],                //
    int pinyin_length,                 //
    const std::wstring &pinyin_string, //
    UINT write_flag                    //
)
{
    // The shared-memory protocol has no client_id/request_id and cannot be
    // made safe in a multi-TSF-thread process. Keep the mapping code only for
    // legacy compatibility; all live event payloads use the named-pipe ABI.
    return WriteDataToNamedPipe(keycode, wch, modifiers_down, point, pinyin_length, pinyin_string, write_flag);
}

/**
 * @brief
 *
 * @param keycode
 * @param modifiers_down
 * @param point
 * @param pinyin_length
 * @param pinyin_string
 * @param write_flag
 * @return int
 */
int WriteDataToNamedPipe(              //
    UINT keycode,                      //
    WCHAR wch,                         //
    UINT modifiers_down,               //
    const int point[2],                //
    int pinyin_length,                 //
    const std::wstring &pinyin_string, //
    UINT write_flag                    //
)
{
    // Every logical event starts from a clean packet so status/UI events can
    // never leak key or pinyin data from the preceding request.
    namedpipeData = {};

    if (write_flag >> 0 & 1u)
    {
        namedpipeData.keycode = keycode;
    }

    if (write_flag >> 1 & 1u)
    {
        namedpipeData.wch = wch;
    }

    if (write_flag >> 2 & 1u)
    {
        namedpipeData.modifiers_down = modifiers_down;
    }

    if (write_flag >> 3 & 1u)
    {
        namedpipeData.point[0] = point[0];
        namedpipeData.point[1] = point[1];
    }

    if (write_flag >> 4 & 1u)
    {
        namedpipeData.pinyin_length = max(0, min(pinyin_length, 127));
    }

    if (write_flag >> 5 & 1u)
    {
        const size_t copyLength = min(pinyin_string.size(), _countof(namedpipeData.pinyin_string) - 1);
        wmemcpy(namedpipeData.pinyin_string, pinyin_string.data(), copyLength);
        namedpipeData.pinyin_string[copyLength] = L'\0';
        namedpipeData.pinyin_length = static_cast<int>(copyLength);
    }

    return 0;
}

KeyEventSendResult SendKeyEventToUIProcess(uint64_t *requestId, unsigned inputStateAction)
{
    return SendKeyEventToUIProcessViaNamedPipe(requestId, inputStateAction);
}

void DebugTsfKeyLatency(const wchar_t *stage, uint64_t requestId, double elapsedMs, HRESULT result)
{
    constexpr double kSlowStageThresholdMs = 8.0;
    if (!Global::TsfDiagnosticLogEnabled.load(std::memory_order_relaxed) ||
        (elapsedMs < kSlowStageThresholdMs && SUCCEEDED(result)))
    {
        return;
    }

    const std::wstring message =
        fmt::format(L"[msime][key-latency] side=tsf stage={} request={} elapsed_ms={:.3f} result=0x{:08X} process={}\n",
                    stage, requestId, elapsedMs, static_cast<unsigned long>(result),
                    Global::current_process_name.empty() ? L"unknown" : Global::current_process_name);
    QueueTsfDiagnosticLog(message);
}

void DebugTsfIssue47(const wchar_t *stage, uint64_t requestId, UINT code, WCHAR wch, UINT category, UINT function,
                     int eaten, BOOL composing, size_t virtualKeyLength, HRESULT result, uint64_t correlationToken)
{
    if (!Global::TsfDiagnosticLogEnabled.load(std::memory_order_relaxed))
    {
        return;
    }

    // This is an explicitly enabled, short-lived diagnostic trace. Keep both
    // the VK and translated code point so a TestKeyDown can be matched to the
    // exact OnKeyDown/edit-session path. Composition/candidate/document text
    // is deliberately still excluded.
    const wchar_t *keyClass = L"other";
    if (wch != L'\0' && std::iswprint(static_cast<wint_t>(wch)) != 0)
    {
        keyClass = L"printable";
    }
    else if (code == VK_BACK)
    {
        keyClass = L"backspace";
    }
    else if (code == VK_DELETE)
    {
        keyClass = L"delete";
    }
    else if (code == VK_LEFT || code == VK_RIGHT || code == VK_UP || code == VK_DOWN || code == VK_HOME ||
             code == VK_END || code == VK_PRIOR || code == VK_NEXT)
    {
        keyClass = L"navigation";
    }
    else if (code == VK_SHIFT || code == VK_CONTROL || code == VK_MENU || code == VK_LWIN || code == VK_RWIN)
    {
        keyClass = L"modifier";
    }
    else if (code == VK_RETURN || code == VK_SPACE || code == VK_TAB || code == VK_ESCAPE)
    {
        keyClass = L"control";
    }
    const wchar_t keyText[2] = {wch != L'\0' && std::iswprint(static_cast<wint_t>(wch)) != 0 ? wch : L'-', L'\0'};

    const uint64_t sequence = issue47Sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::wstring message =
        fmt::format(L"[msime][issue47] seq={} stage={} request={} vk=0x{:02X} wch=U+{:04X} key={} "
                    L"key_class={} category={} function={} eaten={} composing={} buffer_len={} "
                    L"result=0x{:08X} correlation={} process={}\n",
                    sequence, stage, requestId, code, static_cast<unsigned int>(wch), keyText, keyClass, category,
                    function, eaten, composing ? 1 : 0, virtualKeyLength, static_cast<unsigned long>(result),
                    correlationToken, Global::current_process_name.empty() ? L"unknown" : Global::current_process_name);
    QueueTsfDiagnosticLog(message);
}

namespace
{
std::wstring DescribeWindowForDiagnostic(HWND hwnd)
{
    if (!hwnd)
    {
        return L"null";
    }
    WCHAR className[128] = {};
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return fmt::format(L"{:#x}[{} pid={} visible={} rect=({},{},{},{})]", reinterpret_cast<uintptr_t>(hwnd), className,
                       pid, IsWindowVisible(hwnd) ? 1 : 0, rect.left, rect.top, rect.right, rect.bottom);
}
} // namespace

void QueueTsfHostWindowDiagnostic(_In_z_ const wchar_t *event, const std::wstring &detail)
{
    if (!Global::TsfDiagnosticLogEnabled.load(std::memory_order_relaxed))
    {
        return;
    }
    GUITHREADINFO gui{};
    gui.cbSize = sizeof(gui);
    const BOOL haveGui = GetGUIThreadInfo(GetCurrentThreadId(), &gui);
    QueueTsfDiagnosticLog(fmt::format(
        L"[host-window] {} {} process={} fg={} focus={} active={} caret_hwnd={} caret=({},{},{},{}) gui_flags={:#x}",
        event, detail, Global::current_process_name, DescribeWindowForDiagnostic(GetForegroundWindow()),
        DescribeWindowForDiagnostic(haveGui ? gui.hwndFocus : nullptr),
        DescribeWindowForDiagnostic(haveGui ? gui.hwndActive : nullptr),
        DescribeWindowForDiagnostic(haveGui ? gui.hwndCaret : nullptr), gui.rcCaret.left, gui.rcCaret.top,
        gui.rcCaret.right, gui.rcCaret.bottom, gui.flags));
}

void QueueNamedpipeFocusGateDiagnostic(bool allowed)
{
    if (!Global::TsfDiagnosticLogEnabled.load(std::memory_order_relaxed))
    {
        return;
    }
    // Both signals, so a host where TSF and Win32 disagree shows up in the log.
    ITfThreadMgr *threadMgr = boundFocusStateOwner ? boundFocusStateOwner->_GetThreadMgr() : nullptr;
    BOOL focused = FALSE;
    const HRESULT hr = threadMgr ? threadMgr->IsThreadFocus(&focused) : E_POINTER;
    QueueTsfDiagnosticLog(fmt::format(L"[focus-gate] activation={} tsf_hr={:#x} tsf_focus={} win32_foreground={} "
                                      L"process={}",
                                      allowed ? L"sent" : L"deferred", static_cast<uint32_t>(hr), focused != FALSE,
                                      IsCurrentThreadForegroundInput(), Global::current_process_name));
}

void QueueTsfDiagnosticLog(const std::wstring &line)
{
    if (!Global::TsfDiagnosticLogEnabled.load(std::memory_order_relaxed))
    {
        return;
    }

    std::wstring sanitized = line;
    for (wchar_t &ch : sanitized)
    {
        if (ch == L'\r' || ch == L'\n')
        {
            ch = L' ';
        }
    }
    while (!sanitized.empty() && sanitized.back() == L' ')
    {
        sanitized.pop_back();
    }
    constexpr size_t MaxSingleRecordUnits = 1024;
    if (sanitized.size() > MaxSingleRecordUnits)
    {
        sanitized.resize(MaxSingleRecordUnits);
    }
    SYSTEMTIME sourceTime{};
    GetLocalTime(&sourceTime);
    sanitized =
        fmt::format(L"[tsf source={:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} p{}:t{} tick={}] {}\n", sourceTime.wYear,
                    sourceTime.wMonth, sourceTime.wDay, sourceTime.wHour, sourceTime.wMinute, sourceTime.wSecond,
                    sourceTime.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), GetTickCount64(), sanitized);

    std::lock_guard lock(diagnosticLogMutex);
    while (!diagnosticLogRecords.empty() && (diagnosticLogRecords.size() >= MaxDiagnosticRecordCount ||
                                             diagnosticLogQueuedUnits + sanitized.size() > MaxDiagnosticQueuedUnits))
    {
        diagnosticLogQueuedUnits -= diagnosticLogRecords.front().size();
        diagnosticLogRecords.pop_front();
        ++diagnosticLogDroppedCount;
    }
    diagnosticLogQueuedUnits += sanitized.size();
    diagnosticLogRecords.push_back(std::move(sanitized));
    ScheduleDiagnosticFlushLocked();
}

int SendHideCandidateWndEventToUIProcess()
{
    return SendHideCandidateWndEventToUIProcessViaNamedPipe();
}

int SendShowCandidateWndEventToUIProcess()
{
    return SendShowCandidateWndEventToUIProcessViaNamedPipe();
}

int SendMoveCandidateWndEventToUIProcess()
{
    return SendMoveCandidateWndEventToUIProcessViaNamedPipe();
}

int SendLangbarRightClickEventToUIProcess(const RECT *prcArea)
{
    return SendLangbarRightClickEventToUIProcessViaNamedPipe(prcArea);
}

//
// Named pipe
//

bool SendToNamedpipe(bool *deliveryAmbiguous = nullptr)
{
    if (deliveryAmbiguous)
    {
        *deliveryAmbiguous = false;
    }
    FanyImeNamedpipeData packet = namedpipeData;
    packet.client_id = GetPipeClientId();
    const bool isLifecyclePacket = packet.event_type == FanyImePipeEventType::ClientHello ||
                                   packet.event_type == FanyImePipeEventType::ClientActivated ||
                                   packet.event_type == FanyImePipeEventType::ClientDeactivated ||
                                   packet.event_type == FanyImePipeEventType::ClientSuspended;

    // Tell Server never to raise an IME HWND for UILess hosts (games / fullscreen).
    if (Global::IsUiLessMode() && (packet.event_type == FanyImePipeEventType::KeyEvent ||
                                   packet.event_type == FanyImePipeEventType::InputStateAction ||
                                   packet.event_type == FanyImePipeEventType::ShowCandidateWnd ||
                                   packet.event_type == FanyImePipeEventType::MoveCandidateWnd ||
                                   packet.event_type == FanyImePipeEventType::HideCandidateWnd))
    {
        packet.modifiers_down |= FanyImePipeFlags::UiLess;
    }
    if ((packet.event_type == FanyImePipeEventType::KeyEvent ||
         packet.event_type == FanyImePipeEventType::InputStateAction) &&
        Global::SensitiveInputField && Global::AiContextUploadEnabled.load(std::memory_order_relaxed))
    {
        packet.modifiers_down |= FanyImePipeFlags::SensitiveField;
    }
    if (packet.event_type == FanyImePipeEventType::ClientActivated && Global::HostUiLessMode)
    {
        packet.keycode = 1;
    }

    // Lifecycle packets are used by Ensure... itself. Every other packet is
    // authorized only after reverse registration, worker publication, exact
    // token activation, and the worker's FocusSessionReady echo. Preserve the
    // staged packet because Ensure... emits lifecycle packets through this
    // same thread-local buffer.
    if (!isLifecyclePacket)
    {
        if (!Global::g_connected || !EnsureNamedpipeFocusSessionActivated())
        {
            namedpipeData = packet;
            return false;
        }
        namedpipeData = packet;
    }

    const bool mainPipeWasOpen = IsValidPipeHandle(hPipe);

    if (!TryOpenClientPipe(hPipe, FANY_IME_NAMED_PIPE, FanyImePipeRole::Main))
    {
        RequestNamedpipeReconnect();
        return false;
    }

    // A newly opened Main cannot authorize an ordinary packet by itself. In
    // normal operation the barrier above already opened Main; reaching this
    // branch means the transport changed underneath that barrier.
    if (!isLifecyclePacket && !mainPipeWasOpen)
    {
        RequestNamedpipeReconnect();
        return false;
    }
    if (!isLifecyclePacket && (IsLocalSessionResetPending() || !IsExpectedWorkerFocusAcknowledged()))
    {
        // The worker reader can report a disconnect concurrently with the UI
        // thread returning from Ensure. Recheck the exact acknowledgement at
        // the final write boundary instead of sending into an epoch whose
        // result channel has already disappeared.
        RequestNamedpipeReconnect();
        return false;
    }

    DWORD bytesWritten = 0;
    const BOOL writeOk = WriteFile(hPipe, &packet, sizeof(packet), &bytesWritten, nullptr);
    if (writeOk && bytesWritten == sizeof(packet))
    {
        return true;
    }
    // PIPE_NOWAIT reports a full inbound quota as success with zero bytes.
    QueueTsfDiagnosticLog(fmt::format(L"[msime][ipc] main-pipe write failed event_type={} ok={} bytes={} gle={}",
                                      packet.event_type, writeOk ? 1 : 0, bytesWritten,
                                      writeOk ? 0UL : static_cast<unsigned long>(GetLastError())));

    // Delivery is ambiguous after any failed write. Never replay an old
    // activation token, key, status, or UI command on a replacement Main.
    // The reconnect timer performs the complete reset/deactivate/activate
    // transaction, followed by a fresh full status snapshot.
    if (deliveryAmbiguous)
    {
        *deliveryAmbiguous = true;
    }
    ClosePipeHandleIfValid(hPipe);
    RequestNamedpipeReconnect();
    return false;
}

static struct FanyImeNamedpipeDataToTsf *TryReadDataFromServerPipeWithDeadline(uint64_t expectedRequestId,
                                                                               bool abortTransportOnTimeout,
                                                                               DWORD timeoutMs, bool *notReady);

struct FanyImeNamedpipeDataToTsf *TryReadDataFromServerPipeWithTimeout(uint64_t expectedRequestId,
                                                                       bool abortTransportOnTimeout)
{
    return TryReadDataFromServerPipeWithDeadline(expectedRequestId, abortTransportOnTimeout, 50, nullptr);
}

// True when a slice has something to consume right now: a cached reply, a
// frame waiting in the pipe, or a state only the read path can report (no
// request id, no pipe, a dead pipe). False means the read would only block.
static bool HasCommitReplyToRead(uint64_t expectedRequestId)
{
    if (expectedRequestId == FANY_IME_NO_REQUEST_ID || pendingReplies.count(expectedRequestId) != 0 ||
        !IsValidPipeHandle(hFromServerPipe))
    {
        return true;
    }
    // The reply pipe has no read outstanding between slices, so this is the
    // same nonblocking probe TryOpenClientPipe uses.
    DWORD bytesAvailable = 0;
    return !PeekNamedPipe(hFromServerPipe, nullptr, 0, nullptr, &bytesAvailable, nullptr) || bytesAvailable != 0;
}

CommitReplyWaitStatus TryReadCommitReplyFromServerPipe(uint64_t expectedRequestId, DWORD blockingWaitMs,
                                                       _Out_ struct FanyImeNamedpipeDataToTsf **reply)
{
    *reply = nullptr;

    if (blockingWaitMs != 0)
    {
        // Nothing can re-run this session, so it waits the bounded single shot.
        // Without a notReady out-parameter the read path always yields a frame:
        // the reply, or TransportUnavailable for a timeout or a real failure.
        struct FanyImeNamedpipeDataToTsf *fetched = TryReadDataFromServerPipeWithDeadline(
            expectedRequestId, /*abortTransportOnTimeout=*/true, blockingWaitMs, nullptr);
        *reply = fetched;
        return fetched->msg_type == Global::DataFromServerMsgType::TransportUnavailable
                   ? CommitReplyWaitStatus::Transport
                   : CommitReplyWaitStatus::Ready;
    }

    // Same nonblocking probe a slice uses: only read when there is something to
    // consume, or a state only the read path can report (a request id that can
    // never be paired, a missing or dead pipe). A healthy pipe with nothing in
    // it is NotReady — a session must not wait on the host UI thread.
    if (!HasCommitReplyToRead(expectedRequestId))
    {
        return CommitReplyWaitStatus::NotReady;
    }

    bool notReady = true;
    struct FanyImeNamedpipeDataToTsf *fetched =
        TryReadDataFromServerPipeWithDeadline(expectedRequestId, /*abortTransportOnTimeout=*/true, 0, &notReady);
    // With notReady set, the read path returns null only at its cooperative
    // boundaries (nothing readable, or only other requests' frames consumed
    // before the zero-length deadline). Every real failure still comes back as a
    // TransportUnavailable frame, so a broken pipe keeps its full reset.
    if (fetched == nullptr)
    {
        return CommitReplyWaitStatus::NotReady;
    }

    *reply = fetched;
    return fetched->msg_type == Global::DataFromServerMsgType::TransportUnavailable ? CommitReplyWaitStatus::Transport
                                                                                    : CommitReplyWaitStatus::Ready;
}

CommitReplyWaitStatus TryReadCommitReplySlice(uint64_t expectedRequestId, ULONGLONG deadlineTick,
                                              struct FanyImeNamedpipeDataToTsf **reply)
{
    constexpr ULONGLONG kWindowMs = FANY_IME_COMMIT_REPLY_TIMEOUT_MS;

    *reply = nullptr;
    // A slice never blocks the host UI thread: it reads only what is already
    // in the pipe (zero read timeout) and otherwise yields at once. The timer
    // between slices is what spaces the polls out.
    bool notReady = true;
    struct FanyImeNamedpipeDataToTsf *fetched = nullptr;
    if (HasCommitReplyToRead(expectedRequestId))
    {
        notReady = false;
        fetched =
            TryReadDataFromServerPipeWithDeadline(expectedRequestId, /*abortTransportOnTimeout=*/true, 0, &notReady);
    }
    if (!notReady)
    {
        *reply = fetched;
        return fetched->msg_type == Global::DataFromServerMsgType::TransportUnavailable
                   ? CommitReplyWaitStatus::Transport
                   : CommitReplyWaitStatus::Ready;
    }

    if (GetTickCount64() < deadlineTick)
    {
        return CommitReplyWaitStatus::NotReady;
    }

    // The total window is over with no reply. The request was delivered, so the
    // Server may already have committed the selection: take exactly the
    // transport reset a single blocking miss always took.
    const ULONGLONG startedAt = deadlineTick >= kWindowMs ? deadlineTick - kWindowMs : 0;
    DebugTsfKeyLatency(L"reply-id-timeout", expectedRequestId, static_cast<double>(GetTickCount64() - startedAt),
                       HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    ClosePipeHandleIfValid(hFromServerPipe);
    RequestNamedpipeReconnect();
    transportUnavailableReply = {};
    transportUnavailableReply.msg_type = Global::DataFromServerMsgType::TransportUnavailable;
    *reply = &transportUnavailableReply;
    return CommitReplyWaitStatus::Transport;
}

void CacheCommitReplyForEditSession(uint64_t expectedRequestId, const struct FanyImeNamedpipeDataToTsf &reply)
{
    if (expectedRequestId == FANY_IME_UNSOLICITED_REQUEST_ID || expectedRequestId == FANY_IME_NO_REQUEST_ID ||
        reply.msg_type == Global::DataFromServerMsgType::PipeReady ||
        FanyImeProtocol::IsNegotiationReply(reply.msg_type))
    {
        return;
    }
    // Keyed by the request the session asked for, and never the entry evicted
    // to keep the cache bounded.
    StorePendingReply(expectedRequestId, reply);
}

static struct FanyImeNamedpipeDataToTsf *TryReadDataFromServerPipeWithDeadline(uint64_t expectedRequestId,
                                                                               bool abortTransportOnTimeout,
                                                                               DWORD timeoutMs, bool *notReady)
{
    PerfTimer replyWaitTimer;

    auto transportUnavailable = []() {
        transportUnavailableReply = {};
        transportUnavailableReply.msg_type = Global::DataFromServerMsgType::TransportUnavailable;
        return &transportUnavailableReply;
    };
    auto softMiss = [](uint64_t requestId) {
        // Deliberately not TransportUnavailable: preedit waiters fall back to
        // the local reading string without tearing down the reply pipe.
        namedpipeDataFromServer = {};
        namedpipeDataFromServer.msg_type = Global::DataFromServerMsgType::Normal;
        namedpipeDataFromServer.request_id = requestId;
        return &namedpipeDataFromServer;
    };

    if (expectedRequestId == FANY_IME_NO_REQUEST_ID)
    {
        return transportUnavailable();
    }

    if (expectedRequestId == FANY_IME_UNSOLICITED_REQUEST_ID && !unsolicitedReplies.empty())
    {
        namedpipeDataFromServer = unsolicitedReplies.front();
        unsolicitedReplies.pop_front();
        return &namedpipeDataFromServer;
    }
    const auto cached = pendingReplies.find(expectedRequestId);
    if (expectedRequestId != FANY_IME_UNSOLICITED_REQUEST_ID && cached != pendingReplies.end())
    {
        namedpipeDataFromServer = cached->second;
        pendingReplies.erase(cached);
        return &namedpipeDataFromServer;
    }

    if (!IsValidPipeHandle(hFromServerPipe))
    {
        if (!TryOpenClientPipe(hFromServerPipe, FANY_IME_TO_TSF_NAMED_PIPE, FanyImePipeRole::ToTsf))
        {
            RequestNamedpipeReconnect();
            return transportUnavailable();
        }
    }

    const ULONGLONG deadline = GetTickCount64() + timeoutMs;

    while (true)
    {
        DWORD bytesRead = 0;
        namedpipeDataFromServer = {};
        const ULONGLONG now = GetTickCount64();
        const DWORD remainingMs = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
        const OverlappedReadResult readResult = ReadOverlappedWithTimeout(
            hFromServerPipe, &namedpipeDataFromServer, sizeof(namedpipeDataFromServer), remainingMs, bytesRead);
        if (readResult == OverlappedReadResult::TimedOut)
        {
            if (notReady)
            {
                // A cooperative commit poll: this slice ended with no reply,
                // but the pipe is healthy and the total window is not over.
                // Leave the pipe, the reply cache and the composition alone.
                *notReady = true;
                return nullptr;
            }
            DebugTsfKeyLatency(L"reply-wait-timeout", expectedRequestId, replyWaitTimer.ElapsedMs(),
                               HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            if (!abortTransportOnTimeout)
            {
                return softMiss(expectedRequestId);
            }
            ClosePipeHandleIfValid(hFromServerPipe);
            RequestNamedpipeReconnect();
            return transportUnavailable();
        }
        if (readResult == OverlappedReadResult::Failed)
        {
            DebugTsfKeyLatency(L"reply-read-failed", expectedRequestId, replyWaitTimer.ElapsedMs(), E_FAIL);
            ClosePipeHandleIfValid(hFromServerPipe);
            pendingReplies.clear();
            unsolicitedReplies.clear();
            RequestNamedpipeReconnect();
            return transportUnavailable();
        }
        if (bytesRead != sizeof(namedpipeDataFromServer))
        {
            DebugTsfKeyLatency(L"reply-size-invalid", expectedRequestId, replyWaitTimer.ElapsedMs(), E_FAIL);
            ClosePipeHandleIfValid(hFromServerPipe);
            pendingReplies.clear();
            unsolicitedReplies.clear();
            RequestNamedpipeReconnect();
            return transportUnavailable();
        }
        if (!IsValidServerReply(namedpipeDataFromServer))
        {
            DebugTsfKeyLatency(L"reply-frame-invalid", expectedRequestId, replyWaitTimer.ElapsedMs(), E_FAIL);
            ClosePipeHandleIfValid(hFromServerPipe);
            pendingReplies.clear();
            unsolicitedReplies.clear();
            RequestNamedpipeReconnect();
            return transportUnavailable();
        }

        // A delayed reply from a timed-out key must not be consumed as the
        // reply for the next key.
        if (namedpipeDataFromServer.msg_type != Global::DataFromServerMsgType::PipeReady &&
            !FanyImeProtocol::IsNegotiationReply(namedpipeDataFromServer.msg_type) &&
            namedpipeDataFromServer.request_id == expectedRequestId)
        {
            DebugTsfKeyLatency(L"reply-wait", expectedRequestId, replyWaitTimer.ElapsedMs(), S_OK);
            return &namedpipeDataFromServer;
        }

        // Another async edit session owns this frame. Keep it bounded and let
        // that session retrieve it by its explicit request ID later.
        CachePendingReply(namedpipeDataFromServer);

        if (GetTickCount64() >= deadline)
        {
            break;
        }
    }

    // We consumed only other request IDs until this request's deadline. The
    // Server may already have committed a selection, so preserving local
    // composition would split the two state machines. Treat this exactly like
    // a broken transport and schedule a tokenized reset plus local cancel.
    if (notReady)
    {
        // Same as the read-timeout above: a cooperative slice boundary, not a
        // failure. The reply cache was updated, the pipe stays up.
        *notReady = true;
        return nullptr;
    }
    if (!abortTransportOnTimeout)
    {
        DebugTsfKeyLatency(L"reply-soft-miss", expectedRequestId, replyWaitTimer.ElapsedMs(), S_FALSE);
        return softMiss(expectedRequestId);
    }
    DebugTsfKeyLatency(L"reply-id-timeout", expectedRequestId, replyWaitTimer.ElapsedMs(),
                       HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    ClosePipeHandleIfValid(hFromServerPipe);
    RequestNamedpipeReconnect();
    return transportUnavailable();
}

/**
 * @brief 询问用户是否安装已下好并校验通过的版本。只弹窗，不联网、不读文件、不启动程序。
 *
 * 载荷三段：版本号、安装包绝对路径、安装包文件名。缺任何一段都不弹窗：那说明服务端
 * 状态坏了，静默跳过比弹一个点不动的窗口好。路径在这里只用于展示，不参与任何判断；
 * 回给 Server 的只有文件名，真正的路径由它从自己的状态里推。
 */
bool PromptUpdateInstaller(const std::wstring &payload)
{
    std::vector<std::wstring> fields;
    size_t start = 0;
    while (start <= payload.size())
    {
        const size_t tab = payload.find(L'\t', start);
        const size_t end = tab == std::wstring::npos ? payload.size() : tab;
        fields.push_back(payload.substr(start, end - start));
        if (tab == std::wstring::npos)
            break;
        start = tab + 1;
    }
    if (fields.size() != 3 || fields[0].empty() || fields[2].empty())
    {
        return false;
    }

    const std::wstring &version = fields[0];
    const std::wstring &installer = fields[2];
    std::wstring text = L"新版本 v" + version + L" 已经下载完成并通过校验。\n\n现在安装吗？\n\n";
    text += L"安装会先退出输入法后台进程，随后打开安装程序；在安装界面里点完成即可。";
    const int answer =
        MessageBoxW(nullptr, text.c_str(), L"水杉输入法", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST);
    if (answer != IDYES)
    {
        // 用户点了「否」：什么都不发。服务端不会在这次的包上再问一遍，等下一个版本再说。
        return true;
    }
    SendToAuxNamedpipe(L"InstallUpdate|" + installer);
    return true;
}

/**
 * @brief 即用即断，不要求一直连接，但是，要让 Server 端的管道一直在 connect 状态阻塞住等待连接
 *
 * @param pipeData
 */
bool SendToAuxNamedpipe(const std::wstring &pipeData, bool waitForAcknowledgement)
{
    HANDLE hAuxPipe = INVALID_HANDLE_VALUE;

    // 重试几次，等待 Server 准备好
    for (int retry = 0; retry < 5; ++retry)
    {
        hAuxPipe = CreateFileW(                              //
            FANY_IME_AUX_NAMED_PIPE,                         //
            GENERIC_READ | GENERIC_WRITE,                    //
            0,                                               //
            nullptr,                                         //
            OPEN_EXISTING,                                   //
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, //
            nullptr                                          //
        );
        if (hAuxPipe && hAuxPipe != INVALID_HANDLE_VALUE)
        {
            break;
        }
        /* 为了解决 Windows Media Player 会在启动时调用两次 kill 并且第二次失败的问题
         * TODO: 在 msg wnd proc 中去处理打开程序会调用两次 kill 的问题，将其优化成一次
         */
        Sleep(20); // 等待 20ms 再重试
    }
    if (!hAuxPipe || hAuxPipe == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    DWORD bytesWritten = 0;
    // The payload is a single control message, orders of magnitude below DWORD range.
    const DWORD bytesToWrite = static_cast<DWORD>(pipeData.length() * sizeof(wchar_t));
    BOOL ret = WriteFile( //
        hAuxPipe,         //
        pipeData.c_str(), //
        bytesToWrite,     //
        &bytesWritten,    //
        NULL              //
    );
    const bool sent = ret && bytesWritten == bytesToWrite;
    if (!sent || !waitForAcknowledgement)
    {
        CloseHandle(hAuxPipe);
        return sent;
    }

    DWORD pipeMode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
    if (!SetNamedPipeHandleState(hAuxPipe, &pipeMode, nullptr, nullptr))
    {
        CloseHandle(hAuxPipe);
        return false;
    }

    bool acknowledged = false;
    const ULONGLONG deadline = GetTickCount64() + 150;
    while (GetTickCount64() < deadline)
    {
        wchar_t acknowledgement[2] = {};
        DWORD bytesRead = 0;
        if (ReadFile(hAuxPipe, acknowledgement, sizeof(acknowledgement), &bytesRead, nullptr))
        {
            acknowledged =
                bytesRead == sizeof(acknowledgement) && acknowledgement[0] == L'O' && acknowledgement[1] == L'K';
            break;
        }
        if (GetLastError() != ERROR_NO_DATA)
        {
            break;
        }
        Sleep(1);
    }
    CloseHandle(hAuxPipe);
    return acknowledged;
}

/**
 * event_type
 *   0: FanyImeKeyEvent
 *   1: FanyHideCandidateWndEvent
 *   2: FanyShowCandidateWndEvent
 *   3: FanyMoveCandidateWndEvent
 */
KeyEventSendResult SendKeyEventToUIProcessViaNamedPipe(uint64_t *requestId, unsigned inputStateAction)
{
    if (requestId)
    {
        *requestId = FANY_IME_NO_REQUEST_ID;
    }

    // Lifecycle packets share the thread-local staging buffer with key data.
    // Preserve the fully populated key packet while Ensure... emits the
    // pending Deactivated/Activated handshake; otherwise the first key after
    // a focus or transport reset is silently replaced with an empty packet.
    const FanyImeNamedpipeData keyPacket = namedpipeData;

    // Focus changes and transport replacement are transactional: reverse and
    // worker endpoints, Deactivated reset, and explicit tokenized Activated
    // must all precede the first key.
    if (!EnsureNamedpipeFocusSessionActivated())
    {
        namedpipeData = keyPacket;
        RequestNamedpipeReconnect();
        return KeyEventSendResult::DefinitelyNotSent;
    }
    if (IsLocalSessionResetPending())
    {
        namedpipeData = keyPacket;
        return KeyEventSendResult::DefinitelyNotSent;
    }

    const uint64_t newRequestId = NextProtocolId(nextRequestId);
    namedpipeData = keyPacket;
    namedpipeData.request_id = newRequestId;
    namedpipeData.event_type =
        inputStateAction ? FanyImePipeEventType::InputStateAction : FanyImePipeEventType::KeyEvent;
    if (inputStateAction)
        namedpipeData.keycode = inputStateAction;
    bool deliveryAmbiguous = false;
    if (!SendToNamedpipe(&deliveryAmbiguous))
    {
        return deliveryAmbiguous ? KeyEventSendResult::DeliveryAmbiguous : KeyEventSendResult::DefinitelyNotSent;
    }
    if (requestId)
    {
        *requestId = newRequestId;
    }
    return KeyEventSendResult::Sent;
}

int SendHideCandidateWndEventToUIProcessViaNamedPipe()
{
    namedpipeData = {};
    namedpipeData.event_type = FanyImePipeEventType::HideCandidateWnd;
    SendToNamedpipe();

    return 0;
}

int SendHideCaretStateEventToUIProcessViaNamedPipe()
{
    if (!SupportsCaretStateIndicator())
        return 0;
    namedpipeData = {};
    namedpipeData.event_type = FanyImePipeEventType::HideCaretState;
    SendToNamedpipe();
    return 0;
}

int SendShowCandidateWndEventToUIProcessViaNamedPipe()
{
    // CandidateListUIPresenter stages the text/caret payload immediately
    // before this call. Preserve that payload and overwrite only routing
    // fields for this logical event.
    namedpipeData.request_id = 0;
    namedpipeData.event_type = FanyImePipeEventType::ShowCandidateWnd;
    return SendToNamedpipe() ? 0 : -1;
}

int SendMoveCandidateWndEventToUIProcessViaNamedPipe()
{
    // The caller has already staged the new caret point in namedpipeData.
    namedpipeData.request_id = 0;
    namedpipeData.event_type = FanyImePipeEventType::MoveCandidateWnd;
    return SendToNamedpipe() ? 0 : -1;
}

int SendLangbarRightClickEventToUIProcessViaNamedPipe(const RECT *prcArea)
{
    // Menu is global UI: send on the session-less Aux pipe so a suspended /
    // unfocused TIP can still ask Server to show it. Do not use Main / focus
    // session activation — that path is intentionally down after focus loss.
    if (!prcArea)
    {
        return -1;
    }
    SendToAuxNamedpipe(
        fmt::format(L"LangbarRightClick|{}|{}|{}|{}", prcArea->left, prcArea->top, prcArea->right, prcArea->bottom));
    return 0;
}

int SendIMEActivationEventToUIProcessViaNamedPipe()
{
    SendToAuxNamedpipe(L"IMEActivation");

    return 0;
}

int SendClientActivatedEventToServerViaNamedPipe(uint64_t focusToken)
{
    if (focusToken == 0)
    {
        return -1;
    }
    namedpipeData = {};
    namedpipeData.event_type = FanyImePipeEventType::ClientActivated;
    namedpipeData.request_id = focusToken;
    return SendToNamedpipe() ? 0 : -1;
}

int SendClientDeactivatedEventToServerViaNamedPipe(uint64_t focusToken)
{
    namedpipeData = {};
    namedpipeData.event_type = FanyImePipeEventType::ClientDeactivated;
    namedpipeData.request_id = focusToken;
    return SendToNamedpipe() ? 0 : -1;
}

int SendClientSuspendedEventToServerViaNamedPipe()
{
    namedpipeData = {};
    namedpipeData.event_type = FanyImePipeEventType::ClientSuspended;
    const bool sent = SendToNamedpipe();
    return sent ? 0 : -1;
}

bool FlushNamedpipeFocusSessionReset()
{
    if (!boundFocusResetPending || !*boundFocusResetPending)
    {
        return true;
    }
    if (SendClientSuspendedEventToServerViaNamedPipe() != 0)
    {
        return false;
    }
    *boundFocusResetPending = false;
    return true;
}

bool FlushNamedpipeImeDeactivation(uint64_t focusToken)
{
    if (focusToken == 0 && boundExpectedWorkerFocusToken)
    {
        focusToken = boundExpectedWorkerFocusToken->load(std::memory_order_acquire);
    }
    if (SendClientDeactivatedEventToServerViaNamedPipe(focusToken) != 0)
    {
        // Deactivate is about to destroy the message window and all persistent
        // pipes, so its normal reconnect path cannot repair this final write.
        // Use a PID-validated, focus-token-checked Aux request and wait briefly
        // for the Server to confirm that it fenced the exact old session.
        if (focusToken == 0 ||
            !SendToAuxNamedpipe(fmt::format(L"TerminalDeactivation|{}|{}", GetPipeClientId(), focusToken), true))
        {
            return false;
        }
    }
    if (boundFocusResetPending)
    {
        *boundFocusResetPending = false;
    }
    return true;
}

bool EnsureNamedpipeFocusSessionActivated()
{
    if (IsLocalSessionResetPending())
    {
        return false;
    }
    // Connect reverse channels before Main, then establish lifecycle ordering.
    if (!ConnectToAllNamedpipe())
    {
        return false;
    }
    if (IsLocalSessionResetPending())
    {
        return false;
    }

    // A lifecycle write can itself discover a stale Main handle and mark the
    // session dirty. Iterate a small bounded number of times so the newly
    // generated token is the one ultimately acknowledged by the worker pipe.
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        if (IsLocalSessionResetPending())
        {
            return false;
        }
        if (!FlushNamedpipeFocusSessionReset())
        {
            return false;
        }
        if (IsLocalSessionResetPending())
        {
            return false;
        }
        if (!boundActivationRequired || !*boundActivationRequired)
        {
            if (IsExpectedWorkerFocusAcknowledged())
            {
                return true;
            }
            break;
        }

        // Every activation path ends here, including a TIP that has just been
        // activated in a process started in the background (headless Chromium
        // under Playwright, WebView2 before its window shows) and ordinary
        // background packets. Only the TSF thread that currently owns input
        // focus may replace the Server's active client and composition. Keep
        // pending activation intact until thread focus returns; this is not a
        // failure of the transport. Already acknowledged sessions may still
        // clean up.
        const bool threadFocused = IsNamedpipeThreadFocused();
        QueueNamedpipeFocusGateDiagnostic(threadFocused);
        if (!threadFocused)
        {
            return false;
        }

        const uint64_t token =
            boundExpectedWorkerFocusToken ? boundExpectedWorkerFocusToken->load(std::memory_order_acquire) : 0;
        if (SendClientActivatedEventToServerViaNamedPipe(token) != 0)
        {
            return false;
        }
        if (IsLocalSessionResetPending())
        {
            return false;
        }

        const bool tokenStillCurrent =
            !boundExpectedWorkerFocusToken || boundExpectedWorkerFocusToken->load(std::memory_order_acquire) == token;
        if (tokenStillCurrent && boundActivationRequired)
        {
            *boundActivationRequired = false;
        }
        if ((!boundFocusResetPending || !*boundFocusResetPending) &&
            (!boundActivationRequired || !*boundActivationRequired))
        {
            break;
        }
    }

    // ClientActivated is not complete until the worker channel echoes the
    // exact focus token. This closes the first-key window and rejects stale
    // markers that were buffered before OnKill.
    if (boundWorkerCommitReady)
    {
        const ULONGLONG deadline = GetTickCount64() + 150;
        for (;;)
        {
            if (IsLocalSessionResetPending())
            {
                return false;
            }
            if (IsExpectedWorkerFocusAcknowledged())
            {
                return true;
            }
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline)
            {
                break;
            }
            // Block until the IPC worker records the acknowledgement. Sleep(1)
            // rounds up to a timer tick (~15.6 ms by default), so every focus
            // change paid a tick even though the echo arrives in well under a
            // millisecond. The check above runs after every wake-up and the
            // worker signals after its release stores, so a wake-up cannot be
            // lost; a stale signal only costs one more pass. Like Sleep, this
            // wait does not pump messages, so no TSF callback can re-enter here.
            if (!boundWorkerAckEvent ||
                WaitForSingleObject(boundWorkerAckEvent, static_cast<DWORD>(deadline - now)) == WAIT_FAILED)
            {
                Sleep(1);
            }
        }
        MarkNamedpipeSessionDirty();
    }
    return !boundWorkerCommitReady;
}

/**
 * @brief Send IME status to UI process via named pipe
 *
 * @param kbdIsOpen
 * @param fullwidthIsOpen
 * @param puncIsOpen
 * @return int
 */
int SendIMEStatusEventToUIProcessViaNamedPipe(bool kbdIsOpen, bool fullwidthIsOpen, bool puncIsOpen)
{
    return SendIMEStatusSnapshotToUIProcessViaNamedPipe(kbdIsOpen, fullwidthIsOpen, puncIsOpen);
}

int SendIMEStatusSnapshotToUIProcessViaNamedPipe(bool kbdIsOpen, bool fullwidthIsOpen, bool puncIsOpen,
                                                 bool assertsFocusOwnership)
{
    if (!Global::g_connected)
    {
        return -1;
    }

    namedpipeData = {};
    namedpipeData.event_type =
        assertsFocusOwnership ? FanyImePipeEventType::FocusRestored : FanyImePipeEventType::StatusSnapshot;
    namedpipeData.keycode = kbdIsOpen ? 1u : 0u;
    namedpipeData.modifiers_down = fullwidthIsOpen ? 1u : 0u;
    namedpipeData.pinyin_length = puncIsOpen ? 1 : 0;
    namedpipeData.wch = (GetKeyState(VK_CAPITAL) & 0x0001) != 0 ? 1 : 0;
    const bool sent = SendToNamedpipe();
    return sent ? 0 : -1;
}

int SendIMEDeactivationEventToUIProcessViaNamedPipe()
{
    SendToAuxNamedpipe(L"IMEDeactivation");

    return 0;
}

int SendCaretStateSwitchEventToUIProcessViaNamedPipe(UINT eventType, bool enabled, POINT anchor, UINT trigger,
                                                     bool capsLockEnabled, bool imeOpen)
{
    // The opcodes predate the badge; a Server that did not acknowledge the
    // capability would apply them as toolbar state instead.
    if (!SupportsCaretStateIndicator())
        return 0;
    namedpipeData = {};
    namedpipeData.event_type = eventType;
    namedpipeData.keycode = enabled ? 1u : 0u;
    if (eventType == FanyImePipeEventType::IMESwitch)
    {
        namedpipeData.wch = static_cast<decltype(namedpipeData.wch)>(trigger);
        namedpipeData.modifiers_down = FanyImePipeFlags::EncodeImeSwitchCapsLockSnapshot(capsLockEnabled);
    }
    else if (eventType == FanyImePipeEventType::PuncSwitch)
    {
        namedpipeData.wch = imeOpen ? 1 : 0;
    }
    namedpipeData.point[0] = anchor.x;
    namedpipeData.point[1] = anchor.y;
    SendToNamedpipe();
    return 0;
}
