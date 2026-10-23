#pragma once

#include <cstdint>
#include <vector>

namespace VoiceInput
{
// Value-only snapshot of the input target taken when a recording stops. No COM
// pointers or host-owned objects survive the ASR wait: every field is a numeric
// identity or a generation counter. The TSF client / activation epoch / focus
// token fields are filled later by the voice service to bind a commit route;
// the platform adapter always leaves them zero.
struct Target
{
    bool editable = false;
    std::uintptr_t foreground = 0;
    std::uintptr_t focus = 0;
    // The foreground host process and the process/thread that own the focused
    // child window. They differ for cross-process embedded Edit controls: the
    // host owns the top-level window and the TSF route, while the focused Edit
    // belongs to another process. Both identities are part of the snapshot.
    std::uint32_t process = 0;
    std::uint32_t focus_process = 0;
    std::uint32_t thread = 0;
    std::uint64_t generation = 0;
    std::vector<int> runtime_id;
    std::uint64_t client = 0;
    std::uint64_t epoch = 0;
    std::uint64_t focus_token = 0;
};

// True only when both snapshots describe the same still-editable target:
// identical foreground/focus HWNDs, host/focus processes, focus thread, focus
// generation, UIA runtime ID and TSF route binding. Any change, including focus
// leaving and returning to the same field (which advances the generation),
// breaks the match. A target that was never confirmed editable is never "the
// same".
inline bool SameTarget(const Target &before, const Target &now)
{
    return before.editable && now.editable && before.foreground != 0 && before.focus != 0 &&
           before.foreground == now.foreground && before.focus == now.focus && before.process == now.process &&
           before.focus_process == now.focus_process && before.thread == now.thread &&
           before.generation == now.generation && before.runtime_id == now.runtime_id && before.client == now.client &&
           before.epoch == now.epoch && before.focus_token == now.focus_token;
}

// True when both snapshots describe the same composition owner despite a focus
// generation change. This is for cancelling an old composition only: a Win+.
// round-trip bumps the focus generation while the DLL deliberately keeps the
// composition and pipe epoch alive, so a cancellation must still reach that
// owner. Every other SameTarget field, including the UIA runtime ID and the
// client/epoch/focus-token route binding, must still match; a new field's
// normal preedit is never torn down because only its generation differs.
inline bool SameCompositionOwner(const Target &before, const Target &now)
{
    return before.editable && now.editable && before.foreground != 0 && before.focus != 0 &&
           before.foreground == now.foreground && before.focus == now.focus && before.process == now.process &&
           before.focus_process == now.focus_process && before.thread == now.thread &&
           before.runtime_id == now.runtime_id && before.client == now.client && before.epoch == now.epoch &&
           before.focus_token == now.focus_token;
}

// Final routing decision: deliver through the bound route only when the
// stop-time target was editable and the target at finalization is that same
// editable target (SameTarget already requires both sides editable). A target
// that only appeared after stop, that became read-only/disabled, or that
// changed identity routes to the result card.
inline bool CanCommitToTarget(const Target &stop, const Target &now)
{
    return SameTarget(stop, now);
}

// Whether this snapshot carries a bound TSF pipe route. Both the client and
// the activation epoch must be non-zero; SameTarget compares the route fields,
// so the decision-time target carries the same binding as the stop snapshot.
inline bool HasBoundRoute(const Target &target)
{
    return target.client != 0 && target.epoch != 0;
}

// Whether a TSF pipe client identity belongs to this captured foreground
// target. The client is either the foreground host's input thread or the
// thread that owns the focused child window; the latter may be in another
// process for an embedded Edit. PID and TID always match as a pair, so a host
// PID mixed with a child TID (or the reverse) is rejected.
inline bool PipeClientMatchesForeground(const Target &target, std::uint32_t client_pid, std::uint32_t client_tid,
                                        std::uint32_t foreground_thread)
{
    if (client_pid == 0 || client_tid == 0 || foreground_thread == 0 || target.process == 0 ||
        target.focus_process == 0 || target.thread == 0 || target.foreground == 0 || target.focus == 0)
    {
        return false;
    }
    return (client_pid == target.process && client_tid == foreground_thread) ||
           (client_pid == target.focus_process && client_tid == target.thread);
}

// The WinEvent hooks must be installed and uninstalled on the Server UI thread,
// which pumps the WinEvent message queue; the focus-generation callbacks only
// run while that thread is pumping. Both functions are idempotent and safe to
// call again after a failed init (partial hooks are unhooked).
bool InitializeTargetTracking();
void ShutdownTargetTracking();
// Capture a target snapshot on a background thread, never from the keyboard
// hook or the UI thread: this performs the cross-process UIA provider query.
// The 100 ms provider connection/transaction timeouts apply to each UIA call
// individually; a multi-step query over several RPCs can take longer, and there
// is no single whole-capture deadline.
Target CaptureTarget();
// No-COM re-validation for use inside the voice service's send lock right
// before delivering: re-checks tracking is live, the focus generation, the
// foreground window and host pid, and the focus HWND and its pid/tid against
// the snapshot; for the native Edit/RichEdit fast path it also re-checks
// enabled/read-only.
// Returns false for any non-editable snapshot. The full CaptureTarget, which
// performs the UIA provider query, must be called outside the lock.
bool IsTargetCurrentFast(const Target &target);
} // namespace VoiceInput
