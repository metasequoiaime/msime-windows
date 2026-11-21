#pragma once

#include <string>

namespace FanyImeIpc
{
// Bounds how many rescore callbacks may rebuild the page for one input. A new input string gets a fresh budget;
// Reset() is for a composition boundary (commit, hide, clear), where the same string can legitimately be typed
// again and must not inherit the previous composition's spent budget.
class RescoreRequeryBudget
{
  public:
    static constexpr int kMaxRequestsPerInput = 4;

    bool TryAcquire(const std::string &input)
    {
        if (input.empty())
        {
            input_.clear();
            count_ = 0;
            return false;
        }
        if (input != input_)
        {
            input_ = input;
            count_ = 0;
        }
        if (count_ >= kMaxRequestsPerInput)
            return false;
        ++count_;
        return true;
    }

    void Reset()
    {
        input_.clear();
        count_ = 0;
    }

    int count() const
    {
        return count_;
    }

  private:
    std::string input_;
    int count_ = 0;
};
} // namespace FanyImeIpc
