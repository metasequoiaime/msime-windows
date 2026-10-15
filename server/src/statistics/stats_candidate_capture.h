#pragma once

// First-word hit-rate capture (schema v2 counters).
//
// The Chinese candidate selection authority lives here in the Server process:
// ProcessSelectionKey holds (keycode, index, page_index) and derives the
// is-first decision (event_listener_selection.cpp). Once text leaves for TSF
// the reply carries no index, so the selection path queues the counter bumps
// for the shared stats.db handle — the TSF statistics pipe
// (engine/contracts/windows_ipc.h) is deliberately untouched.
//
// The store is shared with the stats pipe listener thread; access is
// serialised by the store's own in-process mutex plus the open-guard mutex
// here. Ledger keeps counts only: no text, no candidate, no application name
// — the privacy contract in PRIVACY.md is unchanged.

#include <filesystem>
#include <string>

#include "engine/core/word_item.h"

struct sqlite3;

namespace MsimeStats
{
class Store;

// CountsTowardFirstWordSelection is the Chinese-candidate gate of the
// first-word hit rate. It follows the user decisions: only selections from an
// engine-ranked Chinese candidate list count; mixed-in English dictionary
// hits, emoji/kaomoji/unicode-style synthetic lists and the translation-page
// glosses do not. Japanese candidates are excluded by scheme at the call site
// (R-mode temporary session), English mode by the mode flag there.
inline constexpr bool CountsTowardFirstWordSelection(const WordItem &item)
{
    switch (item.source)
    {
    case CandidateSource::Database:
    case CandidateSource::UserDatabase:
    case CandidateSource::CloudSuggestion:
    case CandidateSource::AiSuggestion:
    case CandidateSource::QuickPhrase:
    case CandidateSource::Generated:
    case CandidateSource::NeuralDesktop:
    case CandidateSource::NeuralKeyboard:
    case CandidateSource::Collocation:
        return true;
    case CandidateSource::Fallback:
        // The genuine whole-sentence association rows are engine output; the
        // same source value is also used for synthetic raw/translation items,
        // and only the sentence-association marker tells them apart.
        return item.sentence_association;
    case CandidateSource::EnglishDictionary:
    case CandidateSource::Emoji:
    case CandidateSource::Kaomoji:
    case CandidateSource::DateTime:
        return false;
    }
    return false;
}

// EnsureStatsStoreOpen opens (or returns) the process-wide <DataDir>\stats.db.
// Moved here so the stats pipe thread and the candidate capture path share one
// handle; the lazily-opened-while-enabled contract is unchanged.
Store *EnsureStatsStoreOpen();

// CloseSharedStatsStore releases the shared handle. Called by
// ShutdownStatsPipe after its listener thread has joined; also safe when the
// capture path alone has opened the store.
void CloseSharedStatsStore();

// RecordCandidateSelection credits one Chinese candidate commit: a silent
// no-op while the statistics switch is off (the observable half of the
// privacy contract: no stats.db is created while disabled). Called on the IPC
// worker/selection path, so it only resolves the day and queues the upsert on
// a background writer; CloseSharedStatsStore drains that queue.
void RecordCandidateSelection(bool is_first, int day_key = 0);
} // namespace MsimeStats
