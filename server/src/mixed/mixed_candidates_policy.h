#pragma once

#include "mixed/mixed_candidates.h"

namespace FanyImeIpc
{
// An empty result is still a completed answer for the current generation. The task thread has already published a
// page without mixed rows; skipping the callback would leave any UI-only rows that a rebuild preserved in place,
// so the answer must be delivered even when there is nothing to merge.
inline bool ShouldDeliverMixedResult(const MixedCandidates::Result &result, bool stale, bool has_callback)
{
    (void)result;
    return !stale && has_callback;
}
} // namespace FanyImeIpc
