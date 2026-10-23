#include <atomic>
#include <cstdint>
#include <iostream>

using BOOL = int;
using DWORD = unsigned long;
using ULONGLONG = unsigned long long;
constexpr BOOL FALSE = 0;
constexpr DWORD WAIT_FAILED = 0xffffffff;
#define SUCCEEDED(result) ((result) >= 0)

struct ITfThreadMgr
{
    bool focused = false;
    bool querySucceeds = true;
    int IsThreadFocus(BOOL *value)
    {
        *value = focused;
        return querySucceeds ? 0 : -1;
    }
};

struct CMetasequoiaIME
{
    ITfThreadMgr threadMgr;
    ITfThreadMgr *_GetThreadMgr()
    {
        return &threadMgr;
    }
};

CMetasequoiaIME *boundFocusStateOwner = nullptr;
bool activationRequired = true;
bool focusResetPending = false;
bool acknowledged = false;
bool loseFocusWhileConnecting = false;
bool resetPending = false;
bool transportReady = true;
bool *boundActivationRequired = &activationRequired;
bool *boundFocusResetPending = &focusResetPending;
std::atomic<uint64_t> expectedToken{42};
std::atomic<uint64_t> *boundExpectedWorkerFocusToken = &expectedToken;
std::atomic<bool> workerReady{false};
std::atomic<bool> *boundWorkerCommitReady = &workerReady;
int boundWorkerAckEvent = 1;
int activationWrites = 0;
int dirtyResets = 0;
int gateDiagnostics = 0;
uint64_t sentToken = 0;
bool win32Foreground = false;

bool IsCurrentThreadForegroundInput()
{
    return win32Foreground;
}

void QueueNamedpipeFocusGateDiagnostic(bool)
{
    ++gateDiagnostics;
}

bool IsLocalSessionResetPending()
{
    return resetPending;
}

bool ConnectToAllNamedpipe()
{
    if (loseFocusWhileConnecting && boundFocusStateOwner)
        boundFocusStateOwner->threadMgr.focused = false;
    return transportReady;
}

bool FlushNamedpipeFocusSessionReset()
{
    return true;
}

bool IsExpectedWorkerFocusAcknowledged()
{
    return acknowledged;
}

int SendClientActivatedEventToServerViaNamedPipe(uint64_t token)
{
    ++activationWrites;
    sentToken = token;
    acknowledged = true;
    return 0;
}

ULONGLONG GetTickCount64()
{
    static ULONGLONG ticks = 0;
    return ++ticks;
}

DWORD WaitForSingleObject(int, DWORD)
{
    return 0;
}

void Sleep(int)
{
}

void MarkNamedpipeSessionDirty()
{
    ++dirtyResets;
}

#include "focus_activation_source.h"

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
    // A pending activation is never authorized by an unbound/background owner,
    // or a failed TSF focus query outside the Win32 foreground. Deferral keeps
    // its token and transport.
    CHECK(!EnsureNamedpipeFocusSessionActivated());
    CMetasequoiaIME service;
    boundFocusStateOwner = &service;
    CHECK(!EnsureNamedpipeFocusSessionActivated());
    service.threadMgr.focused = true;
    service.threadMgr.querySucceeds = false;
    CHECK(!EnsureNamedpipeFocusSessionActivated());
    CHECK(activationWrites == 0 && activationRequired && expectedToken.load() == 42 && dirtyResets == 0);

    // Test actual focus at the write boundary, after a connection has waited.
    service.threadMgr.querySucceeds = true;
    loseFocusWhileConnecting = true;
    CHECK(!EnsureNamedpipeFocusSessionActivated());
    CHECK(activationWrites == 0 && activationRequired && dirtyResets == 0);
    loseFocusWhileConnecting = false;

    // Focus return completes the pending activation exactly once.
    service.threadMgr.focused = true;
    CHECK(EnsureNamedpipeFocusSessionActivated());
    CHECK(activationWrites == 1 && sentToken == 42 && !activationRequired && acknowledged);
    CHECK(EnsureNamedpipeFocusSessionActivated());
    CHECK(activationWrites == 1);

    // Losing focus alone does not turn cleanup of an acknowledged session into
    // transport failure. A new token, however, must wait for foreground focus.
    service.threadMgr.focused = false;
    CHECK(EnsureNamedpipeFocusSessionActivated());
    CHECK(activationWrites == 1);
    activationRequired = true;
    acknowledged = false;
    expectedToken.store(43);
    CHECK(!EnsureNamedpipeFocusSessionActivated());
    CHECK(activationWrites == 1 && activationRequired && dirtyResets == 0);

    service.threadMgr.focused = true;
    resetPending = true;
    CHECK(!EnsureNamedpipeFocusSessionActivated());
    resetPending = false;
    transportReady = false;
    CHECK(!EnsureNamedpipeFocusSessionActivated());
    CHECK(activationWrites == 1 && activationRequired);
    transportReady = true;
    CHECK(EnsureNamedpipeFocusSessionActivated());
    CHECK(activationWrites == 2 && sentToken == 43 && dirtyResets == 0);

    // When TSF answers, it is authoritative even if Win32 disagrees.
    activationRequired = true;
    acknowledged = false;
    expectedToken.store(44);
    service.threadMgr.focused = false;
    win32Foreground = true;
    CHECK(!EnsureNamedpipeFocusSessionActivated());
    CHECK(activationWrites == 2 && activationRequired);

    // A host that cannot answer IsThreadFocus falls back to Win32 foreground
    // ownership: a process started in the background stays deferred, the
    // window being typed into still connects. The out value is ignored.
    service.threadMgr.querySucceeds = false;
    service.threadMgr.focused = true;
    win32Foreground = false;
    CHECK(!EnsureNamedpipeFocusSessionActivated());
    CHECK(activationWrites == 2 && activationRequired && dirtyResets == 0);
    win32Foreground = true;
    CHECK(EnsureNamedpipeFocusSessionActivated());
    CHECK(activationWrites == 3 && sentToken == 44 && !activationRequired);
    CHECK(gateDiagnostics > 0);

    std::cout << "Foreground activation, delayed focus and acknowledged cleanup guards passed\n";
    return 0;
}
