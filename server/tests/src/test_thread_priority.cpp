#include "utils/thread_priority.h"

#include <cstdio>
#include <windows.h>

namespace
{
constexpr int kSkipExitCode = 77;

bool SetThreadPriorityOrSkip(HANDLE thread, int priority, const char *name, int original_priority)
{
    if (!SetThreadPriority(thread, priority))
    {
        std::printf("skip: cannot set current thread priority to %s (err=%lu)\n", name, GetLastError());
        (void)SetThreadPriority(thread, original_priority);
        return false;
    }

    // A job or policy can accept the call without applying it; the guard test
    // would then fail for an environment restriction, so read the value back.
    const int actual = GetThreadPriority(thread);
    if (actual != priority)
    {
        std::printf("skip: thread priority did not become %s (got %d)\n", name, actual);
        (void)SetThreadPriority(thread, original_priority);
        return false;
    }
    return true;
}
} // namespace

int main()
{
    const HANDLE thread = GetCurrentThread();
    const int original_priority = GetThreadPriority(thread);
    if (original_priority == THREAD_PRIORITY_ERROR_RETURN)
    {
        std::printf("skip: cannot read current thread priority (err=%lu)\n", GetLastError());
        return kSkipExitCode;
    }

    // The guard restores whatever priority it saw at construction, so start
    // from Normal to make the restored value unambiguous.
    if (!SetThreadPriorityOrSkip(thread, THREAD_PRIORITY_NORMAL, "normal", original_priority))
        return kSkipExitCode;

    // Prove the host accepts the boost priority itself before blaming the guard
    // for an environment restriction. A host that refuses it must skip.
    if (!SetThreadPriorityOrSkip(thread, THREAD_PRIORITY_ABOVE_NORMAL, "above normal", original_priority))
        return kSkipExitCode;
    if (!SetThreadPriorityOrSkip(thread, THREAD_PRIORITY_NORMAL, "normal", original_priority))
        return kSkipExitCode;

    bool ok = true;
    {
        ThreadPriority::ScopedBoost disabled(THREAD_PRIORITY_ABOVE_NORMAL, false);
        const int priority = GetThreadPriority(thread);
        if (disabled.active() || priority != THREAD_PRIORITY_NORMAL)
        {
            std::printf("disabled guard active=%d priority=%d\n", disabled.active() ? 1 : 0, priority);
            ok = false;
        }
    }

    {
        ThreadPriority::ScopedBoost boost(THREAD_PRIORITY_ABOVE_NORMAL);
        const int boosted_priority = GetThreadPriority(thread);
        ok = ok && boost.active() && boosted_priority == THREAD_PRIORITY_ABOVE_NORMAL;
        std::printf("boost active=%d priority=%d\n", boost.active() ? 1 : 0, boosted_priority);
    }

    const int restored_priority = GetThreadPriority(thread);
    if (restored_priority != THREAD_PRIORITY_NORMAL)
    {
        std::printf("expected priority %d after guard destruction, got %d\n", THREAD_PRIORITY_NORMAL,
                    restored_priority);
        ok = false;
    }

    // A guard that hard-codes Normal on destruction would pass the check above;
    // entering at Below normal proves the captured value is what gets restored.
    if (!ok)
    {
        (void)SetThreadPriority(thread, original_priority);
        return 1;
    }
    if (!SetThreadPriorityOrSkip(thread, THREAD_PRIORITY_BELOW_NORMAL, "below normal", original_priority))
        return kSkipExitCode;
    {
        ThreadPriority::ScopedBoost boost(THREAD_PRIORITY_ABOVE_NORMAL);
        const int boosted_priority = GetThreadPriority(thread);
        ok = boost.active() && boosted_priority == THREAD_PRIORITY_ABOVE_NORMAL;
    }
    const int captured_restored_priority = GetThreadPriority(thread);
    if (captured_restored_priority != THREAD_PRIORITY_BELOW_NORMAL)
    {
        std::printf("expected priority %d after guard destruction, got %d\n", THREAD_PRIORITY_BELOW_NORMAL,
                    captured_restored_priority);
        ok = false;
    }

    (void)SetThreadPriority(thread, original_priority);
    return ok ? 0 : 1;
}
