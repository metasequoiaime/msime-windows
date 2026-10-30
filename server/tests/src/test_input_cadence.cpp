#include "session/input_cadence.h"
#include "tests/includes/test_framework.h"

#include <chrono>

namespace
{
using Clock = InputCadence::Clock;
// 用产品里的真实阈值，避免测试与实现悄悄分叉。
constexpr int kGateMs = kNeuralDesktopIdleGateMs;
// 固定起点：判定只看相对时间差，用例不读真实时钟。
const Clock::time_point kStart{};
} // namespace

// 0 是回滚路径：闸门关掉时必须与旧行为一致，一次按键都不能进 Typing。
TEST_CASE(cadence_gate_disabled_stays_idle_for_every_key)
{
    InputCadence cadence;
    REQUIRE(!cadence.NoteKey(kStart, 0));
    REQUIRE(!cadence.typing());
    REQUIRE(!cadence.ShouldCloseGate(kStart, 0));
    REQUIRE(cadence.Deadline(0) == Clock::time_point{});

    // 负毫秒按 0 处理（配置读取处已经把负数归零，这里再钉住判定层不吃负数）。
    REQUIRE(!cadence.NoteKey(kStart + std::chrono::seconds(1), -20));
    REQUIRE(!cadence.typing());
    REQUIRE(cadence.Deadline(-20) == Clock::time_point{});
}

// 阈值从非零改成 0 时，残留的 Typing 必须在下一个判定点被收回，否则效果档会一直关着。
TEST_CASE(cadence_gate_disabled_mid_typing_releases_the_gate)
{
    InputCadence cadence;
    REQUIRE(!cadence.NoteKey(kStart, kGateMs));
    REQUIRE(cadence.NoteKey(kStart + std::chrono::milliseconds(100), kGateMs));
    REQUIRE(cadence.typing());
    REQUIRE(cadence.ShouldCloseGate(kStart + std::chrono::milliseconds(100), 0));
    cadence.CloseGate();
    REQUIRE(!cadence.typing());
    REQUIRE(!cadence.NoteKey(kStart + std::chrono::milliseconds(101), 0));
    REQUIRE(!cadence.typing());

    // 配置在打字中途改成 0 时，下一颗键自己也要收掉残留状态，不能等 worker 的超时分支。
    InputCadence next_key;
    REQUIRE(!next_key.NoteKey(kStart, kGateMs));
    REQUIRE(next_key.NoteKey(kStart + std::chrono::milliseconds(1), kGateMs));
    REQUIRE(next_key.typing());
    REQUIRE(!next_key.NoteKey(kStart + std::chrono::milliseconds(2), 0));
    REQUIRE(!next_key.typing());
}

// 单独一个键不压效果档：慢打（间隔不小于阈值）因此从不进打字态，也就不会每键翻一次
// 配置、再在下一键前多跑一次停手重查。
TEST_CASE(cadence_single_and_slow_keys_stay_idle)
{
    InputCadence cadence;
    REQUIRE(!cadence.NoteKey(kStart, kGateMs));
    REQUIRE(!cadence.typing());
    REQUIRE(cadence.Deadline(kGateMs) == Clock::time_point{});
    REQUIRE(!cadence.ShouldCloseGate(kStart + std::chrono::seconds(10), kGateMs));

    const auto gap = std::chrono::milliseconds(kGateMs + 100);
    for (int i = 1; i < 5; ++i)
    {
        REQUIRE(!cadence.NoteKey(kStart + gap * i, kGateMs));
        REQUIRE(!cadence.typing());
        REQUIRE(cadence.Deadline(kGateMs) == Clock::time_point{});
    }
}

TEST_CASE(cadence_classifies_typing_and_idle_by_the_time_since_the_last_key)
{
    InputCadence cadence;
    // 第一键只记账，不压效果档。
    REQUIRE(!cadence.NoteKey(kStart, kGateMs));
    REQUIRE(!cadence.typing());
    // 第二键在阈值内：进入打字态。
    REQUIRE(cadence.NoteKey(kStart + std::chrono::milliseconds(100), kGateMs));
    REQUIRE(cadence.typing());
    // 距上一键不足阈值：仍是打字中。
    REQUIRE(!cadence.ShouldCloseGate(kStart + std::chrono::milliseconds(100 + kGateMs - 1), kGateMs));
    // 恰好到阈值：停手成立（判定用 >=，不是 >）。
    REQUIRE(cadence.ShouldCloseGate(kStart + std::chrono::milliseconds(100 + kGateMs), kGateMs));
    cadence.CloseGate();
    REQUIRE(!cadence.typing());
    // 已经关掉的闸门不会被同一个时间点重复关闭。
    REQUIRE(!cadence.ShouldCloseGate(kStart + std::chrono::seconds(10), kGateMs));
    REQUIRE(cadence.Deadline(kGateMs) == Clock::time_point{});
}

// 连续按键刷新 deadline：只要按键间隔小于阈值，deadline 就跟着最后一键往后走，
// 打字期间不会因为第一键算起的时间到点而提前放行。
TEST_CASE(cadence_consecutive_keys_keep_pushing_the_deadline)
{
    InputCadence cadence;
    constexpr int kKeys = 5;
    for (int i = 0; i < kKeys; ++i)
    {
        const auto key_time = kStart + std::chrono::milliseconds(100 * i);
        REQUIRE(cadence.NoteKey(key_time, kGateMs) == (i > 0));
        REQUIRE(cadence.typing() == (i > 0));
        if (i > 0)
            REQUIRE(cadence.Deadline(kGateMs) == key_time + std::chrono::milliseconds(kGateMs));
        else
            REQUIRE(cadence.Deadline(kGateMs) == Clock::time_point{});
    }
    const auto last_key = kStart + std::chrono::milliseconds(100 * (kKeys - 1));
    REQUIRE(!cadence.ShouldCloseGate(last_key + std::chrono::milliseconds(kGateMs - 1), kGateMs));
    REQUIRE(cadence.ShouldCloseGate(last_key + std::chrono::milliseconds(kGateMs), kGateMs));
    // deadline 到点之后 CloseGate 才把状态收回，调用方负责先关闸再重查。
    REQUIRE(cadence.typing());
    cadence.CloseGate();
    REQUIRE(!cadence.typing());
}

// 闸门只压效果档的模型路径：速度档和其余整句来源都不经过这个函数，契约就是「typing 恒 false」。
TEST_CASE(effective_neural_desktop_is_off_while_typing)
{
    REQUIRE(!EffectiveNeuralDesktop(true, true));
    REQUIRE(!EffectiveNeuralDesktop(false, true));
    // Idle 时完全等于配置值。
    REQUIRE(EffectiveNeuralDesktop(true, false));
    REQUIRE(!EffectiveNeuralDesktop(false, false));
}

// 连续 N 键期间从第二键起效果档一直是 false，停手到点后恢复 true，且迁移只发生一次。
TEST_CASE(cadence_typing_keeps_the_effect_tier_off_until_the_user_stops)
{
    InputCadence cadence;
    int typing_transitions = 0;
    for (int i = 0; i < 8; ++i)
    {
        const auto key_time = kStart + std::chrono::milliseconds(100 * i);
        const bool was_typing = cadence.typing();
        cadence.NoteKey(key_time, kGateMs);
        if (!was_typing && cadence.typing())
            ++typing_transitions;
        // 第一键按配置值放行，从第二键起才压住。
        REQUIRE(EffectiveNeuralDesktop(/*configured=*/true, cadence.typing()) == (i == 0));
    }
    // 只有第二颗键把闸门从 Idle 拉进 Typing，后续按键不再产生迁移。
    REQUIRE(typing_transitions == 1);

    const auto stopped_at = kStart + std::chrono::milliseconds(700 + kGateMs);
    REQUIRE(cadence.ShouldCloseGate(stopped_at, kGateMs));
    cadence.CloseGate();
    REQUIRE(EffectiveNeuralDesktop(/*configured=*/true, cadence.typing()));
    // 关过一次之后不会被同一个停手再关第二次——Idle 重查只排一次。
    REQUIRE(!cadence.ShouldCloseGate(stopped_at + std::chrono::seconds(1), kGateMs));
}
