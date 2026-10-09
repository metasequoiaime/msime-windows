#pragma once

// What a failed key costs. An eaten key is never handed back to the host and
// never sent twice: whatever went wrong, the key is dropped. The reason only
// decides how much session state has to be discarded with it. Kept free of
// TSF/COM and Windows headers so the mapping is unit-tested
// (tests/deferred_key_failure_policy.cpp).

// Why an eaten key could not be applied.
enum class DeferredKeyFailureReason
{
    // Its focus token, composition epoch, reset token or focus generation was
    // replaced before it ran. Whatever replaced it owns the cleanup.
    Superseded,
    // The host refused or failed the edit session (S_FALSE from GetSelection,
    // E_NOTIMPL, TF_E_*), or the handler itself could not apply the key.
    HostEditRejected,
    // RequestEditSession failed, or the edit session could not be allocated.
    EditSessionRequestFailed,
    // The key's async request could not be posted or handed to a context.
    AsyncPostFailed,
    // Write failure, reply read failure or invalid frame, dead pipe, worker
    // disconnect, or a focus session that could not be established.
    TransportBroken,
    // The request was written but its reply never arrived: the Server may
    // already have acted on it (committed a candidate). Never resend.
    DeliveryAmbiguous,
    // The key itself was applied and both sides agree on the result, but it
    // landed in a different state than the one the keys queued behind it were
    // classified for (a language cycle answered as the binary toggle).
    ProjectionInvalidated,
};

// How much state the failure discards.
enum class DeferredKeyFailureKind
{
    // Drop the key only. No reset is started.
    Stale,
    // Offline raw lane: the TSF composition is the only authority. Drop the
    // key and the keys queued behind it; the composition stays.
    Offline,
    // Pipes are fine, but the local composition and the Server may disagree:
    // drop the key and the queue, cancel the composition locally and clear
    // the Server composition on the current focus token.
    Resync,
    // The transport is broken or its state is unknown: drop the key and the
    // queue, and run the full reset (new focus token, reply pipe reopened).
    Transport,
};

inline DeferredKeyFailureKind ResolveDeferredKeyFailure(DeferredKeyFailureReason reason, bool offlineLaneActive,
                                                        bool serverModeMayHaveChanged = false)
{
    if (reason == DeferredKeyFailureReason::ProjectionInvalidated)
    {
        // Nothing to undo on either side; only the queue behind it is wrong.
        return offlineLaneActive ? DeferredKeyFailureKind::Offline : DeferredKeyFailureKind::Resync;
    }
    // The Server changes language before answering a cycle. If that reply is
    // not applied, for any reason including a newer focus token or composition
    // epoch, clearing composition on the same token cannot restore the native
    // mode the Server already switched to (and the requester is left out of the
    // InputModeChanged broadcast): reconnect and receive a fresh snapshot. A
    // focus change is handled by the caller; activation re-sends the language.
    if (serverModeMayHaveChanged && !offlineLaneActive)
    {
        return DeferredKeyFailureKind::Transport;
    }
    switch (reason)
    {
    case DeferredKeyFailureReason::Superseded:
        return DeferredKeyFailureKind::Stale;
    case DeferredKeyFailureReason::TransportBroken:
    case DeferredKeyFailureReason::DeliveryAmbiguous:
        return DeferredKeyFailureKind::Transport;
    case DeferredKeyFailureReason::HostEditRejected:
    case DeferredKeyFailureReason::EditSessionRequestFailed:
    case DeferredKeyFailureReason::AsyncPostFailed:
    default:
        return offlineLaneActive ? DeferredKeyFailureKind::Offline : DeferredKeyFailureKind::Resync;
    }
}

// Reason for an edit session that ran but did not apply its key. The caller
// derives the flags from the session's own validation and HRESULT.
inline DeferredKeyFailureReason ClassifyEditSessionFailure(bool superseded, bool deliveryAmbiguous,
                                                           bool transportBroken, bool projectionInvalidated = false)
{
    if (superseded)
    {
        return DeferredKeyFailureReason::Superseded;
    }
    if (deliveryAmbiguous)
    {
        return DeferredKeyFailureReason::DeliveryAmbiguous;
    }
    if (transportBroken)
    {
        return DeferredKeyFailureReason::TransportBroken;
    }
    if (projectionInvalidated)
    {
        return DeferredKeyFailureReason::ProjectionInvalidated;
    }
    return DeferredKeyFailureReason::HostEditRejected;
}

// Server candidate keys whose reply is an acknowledgement only: they page or
// move the highlight and can never commit text (the Server commits only for
// selection keys, Ctrl+Enter, word-to-character -/= and the punctuation it
// lists by character). A reply missing past the ordinary wait is then a soft
// miss, not a lost commit: no teardown, no queue clear.
// The values are the Win32 virtual-key codes.
inline bool IsCandidateNavigationOnlyKey(unsigned int virtualKey)
{
    constexpr unsigned int kTab = 0x09;      // VK_TAB; Shift+Tab pages back
    constexpr unsigned int kPageUp = 0x21;   // VK_PRIOR
    constexpr unsigned int kPageDown = 0x22; // VK_NEXT
    constexpr unsigned int kUp = 0x26;       // VK_UP
    constexpr unsigned int kDown = 0x28;     // VK_DOWN
    return virtualKey == kTab || virtualKey == kPageUp || virtualKey == kPageDown || virtualKey == kUp ||
           virtualKey == kDown;
}
