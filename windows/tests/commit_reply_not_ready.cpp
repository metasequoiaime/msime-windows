// The client-side commit-reply contract: a committing reply that has not arrived yet must never
// be reported as a transport failure, must never drop the key waiting for it, and must be handed
// back to the cooperative poll on the same window.
//
// The TSF service, its message window and the reply pipe cannot be instantiated without a host,
// so the paths that make that decision are compiled from the production sources into
// commit_reply_source.h (see generate_commit_reply_fixture.py). Only the pipe probe, the message
// window and the clock are doubles here. Regenerate the fixture when any of those paths changes.

#include "Key/DeferredKeyFailurePolicy.h"
#include "../../engine/contracts/windows_ipc.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// --- the slice of the Win32 / project surface the extracted paths are written against --------

using HRESULT = long;
using UINT = unsigned int;
using WCHAR = wchar_t;
using DWORD = unsigned long;
using ULONGLONG = unsigned long long;
using UINT_PTR = unsigned long long;
using WPARAM = unsigned long long;
using LONG = long;
using HWND = UINT_PTR;
using BOOL = int;

constexpr HRESULT S_OK = 0;
constexpr HRESULT E_FAIL = static_cast<HRESULT>(0x80004005L);
constexpr unsigned long SEVERITY_SUCCESS = 0;
constexpr unsigned long FACILITY_ITF = 4;
constexpr unsigned long FACILITY_WIN32 = 7;

constexpr unsigned long MAKE_HRESULT(unsigned long severity, unsigned long facility, unsigned long code)
{
    return (severity << 31) | (facility << 16) | code;
}

// ERROR_TIMEOUT (1460) / ERROR_BROKEN_PIPE (109) through the same mapping winerror.h uses.
constexpr HRESULT HResultFromWin32(unsigned long code)
{
    return (code & 0x0000FFFF) | (FACILITY_WIN32 << 16) | 0x80000000L;
}
constexpr HRESULT ERROR_TIMEOUT = HResultFromWin32(1460);
constexpr HRESULT ERROR_BROKEN_PIPE = HResultFromWin32(109);

// The production hooks carry the Win32 spelling of this mapping.
#define __HRESULT_FROM_WIN32(code) HResultFromWin32(code)
#define HRESULT_FROM_WIN32(code) HResultFromWin32(code)

// windows/src/IME/MetasequoiaIME.h; the private-message base is fixed by Win32.
constexpr UINT WM_USER = 0x0400;
constexpr UINT WM_AsyncFinalizeCandidate = WM_USER + 11;
constexpr UINT WM_AsyncPunctuationCommit = WM_USER + 12;
constexpr UINT WM_AsyncNumberCandidateCommit = WM_USER + 13;
constexpr UINT WM_AsyncServerCandidateKey = WM_USER + 14;
constexpr UINT WM_AsyncCycleInputMode = WM_USER + 29;
constexpr UINT_PTR TIMER_COMMIT_REPLY_POLL = 8;
constexpr UINT COMMIT_REPLY_POLL_INTERVAL_MS = 1;

// The enumerators the fixture round-trips. The values are not asserted; only that whatever the
// message handler classified the key as comes back unchanged for the re-run.
enum KEYSTROKE_FUNCTION
{
    FUNCTION_FINALIZE_CANDIDATELIST = 0,
    FUNCTION_FINALIZE_CANDIDATELIST_AND_INPUT,
    FUNCTION_PUNCTUATION,
    FUNCTION_SELECT_BY_NUMBER,
    FUNCTION_CONVERT,
};

namespace Global
{
namespace DataFromServerMsgType = FanyImeReplyType;
} // namespace Global

#include "commit_reply_types_source.h"

namespace
{
// --- pipe probe / clock doubles --------------------------------------------------------------

bool replyAvailable = false;       // what the nonblocking probe reports
int readCalls = 0;                 // how many times the session entered the read path
DWORD lastReadTimeoutMs = 0;       // the wait it asked the read path for
FanyImeNamedpipeDataToTsf frame{}; // the frame the read path yields, when it yields one
bool readYieldsFrame = false;

ULONGLONG clockMs = 10000;
bool timerAvailable = true;
bool postAvailable = true;
bool timerKilled = false;
UINT nextToken = 100;
// What the message window does with a request the poll timer re-dispatches.
std::function<void(UINT message, UINT token)> onSendMessage;

bool HasCommitReplyToRead(uint64_t)
{
    return replyAvailable;
}

FanyImeNamedpipeDataToTsf *TryReadDataFromServerPipeWithDeadline(uint64_t, bool, DWORD timeoutMs, bool *notReady)
{
    ++readCalls;
    lastReadTimeoutMs = timeoutMs;
    if (!readYieldsFrame)
    {
        if (notReady != nullptr)
        {
            *notReady = true;
        }
        return nullptr;
    }
    if (notReady != nullptr)
    {
        *notReady = false;
    }
    return &frame;
}

BOOL IsWindow(HWND window)
{
    return window != 0 ? 1 : 0;
}

UINT_PTR SetTimer(HWND, UINT_PTR, UINT, void *)
{
    return timerAvailable ? 1 : 0;
}

BOOL PostMessage(HWND, UINT, WPARAM, LONG)
{
    return postAvailable ? 1 : 0;
}

LONG SendMessage(HWND, UINT message, WPARAM token, LONG)
{
    if (onSendMessage)
    {
        onSendMessage(message, static_cast<UINT>(token));
    }
    return 0;
}

BOOL KillTimer(HWND, UINT_PTR)
{
    timerKilled = true;
    return 1;
}

UINT NextWindowMessageToken()
{
    return nextToken++;
}

ULONGLONG GetTickCount64()
{
    return clockMs;
}

void DebugTsfKeyLatency(const wchar_t *, uint64_t, double, HRESULT)
{
}

void resetDoubles()
{
    replyAvailable = false;
    readCalls = 0;
    lastReadTimeoutMs = 0;
    frame = {};
    readYieldsFrame = false;
    clockMs = 10000;
    timerAvailable = true;
    postAvailable = true;
    timerKilled = false;
    nextToken = 100;
    onSendMessage = nullptr;
}

class CMetasequoiaIME
{
  public:
    HWND _msgWndHandle = 1;
    std::mutex _pendingCommitCandidateMutex;
    std::map<UINT, AsyncKeyRequest> _pendingAsyncKeyMessages;
    UINT _sessionCommitReplyMessage = 0;

    bool _RequeueAsyncKeyRequest(UINT token, UINT message, AsyncKeyRequest request);
    DWORD _SessionCommitReplyWaitMs() const;
    void _DispatchPendingCommitReplyPolls();
    void _HandOffUnreadyCommitReply(uint64_t requestId, uint64_t deferredReplayToken, UINT message,
                                    ULONGLONG commitReplyDeadlineTick, UINT code, WCHAR wch,
                                    KEYSTROKE_FUNCTION keystrokeFunction, const std::wstring &prefetchedText,
                                    uint64_t focusToken, uint64_t compositionEpoch);

    // Recorded outcomes of a deferred key: exactly one of these must happen for every key.
    void _CompleteDeferredKeyReplay(uint64_t token)
    {
        completedTokens.push_back(token);
    }

    void _FailDeferredKey(uint64_t token, DeferredKeyFailureReason reason)
    {
        failedTokens.push_back(token);
        failureReasons.push_back(reason);
    }

    void _FailUnansweredCommitKey(uint64_t requestId, uint64_t deferredReplayToken)
    {
        unansweredRequestIds.push_back(requestId);
        unansweredTokens.push_back(deferredReplayToken);
    }

    std::vector<uint64_t> completedTokens;
    std::vector<uint64_t> failedTokens;
    std::vector<DeferredKeyFailureReason> failureReasons;
    std::vector<uint64_t> unansweredRequestIds;
    std::vector<uint64_t> unansweredTokens;
};

#include "commit_reply_source.h"

} // namespace

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::cerr << "Failed at line " << __LINE__ << ": " << #condition << '\n';                                  \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (false)

namespace
{
// The read outcome a session sees for a committing key whose reply has not arrived. Every case
// below is a different production branch, driven through the real production body.
int checkReplyReadContract()
{
    resetDoubles();
    FanyImeNamedpipeDataToTsf *reply = &frame;

    // A healthy pipe with nothing in it must be NotReady, and the session must not even enter
    // the read path: this used to be a 300ms blocking read whose timeout came back as
    // TransportUnavailable, which reset the session and dropped the key.
    replyAvailable = false;
    CommitReplyWaitStatus status = TryReadCommitReplyFromServerPipe(8818, /*blockingWaitMs=*/0, &reply);
    CHECK(status == CommitReplyWaitStatus::NotReady);
    CHECK(reply == nullptr);
    CHECK(readCalls == 0);

    // Readable, but the read path yields nothing at its cooperative boundary (only another
    // request's frames were consumed): still NotReady, and it asked for a zero-length wait.
    replyAvailable = true;
    readYieldsFrame = false;
    status = TryReadCommitReplyFromServerPipe(8818, /*blockingWaitMs=*/0, &reply);
    CHECK(status == CommitReplyWaitStatus::NotReady);
    CHECK(reply == nullptr);
    CHECK(readCalls == 1);
    CHECK(lastReadTimeoutMs == 0);

    // A real transfer failure still comes back as the transport frame, so a broken pipe keeps
    // its full reset: only the timeout stopped being a transport failure.
    resetDoubles();
    replyAvailable = true;
    readYieldsFrame = true;
    frame.msg_type = FanyImeReplyType::TransportUnavailable;
    status = TryReadCommitReplyFromServerPipe(8818, /*blockingWaitMs=*/0, &reply);
    CHECK(status == CommitReplyWaitStatus::Transport);
    CHECK(reply == &frame);

    // An authoritative reply is handed to the caller unchanged.
    resetDoubles();
    replyAvailable = true;
    readYieldsFrame = true;
    frame.msg_type = FanyImeReplyType::Normal;
    status = TryReadCommitReplyFromServerPipe(8818, /*blockingWaitMs=*/0, &reply);
    CHECK(status == CommitReplyWaitStatus::Ready);
    CHECK(reply == &frame);

    // A session nothing can re-run keeps the bounded single-shot wait: it reads even when the
    // probe sees nothing yet, and a timeout comes back as the transport frame, never NotReady.
    resetDoubles();
    replyAvailable = false;
    readYieldsFrame = true;
    frame.msg_type = FanyImeReplyType::TransportUnavailable;
    status = TryReadCommitReplyFromServerPipe(8818, FANY_IME_COMMIT_REPLY_SESSION_WAIT_MS, &reply);
    CHECK(status == CommitReplyWaitStatus::Transport);
    CHECK(reply == &frame);
    CHECK(readCalls == 1);
    CHECK(lastReadTimeoutMs == FANY_IME_COMMIT_REPLY_SESSION_WAIT_MS);

    return 0;
}

// Which sessions may read without waiting: only one whose request a poll owns and can be handed
// back through a live message window.
int checkSessionWaitContract()
{
    resetDoubles();
    CMetasequoiaIME service;
    service._sessionCommitReplyMessage = WM_AsyncFinalizeCandidate;
    CHECK(service._SessionCommitReplyWaitMs() == 0);

    // A key that reached its session without the message window has nothing to hand back to.
    service._sessionCommitReplyMessage = 0;
    CHECK(service._SessionCommitReplyWaitMs() == FANY_IME_COMMIT_REPLY_SESSION_WAIT_MS);

    service._sessionCommitReplyMessage = WM_AsyncFinalizeCandidate;
    service._msgWndHandle = 0;
    CHECK(service._SessionCommitReplyWaitMs() == FANY_IME_COMMIT_REPLY_SESSION_WAIT_MS);
    return 0;
}

// The poll timer pass must keep running while any request still waits, including one an edit
// session handed back under a fresh token during the very dispatch that pass made.
int checkDispatchContract()
{
    resetDoubles();
    CMetasequoiaIME service;
    AsyncKeyRequest waiting;
    waiting.message = WM_AsyncFinalizeCandidate;
    waiting.requestId = 8818;
    waiting.commitReplyDeadlineTick = clockMs + 700;
    service._pendingAsyncKeyMessages[300] = waiting;

    // The handler takes its request off the table and its session hands it back.
    onSendMessage = [&](UINT, UINT token) {
        AsyncKeyRequest request = service._pendingAsyncKeyMessages[token];
        service._pendingAsyncKeyMessages.erase(token);
        service._HandOffUnreadyCommitReply(request.requestId, 77, request.message, request.commitReplyDeadlineTick,
                                           0x20, L' ', FUNCTION_FINALIZE_CANDIDATELIST, L"", 5, 6);
    };
    nextToken = 400;
    service._DispatchPendingCommitReplyPolls();
    CHECK(service._pendingAsyncKeyMessages.size() == 1);
    CHECK(service._pendingAsyncKeyMessages.count(300) == 0);
    CHECK(!timerKilled);
    CHECK(service.unansweredRequestIds.empty());

    // Once the handler settles the request, the timer stops.
    onSendMessage = [&](UINT, UINT token) { service._pendingAsyncKeyMessages.erase(token); };
    service._DispatchPendingCommitReplyPolls();
    CHECK(service._pendingAsyncKeyMessages.empty());
    CHECK(timerKilled);
    return 0;
}

// The hand-off that keeps a not-ready commit alive: it must not extend the window, must not
// lose the key, and must never leave a request that nothing will ever poll.
int checkHandOffContract()
{
    resetDoubles();
    CMetasequoiaIME service;
    clockMs = 20000;
    const ULONGLONG windowEnd = clockMs + 700; // a window the first slice already started

    service._HandOffUnreadyCommitReply(8818, /*deferredReplayToken=*/77, WM_AsyncFinalizeCandidate, windowEnd,
                                       /*code=*/0x20, /*wch=*/L' ', FUNCTION_FINALIZE_CANDIDATELIST,
                                       /*prefetchedText=*/L"一致", /*focusToken=*/5, /*compositionEpoch=*/6);

    // The server is still answering the request the key was sent with; it is back in the poll on
    // exactly the window it already had, with everything the re-run needs.
    CHECK(service._pendingAsyncKeyMessages.size() == 1);
    const AsyncKeyRequest &queued = service._pendingAsyncKeyMessages.begin()->second;
    CHECK(queued.message == WM_AsyncFinalizeCandidate);
    CHECK(queued.requestId == 8818);
    CHECK(queued.deferredReplayToken == 77);
    CHECK(queued.focusToken == 5);
    CHECK(queued.compositionEpoch == 6);
    CHECK(queued.keystrokeFunction == FUNCTION_FINALIZE_CANDIDATELIST);
    CHECK(queued.prefetchedText == L"一致");
    CHECK(queued.commitReplyDeadlineTick == windowEnd);
    CHECK(service.unansweredRequestIds.empty());
    CHECK(service.failedTokens.empty());

    // A request that never started a window opens one now, and never stays at 0: the poll only
    // re-dispatches requests whose wait has started, so a 0 here would hang the key forever.
    service._pendingAsyncKeyMessages.clear();
    service._HandOffUnreadyCommitReply(9000, 78, WM_AsyncPunctuationCommit, /*commitReplyDeadlineTick=*/0,
                                       /*code=*/0xBE, /*wch=*/L'.', FUNCTION_PUNCTUATION, /*prefetchedText=*/L"",
                                       /*focusToken=*/5, /*compositionEpoch=*/7);
    CHECK(service._pendingAsyncKeyMessages.size() == 1);
    CHECK(service._pendingAsyncKeyMessages.begin()->second.commitReplyDeadlineTick ==
          clockMs + FANY_IME_COMMIT_REPLY_TIMEOUT_MS);

    // A session never held a poll slot of its own, so the hand-off must take a free one instead
    // of overwriting a key that is already waiting.
    service._pendingAsyncKeyMessages.clear();
    service._pendingAsyncKeyMessages[300].requestId = 41;
    nextToken = 400;
    service._HandOffUnreadyCommitReply(9100, 79, WM_AsyncNumberCandidateCommit, windowEnd, /*code=*/'9',
                                       /*wch=*/L'9', FUNCTION_SELECT_BY_NUMBER, /*prefetchedText=*/L"", 5, 8);
    CHECK(service._pendingAsyncKeyMessages.size() == 2);
    CHECK(service._pendingAsyncKeyMessages[300].requestId == 41);
    bool foundHandedOff = false;
    for (const auto &entry : service._pendingAsyncKeyMessages)
    {
        if (entry.second.requestId == 9100)
        {
            foundHandedOff = true;
        }
    }
    CHECK(foundHandedOff);
    CHECK(service.unansweredRequestIds.empty());

    // No window handle at all: the request cannot be kept alive, so the key is dropped as
    // unanswered (never left in a table nothing will poll, never sent again).
    resetDoubles();
    CMetasequoiaIME stuck;
    stuck._msgWndHandle = 0;
    stuck._HandOffUnreadyCommitReply(9200, 80, WM_AsyncFinalizeCandidate, 0, /*code=*/0x20, /*wch=*/L' ',
                                     FUNCTION_FINALIZE_CANDIDATELIST, L"", 5, 9);
    CHECK(stuck.unansweredRequestIds.size() == 1);
    CHECK(stuck.unansweredRequestIds.front() == 9200);
    CHECK(stuck.unansweredTokens.front() == 80);

    // The timer may be gone while the message window still works; the posted-message fallback
    // keeps the request, and only losing both drops the key.
    resetDoubles();
    CMetasequoiaIME posted;
    timerAvailable = false;
    posted._HandOffUnreadyCommitReply(9300, 81, WM_AsyncFinalizeCandidate, 0, /*code=*/0x20, /*wch=*/L' ',
                                      FUNCTION_FINALIZE_CANDIDATELIST, L"", 5, 10);
    CHECK(posted.unansweredRequestIds.empty());
    CHECK(posted._pendingAsyncKeyMessages.size() == 1);

    resetDoubles();
    CMetasequoiaIME dead;
    timerAvailable = false;
    postAvailable = false;
    dead._HandOffUnreadyCommitReply(9400, 82, WM_AsyncFinalizeCandidate, 0, /*code=*/0x20, /*wch=*/L' ',
                                    FUNCTION_FINALIZE_CANDIDATELIST, L"", 5, 11);
    CHECK(dead._pendingAsyncKeyMessages.empty());
    CHECK(dead.unansweredRequestIds.size() == 1);
    CHECK(dead.unansweredRequestIds.front() == 9400);

    // Only a committing message owns a poll; anything else is not handed back at all.
    resetDoubles();
    CMetasequoiaIME foreign;
    foreign._HandOffUnreadyCommitReply(9500, 83, WM_USER + 9, windowEnd, 0, L'\0', FUNCTION_FINALIZE_CANDIDATELIST, L"",
                                       5, 12);
    CHECK(foreign._pendingAsyncKeyMessages.empty());
    CHECK(foreign.unansweredRequestIds.size() == 1);

    // A hand-off takes a fresh slot, so it is bounded like a freshly posted key: a full table
    // drops the key as unanswered instead of growing past its bound.
    resetDoubles();
    CMetasequoiaIME full;
    for (UINT token = 1; token <= kMaxPendingAsyncKeys; ++token)
    {
        full._pendingAsyncKeyMessages[token].requestId = token;
    }
    nextToken = 1000;
    full._HandOffUnreadyCommitReply(9600, 84, WM_AsyncFinalizeCandidate, windowEnd, /*code=*/0x20, /*wch=*/L' ',
                                    FUNCTION_FINALIZE_CANDIDATELIST, L"", 5, 13);
    CHECK(full._pendingAsyncKeyMessages.size() == kMaxPendingAsyncKeys);
    CHECK(full.unansweredRequestIds.size() == 1);
    CHECK(full.unansweredRequestIds.front() == 9600);

    return 0;
}

// Both session completion hooks decide whether the key the session was applying is finished,
// failed, or still in flight. A not-ready reply must be the third: neither completing it (it was
// not applied) nor failing it (that is the drop this whole change removes).
int checkSessionCompletionContract()
{
    // KeyHandlerEditSession: a key that applied nothing because its reply has not arrived stays
    // in flight for the hand-off the session itself performs.
    {
        CMetasequoiaIME service;
        {
            DeferredReplayCompletion completion{&service, /*token=*/77, false};
            completion.applied = false;
            completion.superseded = false;
            completion.commitReplyNotReady = true;
        }
        CHECK(service.completedTokens.empty());
        CHECK(service.failedTokens.empty());
    }

    // The same session hook still retires a real transfer failure with the classified reason.
    {
        CMetasequoiaIME service;
        {
            DeferredReplayCompletion completion{&service, /*token=*/78, false};
            completion.applied = false;
            completion.superseded = false;
            completion.transportBroken = true;
        }
        CHECK(service.completedTokens.empty());
        CHECK(service.failedTokens.size() == 1);
        CHECK(service.failureReasons.front() == DeferredKeyFailureReason::TransportBroken);
    }

    // And still completes a key that was applied.
    {
        CMetasequoiaIME service;
        {
            DeferredReplayCompletion completion{&service, /*token=*/79, false};
            completion.applied = true;
        }
        CHECK(service.completedTokens.size() == 1);
        CHECK(service.failedTokens.empty());
    }

    // A session without a deferred key behind it has nothing to retire either way.
    {
        CMetasequoiaIME service;
        {
            DeferredReplayCompletion completion{&service, /*token=*/0, false};
            completion.superseded = false;
        }
        CHECK(service.completedTokens.empty());
        CHECK(service.failedTokens.empty());
    }

    // CPunctuationCommitEditSession: the same rule on the other committing session.
    {
        CMetasequoiaIME service;
        {
            Completion completion{&service, /*token=*/80, false};
            completion.superseded = false;
            completion.result = FANY_S_COMMIT_REPLY_NOT_READY;
        }
        CHECK(service.completedTokens.empty());
        CHECK(service.failedTokens.empty());
    }

    // A punctuation commit that was applied completes, and one that really failed is dropped as
    // ambiguous rather than being mistaken for a missing reply.
    {
        CMetasequoiaIME service;
        {
            Completion completion{&service, /*token=*/81, false};
            completion.applied = true;
            completion.result = S_OK;
        }
        CHECK(service.completedTokens.size() == 1);
        CHECK(service.failedTokens.empty());
    }
    {
        CMetasequoiaIME service;
        {
            Completion completion{&service, /*token=*/82, false};
            completion.superseded = false;
            completion.result = FANY_E_COMMIT_REPLY_AMBIGUOUS;
        }
        CHECK(service.completedTokens.empty());
        CHECK(service.failedTokens.size() == 1);
        CHECK(service.failureReasons.front() == DeferredKeyFailureReason::DeliveryAmbiguous);
    }

    return 0;
}
} // namespace

int main()
{
    if (const int failed = checkReplyReadContract())
    {
        return failed;
    }
    if (const int failed = checkSessionWaitContract())
    {
        return failed;
    }
    if (const int failed = checkHandOffContract())
    {
        return failed;
    }
    if (const int failed = checkDispatchContract())
    {
        return failed;
    }
    if (const int failed = checkSessionCompletionContract())
    {
        return failed;
    }

    std::cout << "The session-side commit reply never resets the session and never loses the key\n";
    return 0;
}
