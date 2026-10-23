// Standalone policy tests for phase-C voice commit routing. This target links
// only the pure policies in voice_commit_policy.h and voice_target.h; the
// Windows adapters (focus capture, UIA, TSF pipe writes) stay in the service
// translation unit. The CHECK macro is plain code (never compiled out), unlike
// assert() in NDEBUG builds.
#include "voice-input/voice_commit_policy.h"
#include "voice-input/voice_control_policy.h"
#include "voice-input/voice_target.h"

#include <cstdint>
#include <deque>
#include <iostream>
#include <stdexcept>

namespace
{
#define CHECK(expr)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            throw std::runtime_error("CHECK failed: " #expr);                                                          \
        }                                                                                                              \
    } while (false)

using VoiceInput::CanCommitToTarget;
using VoiceInput::CommitMode;
using VoiceInput::DecideVoiceCommit;
using VoiceInput::SameTarget;
using VoiceInput::Target;
using VoiceInput::VoiceCommitAction;
using VoiceInput::VoiceCommitContext;
using VoiceInput::ControlPolicy::Command;
using VoiceInput::ControlPolicy::Enqueue;
using VoiceInput::ControlPolicy::EnqueueResult;
using VoiceInput::ControlPolicy::Request;

Target MakeTarget(std::uint64_t generation = 10)
{
    Target target;
    target.editable = true;
    target.foreground = 0x10000;
    target.focus = 0x20000;
    target.process = 0x1234;
    target.focus_process = 0x2345;
    target.thread = 0x5678;
    target.generation = generation;
    target.runtime_id = {1, 2, 3};
    target.client = 7;
    target.epoch = 11;
    target.focus_token = 0xABC;
    return target;
}

VoiceCommitContext MakeContext(CommitMode mode)
{
    VoiceCommitContext ctx;
    ctx.fast_valid = true;
    ctx.mode = mode;
    return ctx;
}

void TestNoStopTargetButTargetAppearsStillCards()
{
    // stop was uneditable; a target appears by finalization. CanCommitToTarget
    // gates on stop.editable, so the decision is the card even though the final
    // target looks live.
    const Target empty;
    const Target now = MakeTarget();
    for (CommitMode mode : {CommitMode::Tsf, CommitMode::SendInput, CommitMode::CtrlV})
    {
        VoiceCommitContext ctx = MakeContext(mode);
        ctx.same_target = CanCommitToTarget(empty, now);
        ctx.has_bound_route = true;
        CHECK(!ctx.same_target);
        CHECK(DecideVoiceCommit(ctx) == VoiceCommitAction::ShowCard);
    }
}

void TestNoTargetAllThreeModesCard()
{
    const Target target = MakeTarget();
    const Target no_target;
    for (CommitMode mode : {CommitMode::Tsf, CommitMode::SendInput, CommitMode::CtrlV})
    {
        VoiceCommitContext ctx = MakeContext(mode);
        ctx.same_target = CanCommitToTarget(target, no_target);
        CHECK(!ctx.same_target);
        CHECK(DecideVoiceCommit(ctx) == VoiceCommitAction::ShowCard);
    }
}

void TestFastInvalidAllThreeModesCard()
{
    // Snapshots match, but the in-lock fast/re-verification failed: the live
    // gate keeps every mode on the card.
    const Target target = MakeTarget();
    for (CommitMode mode : {CommitMode::Tsf, CommitMode::SendInput, CommitMode::CtrlV})
    {
        VoiceCommitContext ctx = MakeContext(mode);
        ctx.same_target = CanCommitToTarget(target, target);
        ctx.has_bound_route = true;
        ctx.fast_valid = false;
        CHECK(DecideVoiceCommit(ctx) == VoiceCommitAction::ShowCard);
    }
}

void TestIdentityChangeOrSwitchAwayCards()
{
    const Target before = MakeTarget(42);
    Target different_runtime = MakeTarget(43);
    Target returned = MakeTarget(45); // away then back advances the generation
    VoiceCommitContext ctx = MakeContext(CommitMode::Tsf);
    ctx.same_target = CanCommitToTarget(before, different_runtime);
    CHECK(!ctx.same_target);
    CHECK(DecideVoiceCommit(ctx) == VoiceCommitAction::ShowCard);
    ctx.same_target = CanCommitToTarget(before, returned);
    CHECK(!ctx.same_target);
    CHECK(DecideVoiceCommit(ctx) == VoiceCommitAction::ShowCard);
}

void TestInlineOwnerMismatchForbidsCompositionCommit()
{
    // Streaming was on, but the final target is not the composition owner (the
    // owner left or a different field took over): CommitComposition is forbidden
    // and a bound route falls back to the plain TSF insert.
    const Target owner = MakeTarget(42);
    Target other = MakeTarget(42);
    other.runtime_id = {4, 5, 6};
    // Starting on the desktop or at A must not prevent delivery to B when B
    // is both the stop and final target. Never commit A's old composition.
    for (const Target &start : {Target{}, owner})
    {
        for (CommitMode mode : {CommitMode::Tsf, CommitMode::SendInput, CommitMode::CtrlV})
        {
            VoiceCommitContext ctx = MakeContext(mode);
            ctx.stream_inline_this_session = start.editable;
            ctx.same_target = CanCommitToTarget(other, other);
            ctx.inline_owner_matches = SameTarget(start, other);
            ctx.has_bound_route = true;
            CHECK(!ctx.inline_owner_matches);
            const VoiceCommitAction expected = mode == CommitMode::Tsf         ? VoiceCommitAction::CommitTsf
                                               : mode == CommitMode::SendInput ? VoiceCommitAction::CommitSendInput
                                                                               : VoiceCommitAction::CommitCtrlV;
            CHECK(DecideVoiceCommit(ctx) == expected);
        }
    }
}

void TestSameTargetPerMode()
{
    const Target target = MakeTarget(42);
    struct
    {
        CommitMode mode;
        VoiceCommitAction expected;
    } cases[] = {
        {CommitMode::SendInput, VoiceCommitAction::CommitSendInput},
        {CommitMode::CtrlV, VoiceCommitAction::CommitCtrlV},
        {CommitMode::Tsf, VoiceCommitAction::CommitTsf},
    };
    for (const auto &entry : cases)
    {
        VoiceCommitContext ctx = MakeContext(entry.mode);
        ctx.same_target = CanCommitToTarget(target, target);
        ctx.has_bound_route = true;
        ctx.stream_inline_this_session = false;
        CHECK(DecideVoiceCommit(ctx) == entry.expected);
    }
}

void TestTsfNoRouteFallbackToSendInput()
{
    // TSF mode with an editable target that carries no bound route keeps the
    // legacy default-commit fallback (SendInput) when the live gate passes.
    Target native = MakeTarget(42);
    native.client = 0;
    native.epoch = 0;
    native.focus_token = 0;
    VoiceCommitContext ctx = MakeContext(CommitMode::Tsf);
    ctx.same_target = CanCommitToTarget(native, native);
    ctx.has_bound_route = false;
    ctx.stream_inline_this_session = false;
    CHECK(DecideVoiceCommit(ctx) == VoiceCommitAction::CommitSendInput);
}

void TestCompositionCommitOnMatchingOwner()
{
    const Target target = MakeTarget(42);
    VoiceCommitContext ctx = MakeContext(CommitMode::Tsf);
    ctx.stream_inline_this_session = true;
    ctx.same_target = CanCommitToTarget(target, target);
    ctx.inline_owner_matches = SameTarget(target, target);
    ctx.has_bound_route = true;
    CHECK(DecideVoiceCommit(ctx) == VoiceCommitAction::CommitComposition);
}

void TestControlQueuePolicy()
{
    std::deque<Request> pending;
    const EnqueueResult idle_stop = Enqueue(pending, Request{Command::Stop}, false, false);
    CHECK(!idle_stop.enqueued);
    CHECK(pending.empty());

    // Recognition is no longer recording/starting, but an explicit Cancel must
    // still reach the control thread to clean the old composition.
    const EnqueueResult recognition_cancel = Enqueue(pending, Request{Command::Cancel}, false, false);
    CHECK(recognition_cancel.enqueued);
    CHECK(pending.size() == 1 && pending.front().command == Command::Cancel);
    pending.clear();

    // A queued Stop is upgraded rather than swallowing the later Cancel.
    CHECK(Enqueue(pending, Request{Command::Stop}, true, false).enqueued);
    const EnqueueResult upgrade = Enqueue(pending, Request{Command::Cancel}, false, false);
    CHECK(upgrade.enqueued);
    CHECK(pending.size() == 1 && pending.front().command == Command::Cancel);
    pending.clear();

    // Popping a not-yet-started Start must not discard the Cancel cleanup.
    CHECK(Enqueue(pending, Request{Command::Start}, false, false).enqueued);
    CHECK(Enqueue(pending, Request{Command::Cancel}, false, false).enqueued);
    CHECK(pending.size() == 1 && pending.front().command == Command::Cancel);
    pending.clear();

    // A UI cancel keeps its cookie even when a Stop is already queued.
    CHECK(Enqueue(pending, Request{Command::Stop}, true, false).enqueued);
    CHECK(Enqueue(pending, Request{Command::Cancel, 2}, false, false).enqueued);
    CHECK(pending.size() == 2);
    CHECK(pending.front().command == Command::Stop);
    CHECK(pending.back().command == Command::Cancel && pending.back().session_cookie == 2);

    // An old UI Cancel must not swallow the active session's Stop or ordinary
    // Cancel just because it is at the queue tail.
    pending.clear();
    CHECK(Enqueue(pending, Request{Command::Cancel, 2}, true, false).enqueued);
    CHECK(Enqueue(pending, Request{Command::Stop}, true, false).enqueued);
    CHECK(pending.size() == 2);
    CHECK(pending.front().command == Command::Cancel && pending.front().session_cookie == 2);
    CHECK(pending.back().command == Command::Stop);

    pending.clear();
    CHECK(Enqueue(pending, Request{Command::Cancel, 2}, true, false).enqueued);
    CHECK(Enqueue(pending, Request{Command::Cancel}, true, false).enqueued);
    CHECK(pending.size() == 2);
    CHECK(pending.front().command == Command::Cancel && pending.front().session_cookie == 2);
    CHECK(pending.back().command == Command::Cancel && pending.back().session_cookie == 0);

    // Stop/Cancel sweep every queued Start, including those separated by Lock.
    for (Command command : {Command::Stop, Command::Cancel})
    {
        pending = {{Command::Start}, {Command::Lock, 0, 2}, {Command::Start}, {Command::Lock, 0, 4}};
        const auto result = Enqueue(pending, Request{command, 0, 2}, false, true);
        CHECK(result.enqueued && result.abort_start);
        CHECK(pending.size() == 3 && pending[0].lock_ticket == 2 && pending[1].lock_ticket == 4);
        CHECK(pending[2].command == command && pending[2].lock_ticket == 2);
    }
    // Different lock tickets and sessions retain their independent cleanup.
    pending = {{Command::Stop, 1, 2}};
    CHECK(Enqueue(pending, Request{Command::Stop, 1, 4}, true, false).enqueued);
    CHECK(Enqueue(pending, Request{Command::Stop, 3, 4}, true, false).enqueued);
    CHECK(pending.size() == 3);

    // A stale cookie queued while a new Start is in progress must not latch an
    // abort for that new recording.
    pending.clear();
    const EnqueueResult stale_cookie = Enqueue(pending, Request{Command::Cancel, 2}, false, true);
    CHECK(stale_cookie.enqueued);
    CHECK(!stale_cookie.abort_start);
}

} // namespace

int main()
{
    const struct
    {
        const char *name;
        void (*fn)();
    } cases[] = {
        {"no stop target but a target appears still cards", TestNoStopTargetButTargetAppearsStillCards},
        {"no editable target all three modes card", TestNoTargetAllThreeModesCard},
        {"fast re-verification failed all three modes card", TestFastInvalidAllThreeModesCard},
        {"identity change or switch-away cards", TestIdentityChangeOrSwitchAwayCards},
        {"inline owner mismatch forbids composition commit", TestInlineOwnerMismatchForbidsCompositionCommit},
        {"same editable target keeps per-mode delivery", TestSameTargetPerMode},
        {"tsf without a route falls back to sendinput", TestTsfNoRouteFallbackToSendInput},
        {"composition commit on matching live owner", TestCompositionCommitOnMatchingOwner},
        {"control queue preserves explicit cancel", TestControlQueuePolicy},
    };
    for (const auto &entry : cases)
    {
        try
        {
            entry.fn();
            std::cout << "[PASS] " << entry.name << "\n";
        }
        catch (const std::exception &ex)
        {
            std::cerr << "[FAIL] " << entry.name << ": " << ex.what() << "\n";
            return 1;
        }
    }
    std::cout << "voice commit policy: all " << sizeof(cases) / sizeof(cases[0]) << " case(s) passed\n";
    return 0;
}
