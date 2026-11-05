#pragma once

#include <cstdint>

namespace FanyImeIpc
{
// The UI stamps an interaction with the page generation it actually painted. The newest published snapshot may be
// newer than the screen (a coalesced settle has not been painted yet), so only the rendered generation is the
// correct reference for deciding whether the interaction belongs to the visible page.
inline bool ShouldLatchCandidateInteraction(std::uint64_t interaction_generation, std::uint64_t rendered_generation)
{
    return interaction_generation != 0 && interaction_generation == rendered_generation;
}
} // namespace FanyImeIpc
