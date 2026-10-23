// The generated control, cancellation, chunk delivery and finalization bodies
// are production code. Only device/network/target/pipe/UI boundaries are doubles.
#include "voice-input/voice_commit_policy.h"
#include "voice-input/voice_control_policy.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
            throw std::runtime_error(#condition);                                                                      \
    } while (false)
#define DIAG_LOGF(...) ((void)0)
using UINT = unsigned;
using ControlCommand = VoiceInput::ControlPolicy::Command;
using ControlRequest = VoiceInput::ControlPolicy::Request;
constexpr unsigned kSampleRate = 16000;
namespace Global::DataFromServerMsgTypeToTsfWorkerThread
{
constexpr UINT InsertText = 1, CancelVoiceComposition = 2, CommitVoiceComposition = 3;
}
struct FanyImeNamedpipeDataToTsfWorkerThread
{
    wchar_t data[8];
};
struct VoiceInputConfig
{
    bool end_sound = false;
    std::string commit_mode = "tsf", asr_provider;
};
struct RecognitionResult
{
    std::string text, error;
};
struct Capture
{
    int stops = 0;
    void stop()
    {
        ++stops;
    }
    bool callback_failed()
    {
        return false;
    }
} g_capture;
struct DoubaoAsrClient
{
    std::string Finish()
    {
        return "recognized";
    }
    std::string LastError()
    {
        return {};
    }
    void Cancel()
    {
    }
};
struct WaveOverlay
{
    enum class CompactStatus
    {
        None,
        Recognizing,
        Processing
    };
    void set_listening(bool)
    {
    }
    void set_input_level(float)
    {
    }
    void hide()
    {
    }
    void show()
    {
    }
    void set_compact_status(CompactStatus)
    {
    }
    void set_show_transcript(bool)
    {
    }
    void set_transcript(const std::wstring &)
    {
    }
    void set_actions_visible(bool)
    {
    }
} g_overlay;
struct ResultCard
{
    int shows = 0;
    std::wstring text;
    void reset(std::uint64_t)
    {
        text.clear();
    }
    void show(std::uint64_t, const std::wstring &value)
    {
        ++shows;
        text = value;
    }
} g_result_card;
struct CuePlayer
{
    void shutdown()
    {
    }
    void play_end()
    {
    }
} g_cue_player;
std::mutex g_send_mutex, g_control_mutex, g_mutex;
std::condition_variable g_control_cv;
std::deque<ControlRequest> g_control_commands;
std::atomic<std::uint64_t> g_voice_session{1}, g_dismissed_overlay_session{0};
std::atomic<bool> g_recording{true}, g_starting{false}, g_abort_start{false}, g_stream_inline_this_session{false},
    g_ime_active{true};
std::atomic<std::uintptr_t> g_lock_state{0};
std::atomic<std::size_t> g_recorded_frames{kSampleRate};
VoiceInput::Target g_inline_owner, g_target;
std::uint64_t g_inline_owner_session = 0;
std::wstring g_last_inline_preedit, g_inserted;
std::vector<float> g_samples;
std::unique_ptr<DoubaoAsrClient> g_doubao_asr;
std::vector<std::future<void>> g_network_tasks;
std::function<void()> g_during_capture, g_during_recognition;
bool g_fast_valid = true, g_route_valid = true;
int g_pipe_attempts = 0, g_pipe_fail_at = 0, g_cancels = 0, g_send_input = 0;
std::atomic<int> g_recognitions{0};
namespace VoiceInput
{
bool IsTargetCurrentFast(const Target &target)
{
    return g_fast_valid && SameTarget(target, g_target);
}
} // namespace VoiceInput
VoiceInput::Target CaptureVoiceTarget()
{
    if (auto callback = std::exchange(g_during_capture, {}))
        callback();
    return g_target;
}
bool IsPipeRouteCurrent(const VoiceInput::Target &target)
{
    return g_route_valid && VoiceInput::HasBoundRoute(target);
}
bool SendToTsfWorkerThreadClientViaNamedpipe(std::uint64_t, std::uint64_t, UINT type, const std::wstring &text)
{
    if (type == Global::DataFromServerMsgTypeToTsfWorkerThread::CancelVoiceComposition)
    {
        ++g_cancels;
        return true;
    }
    if (++g_pipe_attempts == g_pipe_fail_at)
        return false;
    g_inserted += text;
    return true;
}
bool SendVoiceCompositionToTsfWorker(std::uint64_t client, std::uint64_t epoch, UINT type, const std::wstring &text)
{
    return SendToTsfWorkerThreadClientViaNamedpipe(client, epoch, type, text);
}
bool FastVerifiedSendInput(const std::wstring &, const VoiceInput::Target &)
{
    ++g_send_input;
    return true;
}
bool CommitCtrlVViaClipboard(const std::wstring &, const VoiceInput::Target &, std::uint64_t)
{
    return true;
}
std::wstring string_to_wstring(const std::string &text)
{
    return {text.begin(), text.end()};
}
VoiceInputConfig GetConfiguredVoiceInput()
{
    return {};
}
void RestoreSystemAudioIfMuted()
{
}
void ShowVoiceNotice(const wchar_t *)
{
}
void Sleep(unsigned)
{
}
RecognitionResult Recognize(const std::vector<float> &, const VoiceInputConfig &)
{
    ++g_recognitions;
    if (g_during_recognition)
        g_during_recognition();
    return {"recognized", {}};
}
bool ShouldPolish(const std::string &, const VoiceInputConfig &)
{
    return false;
}
std::string Polish(const std::string &text, const VoiceInputConfig &)
{
    return text;
}
bool StartRecording()
{
    std::lock_guard<std::mutex> lock(g_send_mutex);
    ++g_voice_session;
    g_inline_owner_session = g_voice_session;
    g_inline_owner = g_target;
    g_recording = true;
    return true;
}
void StopRecording(std::uint64_t, std::uintptr_t = 0);
void CancelRecording(std::uintptr_t = 0);
void CancelRecordingForSession(std::uint64_t, std::uintptr_t = 0);
void DiscardRecording(const VoiceInputConfig &);
#include "voice_service_source.h"

void WaitForRecognition()
{
    for (auto &task : g_network_tasks)
        task.get();
    g_network_tasks.clear();
}
void Reset()
{
    WaitForRecognition();
    g_control_commands.clear();
    g_voice_session = 1;
    g_recording = true;
    g_starting = g_abort_start = g_stream_inline_this_session = false;
    g_lock_state = 0;
    g_lock_state = 0;
    g_recorded_frames = kSampleRate;
    g_capture.stops = 0;
    g_result_card = {};
    g_target = {};
    g_inline_owner = {};
    g_inline_owner_session = 0;
    g_during_capture = g_during_recognition = {};
    g_fast_valid = g_route_valid = true;
    g_pipe_attempts = g_pipe_fail_at = g_cancels = g_send_input = g_recognitions = 0;
    g_inserted.clear();
}
void BindOwner()
{
    g_target.editable = true;
    g_target.foreground = 1;
    g_target.focus = 2;
    g_target.client = 3;
    g_target.epoch = 4;
    g_target.focus_token = 5;
    g_inline_owner = g_target;
    g_inline_owner_session = 1;
    g_stream_inline_this_session = true;
}
void RunPending()
{
    EnqueueControlCommand(ControlCommand::Exit);
    ControlLoop();
}
void TestQueuedStopCannotAdoptCancelledSession()
{
    for (auto command : {ControlCommand::Stop, ControlCommand::Toggle})
    {
        Reset();
        BindOwner();
        EnqueueControlCommand(command);
        EnqueueCancelFromUi();
        CHECK(g_control_commands.front().session_cookie == 1);
        CHECK(g_control_commands.back().session_cookie == 2);
        RunPending();
        WaitForRecognition();
        CHECK(!g_recording && g_capture.stops == 1);
        CHECK(g_recognitions == 0 && g_result_card.shows == 0 && g_pipe_attempts == 0);
        CHECK(g_cancels == 1 && g_inline_owner_session == 0);
    }
}
void TestSessionAndLockFences()
{
    Reset();
    g_lock_state = 3; // Applied ticket 2 belongs to the cancelled recording.
    EnqueueControlCommand(ControlCommand::Toggle, 2);
    EnqueueCancelFromUi();
    CHECK(g_control_commands.front().session_cookie == 1);
    CHECK(g_control_commands.front().lock_ticket == 2);
    CHECK(g_control_commands.back().session_cookie == 2);
    CHECK(g_control_commands.back().lock_ticket == 2);
    RunPending();
    CHECK(!g_recording && g_capture.stops == 1 && g_recognitions == 0);
    CHECK(g_lock_state == 0);

    Reset();
    g_voice_session = 3;
    g_lock_state = 5; // A newer recording owns applied ticket 4.
    StopRecording(1, 2);
    CancelRecordingForSession(2, 2);
    CHECK(g_recording && g_lock_state == 5 && g_capture.stops == 0);

    g_lock_state = 6; // New pending Lock must survive a valid Stop of ticket 2.
    StopRecording(3, 2);
    CHECK(!g_recording && g_lock_state == 6);
    ApplyLockRequest(6);
    CHECK(g_lock_state == 0);
    WaitForRecognition();
}

void TestCancelDuringRecognition()
{
    Reset();
    std::promise<void> entered, release;
    auto started = entered.get_future();
    auto proceed = release.get_future().share();
    g_during_recognition = [&] {
        entered.set_value();
        proceed.wait();
    };
    StopRecording(1);
    const bool running = started.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    EnqueueCancelFromUi();
    RunPending();
    release.set_value();
    WaitForRecognition();
    CHECK(running && g_recognitions == 1);
    CHECK(g_result_card.shows == 0 && g_pipe_attempts == 0);
}
void TestCancelBeforeStopAndQueuedToggles()
{
    Reset();
    EnqueueCancelFromUi();
    EnqueueControlCommand(ControlCommand::Stop);
    RunPending();
    CHECK(!g_recording && g_capture.stops == 1 && g_recognitions == 0);
    for (auto first : {ControlCommand::Start, ControlCommand::Toggle})
    {
        Reset();
        g_recording = false;
        EnqueueControlCommand(first);
        EnqueueControlCommand(ControlCommand::Toggle);
        RunPending();
        WaitForRecognition();
        CHECK(!g_recording && g_recognitions == 1 && g_capture.stops == 1);
    }
}
void TestFinalTargetCaptureCancellation()
{
    Reset();
    g_during_capture = [] { EnqueueCancelFromUi(); };
    CommitRecognizedText("late", {}, false, {}, 1);
    CHECK(g_result_card.shows == 0 && g_pipe_attempts == 0);
    // Even a result that would use the card stays silent once cancelled.
    CommitRecognizedText("late again", {}, false, {}, 1);
    CHECK(g_result_card.shows == 0);
}
void TestStaleCancelPreservesNewRecording()
{
    Reset();
    EnqueueControlCommand(ControlCommand::Start);
    EnqueueCancelFromUi();
    RunPending();
    CHECK(g_recording && g_voice_session == 3 && g_inline_owner_session == 3);
    CHECK(g_capture.stops == 0 && g_cancels == 0);
}
void TestOwnerCancellationFences()
{
    for (int scenario = 0; scenario != 4; ++scenario)
    {
        Reset();
        BindOwner();
        g_recording = false;
        g_voice_session = 2; // UI cookie differs from the owner recording.
        if (scenario == 1)
            g_route_valid = false;
        if (scenario == 2)
            ++g_target.focus;
        if (scenario == 3)
            g_during_capture = [] { g_inline_owner_session = 3; };
        CancelRecordingForSession(2);
        CHECK(g_cancels == (scenario == 0 ? 1 : 0));
        CHECK(g_inline_owner_session == (scenario == 3 ? 3 : 0));
    }
}
void TestTsfPartialDeliveryKeepsFullCard()
{
    Reset();
    BindOwner();
    g_pipe_fail_at = 2;
    const std::string original = "first packet and remaining text";
    CommitRecognizedText(original, {}, false, g_target, 1);
    CHECK(g_pipe_attempts == 2 && g_inserted == L"first p");
    CHECK(g_result_card.shows == 1 && g_result_card.text == string_to_wstring(original));
    CHECK(g_send_input == 0);
}
int main()
{
    try
    {
        TestQueuedStopCannotAdoptCancelledSession();
        TestSessionAndLockFences();
        TestCancelDuringRecognition();
        TestCancelBeforeStopAndQueuedToggles();
        TestFinalTargetCaptureCancellation();
        TestStaleCancelPreservesNewRecording();
        TestOwnerCancellationFences();
        TestTsfPartialDeliveryKeepsFullCard();
        std::cout << "PASS voice service cancellation, owner and partial-delivery regressions\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
