#pragma once

#include <chrono>
#include <string>

namespace FanyImeIpc
{
// One input gets an immediate first frame from the key path, one coalesced fast-enrichment frame, and at most one
// late frame for sources that intentionally arrive after the user stops typing (effect-tier rerank, gloss updates).
// Worker-thread only.
struct CandidatePresentSettle
{
    using Clock = std::chrono::steady_clock;

    std::string input;
    bool armed = false;
    bool done = false;
    bool late_done = false;
    bool late_pending = false;
    Clock::time_point deadline{};

    void Reset()
    {
        *this = CandidatePresentSettle{};
    }

    void Cancel()
    {
        armed = false;
        done = true;
        late_done = true;
        late_pending = false;
    }

    // Returns false when this request is already covered or already settled; true when it armed or was folded into
    // an armed frame. A late request that arrives before the fast settle folds into it instead of opening a second
    // window.
    bool Request(const std::string &current_input, bool late, Clock::time_point now, Clock::duration delay)
    {
        if (current_input.empty())
            return false;
        if (input != current_input)
        {
            Reset();
            input = current_input;
        }
        if (late ? late_done : done)
            return false;
        if (armed)
            return true;

        late_pending = late && done;
        armed = true;
        deadline = now + delay;
        return true;
    }

    bool Due(Clock::time_point now) const
    {
        return armed && now >= deadline;
    }

    // Consumes a due frame. Returns true only when it should be painted; a stale input or an interaction still
    // spends the frame so the next async result does not retry it.
    bool Consume(Clock::time_point now, const std::string &current_input, bool can_show)
    {
        if (!armed || now < deadline)
            return false;
        const bool late_frame = late_pending;
        armed = false;
        late_pending = false;
        if (late_frame)
            late_done = true;
        else
            done = true;
        return can_show && !current_input.empty() && input == current_input;
    }
};
} // namespace FanyImeIpc
