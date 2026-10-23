#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>

namespace VoiceInput
{
namespace ControlPolicy
{
enum class Command
{
    Start,
    Stop,
    Toggle,
    Lock,
    Cancel,
    Exit
};

struct Request
{
    Command command;
    // Stop and recording-time Toggle carry their enqueue-time session. A UI
    // Cancel carries the invalidated session's successor, fencing cleanup from
    // a newer recording. Idle Toggle keeps zero and resolves on the control thread.
    std::uint64_t session_cookie = 0;
    // Independent of the ASR session: identifies a pending/applied Space lock.
    std::uintptr_t lock_ticket = 0;
};

struct EnqueueResult
{
    bool enqueued = false;
    bool abort_start = false;
};

inline bool IsSessionCancel(const Request &request)
{
    return request.command == Command::Cancel && request.session_cookie != 0;
}

// Only a plain Cancel participates in Stop/Cancel dedupe. A session-cookie
// cancel belongs to an older recording and must not swallow a later Stop or
// Cancel for the recording that is currently active.
inline bool IsPlainCancel(const Request &request)
{
    return request.command == Command::Cancel && request.session_cookie == 0;
}

// Serializes control intent independently of the worker thread. A session
// cancel is deliberately never merged: it must retain its UI-issued cookie even
// when a Stop is already queued. Ordinary Stop keeps the idle dedupe, while an
// explicit Cancel upgrades a queued Stop and remains reachable in the idle
// recognition state.
inline EnqueueResult Enqueue(std::deque<Request> &pending, Request request, bool recording, bool starting)
{
    if (IsSessionCancel(request))
    {
        pending.push_back(request);
        return {true, false};
    }

    if (request.command == Command::Start)
    {
        if (!pending.empty() && pending.back().command == Command::Start)
            return {};
        pending.push_back(request);
        return {true, false};
    }

    if (request.command == Command::Stop)
    {
        const bool abort_start = starting;
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [](const Request &item) { return item.command == Command::Start; }),
                      pending.end());
        if (!recording && !starting)
            return {false, false};
        if (!pending.empty() && (pending.back().command == Command::Stop || IsPlainCancel(pending.back())) &&
            pending.back().lock_ticket == request.lock_ticket &&
            (IsPlainCancel(pending.back()) || pending.back().session_cookie == request.session_cookie))
            return {false, abort_start};
        pending.push_back(request);
        return {true, abort_start};
    }

    if (request.command == Command::Cancel)
    {
        const bool abort_start = starting;
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [](const Request &item) { return item.command == Command::Start; }),
                      pending.end());
        if (!pending.empty() && IsPlainCancel(pending.back()) && pending.back().lock_ticket == request.lock_ticket)
            return {false, abort_start};
        if (!pending.empty() && pending.back().command == Command::Stop &&
            pending.back().lock_ticket == request.lock_ticket)
        {
            pending.back() = request;
            return {true, abort_start};
        }
        pending.push_back(request);
        return {true, abort_start};
    }

    pending.push_back(request);
    return {true, false};
}

} // namespace ControlPolicy
} // namespace VoiceInput
