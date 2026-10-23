// Exercises the real voice_input_service.cpp control queue and keyboard-hook
// entry. Only external effects are substituted: microphone, overlay, cue
// player, Doubao client, IPC, config reads and logging.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <msime/voice/audio_capture.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "config/ime_config.h"
#include "ipc/ipc.h"
#include "log/candidate_diag_log.h"
#include "tests/includes/test_framework.h"
#include "utils/common_utils.h"
#include "utils/network_proxy.h"
#include "voice-input/cue_player.h"
#include "voice-input/doubao_asr_client.h"
#include "voice-input/voice_input_gesture.h"
#include "voice-input/voice_input_service.h"
#include "voice-input/voice_result_card.h"
#include "voice-input/voice_target.h"
#include "voice-input/wave_overlay.h"

namespace
{
using std::chrono::milliseconds;

// Only the control thread's config read may take a test gate. Hook calls and
// RefreshKeyboardHook run on this thread and must keep working while the
// control thread is parked.
std::thread::id g_test_thread_id;

// A test-only checkpoint in the real control loop parks the worker before it
// takes a request. Idle epochs also prove the whole queue has been processed,
// without a sleep or changing the enabled configuration to mask a stale Start.
std::mutex g_queue_gate_mutex;
std::condition_variable g_queue_gate_cv;
bool g_pause_queue = false;
bool g_queue_paused = false;
unsigned g_queue_idle_epoch = 0;

void PauseNextIdleQueue()
{
    std::lock_guard<std::mutex> lock(g_queue_gate_mutex);
    g_pause_queue = true;
}

bool WaitQueuePaused()
{
    std::unique_lock<std::mutex> lock(g_queue_gate_mutex);
    return g_queue_gate_cv.wait_for(lock, std::chrono::seconds(5), [] { return g_queue_paused; });
}

void ReleaseQueue()
{
    {
        std::lock_guard<std::mutex> lock(g_queue_gate_mutex);
        g_pause_queue = false;
    }
    g_queue_gate_cv.notify_all();
}

bool ReleaseQueueAndWaitIdle()
{
    std::unique_lock<std::mutex> lock(g_queue_gate_mutex);
    const unsigned previous = g_queue_idle_epoch;
    g_pause_queue = false;
    g_queue_gate_cv.notify_all();
    return g_queue_gate_cv.wait_for(lock, std::chrono::seconds(5),
                                    [previous] { return g_queue_idle_epoch > previous; });
}

std::mutex g_effect_mutex;
std::condition_variable g_effect_cv;
std::atomic<bool> g_actions_visible{false};
std::atomic<int> g_actions_true_count{0};
std::atomic<int> g_hide_count{0};

void NotifyEffects()
{
    g_effect_cv.notify_all();
}

void ResetObservations()
{
    std::lock_guard<std::mutex> lock(g_effect_mutex);
    g_actions_visible = false;
    g_actions_true_count = 0;
    g_hide_count = 0;
}

void PumpMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

template <typename Predicate> bool WaitFor(Predicate predicate, milliseconds timeout = milliseconds(5000))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        PumpMessages();
        std::unique_lock<std::mutex> lock(g_effect_mutex);
        if (g_effect_cv.wait_for(lock, milliseconds(5), predicate))
            return true;
    }
    PumpMessages();
    return predicate();
}

template <typename Predicate> bool WaitWithoutPumping(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + milliseconds(5000);
    std::unique_lock<std::mutex> lock(g_effect_mutex);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        g_effect_cv.wait_for(lock, milliseconds(5));
    return predicate();
}

constexpr wchar_t kFixedAsrText[] = L"asr-fixed-result";

std::mutex g_commit_mutex;
std::wstring g_inserted_text;
std::atomic<int> g_insert_text_count{0};

void ResetCommit()
{
    {
        std::lock_guard<std::mutex> lock(g_commit_mutex);
        g_inserted_text.clear();
    }
    g_insert_text_count = 0;
}

int ResultCount()
{
    return g_insert_text_count.load();
}

std::wstring DeliveredText()
{
    std::lock_guard<std::mutex> lock(g_commit_mutex);
    return g_inserted_text;
}

bool WaitForResult()
{
    return WaitFor([] { return g_insert_text_count.load() > 0; }, milliseconds(3000));
}

std::mutex g_doubao_mutex;
std::condition_variable g_doubao_cv;
bool g_doubao_finish_entered = false;
bool g_doubao_finish_release = false;

void ResetDoubao()
{
    std::lock_guard<std::mutex> lock(g_doubao_mutex);
    g_doubao_finish_entered = false;
    g_doubao_finish_release = false;
}

bool DoubaoFinishEntered()
{
    std::lock_guard<std::mutex> lock(g_doubao_mutex);
    return g_doubao_finish_entered;
}

bool WaitDoubaoFinishEntered()
{
    return WaitFor([] {
        std::lock_guard<std::mutex> lock(g_doubao_mutex);
        return g_doubao_finish_entered;
    });
}

void ReleaseDoubaoFinish()
{
    {
        std::lock_guard<std::mutex> lock(g_doubao_mutex);
        g_doubao_finish_release = true;
    }
    g_doubao_cv.notify_all();
    NotifyEffects();
}

std::mutex g_overlay_mutex;
std::function<void(WaveOverlay::Action)> g_overlay_action_handler;

bool InvokeOverlayCancel()
{
    std::function<void(WaveOverlay::Action)> handler;
    {
        std::lock_guard<std::mutex> lock(g_overlay_mutex);
        handler = g_overlay_action_handler;
    }
    if (!handler)
        return false;
    handler(WaveOverlay::Action::Cancel);
    return true;
}

std::mutex g_capture_mutex;
std::condition_variable g_capture_cv;
bool g_capture_entered = false;
bool g_capture_release = false;
int g_capture_stop_count = 0;
metasequoia::voice::AudioCapture::AudioCallback g_audio_callback;

void ResetCapture()
{
    std::lock_guard<std::mutex> lock(g_capture_mutex);
    g_capture_entered = false;
    g_capture_release = false;
    g_audio_callback = {};
}

bool CaptureEntered()
{
    std::lock_guard<std::mutex> lock(g_capture_mutex);
    return g_capture_entered;
}

bool InjectAudioFrames(std::size_t frames)
{
    metasequoia::voice::AudioCapture::AudioCallback callback;
    {
        std::lock_guard<std::mutex> lock(g_capture_mutex);
        callback = g_audio_callback;
    }
    if (!callback)
        return false;
    const std::vector<float> samples(frames, 0.2f);
    callback(samples.data(), samples.size());
    return true;
}

bool WaitCaptureEntered()
{
    return WaitFor([] {
        std::lock_guard<std::mutex> lock(g_capture_mutex);
        return g_capture_entered;
    });
}

void AllowCaptureStart()
{
    {
        std::lock_guard<std::mutex> lock(g_capture_mutex);
        g_capture_release = true;
    }
    g_capture_cv.notify_all();
    NotifyEffects();
}

int CaptureStopCount()
{
    std::lock_guard<std::mutex> lock(g_capture_mutex);
    return g_capture_stop_count;
}

std::mutex g_config_mutex;
std::condition_variable g_config_cv;
VoiceInputConfig g_config;
bool g_block_next_config_read = false;
bool g_config_read_entered = false;
bool g_config_read_release = false;
bool g_config_read_waiting = false;
std::atomic<int> g_config_read_count{0};

void SetConfig(const VoiceInputConfig &config)
{
    std::lock_guard<std::mutex> lock(g_config_mutex);
    g_config = config;
}

void BlockNextConfigRead()
{
    std::unique_lock<std::mutex> lock(g_config_mutex);
    // ReleaseConfigRead only notifies; the old waiter may not have reacquired
    // the mutex yet. Wait for it to leave its gate before resetting release,
    // otherwise the old read would be trapped by the new gate. The bound turns
    // a missed release into a test failure instead of a harness hang.
    const bool exited = g_config_cv.wait_for(lock, milliseconds(5000), [] { return !g_config_read_waiting; });
    REQUIRE(exited);
    g_block_next_config_read = true;
    g_config_read_entered = false;
    g_config_read_release = false;
}

bool WaitConfigReadEntered(milliseconds timeout = milliseconds(5000))
{
    std::unique_lock<std::mutex> lock(g_config_mutex);
    return g_config_cv.wait_for(lock, timeout, [] { return g_config_read_entered; });
}

void ReleaseConfigRead()
{
    std::lock_guard<std::mutex> lock(g_config_mutex);
    g_config_read_release = true;
    g_config_cv.notify_all();
}

bool WaitConfigReadAfter(int previous)
{
    return WaitFor([previous] { return g_config_read_count.load() > previous; });
}

VoiceInputConfig TestConfig(bool enabled, bool hotkeys)
{
    VoiceInputConfig config;
    config.enabled = enabled;
    config.asr_provider = "openai";
    config.asr_tokens["openai"] = "test-token";
    config.hotkey_ralt = hotkeys;
    config.hotkey_ctrl_f9 = hotkeys;
    config.hotkey_ctrl_win = hotkeys;
    config.hotkey_rctrl_ralt = hotkeys;
    config.hotkey_hold_space_lock = hotkeys;
    config.start_sound = false;
    config.end_sound = false;
    config.mute_system_audio = false;
    config.polish_text = false;
    return config;
}

// Streaming Doubao is the only provider whose recognition runs through Finish,
// which is what lets a test park the ASR phase. The fake client never opens a
// network connection. TSF commit mode keeps the commit on the fake IPC path
// instead of SendInput or the clipboard; Polish stays off.
VoiceInputConfig TestDoubaoConfig(bool enabled, bool hotkeys)
{
    VoiceInputConfig config = TestConfig(enabled, hotkeys);
    config.asr_provider = "doubao";
    config.asr_token = "doubao-test-token";
    config.asr_tokens["doubao"] = "doubao-test-token";
    config.asr_endpoint = "wss://voice.invalid.test/never";
    config.commit_mode = "tsf";
    config.polish_text = false;
    return config;
}

bool WaitRecording()
{
    return WaitFor([] { return VoiceInput::IsRecording(); });
}

bool WaitNormalStop(int stop_before, int hides_before)
{
    return WaitFor([stop_before, hides_before] {
        return CaptureStopCount() > stop_before && g_hide_count.load() >= hides_before + 2 && !g_actions_visible.load();
    });
}

LRESULT SendKey(UINT vk, bool down, bool system_key = false)
{
    KBDLLHOOKSTRUCT key{};
    key.vkCode = vk;
    const WPARAM message = down ? (system_key ? WM_SYSKEYDOWN : WM_KEYDOWN) : (system_key ? WM_SYSKEYUP : WM_KEYUP);
    return VoiceInput::Detail::KeyboardHookProc(HC_ACTION, message, reinterpret_cast<LPARAM>(&key));
}

void KeyDown(UINT vk, bool system_key = false)
{
    SendKey(vk, true, system_key);
}

void KeyUp(UINT vk, bool system_key = false)
{
    SendKey(vk, false, system_key);
}

void PressSpace()
{
    KeyDown(VK_SPACE);
    KeyUp(VK_SPACE);
}

void PressCtrlF9()
{
    KeyDown(VK_LCONTROL);
    KeyDown(VK_F9);
    KeyUp(VK_F9);
    KeyUp(VK_LCONTROL);
}

enum class Gesture
{
    RAlt,
    CtrlWin,
    RCtrlRAlt
};

void BeginGesture(Gesture gesture)
{
    switch (gesture)
    {
    case Gesture::RAlt:
        KeyDown(VK_RMENU, true);
        break;
    case Gesture::CtrlWin:
        KeyDown(VK_LCONTROL);
        KeyDown(VK_LWIN);
        break;
    case Gesture::RCtrlRAlt:
        KeyDown(VK_RCONTROL);
        KeyDown(VK_RMENU, true);
        break;
    }
}

void EndGesture(Gesture gesture)
{
    switch (gesture)
    {
    case Gesture::RAlt:
        KeyUp(VK_RMENU, true);
        break;
    case Gesture::CtrlWin:
        KeyUp(VK_LWIN);
        KeyUp(VK_LCONTROL);
        break;
    case Gesture::RCtrlRAlt:
        KeyUp(VK_RMENU, true);
        KeyUp(VK_RCONTROL);
        break;
    }
}

void StartGesture(Gesture gesture)
{
    ResetCapture();
    BeginGesture(gesture);
    PumpMessages();
    REQUIRE(WaitCaptureEntered());
}

void ReleaseTestKeys()
{
    KeyUp(VK_RMENU, true);
    KeyUp(VK_LCONTROL);
    KeyUp(VK_RCONTROL);
    KeyUp(VK_LWIN);
    KeyUp(VK_RWIN);
    KeyUp(VK_F9);
}

// A Start sent while no recording exists parks in the disabled config read.
// Reaching that read proves the control thread crossed every earlier command,
// including the action whose effect the test is about.
bool QueueDisabledStartBarrier()
{
    SetConfig(TestConfig(false, true));
    BlockNextConfigRead();
    VoiceInput::StartRecording();
    return WaitConfigReadEntered(milliseconds(2000));
}

// Toggle reaches the config read whether the control thread is idle (Start) or
// a build without the startup marker has already committed a recording (Stop),
// so the config-gate tests can report the capture flag instead of hanging on
// the capture gate. Releasing the capture gate only runs after the barrier
// times out, which is already a failure.
bool QueueToggleBarrierAllowingBrokenCapture()
{
    SetConfig(TestConfig(false, true));
    BlockNextConfigRead();
    VoiceInput::ToggleRecording();
    bool reached = WaitConfigReadEntered(milliseconds(2000));
    if (!reached)
    {
        AllowCaptureStart();
        reached = WaitConfigReadEntered();
    }
    return reached;
}

void VerifyNextHoldStartsAndStops()
{
    SetConfig(TestConfig(true, true));
    StartGesture(Gesture::RAlt);
    AllowCaptureStart();
    REQUIRE(WaitRecording());
    const int stop_before = CaptureStopCount();
    const int hides_before = g_hide_count.load();
    EndGesture(Gesture::RAlt);
    REQUIRE(WaitNormalStop(stop_before, hides_before));
    REQUIRE(!VoiceInput::IsRecording());
}

void PreparePendingAsr()
{
    SetConfig(TestDoubaoConfig(true, true));
    StartGesture(Gesture::RAlt);
    AllowCaptureStart();
    REQUIRE(WaitRecording());
    REQUIRE(InjectAudioFrames(4800)); // 0.3 s at 16 kHz: more than 0.25 s
    VoiceInput::StopRecording();
    REQUIRE(WaitDoubaoFinishEntered());
    REQUIRE(!VoiceInput::IsRecording());
    // Release the physical key only after recording ended: this posts an idle
    // Stop (filtered by the entry rules) and clears the hook suppression state,
    // so a following Refresh cannot fall back to SendInput.
    EndGesture(Gesture::RAlt);
    PumpMessages();
}

void ExpectPendingAsrCommit()
{
    const bool barriered = QueueDisabledStartBarrier();
    ReleaseConfigRead();
    ReleaseDoubaoFinish();
    REQUIRE(barriered);
    REQUIRE(WaitForResult());
    REQUIRE_EQ(DeliveredText(), std::wstring(kFixedAsrText));
}

// Public Start, no physical hold: Refresh must not find a synthetic RAlt to
// release, and a disabled config means it installs no OS hook. Only the real
// control queue and capture.start gate are involved.
void PrepareCapturingAudio()
{
    SetConfig(TestDoubaoConfig(true, true));
    ResetCapture();
    VoiceInput::StartRecording();
    REQUIRE(WaitCaptureEntered());
    AllowCaptureStart();
    REQUIRE(WaitRecording());
    REQUIRE(InjectAudioFrames(4800));
}

struct ServiceFixture
{
    // The teardown Cancel covers a start still parked in the config or capture
    // gate. Negative lifecycle tests below disable it so a teardown Cancel
    // cannot supply the stop/finish they assert absent; cleanup still releases
    // every gate and Shutdown waits for all recognition futures.
    explicit ServiceFixture(bool cancel_on_teardown = true) : cancel_on_teardown_(cancel_on_teardown)
    {
        SetConfig(TestConfig(true, false));
        ResetCapture();
        ResetObservations();
        ResetCommit();
        ResetDoubao();
        if (!VoiceInput::Initialize())
            throw std::runtime_error("VoiceInput::Initialize failed");
        VoiceInput::SetImeActive(true);
        // No OS hook was installed because the initial config disabled every
        // hotkey. Enable them for the direct handler calls only.
        SetConfig(TestConfig(true, true));
    }

    ~ServiceFixture()
    {
        // Release every gate before Shutdown: an assertion can fail while the
        // control thread is in the config or capture gate and while the ASR
        // worker is parked in Finish.
        ReleaseQueue();
        {
            std::lock_guard<std::mutex> lock(g_config_mutex);
            g_block_next_config_read = false;
            g_config_read_release = true;
        }
        g_config_cv.notify_all();
        {
            std::lock_guard<std::mutex> lock(g_doubao_mutex);
            g_doubao_finish_release = true;
        }
        g_doubao_cv.notify_all();
        {
            std::lock_guard<std::mutex> lock(g_capture_mutex);
            g_capture_release = true;
        }
        g_capture_cv.notify_all();
        NotifyEffects();
        // The real handler clears the physical-key suppression state that
        // Shutdown would otherwise release with SendInput.
        ReleaseTestKeys();
        if (cancel_on_teardown_)
            VoiceInput::CancelRecording();
        // Shutdown waits for every recognition future, so leaving the fixture
        // scope is a real "the future finished" barrier.
        VoiceInput::Shutdown();
    }

    ServiceFixture(const ServiceFixture &) = delete;
    ServiceFixture &operator=(const ServiceFixture &) = delete;

  private:
    bool cancel_on_teardown_ = true;
};

enum class QueuedStartEnd
{
    Stop,
    Cancel,
    ImeReactivation
};

void VerifyQueuedLockedStartIsRetired(QueuedStartEnd action)
{
    ServiceFixture fixture;
    PreparePendingAsr();

    // Park on the next idle checkpoint after a disabled sentinel Start. The
    // earlier ASR stays blocked in Finish throughout the queued-start race.
    REQUIRE(QueueDisabledStartBarrier());
    PauseNextIdleQueue();
    ReleaseConfigRead();
    REQUIRE(WaitQueuePaused());
    SetConfig(TestDoubaoConfig(true, true));
    ResetCapture();
    AllowCaptureStart(); // A broken build must report an attempt, not hang.

    BeginGesture(Gesture::RAlt);
    PressSpace();
    EndGesture(Gesture::RAlt); // Pending lock suppresses a hold-release Stop.
    PumpMessages();            // Start -> Lock, both still waiting for the control thread.
    switch (action)
    {
    case QueuedStartEnd::Stop:
        VoiceInput::StopRecording();
        break;
    case QueuedStartEnd::Cancel:
        VoiceInput::CancelRecording();
        break;
    case QueuedStartEnd::ImeReactivation:
        VoiceInput::SetImeActive(false);
        VoiceInput::SetImeActive(true);
        break;
    }

    REQUIRE(ReleaseQueueAndWaitIdle());
    REQUIRE(!CaptureEntered());
    REQUIRE(!VoiceInput::IsRecording());
    ReleaseDoubaoFinish();
    REQUIRE(WaitForResult());
    REQUIRE_EQ(DeliveredText(), std::wstring(kFixedAsrText));
    VerifyNextHoldStartsAndStops();
}

void VerifyDelayedRecordingMessage(bool toggle, bool during_startup)
{
    ServiceFixture fixture;
    StartGesture(Gesture::RAlt);
    AllowCaptureStart();
    REQUIRE(WaitRecording());
    const int before_cancel = CaptureStopCount();
    if (toggle)
        PressCtrlF9();
    else
        EndGesture(Gesture::RAlt);
    // Leave A's window message pending while direct API calls cancel A and
    // start B. None of the waits below dispatch that message early.
    VoiceInput::CancelRecording();
    REQUIRE(WaitWithoutPumping(
        [before_cancel] { return CaptureStopCount() > before_cancel && !VoiceInput::IsRecording(); }));
    ResetCapture();
    VoiceInput::StartRecording();
    REQUIRE(WaitWithoutPumping(CaptureEntered));
    const int before_delivery = CaptureStopCount();
    if (!during_startup)
    {
        PauseNextIdleQueue();
        AllowCaptureStart();
        REQUIRE(WaitQueuePaused());
    }

    PumpMessages();
    // A harmless Start supplies a queue checkpoint even when the stale
    // message is rejected without waking the control thread.
    VoiceInput::StartRecording();
    if (during_startup)
    {
        PauseNextIdleQueue();
        AllowCaptureStart();
        REQUIRE(WaitQueuePaused());
    }
    else
        REQUIRE(ReleaseQueueAndWaitIdle());
    REQUIRE(VoiceInput::IsRecording());
    REQUIRE_EQ(CaptureStopCount(), before_delivery);
    ReleaseQueue();
}
} // namespace

TEST_CASE(delayed_stop_message_does_not_stop_new_recording)
{
    VerifyDelayedRecordingMessage(false, false);
}

TEST_CASE(delayed_toggle_message_does_not_stop_new_recording)
{
    VerifyDelayedRecordingMessage(true, false);
}

TEST_CASE(delayed_stop_message_does_not_abort_new_start)
{
    VerifyDelayedRecordingMessage(false, true);
}

TEST_CASE(delayed_toggle_message_does_not_abort_new_start)
{
    VerifyDelayedRecordingMessage(true, true);
}

TEST_CASE(queued_start_lock_stop_retires_start_and_preserves_asr)
{
    VerifyQueuedLockedStartIsRetired(QueuedStartEnd::Stop);
}

TEST_CASE(queued_start_lock_cancel_retires_start_and_preserves_asr)
{
    VerifyQueuedLockedStartIsRetired(QueuedStartEnd::Cancel);
}

TEST_CASE(queued_start_lock_ime_reactivation_retires_start_and_preserves_asr)
{
    VerifyQueuedLockedStartIsRetired(QueuedStartEnd::ImeReactivation);
}

// Start is still initializing when Space requests the lock and the hold
// shortcut is released. The start must commit, and the next hold must choose
// Stop rather than leaving a lock with no recording.
TEST_CASE(space_during_startup_locks_and_next_gesture_stops)
{
    ServiceFixture fixture;
    StartGesture(Gesture::RAlt);

    PressSpace();
    PressSpace();
    PumpMessages();
    EndGesture(Gesture::RAlt);
    PumpMessages();

    AllowCaptureStart();
    REQUIRE(WaitRecording());
    REQUIRE(WaitFor([] { return g_actions_true_count.load() >= 2; }));
    REQUIRE(g_actions_visible.load());

    const int stop_before = CaptureStopCount();
    const int hides_before = g_hide_count.load();
    BeginGesture(Gesture::RAlt);
    PumpMessages();
    EndGesture(Gesture::RAlt);
    REQUIRE(WaitNormalStop(stop_before, hides_before));
    REQUIRE(!VoiceInput::IsRecording());
    // Start + one Lock + Stop's batch-recognition state.
    REQUIRE_EQ(g_actions_true_count.load(), 3);

    // A fresh hold starts and its release stops: no stale lock remains.
    ResetObservations();
    StartGesture(Gesture::RAlt);
    AllowCaptureStart();
    REQUIRE(WaitRecording());
    const int second_stop = CaptureStopCount();
    const int second_hides = g_hide_count.load();
    EndGesture(Gesture::RAlt);
    REQUIRE(WaitNormalStop(second_stop, second_hides));
    REQUIRE(!VoiceInput::IsRecording());
}

// A Start that fails before touching the microphone must not leave a queued
// Lock that makes the next hold choose Stop.
TEST_CASE(failed_start_with_queued_lock_does_not_lock_next_gesture)
{
    ServiceFixture fixture;
    SetConfig(TestConfig(false, true));
    BlockNextConfigRead();

    // The public Start (not a hook call) takes the config gate.
    VoiceInput::StartRecording();
    REQUIRE(WaitConfigReadEntered());

    ResetCapture();
    BeginGesture(Gesture::RAlt); // queues a second, disabled Start
    PumpMessages();
    PressSpace(); // pending ticket
    PumpMessages();
    const int before_release = g_config_read_count.load();
    ReleaseConfigRead();
    // The queued Start reads the disabled config first; the Lock follows it.
    REQUIRE(WaitConfigReadAfter(before_release));

    // A Start queued after the Lock reaches the same read only after the Lock
    // has run, so this wait crosses the queue execution point in order.
    const int before_sentinel = g_config_read_count.load();
    VoiceInput::StartRecording();
    REQUIRE(WaitConfigReadAfter(before_sentinel));
    EndGesture(Gesture::RAlt);
    SetConfig(TestConfig(true, true));

    for (int i = 0; i < 3; ++i)
    {
        StartGesture(Gesture::RAlt);
        AllowCaptureStart();
        REQUIRE(WaitRecording());
        const int stop_before = CaptureStopCount();
        const int hides_before = g_hide_count.load();
        EndGesture(Gesture::RAlt);
        REQUIRE(WaitNormalStop(stop_before, hides_before));
        REQUIRE(!VoiceInput::IsRecording());
    }
}

// Cancelling a starting capture must invalidate its own queued Lock before a
// new Start exists. The stale Lock arriving after that Start must not lock the
// new recording, and the next hold must start rather than post Stop.
TEST_CASE(external_cancel_with_queued_lock_does_not_lock_next_gesture)
{
    ServiceFixture fixture;
    StartGesture(Gesture::RAlt);

    PressSpace();                  // pending ticket; Lock message not dispatched
    VoiceInput::CancelRecording(); // carries that ticket
    VoiceInput::StartRecording();  // new Start before the old Lock is dispatched
    PumpMessages();                // dispatch the old Lock behind the new Start
    const int stop_before = CaptureStopCount();
    AllowCaptureStart(); // abort first Start, cancel, then start again
    REQUIRE(WaitRecording());

    const int hides_before = g_hide_count.load();
    EndGesture(Gesture::RAlt); // the stale Lock must not keep this locked
    REQUIRE(WaitNormalStop(stop_before, hides_before));
    REQUIRE(!VoiceInput::IsRecording());

    for (int i = 0; i < 2; ++i)
    {
        StartGesture(Gesture::RAlt);
        AllowCaptureStart();
        REQUIRE(WaitRecording());
        const int loop_stop = CaptureStopCount();
        const int loop_hides = g_hide_count.load();
        EndGesture(Gesture::RAlt);
        REQUIRE(WaitNormalStop(loop_stop, loop_hides));
        REQUIRE(!VoiceInput::IsRecording());
    }
}

// Without Space, releasing the hold shortcut still aborts a starting capture
// and stops a committed recording.
TEST_CASE(plain_hold_release_aborts_startup_and_stops_recording)
{
    ServiceFixture fixture;
    StartGesture(Gesture::RAlt);

    const int abort_stop = CaptureStopCount();
    EndGesture(Gesture::RAlt);
    // Deliver Stop while capture.start() is still gated, so this is the abort
    // path rather than a race with a just-committed recording.
    PumpMessages();
    AllowCaptureStart();
    REQUIRE(WaitFor([abort_stop] { return CaptureStopCount() > abort_stop; }));
    REQUIRE(!VoiceInput::IsRecording());

    StartGesture(Gesture::RAlt);
    AllowCaptureStart();
    REQUIRE(WaitRecording());
    const int stop_before = CaptureStopCount();
    const int hides_before = g_hide_count.load();
    EndGesture(Gesture::RAlt);
    REQUIRE(WaitNormalStop(stop_before, hides_before));
    REQUIRE(!VoiceInput::IsRecording());
}

// Ctrl+F9 toggles through the same hook and control queue.
TEST_CASE(ctrl_f9_toggle_starts_and_stops)
{
    ServiceFixture fixture;
    ResetCapture();
    PressCtrlF9();
    PumpMessages();
    REQUIRE(WaitCaptureEntered());
    AllowCaptureStart();
    REQUIRE(WaitRecording());

    const int stop_before = CaptureStopCount();
    const int hides_before = g_hide_count.load();
    PressCtrlF9();
    PumpMessages();
    REQUIRE(WaitNormalStop(stop_before, hides_before));
    REQUIRE(!VoiceInput::IsRecording());
}

// All three supported hold shortcuts share the hook's gesture lifecycle.
TEST_CASE(hold_shortcuts_share_gesture_lifecycle)
{
    ServiceFixture fixture;
    for (const Gesture gesture : {Gesture::RAlt, Gesture::CtrlWin, Gesture::RCtrlRAlt})
    {
        StartGesture(gesture);
        AllowCaptureStart();
        REQUIRE(WaitRecording());
        const int stop_before = CaptureStopCount();
        const int hides_before = g_hide_count.load();
        EndGesture(gesture);
        REQUIRE(WaitNormalStop(stop_before, hides_before));
        REQUIRE(!VoiceInput::IsRecording());
    }
}

// A Lock still waiting in the message queue is a real request: the next hold
// must end that recording instead of starting another one.
TEST_CASE(pending_lock_makes_next_hold_stop)
{
    ServiceFixture fixture;
    StartGesture(Gesture::RAlt);
    AllowCaptureStart();
    REQUIRE(WaitRecording());

    PressSpace();              // pending; do not dispatch the Lock yet
    EndGesture(Gesture::RAlt); // pending must not be cancelled on release
    const int stop_before = CaptureStopCount();
    const int hides_before = g_hide_count.load();

    BeginGesture(Gesture::RAlt); // pending -> Stop, not Start
    PumpMessages();              // Lock then Stop
    REQUIRE(WaitNormalStop(stop_before, hides_before));
    REQUIRE(!VoiceInput::IsRecording());
    EndGesture(Gesture::RAlt);

    // A fresh hold starts and stops normally.
    StartGesture(Gesture::RAlt);
    AllowCaptureStart();
    REQUIRE(WaitRecording());
    const int next_stop = CaptureStopCount();
    const int next_hides = g_hide_count.load();
    EndGesture(Gesture::RAlt);
    REQUIRE(WaitNormalStop(next_stop, next_hides));
    REQUIRE(!VoiceInput::IsRecording());
}

// A Stop created for an older request must not clear a newer pending Space
// request when it is finally executed before that request's Lock.
TEST_CASE(stale_stop_does_not_clear_next_space_request)
{
    ServiceFixture fixture;
    StartGesture(Gesture::RAlt); // first Start is gated in capture.start()

    PressSpace();                    // pending T1
    PumpMessages();                  // Lock T1 queued behind the blocked Start
    VoiceInput::StopRecording();     // Stop T1, still queued behind the Start
    VoiceInput::SetImeActive(false); // resets all lock state; queues Cancel 0
    VoiceInput::SetImeActive(true);
    ResetObservations();

    BeginGesture(Gesture::RAlt); // state 0 -> Start
    PressSpace();                // pending T2
    PumpMessages();              // dispatch Start and Lock T2
    AllowCaptureStart();         // abort first Start, then Cancel, Start, Lock

    REQUIRE(WaitRecording());
    REQUIRE(WaitFor([] { return g_actions_true_count.load() >= 2; }));
    REQUIRE(g_actions_visible.load());

    const int stop_before = CaptureStopCount();
    const int hides_before = g_hide_count.load();
    EndGesture(Gesture::RAlt); // applied lock: release must not stop

    // The lock is still applied, so the next hold ends the recording instead
    // of starting another one; its stop completion is the evidence.
    BeginGesture(Gesture::RAlt);
    PumpMessages();
    REQUIRE(WaitNormalStop(stop_before, hides_before));
    REQUIRE(!VoiceInput::IsRecording());
    EndGesture(Gesture::RAlt);

    StartGesture(Gesture::RAlt);
    AllowCaptureStart();
    REQUIRE(WaitRecording());
    const int next_stop = CaptureStopCount();
    const int next_hides = g_hide_count.load();
    EndGesture(Gesture::RAlt);
    REQUIRE(WaitNormalStop(next_stop, next_hides));
    REQUIRE(!VoiceInput::IsRecording());
}

// Switching IMEs while a Space-locked start is still initializing must abort
// that start instead of committing the recording after deactivation.
TEST_CASE(ime_deactivation_cancels_pending_space_locked_start)
{
    ServiceFixture fixture;
    StartGesture(Gesture::RAlt);

    PressSpace();
    PumpMessages();
    EndGesture(Gesture::RAlt);
    PumpMessages();
    const int stop_before = CaptureStopCount();
    VoiceInput::SetImeActive(false);
    AllowCaptureStart();
    REQUIRE(WaitFor([stop_before] { return CaptureStopCount() > stop_before; }));
    REQUIRE(!VoiceInput::IsRecording());
}

// Disabling voice input while a Space-locked start is still initializing must
// abort that start, and the next hold must work after re-enabling it.
TEST_CASE(refresh_disabled_aborts_starting_space_locked_capture)
{
    ServiceFixture fixture;
    StartGesture(Gesture::RAlt);

    PressSpace();
    PumpMessages();
    EndGesture(Gesture::RAlt); // pending lock: release posts no Stop

    SetConfig(TestConfig(false, true));
    const int stop_before = CaptureStopCount();
    VoiceInput::RefreshKeyboardHook();
    AllowCaptureStart();
    REQUIRE(WaitFor([stop_before] { return CaptureStopCount() > stop_before; }));
    REQUIRE(!VoiceInput::IsRecording());

    SetConfig(TestConfig(true, true));
    StartGesture(Gesture::RAlt);
    AllowCaptureStart();
    REQUIRE(WaitRecording());
    const int next_stop = CaptureStopCount();
    const int next_hides = g_hide_count.load();
    EndGesture(Gesture::RAlt);
    REQUIRE(WaitNormalStop(next_stop, next_hides));
    REQUIRE(!VoiceInput::IsRecording());
}

// A lock from the previous hold must be gone before Ctrl+F9 restarts within
// the same physical hold, so releasing the key stops the new recording.
TEST_CASE(ctrl_f9_restart_with_held_shortcut_stops_on_release)
{
    ServiceFixture fixture;
    StartGesture(Gesture::RAlt);
    AllowCaptureStart();
    REQUIRE(WaitRecording());

    PressSpace();
    PumpMessages();
    REQUIRE(WaitFor([] { return g_actions_true_count.load() >= 2; }));

    const int first_stop = CaptureStopCount();
    const int first_hides = g_hide_count.load();
    PressCtrlF9();
    REQUIRE(WaitNormalStop(first_stop, first_hides));
    REQUIRE(!VoiceInput::IsRecording());

    ResetCapture();
    PressCtrlF9();
    REQUIRE(WaitCaptureEntered());
    AllowCaptureStart();
    REQUIRE(WaitRecording());

    const int second_stop = CaptureStopCount();
    const int second_hides = g_hide_count.load();
    EndGesture(Gesture::RAlt);
    REQUIRE(WaitNormalStop(second_stop, second_hides));
    REQUIRE(!VoiceInput::IsRecording());
}

// A Stop arriving after recording ended but while Finish still blocks the ASR
// worker was filtered by the legacy idle rule; the recognized text must commit.
TEST_CASE(stop_during_pending_asr_finish_commits_result)
{
    ServiceFixture fixture;
    PreparePendingAsr();
    VoiceInput::StopRecording();
    ExpectPendingAsrCommit();
}

// Same phase for the public Cancel entry: the legacy idle entry filter dropped
// it, so the already-finished audio still produced the fixed text.
TEST_CASE(cancel_during_pending_asr_finish_commits_result)
{
    ServiceFixture fixture;
    PreparePendingAsr();
    VoiceInput::CancelRecording();
    ExpectPendingAsrCommit();
}

// Switching IMEs while Finish is pending must not invalidate the ASR result.
TEST_CASE(ime_deactivation_during_pending_asr_finish_commits_result)
{
    ServiceFixture fixture;
    PreparePendingAsr();
    VoiceInput::SetImeActive(false);
    // Reactivate so the config-read sentinel can run; the deactivation's Cancel
    // is already enqueued ahead of it.
    VoiceInput::SetImeActive(true);
    ExpectPendingAsrCommit();
}

// Disabling voice input through Refresh while Finish is pending must not
// invalidate the ASR result.
TEST_CASE(disabled_refresh_during_pending_asr_finish_commits_result)
{
    ServiceFixture fixture;
    PreparePendingAsr();
    SetConfig(TestConfig(false, true));
    VoiceInput::RefreshKeyboardHook();
    ExpectPendingAsrCommit();
}

// The overlay's own Cancel is the one legacy path that intentionally invalidated
// a pending result: its callback bumps g_voice_session before the command runs.
TEST_CASE(overlay_cancel_during_pending_asr_finish_discards_result)
{
    // No teardown Cancel: leaving the fixture scope is the only event after the
    // release, and its Shutdown waits for the recognition future to finish. The
    // commit decision therefore cannot be masked by an idle teardown Cancel that
    // bumps g_voice_session. The pending ASR has no capture/start to cancel.
    {
        ServiceFixture fixture(false);
        PreparePendingAsr();
        REQUIRE(InvokeOverlayCancel());
        PumpMessages(); // dispatch the callback's Cancel before the sentinel Start

        const bool barriered = QueueDisabledStartBarrier();
        ReleaseConfigRead();
        ReleaseDoubaoFinish();
        REQUIRE(barriered);
    }
    REQUIRE_EQ(ResultCount(), 0);
}

// Cancel while capture is already running stops the device and discards the
// buffered audio; it must never move on to Finish/recognition.
TEST_CASE(cancel_during_capturing_stops_without_finish_or_commit)
{
    int stop_before = 0;
    {
        // Without the teardown Cancel, only the action's own command can stop
        // this capture; a filtered Cancel leaves the recording for Shutdown's
        // Stop, which would enter Finish and fail the negative assertions.
        ServiceFixture fixture(false);
        PrepareCapturingAudio();
        stop_before = CaptureStopCount();
        VoiceInput::CancelRecording();
    }
    REQUIRE(CaptureStopCount() > stop_before);
    REQUIRE(!VoiceInput::IsRecording());
    REQUIRE(!DoubaoFinishEntered());
    REQUIRE_EQ(ResultCount(), 0);
}

// IME deactivation while capture is running discards that recording, exactly
// like an explicit Cancel, and never starts recognition.
TEST_CASE(ime_deactivation_during_capturing_discards_without_finish_or_commit)
{
    int stop_before = 0;
    {
        ServiceFixture fixture(false);
        PrepareCapturingAudio();
        stop_before = CaptureStopCount();
        VoiceInput::SetImeActive(false);
    }
    REQUIRE(CaptureStopCount() > stop_before);
    REQUIRE(!VoiceInput::IsRecording());
    REQUIRE(!DoubaoFinishEntered());
    REQUIRE_EQ(ResultCount(), 0);
}

// Disabling voice input while capturing is the legacy "Stop" path: the
// recording is finalized, Finish is entered, and the recognized text commits.
TEST_CASE(disabled_refresh_during_capturing_stops_and_commits_result)
{
    ServiceFixture fixture;
    PrepareCapturingAudio();
    SetConfig(TestDoubaoConfig(false, true));
    VoiceInput::RefreshKeyboardHook();
    REQUIRE(WaitDoubaoFinishEntered());
    REQUIRE(!VoiceInput::IsRecording());
    ReleaseDoubaoFinish();
    REQUIRE(WaitForResult());
    REQUIRE_EQ(DeliveredText(), std::wstring(kFixedAsrText));
}

// Releasing the hold shortcut while Start is still inside GetConfig must abort
// that start: it may neither begin recording nor touch capture.start().
TEST_CASE(hold_release_during_config_read_does_not_start_capture)
{
    ServiceFixture fixture;
    ResetCapture();
    BlockNextConfigRead();
    BeginGesture(Gesture::RAlt);
    PumpMessages();
    REQUIRE(WaitConfigReadEntered());

    EndGesture(Gesture::RAlt);
    PumpMessages();
    ReleaseConfigRead();

    const bool barriered = QueueToggleBarrierAllowingBrokenCapture();
    const bool capture_started = CaptureEntered();
    ReleaseConfigRead();
    REQUIRE(barriered);
    REQUIRE(!capture_started);
    REQUIRE(!VoiceInput::IsRecording());
    VerifyNextHoldStartsAndStops();
}

// A Cancel issued while Start is still inside GetConfig must not let that start
// reach capture.start().
TEST_CASE(cancel_during_config_read_does_not_start_capture)
{
    ServiceFixture fixture;
    ResetCapture();
    BlockNextConfigRead();
    BeginGesture(Gesture::RAlt);
    PumpMessages();
    REQUIRE(WaitConfigReadEntered());

    VoiceInput::CancelRecording();
    EndGesture(Gesture::RAlt);
    PumpMessages();
    ReleaseConfigRead();

    const bool barriered = QueueToggleBarrierAllowingBrokenCapture();
    const bool capture_started = CaptureEntered();
    ReleaseConfigRead();
    REQUIRE(barriered);
    REQUIRE(!capture_started);
    REQUIRE(!VoiceInput::IsRecording());
    VerifyNextHoldStartsAndStops();
}

// Deactivating the IME while Start is inside GetConfig must latch the abort even
// if the IME is reactivated before the config read returns.
TEST_CASE(ime_deactivation_during_config_read_does_not_start_capture)
{
    ServiceFixture fixture;
    ResetCapture();
    BlockNextConfigRead();
    BeginGesture(Gesture::RAlt);
    PumpMessages();
    REQUIRE(WaitConfigReadEntered());

    VoiceInput::SetImeActive(false);
    VoiceInput::SetImeActive(true);
    EndGesture(Gesture::RAlt);
    PumpMessages();
    ReleaseConfigRead();

    const bool barriered = QueueToggleBarrierAllowingBrokenCapture();
    const bool capture_started = CaptureEntered();
    ReleaseConfigRead();
    REQUIRE(barriered);
    REQUIRE(!capture_started);
    REQUIRE(!VoiceInput::IsRecording());
    VerifyNextHoldStartsAndStops();
}

// A Toggle that starts a recording and a Stop issued while its GetConfig is
// still blocked must not leave the toggle's Start running to capture.start().
TEST_CASE(toggle_stop_during_config_read_does_not_start_capture)
{
    ServiceFixture fixture;
    ResetCapture();
    BlockNextConfigRead();
    VoiceInput::ToggleRecording();
    REQUIRE(WaitConfigReadEntered());

    VoiceInput::StopRecording();
    ReleaseConfigRead();

    const bool barriered = QueueToggleBarrierAllowingBrokenCapture();
    const bool capture_started = CaptureEntered();
    ReleaseConfigRead();
    REQUIRE(barriered);
    REQUIRE(!capture_started);
    REQUIRE(!VoiceInput::IsRecording());
    VerifyNextHoldStartsAndStops();
}

namespace test
{
std::vector<TestCase> &registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

Registrar::Registrar(const char *name, std::function<void()> fn)
{
    registry().push_back({name, std::move(fn)});
}
} // namespace test

int main()
{
    g_test_thread_id = std::this_thread::get_id();
    int failures = 0;
    for (const auto &test_case : test::registry())
    {
        try
        {
            test_case.fn();
            std::cout << "[PASS] " << test_case.name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cerr << "[FAIL] " << test_case.name << ": " << error.what() << '\n';
        }
        catch (...)
        {
            ++failures;
            std::cerr << "[FAIL] " << test_case.name << ": unknown exception\n";
        }
    }
    if (failures != 0)
    {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << test::registry().size() << " test(s) passed\n";
    return 0;
}

// ---------------------------------------------------------------------------
// External-effect substitutes. The service translation unit is the real one.
// ---------------------------------------------------------------------------

VoiceInputConfig GetConfiguredVoiceInput()
{
    std::unique_lock<std::mutex> lock(g_config_mutex);
    const VoiceInputConfig snapshot = g_config;
    // Count only after the snapshot is fixed: a waiter may then change the
    // config without changing what this read returns.
    ++g_config_read_count;
    NotifyEffects();
    if (g_block_next_config_read && std::this_thread::get_id() != g_test_thread_id)
    {
        // Only the control thread's one read is gated; test-thread hook calls
        // and RefreshKeyboardHook must keep working while it is parked.
        g_block_next_config_read = false;
        g_config_read_entered = true;
        g_config_read_waiting = true;
        g_config_cv.notify_all();
        g_config_cv.wait(lock, [] { return g_config_read_release; });
        // Exit before a following BlockNextConfigRead may reset g_config_read_release.
        g_config_read_waiting = false;
        g_config_cv.notify_all();
    }
    return snapshot;
}

namespace CommonUtils
{
std::wstring get_ime_data_path_w()
{
    return {};
}
} // namespace CommonUtils

std::wstring string_to_wstring(const std::string &text)
{
    if (text.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

namespace NetworkProxy
{
void ApplyToCurl(CURL *)
{
}
} // namespace NetworkProxy

namespace DiagnosticLog
{
bool IsEnabled()
{
    return false;
}

void Write(const std::wstring &)
{
}
} // namespace DiagnosticLog

namespace mvi_utils
{
HMONITOR GetForegroundMonitor()
{
    return nullptr;
}
} // namespace mvi_utils

namespace VoiceInput
{
namespace Detail
{
void ControlQueueCheckpoint(bool idle)
{
    std::unique_lock<std::mutex> lock(g_queue_gate_mutex);
    if (!idle)
        return;
    ++g_queue_idle_epoch;
    g_queue_paused = g_pause_queue;
    g_queue_gate_cv.notify_all();
    g_queue_gate_cv.wait(lock, [] { return !g_pause_queue; });
    g_queue_paused = false;
}
} // namespace Detail

void MuteOtherSystemAudio()
{
}

void RestoreOtherSystemAudio()
{
}
// Keep this fixture independent of the desktop focus. Finished recognition
// reaches the result card; the same delivery observations cover both routes.
bool InitializeTargetTracking()
{
    return true;
}
void ShutdownTargetTracking()
{
}
Target CaptureTarget()
{
    return {};
}
bool IsTargetCurrentFast(const Target &)
{
    return false;
}
} // namespace VoiceInput

PipeClientActivation GetActivePipeClient()
{
    PipeClientActivation active;
    active.client_id = 7;
    active.epoch = 11;
    return active;
}

bool SendToTsfWorkerThreadClientViaNamedpipe(uint64_t, uint64_t, UINT msg_type, const std::wstring &text)
{
    if (msg_type == Global::DataFromServerMsgTypeToTsfWorkerThread::InsertText)
    {
        {
            std::lock_guard<std::mutex> lock(g_commit_mutex);
            g_inserted_text += text;
        }
        ++g_insert_text_count;
        NotifyEffects();
    }
    return true;
}

bool SendVoiceCompositionToTsfWorker(uint64_t, uint64_t, UINT, const std::wstring &)
{
    return true;
}

namespace metasequoia::voice
{
struct AudioCapture::Impl
{
};

AudioCapture::AudioCapture() = default;
AudioCapture::~AudioCapture() = default;

bool AudioCapture::start(AudioCallback callback)
{
    std::unique_lock<std::mutex> lock(g_capture_mutex);
    g_capture_entered = true;
    g_capture_cv.notify_all();
    NotifyEffects();
    g_capture_cv.wait(lock, [] { return g_capture_release; });
    g_audio_callback = std::move(callback);
    return true;
}

void AudioCapture::stop()
{
    {
        std::lock_guard<std::mutex> lock(g_capture_mutex);
        ++g_capture_stop_count;
    }
    NotifyEffects();
}

bool AudioCapture::callback_failed() const
{
    return false;
}
} // namespace metasequoia::voice

WaveOverlay::WaveOverlay() = default;
WaveOverlay::~WaveOverlay() = default;

bool WaveOverlay::init(HINSTANCE, std::function<void(Action)> action_handler)
{
    {
        std::lock_guard<std::mutex> lock(g_overlay_mutex);
        g_overlay_action_handler = std::move(action_handler);
    }
    return true;
}

void WaveOverlay::set_anchor(const VoiceOverlayAnchor &)
{
}

void WaveOverlay::shutdown()
{
}

void WaveOverlay::show()
{
    NotifyEffects();
}

void WaveOverlay::hide()
{
    ++g_hide_count;
    NotifyEffects();
}

void WaveOverlay::set_listening(bool)
{
}

void WaveOverlay::set_input_level(float)
{
}

void WaveOverlay::set_transcript(const std::wstring &)
{
}

void WaveOverlay::set_show_transcript(bool)
{
}

void WaveOverlay::set_compact_status(CompactStatus)
{
}

void WaveOverlay::set_actions_visible(bool visible)
{
    g_actions_visible = visible;
    if (visible)
        ++g_actions_true_count;
    NotifyEffects();
}

VoiceResultCard::VoiceResultCard() = default;
VoiceResultCard::~VoiceResultCard() = default;
bool VoiceResultCard::init(HINSTANCE)
{
    return true;
}
void VoiceResultCard::shutdown()
{
}
void VoiceResultCard::set_anchor(const VoiceOverlayAnchor &)
{
}
void VoiceResultCard::reset(std::uint64_t session)
{
    model_.reset(session);
}
void VoiceResultCard::show(std::uint64_t session, const std::wstring &text)
{
    if (!model_.show(session, text))
        return;
    {
        std::lock_guard<std::mutex> lock(g_commit_mutex);
        g_inserted_text += text;
    }
    ++g_insert_text_count;
    NotifyEffects();
}

CuePlayer::CuePlayer() = default;
CuePlayer::~CuePlayer() = default;

bool CuePlayer::init(const std::wstring &, const std::wstring &, bool)
{
    return true;
}

void CuePlayer::shutdown()
{
}

void CuePlayer::play_start()
{
}

void CuePlayer::play_end()
{
}

DoubaoAsrClient::DoubaoAsrClient(std::string, bool, std::string, std::string, std::string, bool, bool, bool,
                                 std::string, TranscriptCallback)
{
}

DoubaoAsrClient::~DoubaoAsrClient() = default;

bool DoubaoAsrClient::Start()
{
    return true;
}

void DoubaoAsrClient::PushFloatSamples(const float *, std::size_t)
{
}

std::string DoubaoAsrClient::Finish()
{
    {
        std::lock_guard<std::mutex> lock(g_doubao_mutex);
        g_doubao_finish_entered = true;
    }
    g_doubao_cv.notify_all();
    NotifyEffects();
    std::unique_lock<std::mutex> lock(g_doubao_mutex);
    g_doubao_cv.wait(lock, [] { return g_doubao_finish_release; });
    return "asr-fixed-result";
}

std::string DoubaoAsrClient::LastError() const
{
    return {};
}

void DoubaoAsrClient::Cancel()
{
}

std::string DoubaoAsrClient::TestCredentials(const std::string &, bool, const std::string &, const std::string &,
                                             const std::string &)
{
    return {};
}
