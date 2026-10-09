// 命名管道服务端：各管道的监听线程、主管道客户端的握手与事件路由，以及关停时唤醒监听。
#include "ipc/event_listener_internal.h"
#include <Windows.h>
#include <namedpipeapi.h>
#include <string>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <iterator>
#include <thread>
#include "ipc.h"
#include "ipc/focus_session_policy.h"
#include "defines/defines.h"
#include "defines/globals.h"
#include "fmt/xchar.h"
#include "global/globals.h"
#include "config/ime_config.h"
#include "window/window_hook.h"
#include "log/candidate_diag_log.h"
#include "statistics/stats_pipe.h"
#include <cwchar>

using namespace event_listener_detail;

namespace
{
constexpr auto kPipeHelloTimeout = std::chrono::seconds(2);

class ScopedPipeClientHandler
{
  public:
    explicit ScopedPipeClientHandler(uint64_t handler_id) : handler_id_(handler_id)
    {
    }

    ~ScopedPipeClientHandler()
    {
        EndPipeClientHandler(handler_id_);
    }

    ScopedPipeClientHandler(const ScopedPipeClientHandler &) = delete;
    ScopedPipeClientHandler &operator=(const ScopedPipeClientHandler &) = delete;

  private:
    uint64_t handler_id_ = 0;
};

bool SetPipeWaitMode(HANDLE pipe, bool wait)
{
    DWORD mode = PIPE_READMODE_MESSAGE | (wait ? PIPE_WAIT : PIPE_NOWAIT);
    return SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) != FALSE;
}

bool ReadExactPipeMessageUntil(HANDLE pipe, void *destination, DWORD destination_size,
                               std::chrono::steady_clock::time_point deadline, DWORD &bytes_read)
{
    bytes_read = 0;
    while (pipe_running && std::chrono::steady_clock::now() < deadline)
    {
        const BOOL result = ReadFile(pipe, destination, destination_size, &bytes_read, nullptr);
        if (result)
        {
            return bytes_read == destination_size;
        }

        const DWORD error = GetLastError();
        if (error != ERROR_NO_DATA)
        {
            return false;
        }
        Sleep(2);
    }

    SetLastError(pipe_running ? ERROR_SEM_TIMEOUT : ERROR_OPERATION_ABORTED);
    return false;
}

void LogPipeConnectResult(const wchar_t *pipe_name, BOOL connected)
{
    const DWORD gle = connected ? ERROR_SUCCESS : GetLastError();
    CAND_DIAG_LOGF(L"pipe connect name={} connected={} gle={}", pipe_name, connected != FALSE, gle);
    if (connected)
    {
        FANY_IPC_LOGF(L"[msime]: [ipc] {} connected", pipe_name);
    }
    else
    {
        FANY_IPC_LOGF(L"[msime]: [ipc] {} ConnectNamedPipe returned false: gle={}", pipe_name, gle);
    }
}

void LogPipeReadFailure(const wchar_t *pipe_name, DWORD bytes_read)
{
    const DWORD gle = GetLastError();
    FANY_IPC_LOGF(L"[msime]: [ipc] {} ReadFile failed or returned empty: gle={}, bytes_read={}", pipe_name, gle,
                  bytes_read);
    CAND_DIAG_LOGF(L"pipe read failure name={} gle={} bytes={}", pipe_name, gle, bytes_read);
}

void LogPipeDisconnect(const wchar_t *pipe_name)
{
    FANY_IPC_LOGF(L"[msime]: [ipc] {} disconnected", pipe_name);
    CAND_DIAG_LOGF(L"pipe disconnected name={}", pipe_name);
}

void LogPipeEvent(const wchar_t *pipe_name, UINT event_type, UINT keycode, WCHAR wch, UINT modifiers_down)
{
    FANY_IPC_LOGF(L"[msime]: [ipc] {} event: type={}, keycode={}, wch={}, modifiers={}", pipe_name, event_type, keycode,
                  static_cast<unsigned int>(wch), modifiers_down);
}

void LogClientLifecycle(const wchar_t *phase, uint64_t client_id, UINT event_type)
{
    FANY_IPC_LOGF(L"[msime]: [ipc] client lifecycle: phase={}, client_id={}, event_type={}", phase, client_id,
                  event_type);
}

void LogClientRouting(uint64_t client_id, UINT event_type, bool is_active)
{
    FANY_IPC_LOGF(L"[msime]: [ipc] client routing: client_id={}, event_type={}, is_active={}", client_id, event_type,
                  is_active);
}

bool IsImplicitActivationEvent(UINT event_type)
{
    // A plain StatusSnapshot or compartment notification can be a delayed
    // background callback and must never steal routing from the focused TSF
    // client. A real key, and a FocusRestored the tip only emits after
    // ITfThreadMgr::IsThreadFocus confirmed ownership, are unambiguous.
    return event_type == FanyImePipeEventType::KeyEvent || event_type == FanyImePipeEventType::FocusRestored;
}

// client_id is PID << 32 | TID of the TSF thread. A match against the
// foreground window's thread, or the thread owning its keyboard focus (a
// child window on an attached thread), means the user is looking at this TIP.
bool IsForegroundInputThreadClient(uint64_t client_id)
{
    const HWND foreground = GetForegroundWindow();
    if (!foreground)
    {
        return false;
    }
    DWORD process_id = 0;
    const DWORD thread_id = GetWindowThreadProcessId(foreground, &process_id);
    if (thread_id == 0 || static_cast<DWORD>(client_id >> 32) != process_id)
    {
        return false;
    }
    const DWORD client_thread_id = static_cast<DWORD>(client_id & 0xFFFFFFFFull);
    if (client_thread_id == thread_id)
    {
        return true;
    }
    GUITHREADINFO info{sizeof(info)};
    if (!GetGUIThreadInfo(thread_id, &info) || !info.hwndFocus)
    {
        return false;
    }
    DWORD focus_process_id = 0;
    const DWORD focus_thread_id = GetWindowThreadProcessId(info.hwndFocus, &focus_process_id);
    return focus_process_id == process_id && focus_thread_id == client_thread_id;
}

void SendFocusSessionReady(const PipeClientActivation &activation)
{
    if (!FanyImeIpc::CanSendFocusSessionReady(activation.client_id, activation.epoch, activation.focus_token))
    {
        return;
    }

    // This packet is an ordered focus-session fence on the same worker
    // endpoint used for candidate commits. The activation request id is a TSF
    // focus token and is echoed verbatim; unlike the Server-only epoch, it lets
    // TSF reject a buffered marker from an older focus session.
    SendToTsfWorkerThreadClientViaNamedpipe(activation.client_id, activation.epoch,
                                            Global::DataFromServerMsgTypeToTsfWorkerThread::FocusSessionReady,
                                            std::to_wstring(activation.focus_token));
}

void SendInputModeState(const PipeClientActivation &activation)
{
    if (activation.client_id == 0 || activation.epoch == 0)
    {
        return;
    }

    // Input candidates use the Server's current session/config, whereas the
    // TSF language-bar icon is cached inside every host process.  Re-send the
    // authoritative mode at focus/activation boundaries so a background host
    // that missed the original broadcast cannot retain a stale Japanese or
    // Chinese icon indefinitely.
    SendToTsfWorkerThreadClientViaNamedpipe(activation.client_id, activation.epoch,
                                            Global::DataFromServerMsgTypeToTsfWorkerThread::InputModeChanged,
                                            GetActiveInputMode() == "japanese" ? L"1" : L"0");
    SendTrilingualCycleState(activation.client_id, activation.epoch);
}

bool IsKnownMainPipeEvent(UINT event_type)
{
    switch (event_type)
    {
    case FanyImePipeEventType::KeyEvent:
    case FanyImePipeEventType::HideCandidateWnd:
    case FanyImePipeEventType::ShowCandidateWnd:
    case FanyImePipeEventType::MoveCandidateWnd:
    // LangbarRightClick is Aux-only (session-less UI); do not accept on Main.
    case FanyImePipeEventType::IMESwitch:
    case FanyImePipeEventType::PuncSwitch:
    case FanyImePipeEventType::DoubleSingleByteSwitch:
    case FanyImePipeEventType::ClientHello:
    case FanyImePipeEventType::ClientActivated:
    case FanyImePipeEventType::ClientDeactivated:
    case FanyImePipeEventType::StatusSnapshot:
    case FanyImePipeEventType::ClientSuspended:
    case FanyImePipeEventType::FocusRestored:
    case FanyImePipeEventType::HideCaretState:
        return true;
    default:
        return false;
    }
}

bool IsValidMainPipeFrame(const FanyImeNamedpipeData &pipe_data)
{
    if (!IsKnownMainPipeEvent(pipe_data.event_type) || pipe_data.pinyin_length < 0 ||
        pipe_data.pinyin_length >= static_cast<int>(std::size(pipe_data.pinyin_string)) ||
        pipe_data.pinyin_string[std::size(pipe_data.pinyin_string) - 1] != L'\0' ||
        pipe_data.pinyin_string[pipe_data.pinyin_length] != L'\0')
    {
        return false;
    }

    if ((pipe_data.event_type == FanyImePipeEventType::StatusSnapshot ||
         pipe_data.event_type == FanyImePipeEventType::FocusRestored) &&
        (pipe_data.keycode > 1 || pipe_data.modifiers_down > 1 || pipe_data.pinyin_length > 1))
    {
        return false;
    }
    if (pipe_data.event_type == FanyImePipeEventType::KeyEvent && pipe_data.request_id == 0)
    {
        return false;
    }
    if (pipe_data.event_type == FanyImePipeEventType::ClientActivated && pipe_data.request_id == 0)
    {
        // FocusSessionReady can never acknowledge token zero. Reject the
        // activation instead of creating a server epoch that TSF cannot fence.
        return false;
    }
    return true;
}

bool WaitForPipeClient(HANDLE pipe)
{
    BOOL connected = ConnectNamedPipe(pipe, NULL);
    if (connected)
    {
        return true;
    }
    return GetLastError() == ERROR_PIPE_CONNECTED;
}

bool PipeClientIdMatchesConnectedProcess(HANDLE pipe, uint64_t client_id)
{
    ULONG client_process_id = 0;
    if (!GetNamedPipeClientProcessId(pipe, &client_process_id))
    {
        // Best effort for older/exceptional hosts. When Windows provides the
        // process identity, however, never accept a spoofed routing id.
        return true;
    }
    return static_cast<DWORD>(client_id >> 32) == static_cast<DWORD>(client_process_id);
}

bool ReadPipeHello(HANDLE pipe, UINT expected_pipe_role, FanyImePipeHello &hello)
{
    if (!SetPipeWaitMode(pipe, false))
    {
        return false;
    }
    DWORD bytesRead = 0;
    const bool readResult = ReadExactPipeMessageUntil(pipe, &hello, sizeof(hello),
                                                      std::chrono::steady_clock::now() + kPipeHelloTimeout, bytesRead);
    return readResult && pipe_running && hello.client_id != 0 && hello.pipe_role == expected_pipe_role &&
           PipeClientIdMatchesConnectedProcess(pipe, hello.client_id);
}

void WakePipeListener(const wchar_t *pipe_name)
{
    for (int retry = 0; retry < 20; ++retry)
    {
        HANDLE wake_pipe = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (wake_pipe && wake_pipe != INVALID_HANDLE_VALUE)
        {
            CloseHandle(wake_pipe);
            return;
        }

        const DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY)
        {
            return;
        }
        WaitNamedPipeW(pipe_name, 10);
    }
}
} // namespace

namespace event_listener_detail
{
void WakeNamedPipeListenersForShutdown()
{
    WakePipeListener(FANY_IME_NAMED_PIPE);
    WakePipeListener(FANY_IME_TO_TSF_NAMED_PIPE);
    WakePipeListener(FANY_IME_TO_TSF_WORKER_THREAD_NAMED_PIPE);
    WakePipeListener(FANY_IME_AUX_NAMED_PIPE);
    WakePipeListener(FANY_IME_TSF_DIAGNOSTIC_NAMED_PIPE);
    WakePipeListener(FANY_IME_VOICE_CONTROL_NAMED_PIPE);
    // The statistics listener blocks in ConnectNamedPipe on a PIPE_WAIT
    // instance just like the others, but its name carries the session id.
    // Without this wake a Server that never saw a statistics client would hang
    // in stats_pipe_listener.join() forever.
    WakePipeListener(MsimeStats::BuildStatsPipeName(MsimeStats::CurrentStatsSessionId()).c_str());
}
} // namespace event_listener_detail

namespace FanyNamedPipe
{
void MainPipeClientThread(HANDLE clientPipe, uint64_t handlerId);
void RegisteredPipeMonitorThread(HANDLE clientPipe, UINT pipeRole, uint64_t handlerId);

void EventListenerLoopThread()
{
    HANDLE listeningPipe = hPipe;
    hPipe = INVALID_HANDLE_VALUE;

    while (pipe_running)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
        {
            listeningPipe = CreateMainNamedPipeInstance();
            if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
            {
                FANY_IPC_LOGF(L"[msime]: [ipc] failed to create next main-pipe instance: gle={}", GetLastError());
                Sleep(50);
                continue;
            }
        }

        BOOL connected = WaitForPipeClient(listeningPipe);
        LogPipeConnectResult(L"main-pipe", connected);
        if (connected)
        {
            if (!pipe_running)
            {
                DisconnectNamedPipe(listeningPipe);
                CloseHandle(listeningPipe);
                listeningPipe = INVALID_HANDLE_VALUE;
                break;
            }
            HANDLE clientPipe = listeningPipe;
            listeningPipe = CreateMainNamedPipeInstance();
            const uint64_t handlerId = BeginPipeClientHandler(clientPipe);
            if (handlerId == 0)
            {
                DisconnectNamedPipe(clientPipe);
                CloseHandle(clientPipe);
            }
            else
            {
                try
                {
                    std::thread(MainPipeClientThread, clientPipe, handlerId).detach();
                }
                catch (...)
                {
                    EndPipeClientHandler(handlerId);
                    DisconnectNamedPipe(clientPipe);
                    CloseHandle(clientPipe);
                }
            }
        }
        else
        {
            CloseHandle(listeningPipe);
            listeningPipe = INVALID_HANDLE_VALUE;
        }
    }

    if (listeningPipe && listeningPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(listeningPipe);
    }
}

void MainPipeClientThread(HANDLE clientPipe, uint64_t handlerId)
{
    ScopedPipeClientHandler handler(handlerId);
    uint64_t clientId = 0;
    uint64_t mainRegistrationId = 0;
    bool helloReceived = false;
    if (!SetPipeWaitMode(clientPipe, false))
    {
        DisconnectNamedPipe(clientPipe);
        CloseHandle(clientPipe);
        return;
    }
    while (pipe_running)
    {
        FanyImeNamedpipeData pipeData = {};
        DWORD bytesRead = 0;
        const BOOL readResult =
            helloReceived ? ReadFile(clientPipe, &pipeData, sizeof(pipeData), &bytesRead, nullptr)
                          : ReadExactPipeMessageUntil(clientPipe, &pipeData, sizeof(pipeData),
                                                      std::chrono::steady_clock::now() + kPipeHelloTimeout, bytesRead);
        if (!readResult || bytesRead != sizeof(pipeData))
        {
            LogPipeReadFailure(L"main-pipe", bytesRead);
            break;
        }
        if (!pipe_running)
        {
            break;
        }
        if (!IsValidMainPipeFrame(pipeData))
        {
            FANY_IPC_LOGF(L"[msime]: [ipc] rejected malformed main-pipe frame: type={}, client_id={}, pinyin_length={}",
                          pipeData.event_type, pipeData.client_id, pipeData.pinyin_length);
            break;
        }

        if (!helloReceived)
        {
            if (pipeData.event_type != FanyImePipeEventType::ClientHello || pipeData.client_id == 0 ||
                !PipeClientIdMatchesConnectedProcess(clientPipe, pipeData.client_id))
            {
                FANY_IPC_LOGF(L"[msime]: [ipc] rejected main pipe without a valid hello: type={}, client_id={}",
                              pipeData.event_type, pipeData.client_id);
                break;
            }
            clientId = pipeData.client_id;
            LogClientLifecycle(L"hello", clientId, pipeData.event_type);
            mainRegistrationId = RegisterMainPipeClient(clientId, clientPipe);
            if (mainRegistrationId == 0)
            {
                break;
            }
            if (!NegotiateMainPipeClient(pipeData, mainRegistrationId))
            {
                break;
            }
            if (!pipe_running || !SetPipeWaitMode(clientPipe, true))
            {
                break;
            }
            helloReceived = true;
            continue;
        }

        if (pipeData.client_id != clientId)
        {
            FANY_IPC_LOGF(L"[msime]: [ipc] rejected client-id change on main pipe: pinned={}, received={}", clientId,
                          pipeData.client_id);
            break;
        }
        if (!IsPipeClientRegistrationCurrent(clientId, FanyImePipeRole::Main, mainRegistrationId))
        {
            break;
        }
        if (pipeData.event_type == FanyImePipeEventType::ClientHello)
        {
            // A repeated hello from the same pinned connection is harmless.
            continue;
        }
        if (pipeData.event_type == FanyImePipeEventType::ClientActivated)
        {
            LogClientLifecycle(L"activated", clientId, pipeData.event_type);
            const PipeClientActivation activation =
                ActivatePipeClient(clientId, mainRegistrationId, true, pipeData.request_id, true);
            // client_id 0 means the reverse pipes never reported ready inside the
            // 100ms budget, so the whole packet is about to be discarded.
            SendFocusSessionReady(activation);
            SendInputModeState(activation);
            if (activation.changed)
            {
                EnqueueTask(TaskType::ClientActivated, pipeData, activation.epoch);
            }
            continue;
        }
        if (FanyImePipeEventType::IsRouteDeactivation(pipeData.event_type))
        {
            const bool terminalDeactivation = FanyImePipeEventType::IsTerminalDeactivation(pipeData.event_type);
            LogClientLifecycle(terminalDeactivation ? L"deactivated" : L"suspended", clientId, pipeData.event_type);
            uint64_t deactivationEpoch = DeactivatePipeClient(clientId, mainRegistrationId);
            if (terminalDeactivation && deactivationEpoch == 0)
            {
                // ClientSuspended may already have put routing into the
                // inactive state. Preserve exact terminal cleanup for that
                // owner; a subsequent activation makes this task stale.
                deactivationEpoch = ResolvePipeClientTerminalDeactivationEpoch(clientId);
            }
            if (terminalDeactivation && deactivationEpoch == 0 && IsForegroundInputThreadClient(clientId))
            {
                // Routing is owned by some other client — one that never
                // suspended (TextInputHost after Win+., the taskbar), or one
                // that suspended after this client did. Ownership alone would
                // drop this event and leave the toolbar up after the user
                // switched input methods in the window they are looking at.
                // A background TIP unloading still cannot hide it.
                deactivationEpoch = DeactivatePipeRouteForForegroundClient(clientId, mainRegistrationId);
                CAND_DIAG_LOGF(L"terminal deactivation accepted from foreground non-owner client={} epoch={}", clientId,
                               deactivationEpoch);
            }
            if (deactivationEpoch != 0)
            {
                EnqueueTask(terminalDeactivation ? TaskType::ClientDeactivated : TaskType::ClientSuspended, pipeData,
                            deactivationEpoch);
            }
            continue;
        }

        PipeClientActivation activation = GetActivePipeClient();
        if (IsImplicitActivationEvent(pipeData.event_type))
        {
            activation = ActivatePipeClient(clientId, mainRegistrationId, false);
            // A real key can be the first observable foreground signal after
            // Win+. returns, before the TSF reconnect timer has replayed its
            // explicit activation. FocusRestored plays the same role when
            // document focus returns to a client that never lost its session
            // and therefore never re-activates. Fence the worker stream before
            // enqueueing the corresponding task. Repeated markers for one
            // epoch are intentional and harmless.
            SendFocusSessionReady(activation);
            if (pipeData.event_type == FanyImePipeEventType::FocusRestored)
            {
                SendInputModeState(activation);
            }
            if (activation.changed)
            {
                EnqueueTask(TaskType::ClientActivated, pipeData, activation.epoch);
            }
        }

        const bool isActiveClient =
            activation.client_id == clientId && activation.epoch != 0 && IsActivePipeClient(clientId, activation.epoch);
        LogClientRouting(clientId, pipeData.event_type, isActiveClient);
        if (!isActiveClient)
        {
            FANY_IPC_LOGF(L"[msime]: [ipc] ignored inactive main-pipe event: client_id={}, type={}", clientId,
                          pipeData.event_type);
            CAND_DIAG_LOGF(L"main-pipe event ignored inactive client={} type={} request={}", clientId,
                           pipeData.event_type, pipeData.request_id);
            continue;
        }

        LogPipeEvent(L"main-pipe", pipeData.event_type, pipeData.keycode, pipeData.wch, pipeData.modifiers_down);
        switch (pipeData.event_type)
        {
        case FanyImePipeEventType::KeyEvent: {
            // 与 worker 侧的 dispatch 探针配对：reader 收到键包即记，两边对照可把丢键
            // 精确到「reader 未收到」还是「worker 未派发」。request_id 是 DLL 侧分配的，
            // 可直接与 [msime][issue47] 的 keydown-sent request 对齐。
            CAND_DIAG_LOGF(L"main-pipe KeyEvent received request={} keycode=0x{:X} wch=U+{:04X}", pipeData.request_id,
                           pipeData.keycode, static_cast<unsigned>(pipeData.wch));
            EnqueueTask(TaskType::ImeKeyEvent, pipeData, activation.epoch);
            break;
        }

        case FanyImePipeEventType::HideCandidateWnd: {
            EnqueueTask(TaskType::HideCandidate, pipeData, activation.epoch);
            break;
        }

        case FanyImePipeEventType::HideCaretState: {
            EnqueueTask(TaskType::HideCaretState, pipeData, activation.epoch);
            break;
        }

        case FanyImePipeEventType::ShowCandidateWnd: {
            EnqueueTask(TaskType::ShowCandidate, pipeData, activation.epoch);
            break;
        }

        case FanyImePipeEventType::MoveCandidateWnd: {
            EnqueueTask(TaskType::MoveCandidate, pipeData, activation.epoch);
            break;
        }

        case FanyImePipeEventType::IMESwitch: {
            EnqueueTask(TaskType::IMESwitch, pipeData, activation.epoch);
            break;
        }

        case FanyImePipeEventType::PuncSwitch: {
            EnqueueTask(TaskType::PuncSwitch, pipeData, activation.epoch);
            break;
        }

        case FanyImePipeEventType::DoubleSingleByteSwitch: {
            EnqueueTask(TaskType::DoubleSingleByteSwitch, pipeData, activation.epoch);
            break;
        }

        case FanyImePipeEventType::StatusSnapshot:
        case FanyImePipeEventType::FocusRestored: {
            // The ownership claim was already consumed above; the payload is
            // identical, so both feed the same toolbar update.
            EnqueueTask(TaskType::StatusSnapshot, pipeData, activation.epoch);
            // A plain StatusSnapshot is also emitted by OnSetThreadFocus in
            // hosts that do not produce a document-focus transition.  Replying
            // here makes that path converge too.  FocusRestored was already
            // answered above, so avoid a duplicate worker frame.
            if (pipeData.event_type == FanyImePipeEventType::StatusSnapshot)
            {
                SendInputModeState(activation);
            }
            break;
        }
        }
    }

    const PipeClientUnregisterResult unregisterResult =
        UnregisterPipeClientHandle(clientId, FanyImePipeRole::Main, clientPipe, mainRegistrationId);
    if (unregisterResult.removed)
    {
        ForgetClientStatusSnapshot(clientId);
    }
    uint64_t disconnectEpoch = unregisterResult.deactivation_epoch;
    if (disconnectEpoch == 0 && unregisterResult.removed)
    {
        // A process can disconnect its Main pipe after it suspended the route.
        // Reuse only that owner's inactive epoch; a replacement Main that has
        // already activated makes this terminal cleanup stale.
        disconnectEpoch = ResolvePipeClientTerminalDeactivationEpoch(clientId);
    }
    if (disconnectEpoch != 0)
    {
        FanyImeNamedpipeData disconnectData = {};
        disconnectData.event_type = FanyImePipeEventType::ClientSuspended;
        disconnectData.client_id = clientId;
        EnqueueTask(TaskType::ClientSuspended, disconnectData, disconnectEpoch);
    }
    LogPipeDisconnect(L"main-pipe");
    DisconnectNamedPipe(clientPipe);
    CloseHandle(clientPipe);
}

void ToTsfPipeEventListenerLoopThread()
{
    HANDLE listeningPipe = hToTsfPipe;
    hToTsfPipe = INVALID_HANDLE_VALUE;
    while (pipe_running)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
        {
            listeningPipe = CreateToTsfNamedPipeInstance();
            if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
            {
                FANY_IPC_LOGF(L"[msime]: [ipc] failed to create next to-tsf-pipe instance: gle={}", GetLastError());
                Sleep(50);
                continue;
            }
        }

        BOOL connected = WaitForPipeClient(listeningPipe);
#ifdef FANY_DEBUG
        (void)0;
#endif
        LogPipeConnectResult(L"to-tsf-pipe", connected);
        if (connected)
        {
            if (!pipe_running)
            {
                DisconnectNamedPipe(listeningPipe);
                CloseHandle(listeningPipe);
                listeningPipe = INVALID_HANDLE_VALUE;
                break;
            }
            HANDLE clientPipe = listeningPipe;
            listeningPipe = CreateToTsfNamedPipeInstance();
            const uint64_t handlerId = BeginPipeClientHandler(clientPipe);
            if (handlerId == 0)
            {
                DisconnectNamedPipe(clientPipe);
                CloseHandle(clientPipe);
            }
            else
            {
                try
                {
                    std::thread(RegisteredPipeMonitorThread, clientPipe, FanyImePipeRole::ToTsf, handlerId).detach();
                }
                catch (...)
                {
                    EndPipeClientHandler(handlerId);
                    DisconnectNamedPipe(clientPipe);
                    CloseHandle(clientPipe);
                }
            }
        }
        else
        {
            CloseHandle(listeningPipe);
            listeningPipe = INVALID_HANDLE_VALUE;
        }
    }

    if (listeningPipe && listeningPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(listeningPipe);
    }
}

void ToTsfWorkerThreadPipeEventListenerLoopThread()
{
    HANDLE listeningPipe = hToTsfWorkerThreadPipe;
    hToTsfWorkerThreadPipe = INVALID_HANDLE_VALUE;
    while (pipe_running)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
        {
            listeningPipe = CreateToTsfWorkerThreadNamedPipeInstance();
            if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
            {
                FANY_IPC_LOGF(L"[msime]: [ipc] failed to create next to-tsf-worker-pipe instance: gle={}",
                              GetLastError());
                Sleep(50);
                continue;
            }
        }

        BOOL connected = WaitForPipeClient(listeningPipe);
        LogPipeConnectResult(L"to-tsf-worker-pipe", connected);
        if (connected)
        {
#ifdef FANY_DEBUG
            (void)0;
#endif
            if (!pipe_running)
            {
                DisconnectNamedPipe(listeningPipe);
                CloseHandle(listeningPipe);
                listeningPipe = INVALID_HANDLE_VALUE;
                break;
            }
            HANDLE clientPipe = listeningPipe;
            listeningPipe = CreateToTsfWorkerThreadNamedPipeInstance();
            const uint64_t handlerId = BeginPipeClientHandler(clientPipe);
            if (handlerId == 0)
            {
                DisconnectNamedPipe(clientPipe);
                CloseHandle(clientPipe);
            }
            else
            {
                try
                {
                    std::thread(RegisteredPipeMonitorThread, clientPipe, FanyImePipeRole::ToTsfWorkerThread, handlerId)
                        .detach();
                }
                catch (...)
                {
                    EndPipeClientHandler(handlerId);
                    DisconnectNamedPipe(clientPipe);
                    CloseHandle(clientPipe);
                }
            }
        }
        else
        {
            CloseHandle(listeningPipe);
            listeningPipe = INVALID_HANDLE_VALUE;
        }
    }

    if (listeningPipe && listeningPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(listeningPipe);
    }
}

void RegisteredPipeMonitorThread(HANDLE clientPipe, UINT pipeRole, uint64_t handlerId)
{
    ScopedPipeClientHandler handler(handlerId);
    FanyImePipeHello hello = {};
    if (!ReadPipeHello(clientPipe, pipeRole, hello))
    {
        LogPipeReadFailure(pipeRole == FanyImePipeRole::ToTsf ? L"to-tsf-pipe" : L"to-tsf-worker-pipe", 0);
        DisconnectNamedPipe(clientPipe);
        CloseHandle(clientPipe);
        return;
    }

    HANDLE monitorPipe = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), clientPipe, GetCurrentProcess(), &monitorPipe, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
    {
        monitorPipe = INVALID_HANDLE_VALUE;
    }

    uint64_t registrationId = 0;
    if (pipeRole == FanyImePipeRole::ToTsf)
    {
        registrationId = RegisterToTsfPipeClient(hello.client_id, clientPipe);
    }
    else if (pipeRole == FanyImePipeRole::ToTsfWorkerThread)
    {
        registrationId = RegisterToTsfWorkerThreadPipeClient(hello.client_id, clientPipe);
        if (registrationId != 0)
        {
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::PagingCommaPeriodChanged,
                FormatPagingCommaPeriodWorkerPayload());
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationChanged,
                GetConfiguredSmartPunctuationEnabled() ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationSpaceConvertChanged,
                GetConfiguredSmartPunctuationSpaceConvertEnabled() ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationDirectDigitChanged,
                GetConfiguredSmartPunctuationDirectDigitEnabled() ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationDirectLetterChanged,
                GetConfiguredSmartPunctuationDirectLetterEnabled() ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationRepeatToChineseChanged,
                GetConfiguredSmartPunctuationRepeatToChineseEnabled() ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::PairedPunctuationChanged,
                GetConfiguredPairedPunctuationEnabled() ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::MicrosoftShuangpinChanged,
                IsConfiguredShuangpinSemicolonFinal() ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::MidSentenceHelpcodeChanged,
                FormatMidSentenceHelpcodeWorkerPayload());
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::MidSentenceHelpcodeSemicolonChanged,
                FormatMidSentenceHelpcodeSemicolonWorkerPayload());
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::DirectHelpcodeChanged,
                FormatDirectHelpcodeWorkerPayload());
            SendToTsfWorkerThreadClientViaNamedpipe(hello.client_id,
                                                    Global::DataFromServerMsgTypeToTsfWorkerThread::VModeChanged,
                                                    FormatVModeWorkerPayload());
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::MidSentenceHelpcodeUppercaseChanged,
                FormatMidSentenceHelpcodeUppercaseWorkerPayload());
            SendToTsfWorkerThreadClientViaNamedpipe(hello.client_id,
                                                    Global::DataFromServerMsgTypeToTsfWorkerThread::InputModeChanged,
                                                    GetActiveInputMode() == "japanese" ? L"1" : L"0");
            SendTrilingualCycleState(hello.client_id);
            SendToTsfWorkerThreadClientViaNamedpipe(hello.client_id,
                                                    Global::DataFromServerMsgTypeToTsfWorkerThread::CapsLockChanged,
                                                    GetServerCapsLockState() != 0 ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::TsfDiagnosticLogChanged,
                GetConfiguredTsfDiagnosticLogEnabled() ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::PunctuationLockChanged,
                FormatPunctuationLockWorkerPayload());
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::StatisticsEnabledChanged,
                GetConfiguredStatisticsEnabled() ? L"1" : L"0");
        }
    }

    if (registrationId == 0)
    {
        if (monitorPipe && monitorPipe != INVALID_HANDLE_VALUE)
        {
            CloseHandle(monitorPipe);
        }
        DisconnectNamedPipe(clientPipe);
        CloseHandle(clientPipe);
        return;
    }

    if (!monitorPipe || monitorPipe == INVALID_HANDLE_VALUE)
    {
        const PipeClientUnregisterResult result =
            UnregisterPipeClientHandle(hello.client_id, pipeRole, clientPipe, registrationId);
        EnqueuePipeSessionInvalidatedTask(hello.client_id, result.deactivation_epoch);
        return;
    }

    const wchar_t *pipeName = pipeRole == FanyImePipeRole::ToTsf ? L"to-tsf-pipe" : L"to-tsf-worker-pipe";
    while (pipe_running && IsPipeClientRegistrationCurrent(hello.client_id, pipeRole, registrationId))
    {
        DWORD bytesAvailable = 0;
        if (!PeekNamedPipe(monitorPipe, nullptr, 0, nullptr, &bytesAvailable, nullptr))
        {
            LogPipeReadFailure(pipeName, 0);
            break;
        }
        Sleep(20);
    }

    const PipeClientUnregisterResult result =
        UnregisterPipeClientHandle(hello.client_id, pipeRole, clientPipe, registrationId);
    EnqueuePipeSessionInvalidatedTask(hello.client_id, result.deactivation_epoch);
    LogPipeDisconnect(pipeName);
    CloseHandle(monitorPipe);
}

void AuxPipeEventListenerLoopThread()
{
    HANDLE listeningPipe = hAuxPipe;
    hAuxPipe = INVALID_HANDLE_VALUE;
    while (pipe_running)
    {
        if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
        {
            listeningPipe = CreateAuxNamedPipeInstance();
            if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
            {
                Sleep(50);
                continue;
            }
        }

        const BOOL connected = WaitForPipeClient(listeningPipe);
        LogPipeConnectResult(L"aux-pipe", connected);
        if (connected)
        {
            if (!pipe_running)
            {
                DisconnectNamedPipe(listeningPipe);
                break;
            }

            wchar_t buffer[128] = {0};
            DWORD bytesRead = 0;
            BOOL readResult = FALSE;
            DWORD pipeMode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
            if (SetNamedPipeHandleState(listeningPipe, &pipeMode, nullptr, nullptr))
            {
                // Polling a NOWAIT server handle keeps shutdown bounded even if a client connects and never sends its
                // auxiliary message. The deadline is what frees the single Aux instance in that case: without it the
                // poll spins forever, the loop never returns to ConnectNamedPipe, and no later client
                // (LangbarRightClick, TerminalDeactivation) is ever accepted.
                const auto deadline = std::chrono::steady_clock::now() + kPipeHelloTimeout;
                while (pipe_running && std::chrono::steady_clock::now() < deadline)
                {
                    readResult = ReadFile(listeningPipe, buffer, sizeof(buffer), &bytesRead, nullptr);
                    if (readResult || GetLastError() != ERROR_NO_DATA)
                    {
                        break;
                    }
                    Sleep(1);
                }
            }
            if (!readResult || bytesRead == 0) // Disconnected or error
            {
                LogPipeReadFailure(L"aux-pipe", bytesRead);
            }
            else
            {
                std::wstring message(buffer, bytesRead / sizeof(wchar_t));
                FANY_IPC_LOGF(L"[msime]: [ipc] aux-pipe message: {}", message);

                // Aux normally carries session-less UI notifications. Terminal
                // deactivation is the one lifecycle exception: it is a bounded,
                // token-checked fallback for a failed Main-pipe teardown write.
                int left = 0;
                int top = 0;
                int right = 0;
                int bottom = 0;
                if (swscanf_s(message.c_str(), L"LangbarRightClick|%d|%d|%d|%d", &left, &top, &right, &bottom) == 4)
                {
                    FanyImeNamedpipeData pipeData = {};
                    pipeData.event_type = FanyImePipeEventType::LangbarRightClick;
                    pipeData.point[0] = left;
                    pipeData.point[1] = top;
                    pipeData.keycode = static_cast<UINT>(right);
                    pipeData.modifiers_down = static_cast<UINT>(bottom);
                    // client_id/epoch stay 0 so WorkerThread skips active-client
                    // gating and never activates a suspended TIP for a menu click.
                    EnqueueTask(TaskType::LangbarRightClick, pipeData, 0);
                }
                else if (message == L"RestartServer")
                {
                    // Settings runs in its own process, so the restart it offers for
                    // backend changes has to travel the same cross-integrity Aux path
                    // the config notifications use. The Watchdog relaunches us.
                    RestartServerProcess();
                }
                else if (message == L"ConfigChanged" || message == L"InputSchemeChanged" ||
                         message == L"CandidateSkinRefresh")
                {
                    const UINT configMessage =
                        message == L"InputSchemeChanged" ? WM_APPLY_IME_INPUT_SCHEME : WM_APPLY_IME_CONFIG;
                    const WPARAM configWParam = message == L"CandidateSkinRefresh" ? 1 : 0;
                    const HWND candidateWindow = ::global_hwnd;
                    if (candidateWindow && IsWindow(candidateWindow))
                    {
                        PostMessageW(candidateWindow, configMessage, configWParam, 0);
                    }
                    else
                    {
                        // The next startup load reads the already-persisted
                        // config. Explicit invalidation also prevents an early
                        // cached timestamp from suppressing that convergence.
                        InvalidateImeConfigWriteTime();
                    }
                }
                else
                {
                    unsigned long long clientId = 0;
                    unsigned long long focusToken = 0;
                    if (swscanf_s(message.c_str(), L"TerminalDeactivation|%llu|%llu", &clientId, &focusToken) == 2 &&
                        PipeClientIdMatchesConnectedProcess(listeningPipe, static_cast<uint64_t>(clientId)))
                    {
                        const uint64_t deactivationEpoch = DeactivatePipeClientByFocusToken(
                            static_cast<uint64_t>(clientId), static_cast<uint64_t>(focusToken));
                        if (deactivationEpoch != 0)
                        {
                            FanyImeNamedpipeData pipeData = {};
                            pipeData.event_type = FanyImePipeEventType::ClientDeactivated;
                            pipeData.client_id = static_cast<uint64_t>(clientId);
                            EnqueueTask(TaskType::ClientDeactivated, pipeData, deactivationEpoch);

                            constexpr wchar_t acknowledgement[] = L"OK";
                            DWORD bytesWritten = 0;
                            WriteFile(listeningPipe, acknowledgement, sizeof(acknowledgement) - sizeof(wchar_t),
                                      &bytesWritten, nullptr);
                        }
                    }
                }
            }
        }
        else
        {
            if (pipe_running)
            {
                Sleep(10);
            }
        }
        LogPipeDisconnect(L"aux-pipe");
        DisconnectNamedPipe(listeningPipe);
        CloseHandle(listeningPipe);
        listeningPipe = INVALID_HANDLE_VALUE;
    }
    if (listeningPipe && listeningPipe != INVALID_HANDLE_VALUE)
    {
        DisconnectNamedPipe(listeningPipe);
        CloseHandle(listeningPipe);
    }
}

void TsfDiagnosticPipeEventListenerLoopThread()
{
    HANDLE listeningPipe = hTsfDiagnosticPipe;
    hTsfDiagnosticPipe = INVALID_HANDLE_VALUE;
    while (pipe_running)
    {
        if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
        {
            listeningPipe = CreateTsfDiagnosticNamedPipeInstance();
            if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
            {
                Sleep(50);
                continue;
            }
        }

        const BOOL connected = WaitForPipeClient(listeningPipe);
        if (connected && pipe_running)
        {
            std::vector<unsigned char> frame(FANY_IME_TSF_DIAGNOSTIC_MAX_FRAME_BYTES);
            DWORD bytesRead = 0;
            BOOL readResult = FALSE;
            DWORD pipeMode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
            if (SetNamedPipeHandleState(listeningPipe, &pipeMode, nullptr, nullptr))
            {
                // Same bound as the Aux pipe: a client that connects without ever writing must not hold the single
                // diagnostic instance for the process lifetime.
                const auto deadline = std::chrono::steady_clock::now() + kPipeHelloTimeout;
                while (pipe_running && std::chrono::steady_clock::now() < deadline)
                {
                    readResult =
                        ReadFile(listeningPipe, frame.data(), static_cast<DWORD>(frame.size()), &bytesRead, nullptr);
                    if (readResult || GetLastError() != ERROR_NO_DATA)
                    {
                        break;
                    }
                    Sleep(1);
                }
            }

            FanyImeTsfDiagnosticBatchHeader header{};
            if (readResult && bytesRead >= sizeof(header))
            {
                memcpy(&header, frame.data(), sizeof(header));
                ULONG clientProcessId = 0;
                const bool clientMatches = GetNamedPipeClientProcessId(listeningPipe, &clientProcessId) &&
                                           clientProcessId == header.source_process_id;
                const bool frameValid = clientMatches && header.magic == FANY_IME_TSF_DIAGNOSTIC_MAGIC &&
                                        header.version == FANY_IME_TSF_DIAGNOSTIC_VERSION &&
                                        header.header_size == sizeof(header) && header.record_count != 0 &&
                                        header.payload_bytes != 0 && (header.payload_bytes % sizeof(wchar_t)) == 0 &&
                                        sizeof(header) + header.payload_bytes == bytesRead;
                if (frameValid && GetConfiguredTsfDiagnosticLogEnabled())
                {
                    std::wstring payload(header.payload_bytes / sizeof(wchar_t), L'\0');
                    memcpy(payload.data(), frame.data() + sizeof(header), header.payload_bytes);
                    if (header.dropped_count != 0)
                    {
                        DiagnosticLog::Write(fmt::format(L"[tsf-log] source_pid={} dropped_records={}",
                                                         header.source_process_id, header.dropped_count));
                    }
                    size_t start = 0;
                    while (start < payload.size())
                    {
                        const size_t end = payload.find(L'\n', start);
                        const size_t length = end == std::wstring::npos ? payload.size() - start : end - start;
                        if (length != 0)
                        {
                            DiagnosticLog::Write(payload.substr(start, length));
                        }
                        if (end == std::wstring::npos)
                        {
                            break;
                        }
                        start = end + 1;
                    }
                }
            }
        }

        if (listeningPipe && listeningPipe != INVALID_HANDLE_VALUE)
        {
            DisconnectNamedPipe(listeningPipe);
            CloseHandle(listeningPipe);
            listeningPipe = INVALID_HANDLE_VALUE;
        }
    }
}
} // namespace FanyNamedPipe
