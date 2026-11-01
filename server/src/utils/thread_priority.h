#pragma once

#include <windows.h>

namespace ThreadPriority
{
// RAII boost for the calling thread. The constructor is a no-op when disabled,
// when the current priority cannot be read, or when the new priority is
// rejected, so a rejected boost never interrupts the caller's work.
class ScopedBoost
{
  public:
    explicit ScopedBoost(int priority, bool enabled = true);
    ~ScopedBoost();

    ScopedBoost(const ScopedBoost &) = delete;
    ScopedBoost &operator=(const ScopedBoost &) = delete;

    bool active() const;

  private:
    int previous_ = THREAD_PRIORITY_ERROR_RETURN;
    bool active_ = false;
};
} // namespace ThreadPriority
