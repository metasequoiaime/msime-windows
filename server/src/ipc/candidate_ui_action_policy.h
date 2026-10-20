#pragma once

#include <atomic>
#include <cstdint>

namespace FanyImeIpc
{
enum class CandidateExpandState
{
    Unavailable,
    Collapsed,
    Requested,
    Expanded,
};

inline void PublishCandidateExpandAvailability(std::atomic<CandidateExpandState> &state, bool available, bool expanded)
{
    if (!available)
        state.store(CandidateExpandState::Unavailable, std::memory_order_release);
    else if (expanded)
        state.store(CandidateExpandState::Expanded, std::memory_order_release);
    else
    {
        // A repaint must preserve a keyboard request queued for the UI thread.
        auto expected = CandidateExpandState::Unavailable;
        state.compare_exchange_strong(expected, CandidateExpandState::Collapsed, std::memory_order_release,
                                      std::memory_order_relaxed);
    }
}

inline bool RequestCandidateExpandOnDown(uint32_t key, uint32_t modifiers, bool ui_less,
                                         std::atomic<CandidateExpandState> &state)
{
    if (key != 0x28u || (modifiers & 0b111u) != 0 || ui_less) // VK_DOWN; Shift / Ctrl / Alt
        return false;
    auto expected = CandidateExpandState::Collapsed;
    return state.compare_exchange_strong(expected, CandidateExpandState::Requested, std::memory_order_acq_rel,
                                         std::memory_order_acquire);
}

inline constexpr int kMinCandidateUiOneBasedIndex = 1;
inline constexpr int kMaxCandidateUiOneBasedIndex = 10;

constexpr bool IsValidCandidateUiOneBasedIndex(int index)
{
    return index >= kMinCandidateUiOneBasedIndex && index <= kMaxCandidateUiOneBasedIndex;
}
} // namespace FanyImeIpc
