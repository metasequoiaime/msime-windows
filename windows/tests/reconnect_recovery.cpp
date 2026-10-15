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

    std::cout << "Reconnect recovery and offline-composition guards passed\n";
    return 0;
}
