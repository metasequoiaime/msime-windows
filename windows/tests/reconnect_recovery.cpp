#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>

using UINT = unsigned int;
using HWND = int;
using std::min;

constexpr UINT CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS = 50;
constexpr UINT CONNECT_NAMEDPIPE_MAX_RETRY_INTERVAL_MS = 2000;
constexpr UINT TIMER_CONNECT_ALL_NAMEDPIPE = 1;
constexpr UINT WM_IpcReconnect = 2;

#define SUCCEEDED(result) ((result) >= 0)

struct ITfDocumentMgr
{
    void Release()
    {
    }
};

struct FakeThreadMgr
{
    bool hasDocument = true;
    ITfDocumentMgr document;
    int GetFocus(ITfDocumentMgr **focused)
    {
        *focused = hasDocument ? &document : nullptr;
        return 0;
    }
};

namespace Global
{
bool g_connected = true;
}

namespace
{
bool transportReady = false;
int handshakeAttempts = 0;
int stoppedTimers = 0;
int armedTimers = 0;
bool threadFocused = true;
bool ownsFocusState = true;
int reconnectMessages = 0;
int activationRequests = 0;

bool IsNamedpipeThreadFocused()
{
    return threadFocused;
}

bool IsNamedpipeFocusStateOwner(const void *)
{
    return ownsFocusState;
}

void RequireNamedpipeFocusActivation()
{
    ++activationRequests;
}

void PostMessage(HWND, UINT message, int, int)
{
    if (message == WM_IpcReconnect)
        ++reconnectMessages;
}

bool EnsureNamedpipeFocusSessionActivated()
{
    ++handshakeAttempts;
    return transportReady;
}

void KillTimer(HWND, UINT)
{
    ++stoppedTimers;
}

void SetTimer(HWND, UINT, UINT, void *)
{
    ++armedTimers;
}
} // namespace

class CMetasequoiaIME
{
  public:
    bool _serverUnavailableFallbackActive = true;
    bool _focusLostToWindowsTextInputHost = false;
    bool composing = false;
    bool releaseReset = false;
    std::atomic<uint64_t> _expectedWorkerFocusToken{42};
    std::atomic<uint64_t> _acknowledgedWorkerFocusToken{42};
    std::atomic<bool> _workerCommitReady{true};
    std::atomic<bool> _localSessionResetPending{false};
    std::atomic<UINT> _localSessionResetToken{7};
    UINT _ipcReconnectDelayMs = 100;
    UINT _ipcConsecutiveFailures = 4;
    int snapshots = 0;
    int drains = 0;
    bool fallbackAtSnapshot = true;
    bool fallbackAtDrain = true;
    FakeThreadMgr threadMgr;

    FakeThreadMgr *_GetThreadMgr()
    {
        return &threadMgr;
    }

    void _RefreshLanguageBarThemeIcons()
    {
    }
    void _ClearSmartPunctuationAction()
    {
    }

    bool _IsComposing()
    {
        return composing;
    }

    void _RequestLocalSessionReset(void *, UINT)
    {
        if (releaseReset)
        {
            _localSessionResetPending.store(false);
            composing = false;
        }
    }

    void _TryLeaveServerUnavailableFallback();

    void _ScheduleDeferredKeyDownDrain()
    {
        ++drains;
        fallbackAtDrain = _serverUnavailableFallbackActive;
    }
};

void SendCurrentImeStatusSnapshot(CMetasequoiaIME *service)
{
    ++service->snapshots;
    service->fallbackAtSnapshot = service->_serverUnavailableFallbackActive;
}

// The generator supplies the actual timer/message handlers and fallback transition.
#include "reconnect_recovery_source.h"

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::cerr << "Failed at line " << __LINE__ << ": " << #condition << '\n';                                  \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (false)

int main()
{
    // A key arrived while the Server was restarting. The immediate attempt failed,
    // leaving raw fallback active until the background timer completes the handshake.
    CMetasequoiaIME delayed;
    RunReconnectMessage(delayed);
    CHECK(delayed._serverUnavailableFallbackActive);
    CHECK(delayed.snapshots == 0 && delayed.drains == 0 && armedTimers == 1);
    transportReady = true;
    RunReconnectTimer(delayed);
    CHECK(!delayed._serverUnavailableFallbackActive);
    CHECK(!delayed.fallbackAtSnapshot && !delayed.fallbackAtDrain);
    CHECK(delayed.snapshots == 1 && delayed.drains == 1 && stoppedTimers == 1);
    CHECK(delayed._ipcReconnectDelayMs == CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS);
    CHECK(delayed._ipcConsecutiveFailures == 0);

    // Immediate and delayed success must resume the same input lane.
    CMetasequoiaIME immediate;
    RunReconnectMessage(immediate);
    CHECK(!immediate._serverUnavailableFallbackActive);
    CHECK(!immediate.fallbackAtSnapshot && !immediate.fallbackAtDrain);

    // An offline composition owns its raw text until it has been finalized.
    CMetasequoiaIME composing;
    composing.composing = true;
    RunReconnectTimer(composing);
    CHECK(composing._serverUnavailableFallbackActive && composing.fallbackAtDrain);
    composing.composing = false;
    composing._TryLeaveServerUnavailableFallback();
    CHECK(!composing._serverUnavailableFallbackActive);

    // Invalid or stale worker acknowledgements must not release raw fallback.
    for (int invalid = 0; invalid < 3; ++invalid)
    {
        CMetasequoiaIME unacknowledged;
        if (invalid == 0)
            unacknowledged._expectedWorkerFocusToken.store(0);
        else if (invalid == 1)
            unacknowledged._acknowledgedWorkerFocusToken.store(41);
        else
            unacknowledged._workerCommitReady.store(false);
        unacknowledged._TryLeaveServerUnavailableFallback();
        CHECK(unacknowledged._serverUnavailableFallbackActive);
    }

    // A local reset still waiting for its edit session must block the handshake.
    CMetasequoiaIME resetting;
    resetting._localSessionResetPending.store(true);
    const int attemptsBeforeReset = handshakeAttempts;
    RunReconnectTimer(resetting);
    CHECK(resetting._serverUnavailableFallbackActive);
    CHECK(resetting.snapshots == 0 && resetting.drains == 0);
    CHECK(handshakeAttempts == attemptsBeforeReset);
    resetting.releaseReset = true;
    RunReconnectTimer(resetting);
    CHECK(!resetting._serverUnavailableFallbackActive && !resetting.fallbackAtDrain);

    // A failed handshake, or focus that moved away, cannot restore Server input.
    transportReady = false;
    CMetasequoiaIME unavailable;
    RunReconnectTimer(unavailable);
    CHECK(unavailable._serverUnavailableFallbackActive);
    CHECK(unavailable.snapshots == 0 && unavailable.drains == 0);
    transportReady = true;
    Global::g_connected = false;
    CMetasequoiaIME unfocused;
    const int attemptsBeforeFocusLoss = handshakeAttempts;
    RunReconnectTimer(unfocused);
    CHECK(unfocused._serverUnavailableFallbackActive);
    CHECK(unfocused.snapshots == 0 && unfocused.drains == 0);
    CHECK(handshakeAttempts == attemptsBeforeFocusLoss);

    // Document focus can remain set on a background thread. Neither a queued
    // reconnect message nor a retry timer may activate that thread's session.
    Global::g_connected = true;
    threadFocused = false;
    CMetasequoiaIME background;
    const int timersBeforeBackground = armedTimers;
    RunReconnectMessage(background);
    RunReconnectTimer(background);
    CHECK(handshakeAttempts == attemptsBeforeFocusLoss);
    CHECK(armedTimers == timersBeforeBackground);
    CHECK(background.snapshots == 0 && background.drains == 0);

    // A delayed thread-focus message cannot claim input focus after it moved on.
    Global::g_connected = false;
    RunThreadFocusMessage(background);
    CHECK(!Global::g_connected && reconnectMessages == 0 && activationRequests == 0);

    // A background document's first real thread-focus return need not produce
    // another document notification. It resumes activation without a keypress.
    threadFocused = true;
    ownsFocusState = false;
    RunThreadFocusMessage(background);
    CHECK(!Global::g_connected && reconnectMessages == 0 && activationRequests == 0);
    ownsFocusState = true;
    RunThreadFocusMessage(background);
    CHECK(Global::g_connected && reconnectMessages == 1 && activationRequests == 1);
    RunReconnectMessage(background);
    CHECK(handshakeAttempts == attemptsBeforeFocusLoss + 1);
    CHECK(!background._serverUnavailableFallbackActive);

    // Returning with an existing session resumes retries without rotating its
    // activation token. A non-editable/no-document return starts no session.
    RunThreadFocusMessage(background);
    CHECK(reconnectMessages == 2 && activationRequests == 1);
    Global::g_connected = false;
    background.threadMgr.hasDocument = false;
    RunThreadFocusMessage(background);
    CHECK(!Global::g_connected && reconnectMessages == 2 && activationRequests == 1);

    // TextInputHost owns a separate client. A return with no repeated document
    // notification still needs a fresh activation even if our pipes are live.
    Global::g_connected = true;
    background.threadMgr.hasDocument = true;
    background._focusLostToWindowsTextInputHost = true;
    RunThreadFocusMessage(background);
    CHECK(reconnectMessages == 3 && activationRequests == 2);
    CHECK(!background._focusLostToWindowsTextInputHost);

    std::cout << "Reconnect recovery and offline-composition guards passed\n";
    return 0;
}
