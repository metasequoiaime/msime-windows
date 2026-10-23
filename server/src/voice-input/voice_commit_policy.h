#pragma once

#include "voice_target.h"

#include <cstdint>

// Phase-C voice commit routing policy. This header is a pure, platform-free
// decision unit: it takes context computed by the voice service and returns a
// single action the service then executes under its send lock. The Windows
// adapters (focus capture, UIA, TSF pipe writes) stay in the translation units
// that own them, so this logic is directly unit-testable.
namespace VoiceInput
{
// Normalized commit mode. The raw config string is a product-local string; the
// service maps it once so this policy never parses configuration text.
enum class CommitMode : std::uint8_t
{
    Tsf,
    SendInput,
    CtrlV,
};

// What the voice service must do with the finalized text. The action states a
// destination, never the mechanism details (clipboard vs SendInput vs TSF
// composition), so the policy stays honest about what it can decide.
enum class VoiceCommitAction : std::uint8_t
{
    // No editable stop target, or the stop target changed/vanished: keep the
    // full text in the persistent result card. This path never touches the
    // clipboard, SendInput or TSF.
    ShowCard,
    // Commit through the live inline TSF composition of the bound owner.
    CommitComposition,
    // Commit through plain TSF insert on the captured route.
    CommitTsf,
    // Deliver via SendInput to a still-current editable target.
    CommitSendInput,
    // Copy to clipboard and send Ctrl+V after the target is re-verified.
    CommitCtrlV,
};

struct VoiceCommitContext
{
    // CanCommitToTarget(stop, final): the stop-time target was editable and the
    // final target is the exact same editable target (all identity fields plus
    // the TSF route binding).
    bool same_target = false;
    // Inline preedit streaming was enabled for this session.
    bool stream_inline_this_session = false;
    // The final target still is the inline composition owner (SameTarget of the
    // start-time owner and the final target). Meaningless when streaming is off.
    bool inline_owner_matches = false;
    // The final target carries a bound TSF route that is still current at this
    // moment. Only relevant for Tsf-mode commit; SendInput/cut modes ignore it.
    bool has_bound_route = false;
    // The final target is still current right now, re-verified inside the send
    // lock with the no-COM fast check (and, when a route is bound, against the
    // live pipe route). This is the last gate before any delivery: a target
    // that moved, vanished, became read-only/disabled, or lost its route
    // during the ASR wait routes to the card.
    bool fast_valid = false;
    CommitMode mode = CommitMode::Tsf;
};

// CommitRecognizedText checks session validity under its send lock before
// calling this destination policy. Guarantees for that current session:
//  * "No editable target at stop" or "the target changed/vanished by
//    finalization" always routes to the card, for every commit mode, and never
//    auto-copies to the clipboard.
//  * A target that stopped being the current editable focus or lost its route
//    while the result was being finalized (fast_valid) routes to the card too.
//  * A same editable target keeps the original per-mode delivery, and a Tsf
//    mode without a live captured route falls back to SendInput exactly as the
//    legacy default-commit fallback did.
//  * Inline composition is only taken when the committed target is still the
//    composition owner; an owner mismatch forbids CommitComposition and uses
//    the plain insert instead.
inline VoiceCommitAction DecideVoiceCommit(const VoiceCommitContext &ctx)
{
    if (!ctx.same_target || !ctx.fast_valid)
    {
        return VoiceCommitAction::ShowCard;
    }
    switch (ctx.mode)
    {
    case CommitMode::SendInput:
        return VoiceCommitAction::CommitSendInput;
    case CommitMode::CtrlV:
        return VoiceCommitAction::CommitCtrlV;
    case CommitMode::Tsf:
        return ctx.stream_inline_this_session && ctx.inline_owner_matches && ctx.has_bound_route
                   ? VoiceCommitAction::CommitComposition
                   : (ctx.has_bound_route ? VoiceCommitAction::CommitTsf : VoiceCommitAction::CommitSendInput);
    }
    return VoiceCommitAction::ShowCard;
}
} // namespace VoiceInput
