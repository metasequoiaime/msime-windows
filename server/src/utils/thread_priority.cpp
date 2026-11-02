#include "utils/thread_priority.h"

namespace ThreadPriority
{
ScopedBoost::ScopedBoost(int priority, bool enabled)
{
    if (!enabled)
        return;

    const int previous = GetThreadPriority(GetCurrentThread());
    if (previous == THREAD_PRIORITY_ERROR_RETURN)
        return;

    if (SetThreadPriority(GetCurrentThread(), priority))
    {
        previous_ = previous;
        active_ = true;
    }
}

ScopedBoost::~ScopedBoost()
{
    if (active_)
    {
        // Restoration is best-effort: the task already ran at the boosted
        // priority, and a failure here must not mask that work.
        (void)SetThreadPriority(GetCurrentThread(), previous_);
    }
}

bool ScopedBoost::active() const
{
    return active_;
}
} // namespace ThreadPriority
