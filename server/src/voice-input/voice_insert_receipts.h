#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace VoiceInput
{
class VoiceInsertReceipts
{
  public:
    std::uint64_t Begin(std::uint64_t client, std::size_t chunks)
    {
        std::lock_guard lock(mutex_);
        const auto first = next_;
        next_ += chunks;
        pending_.emplace(first, Pending{client, std::vector<bool>(chunks), chunks, false});
        return first;
    }

    void Record(std::uint64_t client, std::uint64_t receipt, bool accepted)
    {
        std::lock_guard lock(mutex_);
        auto next = pending_.upper_bound(receipt);
        if (next == pending_.begin())
            return;
        auto &[first, delivery] = *--next;
        if (delivery.client != client || receipt - first >= delivery.confirmed.size())
            return;
        const auto index = static_cast<std::size_t>(receipt - first);
        if (delivery.confirmed[index])
            return;
        if (accepted)
        {
            delivery.confirmed[index] = true;
            --delivery.remaining;
        }
        else
        {
            delivery.rejected = true;
        }
        if (delivery.remaining == 0 || delivery.rejected)
            changed_.notify_all();
    }

    bool Wait(std::uint64_t first, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        const auto it = pending_.find(first);
        const auto &delivery = it->second;
        changed_.wait_for(lock, timeout, [&] { return delivery.remaining == 0 || delivery.rejected; });
        const bool accepted = delivery.remaining == 0 && !delivery.rejected;
        pending_.erase(it);
        return accepted;
    }

    void Forget(std::uint64_t first)
    {
        std::lock_guard lock(mutex_);
        pending_.erase(first);
    }

  private:
    struct Pending
    {
        std::uint64_t client;
        std::vector<bool> confirmed;
        std::size_t remaining;
        bool rejected;
    };
    std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t next_ = 1;
    std::map<std::uint64_t, Pending> pending_;
};
} // namespace VoiceInput
