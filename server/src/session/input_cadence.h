#pragma once

#include <chrono>

// 从「打字中」到「停手」的判定阈值。取 300 ms 而不是 150：常见输入间隔约 200 ms，
// 150 ms 会在每两键之间关一次闸，效果档照样每键跑一遍。每段输入的第一键不压效果档，
// 所以只有间隔小于本阈值的后续按键才进入打字态——慢打（间隔不小于阈值）从不触发闸门，
// 也就不会每键翻一次配置、再在下一键前多跑一次全量重查。
constexpr int kNeuralDesktopIdleGateMs = 300;

// 输入节奏闸门：按最近的按键间隔判断用户是在「连续打字」还是「停下来看候选」。
// 打字期间神经效果档不交给引擎（省掉每键一次的模型路径 stat 和 12-best 词格解码），
// 停手到点后放行一次并重查候选。
//
// 这里只有 InputCadence 的判定：不读时钟也不碰全局状态，测试直接构造 InputCadence
// 推进时间。文件末尾另有一个进程级的「正在打字」标记，只有 IPC worker 线程会写它。
struct InputCadence
{
    using Clock = std::chrono::steady_clock;

    // 记一次按键到达；返回这一键是否按「打字中」处理（压制效果档）。
    // 只有上一键还在阈值内才算连续打字：单独一个键（慢打，或每段输入的第一键）不压效果档，
    // 否则每次停顿都要翻一次配置、清一次整句缓存，再在下一键前多跑一次全量重查。
    // gate_ms <= 0 表示闸门被配置关掉：始终 Idle，顺手把可能残留的打字状态收回
    // （阈值从非零改成 0 时，不能让效果档一直关着）。
    bool NoteKey(Clock::time_point now, int gate_ms);

    // 最近一次按键距今已达到阈值、可以从 Typing 转为 Idle。判定本身不改状态，
    // 调用方拿到 true 后自己调 CloseGate。闸门被配置关掉时，残留的 Typing 同样返回 true，
    // 保证中途改成 0 的那次也能收尾。
    bool ShouldCloseGate(Clock::time_point now, int gate_ms) const;

    bool typing() const
    {
        return typing_;
    }

    // 打字中闸门到点的时刻；不在打字、或闸门被关掉时返回 time_point{}，
    // 调用方据此决定要不要走带超时的等待。
    Clock::time_point Deadline(int gate_ms) const;

    // Typing -> Idle。只有 IPC worker 线程在到点后调用。
    void CloseGate()
    {
        typing_ = false;
    }

  private:
    Clock::time_point last_key_{};
    bool has_key_ = false;
    bool typing_ = false;
};

// 闸门只压效果档的模型路径（= 神经重排能力）：打字中返回 false，开关本身与其余整句来源
// 原样返回。抽成纯函数是为了让单测钉住「闸门不影响速度档、也不翻整句来源开关」这条契约。
bool EffectiveNeuralDesktop(bool configured, bool typing);

// 进程级的「正在打字」标记。
//
// 线程归属：写的人只有 IPC 的 WorkerThread（每个未被 stale 丢弃的 ImeKeyEvent
// 任务出队时置位、到点后清零）；读的人只有 EngineInputSession::ApplyConfiguration——它的全部调用点
// （handle_key / recompute_candidates / switch_scheme / refresh_configuration）都在 WorkerThread 的任务里。
// 用原子量是防御性的：万一以后有别的线程直接调 recompute_candidates，也不会读到
// 一个没有同步保证的 bool。读写都用 relaxed，闸门只表达「最近有没有按键」，不需要
// 与其他内存操作排序。
void SetTypingInProgress(bool typing);
bool TypingInProgress();
