#include "Key/DeferredKeyFailurePolicy.h"

namespace
{
using Kind = DeferredKeyFailureKind;
using Reason = DeferredKeyFailureReason;

bool Resolves(Reason reason, bool offline, Kind expected)
{
    return ResolveDeferredKeyFailure(reason, offline) == expected;
}
} // namespace

int main()
{
    // A superseded key never starts another reset, online or offline.
    if (!Resolves(Reason::Superseded, false, Kind::Stale))
        return 1;
    if (!Resolves(Reason::Superseded, true, Kind::Stale))
        return 2;

    // Host quirks resync the composition over the healthy pipes.
    if (!Resolves(Reason::HostEditRejected, false, Kind::Resync))
        return 3;
    if (!Resolves(Reason::EditSessionRequestFailed, false, Kind::Resync))
        return 4;
    if (!Resolves(Reason::AsyncPostFailed, false, Kind::Resync))
        return 5;

    // Offline, the local composition is the only authority: nothing to resync.
    if (!Resolves(Reason::HostEditRejected, true, Kind::Offline))
        return 6;
    if (!Resolves(Reason::EditSessionRequestFailed, true, Kind::Offline))
        return 7;
    if (!Resolves(Reason::AsyncPostFailed, true, Kind::Offline))
        return 8;

    // A broken or ambiguous transport always takes the full reset.
    if (!Resolves(Reason::TransportBroken, false, Kind::Transport))
        return 9;
    if (!Resolves(Reason::TransportBroken, true, Kind::Transport))
        return 10;
    if (!Resolves(Reason::DeliveryAmbiguous, false, Kind::Transport))
        return 11;
    if (!Resolves(Reason::DeliveryAmbiguous, true, Kind::Transport))
        return 12;

    // Edit-session outcomes: validation beats the HRESULT, ambiguity beats a
    // plain transport error, and anything else is a host rejection.
    if (ClassifyEditSessionFailure(true, true, true) != Reason::Superseded)
        return 13;
    if (ClassifyEditSessionFailure(false, true, true) != Reason::DeliveryAmbiguous)
        return 14;
    if (ClassifyEditSessionFailure(false, false, true) != Reason::TransportBroken)
        return 15;
    if (ClassifyEditSessionFailure(false, false, false) != Reason::HostEditRejected)
        return 16;

    // A failed cycle reply must refresh native mode, which the Server changed
    // before replying. Ordinary host failures still use the same-token resync.
    const Reason cycleFailures[] = {Reason::HostEditRejected, Reason::EditSessionRequestFailed,
                                    Reason::AsyncPostFailed};
    for (const auto reason : cycleFailures)
    {
        if (ResolveDeferredKeyFailure(reason, false, true) != Kind::Transport)
            return 19;
        if (ResolveDeferredKeyFailure(reason, false) != Kind::Resync)
            return 20;
        // An offline cycle never reached the Server: discard its dependent
        // queue through Offline while keeping the local raw composition.
        if (ResolveDeferredKeyFailure(reason, true, true) != Kind::Offline)
            return 22;
    }
    // The Server already switched language and left the requester out of the
    // broadcast: an unapplied cycle reply needs a fresh snapshot even when a
    // newer token superseded it.
    if (ResolveDeferredKeyFailure(Reason::Superseded, false, true) != Kind::Transport)
        return 21;
    if (ResolveDeferredKeyFailure(Reason::Superseded, false) != Kind::Stale)
        return 23;
    // A cycle answered as the binary toggle was applied on both sides; only
    // the keys queued behind it are wrong.
    if (ResolveDeferredKeyFailure(Reason::ProjectionInvalidated, false, true) != Kind::Resync)
        return 24;
    if (ResolveDeferredKeyFailure(Reason::ProjectionInvalidated, true, true) != Kind::Offline)
        return 25;
    if (ClassifyEditSessionFailure(false, false, false, true) != Reason::ProjectionInvalidated)
        return 26;
    if (ClassifyEditSessionFailure(false, false, true, true) != Reason::TransportBroken)
        return 27;

    // Paging and highlight moves are acknowledgement-only.
    const unsigned int navigationKeys[] = {0x09 /*Tab*/, 0x21 /*PageUp*/, 0x22 /*PageDown*/, 0x26 /*Up*/,
                                           0x28 /*Down*/};
    for (const unsigned int key : navigationKeys)
    {
        if (!IsCandidateNavigationOnlyKey(key))
            return 17;
    }
    // Keys the Server may turn into a commit keep the ambiguous-commit rule:
    // configurable paging punctuation (, . [ ] - =), Enter (Ctrl+Enter
    // translation), Space and the selection digits. Composition edit keys are
    // not candidate keys at all.
    const unsigned int commitCapableKeys[] = {0xBC /*,*/,    0xBE /*.*/,     0xDB /*[*/,     0xDD /*]*/, 0xBD /*-*/,
                                              0xBB /*=*/,    0x0D /*Enter*/, 0x20 /*Space*/, '1',        '9',
                                              0x24 /*Home*/, 0x23 /*End*/,   0x25 /*Left*/,  0x27 /*Right*/};
    for (const unsigned int key : commitCapableKeys)
    {
        if (IsCandidateNavigationOnlyKey(key))
            return 18;
    }

    return 0;
}
