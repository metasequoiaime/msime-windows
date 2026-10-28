// 输入节奏闸门的判定与进程级「打字中」标记。判定部分不依赖时钟，标记只是给会话层看的
// 一个布尔量，两者都放在这个翻译单元里，测试可以只链这一份。
#include "session/input_cadence.h"

#include <atomic>

bool InputCadence::NoteKey(Clock::time_point now, int gate_ms)
{
    // 先按「上一键」的距离判断这一键算不算连续打字，再记下本键时间。
    // 第一键、以及间隔已经超过阈值的键都不进打字态：它们各自只需要一次正常查询，
    // 压住效果档反而会在停手后多排一次全量重查。
    const bool follows_previous = has_key_ && now - last_key_ < std::chrono::milliseconds(gate_ms);
    last_key_ = now;
    has_key_ = true;
    if (gate_ms <= 0)
    {
        // 闸门关着就是旧行为：始终 Idle。这里也把打字状态收回，阈值从非零改成 0 的
        // 那次按键就不会继续压着效果档。
        typing_ = false;
        return false;
    }
    typing_ = follows_previous;
    return typing_;
}

bool InputCadence::ShouldCloseGate(Clock::time_point now, int gate_ms) const
{
    if (!typing_)
        return false;
    if (gate_ms <= 0)
        return true;
    return now - last_key_ >= std::chrono::milliseconds(gate_ms);
}

InputCadence::Clock::time_point InputCadence::Deadline(int gate_ms) const
{
    if (!typing_ || gate_ms <= 0)
        return Clock::time_point{};
    return last_key_ + std::chrono::milliseconds(gate_ms);
}

bool EffectiveNeuralDesktop(bool configured, bool typing)
{
    return configured && !typing;
}

namespace
{
std::atomic_bool g_typing_in_progress{false};
} // namespace

void SetTypingInProgress(bool typing)
{
    g_typing_in_progress.store(typing, std::memory_order_relaxed);
}

bool TypingInProgress()
{
    return g_typing_in_progress.load(std::memory_order_relaxed);
}
