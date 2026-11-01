#include <msime/voice/audio_capture.h>
#include <msime/voice/provider_protocol.h>
#include <msime/voice/stt_service.h>
#include <limits>

#include "voice_input_service.h"
#include "voice_input_gesture.h"
#include "cloud/ai_wire.h"
#include "config/ime_config.h"
#include "doubao_asr_client.h"
#include "log/candidate_diag_log.h"
#include "voice_providers.h"
#include "voice_batch_protocol.h"
#include "ipc/ipc.h"
#include "system_audio_muter.h"
#include "utils/common_utils.h"
#include "wave_overlay.h"
#include "cue_player.h"
#include "voice_target.h"
#include "voice_result_card.h"
#include "voice_commit_policy.h"
#include "voice_control_policy.h"
#include "voice_input_delivery.h"

#include "utils/network_proxy.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef MSIME_VOICE_CONTROL_TEST
namespace VoiceInput::Detail
{
void ControlQueueCheckpoint(bool idle);
}
#endif

namespace
{
constexpr unsigned kSampleRate = 16000;
constexpr long kPolishTimeoutMs = 3000L;
// DeepSeek thinking spends seconds reasoning before it answers; the user opted into that wait.
constexpr long kPolishThinkingTimeoutMs = 30000L;
metasequoia::voice::AudioCapture g_capture;
std::mutex g_mutex;
std::vector<float> g_samples;
std::unique_ptr<DoubaoAsrClient> g_doubao_asr;
std::atomic<std::size_t> g_recorded_frames{0};
std::atomic<std::uint64_t> g_voice_session{0};
std::atomic<std::uint64_t> g_dismissed_overlay_session{0};
std::vector<std::future<void>> g_network_tasks;
std::mutex g_send_mutex;
std::atomic<bool> g_stream_inline_this_session{false};
// Inline composition owner route for the current session; guarded by g_send_mutex.
VoiceInput::Target g_inline_owner;
// Session that bound g_inline_owner; guarded by g_send_mutex. It keeps a
// cancellation from reaching a composition another session bound while the
// cancellation's UIA capture ran outside the lock.
std::uint64_t g_inline_owner_session = 0;
std::wstring g_last_inline_preedit;
std::thread g_control_thread;
std::mutex g_control_mutex;
std::condition_variable g_control_cv;
using ControlCommand = VoiceInput::ControlPolicy::Command;
using ControlRequest = VoiceInput::ControlPolicy::Request;
std::deque<ControlRequest> g_control_commands;
// 0 = no lock; an even ticket = Space asked for a lock the control thread has
// not applied yet; ticket | 1 = applied. Tickets are never reused, so a command
// carrying an old ticket cannot touch a newer request.
std::atomic<std::uintptr_t> g_lock_state{0};
std::atomic<std::uintptr_t> g_next_lock_ticket{2};
std::atomic<bool> g_recording{false};
std::atomic<bool> g_starting{false};
std::atomic<bool> g_abort_start{false};
std::atomic<bool> g_muted_system_audio{false};
std::atomic<bool> g_ime_active{false};
bool g_initialized = false;
WaveOverlay g_overlay;
VoiceResultCard g_result_card;
CuePlayer g_cue_player;
HHOOK g_keyboard_hook = nullptr;
HWND g_message_window = nullptr;
std::atomic<bool> g_ralt_pressed{false};
std::atomic<bool> g_lctrl_pressed{false};
std::atomic<bool> g_rctrl_pressed{false};
std::atomic<bool> g_lwin_pressed{false};
std::atomic<bool> g_rwin_pressed{false};
std::atomic<bool> g_f9_pressed{false};
std::atomic<bool> g_ctrl_f9_consumed{false};
std::atomic<bool> g_suppress_ralt_until_up{false};
std::atomic<bool> g_suppress_win_until_up{false};
enum class HoldShortcut
{
    None,
    RAlt,
    CtrlWin,
    RCtrlRAlt
};
std::atomic<HoldShortcut> g_active_hold_shortcut{HoldShortcut::None};
constexpr UINT kStartRecordingMessage = WM_APP + 181;
constexpr UINT kStopRecordingMessage = WM_APP + 182;
constexpr UINT kToggleRecordingMessage = WM_APP + 183;
constexpr UINT kLockRecordingMessage = WM_APP + 184;
constexpr UINT kCancelRecordingMessage = WM_APP + 185;
constexpr wchar_t kMessageWindowClass[] = L"MetasequoiaImeVoiceInputMessageWindow";

std::wstring ResolveCuePath(const wchar_t *filename)
{
    const std::wstring ime_data = CommonUtils::get_ime_data_path_w();
    if (!ime_data.empty())
    {
        const std::wstring installed = ime_data + L"\\assets\\audios\\" + filename;
        if (GetFileAttributesW(installed.c_str()) != INVALID_FILE_ATTRIBUTES)
            return installed;
    }
    wchar_t executable[MAX_PATH]{};
    const DWORD executable_size = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (!executable_size || executable_size >= MAX_PATH)
        return {};
    std::wstring directory(executable, executable_size);
    const size_t slash = directory.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        directory.resize(slash);
    return directory + L"\\assets\\audios\\" + filename;
}

bool StartRecording();
void StopRecording(std::uint64_t voice_session, std::uintptr_t lock_ticket);
void CancelRecording(std::uintptr_t lock_ticket);
void CancelRecordingForSession(std::uint64_t session_cookie, std::uintptr_t lock_ticket);
void DiscardRecording(const VoiceInputConfig &config);

void MuteSystemAudioIfEnabled(const VoiceInputConfig &config)
{
    if (!config.mute_system_audio)
        return;
    VoiceInput::MuteOtherSystemAudio();
    g_muted_system_audio = true;
}

void RestoreSystemAudioIfMuted()
{
    if (!g_muted_system_audio.exchange(false))
        return;
    VoiceInput::RestoreOtherSystemAudio();
}

std::uintptr_t LockTicketOf(std::uintptr_t state)
{
    return state & ~std::uintptr_t{1};
}

std::uintptr_t CurrentLockTicket()
{
    return LockTicketOf(g_lock_state.load(std::memory_order_acquire));
}

// Clear the pending or applied lock only when it belongs to the request the
// command was created for. A newer Space request always has a newer ticket.
void ClearLockForRequest(std::uintptr_t lock_ticket)
{
    if (lock_ticket == 0)
        return;
    std::uintptr_t state = g_lock_state.load(std::memory_order_acquire);
    while (LockTicketOf(state) == lock_ticket)
    {
        if (g_lock_state.compare_exchange_weak(state, 0, std::memory_order_acq_rel, std::memory_order_acquire))
            return;
    }
}

// A Stop/Cancel executes on the control thread, so an applied lock present at
// its entry belongs to the recording it is ending. Clear that applied lock, or
// the pending request carrying this command's ticket; a newer pending request
// must stay for its own Lock command.
void ClearRecordingLock(std::uintptr_t lock_ticket)
{
    std::uintptr_t state = g_lock_state.load(std::memory_order_acquire);
    while (state != 0 && ((state & std::uintptr_t{1}) != 0 || LockTicketOf(state) == lock_ticket))
    {
        if (g_lock_state.compare_exchange_weak(state, 0, std::memory_order_acq_rel, std::memory_order_acquire))
            return;
    }
}

void EnqueueControlCommand(ControlCommand command, std::uintptr_t lock_ticket = 0, std::uint64_t posted_session = 0)
{
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(g_control_mutex);
        // Recording-time window messages retain their original session. Check
        // before queue policy can remove Starts or abort a newer startup. Use
        // the same lock order as UI cancellation, fencing recording changes too.
        std::unique_lock<std::mutex> send_lock(g_send_mutex, std::defer_lock);
        if (posted_session != 0)
        {
            send_lock.lock();
            if (!g_recording || g_voice_session != posted_session)
            {
                ClearLockForRequest(lock_ticket);
                return;
            }
        }
        if ((command == ControlCommand::Stop || command == ControlCommand::Cancel) && !g_recording && !g_starting)
        {
            // Retire queued starts and this ticket while preserving a finished ASR.
            VoiceInput::ControlPolicy::Enqueue(g_control_commands, ControlRequest{ControlCommand::Stop, 0, lock_ticket},
                                               false, false);
            ClearLockForRequest(lock_ticket);
            g_abort_start = false;
            return;
        }
        // Bind a stop (including a stop-type Toggle) before a UI cancel can
        // advance the session. UI cancellation queues under this same lock.
        const std::uint64_t session_cookie =
            posted_session != 0 ? posted_session
            : command == ControlCommand::Stop || (command == ControlCommand::Toggle && g_recording.load())
                ? g_voice_session.load()
                : 0;
        const auto result =
            VoiceInput::ControlPolicy::Enqueue(g_control_commands, ControlRequest{command, session_cookie, lock_ticket},
                                               g_recording.load(), g_starting.load());
        if (result.abort_start)
            g_abort_start = true;
        notify = result.enqueued;
    }
    if (notify)
        g_control_cv.notify_one();
}

void ApplyLockRequest(std::uintptr_t lock_ticket)
{
    if (lock_ticket == 0)
        return;
    std::uintptr_t expected = lock_ticket;
    if (g_recording)
    {
        if (g_lock_state.compare_exchange_strong(expected, lock_ticket | std::uintptr_t{1}, std::memory_order_acq_rel,
                                                 std::memory_order_acquire))
            g_overlay.set_actions_visible(true);
    }
    else
    {
        g_lock_state.compare_exchange_strong(expected, 0, std::memory_order_acq_rel, std::memory_order_acquire);
    }
}

void ControlLoop()
{
    for (;;)
    {
#ifdef MSIME_VOICE_CONTROL_TEST
        // The test gate runs outside the queue mutex so hook messages and
        // Stop/Cancel can enqueue while the worker is parked before dequeue.
        bool idle;
        {
            std::lock_guard<std::mutex> lock(g_control_mutex);
            idle = g_control_commands.empty();
        }
        VoiceInput::Detail::ControlQueueCheckpoint(idle);
#endif
        ControlRequest request;
        bool starts_recording = false;
        {
            std::unique_lock<std::mutex> lock(g_control_mutex);
            g_control_cv.wait(lock, [] { return !g_control_commands.empty(); });
            request = g_control_commands.front();
            g_control_commands.pop_front();
            // Mark startup in the same critical section that removes the
            // request. A Stop/Cancel arriving while GetConfig or the provider
            // prep still runs must see g_starting instead of an idle gap.
            if ((request.command == ControlCommand::Start ||
                 (request.command == ControlCommand::Toggle && request.session_cookie == 0)) &&
                !g_recording)
            {
                starts_recording = true;
                g_starting = true;
            }
        }
        if (request.command == ControlCommand::Exit)
        {
            g_cue_player.shutdown();
            return;
        }
        if (request.command == ControlCommand::Start)
            StartRecording();
        else if (request.command == ControlCommand::Stop)
            StopRecording(request.session_cookie, request.lock_ticket);
        else if (request.command == ControlCommand::Toggle)
        {
            // The decision was already made when the request was dequeued;
            // only this thread changes g_recording between the two points.
            if (starts_recording)
                StartRecording();
            else
                StopRecording(request.session_cookie != 0 ? request.session_cookie : g_voice_session.load(),
                              request.lock_ticket);
        }
        else if (request.command == ControlCommand::Lock)
            ApplyLockRequest(request.lock_ticket);
        else if (request.command == ControlCommand::Cancel)
        {
            if (request.session_cookie != 0)
                CancelRecordingForSession(request.session_cookie, request.lock_ticket);
            else
                CancelRecording(request.lock_ticket);
        }
        if (starts_recording)
        {
            // Every StartRecording return path lands here. A late Stop/Cancel
            // may have set g_abort_start; that request is queued and will
            // handle the recording, but the flag must not poison the next start.
            std::lock_guard<std::mutex> lock(g_control_mutex);
            g_starting = false;
            g_abort_start = false;
        }
    }
}

bool IsCtrlPressed()
{
    return g_lctrl_pressed || g_rctrl_pressed;
}
bool IsWinPressed()
{
    return g_lwin_pressed || g_rwin_pressed;
}

bool ShouldInstallKeyboardHook(const VoiceInputConfig &config)
{
    return config.enabled &&
           (config.hotkey_ralt || config.hotkey_ctrl_f9 || config.hotkey_ctrl_win || config.hotkey_rctrl_ralt);
}

void ResetKeyboardShortcutState()
{
    g_ralt_pressed = false;
    g_lctrl_pressed = false;
    g_rctrl_pressed = false;
    g_lwin_pressed = false;
    g_rwin_pressed = false;
    g_f9_pressed = false;
    g_ctrl_f9_consumed = false;
    g_suppress_ralt_until_up = false;
    g_suppress_win_until_up = false;
    g_active_hold_shortcut = HoldShortcut::None;
    g_lock_state = 0;
}

bool IsHoldShortcutPressed(HoldShortcut shortcut)
{
    switch (shortcut)
    {
    case HoldShortcut::RAlt:
        return g_ralt_pressed;
    case HoldShortcut::CtrlWin:
        return IsCtrlPressed() && IsWinPressed();
    case HoldShortcut::RCtrlRAlt:
        return g_rctrl_pressed && g_ralt_pressed;
    default:
        return false;
    }
}

// Keep the full 64-bit cookie on both x86 and x64. Startup/idle messages keep
// their existing queue semantics; only an already recording session is bound.
void PostRecordingControlMessage(UINT message, std::uintptr_t lock_ticket)
{
    const std::uint64_t session = g_voice_session.load();
    auto posted_session = std::make_unique<std::uint64_t>(g_recording.load() ? session : 0);
    if (PostMessageW(g_message_window, message, lock_ticket, reinterpret_cast<LPARAM>(posted_session.get())))
        posted_session.release();
}

void ActivateHoldShortcut(HoldShortcut shortcut)
{
    g_active_hold_shortcut = shortcut;
    if (shortcut == HoldShortcut::RAlt || shortcut == HoldShortcut::RCtrlRAlt)
        g_suppress_ralt_until_up = true;
    else if (shortcut == HoldShortcut::CtrlWin)
        g_suppress_win_until_up = true;
    const std::uintptr_t lock_state = g_lock_state.load(std::memory_order_acquire);
    if (lock_state != 0)
        PostRecordingControlMessage(kStopRecordingMessage, LockTicketOf(lock_state));
    else
        PostMessageW(g_message_window, kStartRecordingMessage, 0, 0);
}

void RequestLock()
{
    if (g_lock_state.load(std::memory_order_acquire) != 0)
        return;
    const std::uintptr_t lock_ticket = g_next_lock_ticket.fetch_add(2, std::memory_order_relaxed);
    std::uintptr_t expected = 0;
    if (!g_lock_state.compare_exchange_strong(expected, lock_ticket, std::memory_order_acq_rel,
                                              std::memory_order_acquire))
        return;
    if (!PostMessageW(g_message_window, kLockRecordingMessage, lock_ticket, 0))
        ClearLockForRequest(lock_ticket);
}

void ReleaseHoldShortcut()
{
    // Do not cancel a pending lock here; its Lock command decides whether the
    // recording exists to apply it to. Only a truly unlocked hold stops now.
    if (g_lock_state.load(std::memory_order_acquire) == 0)
        PostRecordingControlMessage(kStopRecordingMessage, 0);
}

void ForceReleaseRAlt()
{
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = VK_RMENU;
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(input));
}

LRESULT CALLBACK VoiceMessageWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case kStartRecordingMessage:
        EnqueueControlCommand(ControlCommand::Start);
        return 0;
    case kStopRecordingMessage:
    case kToggleRecordingMessage: {
        // Posted recording cookies are owned by this handler. A sender that
        // posts without one still gets ordinary queue semantics (session 0).
        const std::unique_ptr<std::uint64_t> posted_session(reinterpret_cast<std::uint64_t *>(lparam));
        const std::uint64_t session = posted_session ? *posted_session : 0;
        EnqueueControlCommand(message == kStopRecordingMessage ? ControlCommand::Stop : ControlCommand::Toggle,
                              static_cast<std::uintptr_t>(wparam), session);
        return 0;
    }
    case kLockRecordingMessage:
        EnqueueControlCommand(ControlCommand::Lock, static_cast<std::uintptr_t>(wparam));
        return 0;
    case kCancelRecordingMessage:
        EnqueueControlCommand(ControlCommand::Cancel, static_cast<std::uintptr_t>(wparam));
        return 0;
    case WM_NCDESTROY: {
        // Undispatched messages still own their cookie when the window closes.
        MSG pending{};
        while (PeekMessageW(&pending, hwnd, kStopRecordingMessage, kToggleRecordingMessage, PM_REMOVE))
            delete reinterpret_cast<std::uint64_t *>(pending.lParam);
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

LRESULT CALLBACK HandleKeyboardHook(int code, WPARAM wparam, LPARAM lparam)
{
    if (code != HC_ACTION)
        return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);
    const auto *key = reinterpret_cast<KBDLLHOOKSTRUCT *>(lparam);
    if (!key)
        return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);
    const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
    const bool up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;

    const VoiceInputConfig config = GetConfiguredVoiceInput();
    const HoldShortcut active_before = g_active_hold_shortcut.load();

    if (key->vkCode == VK_LCONTROL || key->vkCode == VK_RCONTROL)
    {
        auto &state = key->vkCode == VK_LCONTROL ? g_lctrl_pressed : g_rctrl_pressed;
        if (down)
            state = true;
        else if (up)
            state = false;
    }
    else if (key->vkCode == VK_LWIN || key->vkCode == VK_RWIN)
    {
        auto &state = key->vkCode == VK_LWIN ? g_lwin_pressed : g_rwin_pressed;
        if (down)
            state = true;
        else if (up)
            state = false;
    }
    else if (key->vkCode == VK_RMENU)
    {
        if (down)
            g_ralt_pressed = true;
        else if (up)
            g_ralt_pressed = false;
    }

    // 录音期间即使 TIP 已经失去激活，也必须还能用 Esc 收掉这次录音。提示条是
    // HWND_TOPMOST 的独立窗口，不随输入法激活状态消失；而长按热键的松手事件在
    // 切走输入法时就收不到了（下面的 !g_ime_active 分支会把它直接放行），
    // 这时 Esc 是唯一的兜底。
    if (key->vkCode == VK_ESCAPE && g_recording.load())
    {
        if (down)
            PostMessageW(g_message_window, kCancelRecordingMessage, CurrentLockTicket(), 0);
        return 1;
    }

    if (!g_ime_active)
    {
        // A shortcut key-down already consumed while this TIP was active must
        // keep its matching key-up consumed, even if the user switches IMEs.
        const bool suppress_ralt = key->vkCode == VK_RMENU && g_suppress_ralt_until_up;
        const bool suppress_win = (key->vkCode == VK_LWIN || key->vkCode == VK_RWIN) && g_suppress_win_until_up;
        if (up && key->vkCode == VK_RMENU)
            g_suppress_ralt_until_up = false;
        if (up && (key->vkCode == VK_LWIN || key->vkCode == VK_RWIN))
            g_suppress_win_until_up = false;
        if (suppress_ralt || suppress_win)
            return 1;
        return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);
    }

    if (key->vkCode == VK_F9 && config.hotkey_ctrl_f9)
    {
        if (down && !g_f9_pressed.exchange(true) && IsCtrlPressed())
        {
            g_ctrl_f9_consumed = true;
            PostRecordingControlMessage(kToggleRecordingMessage, CurrentLockTicket());
            return 1;
        }
        if (up)
        {
            g_f9_pressed = false;
            if (g_ctrl_f9_consumed.exchange(false))
                return 1;
        }
    }
    else if (key->vkCode == VK_F9 && up)
    {
        g_f9_pressed = false;
        g_ctrl_f9_consumed = false;
    }

    if (active_before == HoldShortcut::None && down)
    {
        if (config.hotkey_rctrl_ralt && key->vkCode == VK_RMENU && g_rctrl_pressed)
            ActivateHoldShortcut(HoldShortcut::RCtrlRAlt);
        else if (config.hotkey_ctrl_win && (key->vkCode == VK_LWIN || key->vkCode == VK_RWIN) && IsCtrlPressed())
            ActivateHoldShortcut(HoldShortcut::CtrlWin);
        else if (config.hotkey_ralt && key->vkCode == VK_RMENU)
            ActivateHoldShortcut(HoldShortcut::RAlt);
    }
    else if (active_before != HoldShortcut::None && up && !IsHoldShortcutPressed(active_before))
    {
        g_active_hold_shortcut = HoldShortcut::None;
        ReleaseHoldShortcut();
    }

    const HoldShortcut active_now = g_active_hold_shortcut.load();
    if (key->vkCode == VK_SPACE && active_now != HoldShortcut::None && config.hotkey_hold_space_lock)
    {
        if (down)
            RequestLock();
        return 1;
    }

    // 组合键按书写顺序触发（先 Ctrl，后 Win/RAlt），只屏蔽第二个键的完整
    // 按下/抬起周期。这样既不会打开开始菜单或触发 Alt 行为，也不会制造粘键。
    const bool suppress_ralt = key->vkCode == VK_RMENU && g_suppress_ralt_until_up;
    const bool suppress_win = (key->vkCode == VK_LWIN || key->vkCode == VK_RWIN) && g_suppress_win_until_up;
    if (up && key->vkCode == VK_RMENU)
        g_suppress_ralt_until_up = false;
    if (up && (key->vkCode == VK_LWIN || key->vkCode == VK_RWIN))
        g_suppress_win_until_up = false;
    if (suppress_ralt || suppress_win)
        return 1;
    return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);
}

size_t WriteResponse(char *data, size_t size, size_t count, void *user)
{
    constexpr size_t limit = 1024 * 1024;
    if (size && count > (std::numeric_limits<size_t>::max)() / size)
        return 0;
    const size_t length = size * count;
    auto &response = *static_cast<std::string *>(user);
    if (length > limit - response.size())
        return 0;
    try
    {
        response.append(data, length);
    }
    catch (...)
    {
        return 0;
    }
    return length;
}

size_t WriteHeader(char *data, size_t size, size_t count, void *user)
{
    const size_t n = size * count;
    std::string line(data, n);
    for (char &ch : line)
    {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    const char prefix[] = "x-siliconcloud-trace-id:";
    const auto pos = line.find(prefix);
    if (pos != std::string::npos)
    {
        std::string value = line.substr(pos + sizeof(prefix) - 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.erase(value.begin());
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' '))
            value.pop_back();
        *static_cast<std::string *>(user) = std::move(value);
    }
    return n;
}

bool IsSiliconFlowAsr(const VoiceInputConfig &config)
{
    std::string provider = config.asr_provider;
    for (char &ch : provider)
    {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    return provider == "siliconflow";
}

struct RecognitionResult
{
    std::string text;
    std::string error;
};

std::string JsonErrorMessage(const std::string &body)
{
    try
    {
        const auto json = nlohmann::json::parse(body);
        if (json.contains("error"))
        {
            const auto &error = json["error"];
            if (error.is_string())
                return error.get<std::string>();
            if (error.is_object())
            {
                if (error.contains("message") && error["message"].is_string())
                    return error["message"].get<std::string>();
            }
        }
        if (json.contains("message") && json["message"].is_string())
        {
            std::string message = json["message"].get<std::string>();
            if (json.contains("code") && !json["code"].is_null())
                message += "（code " + json["code"].dump() + "）";
            if (json.contains("data") && json["data"].is_string() && !json["data"].get<std::string>().empty())
                message += " " + json["data"].get<std::string>();
            return message;
        }
    }
    catch (...)
    {
    }
    if (body.size() > 240)
        return body.substr(0, 240) + "...";
    return body;
}

RecognitionResult Recognize(const std::vector<float> &samples, const VoiceInputConfig &config)
{
    const std::string asr_token = VoiceInput::ResolveAsrToken(config);
    if (asr_token.empty() || config.asr_endpoint.empty())
        return {{}, "ASR Token 或接口地址为空。"};
    const std::string model_name = VoiceInput::ResolveAsrModel(config);
    if (model_name.empty())
        return {{}, "ASR 模型名为空。"};
    const bool siliconflow = IsSiliconFlowAsr(config);
    metasequoia::voice::MultipartRequest payload;
    try
    {
        payload = VoiceInput::BuildBatchTranscription(samples, config);
    }
    catch (const metasequoia::voice::VoiceError &)
    {
        return {{}, "录音数据无效或超过 20 MiB 上传限制。"};
    }
    const int attempts = siliconflow ? 2 : 1;
    std::string response;
    std::string trace_id;
    char curl_error[CURL_ERROR_SIZE] = {};
    CURLcode result = CURLE_OK;
    long http_status = 0;
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        if (attempt > 0)
            Sleep(400);
        response.clear();
        trace_id.clear();
        curl_error[0] = 0;
        CURL *curl = curl_easy_init();
        if (!curl)
            return {{}, "无法初始化网络请求。"};
        curl_slist *headers = nullptr;
        headers = curl_slist_append(headers, ("Authorization: Bearer " + asr_token).c_str());
        // Some gateways 500 on libcurl's default Expect: 100-continue.
        headers = curl_slist_append(headers, "Expect:");
        headers = curl_slist_append(headers, ("Content-Type: " + payload.content_type).c_str());
        curl_easy_setopt(curl, CURLOPT_URL, config.asr_endpoint.c_str());
        NetworkProxy::ApplyToCurl(curl);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.body.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteHeader);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &trace_id);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        result = curl_easy_perform(curl);
        http_status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (result == CURLE_OK && http_status < 500)
            break;
    }
    if (result != CURLE_OK)
    {
        const char *detail = curl_error[0] ? curl_error : curl_easy_strerror(result);
        return {{}, std::string("语音识别请求失败：") + detail};
    }
    if (http_status < 200 || http_status >= 300)
    {
        std::string detail = JsonErrorMessage(response);
        if (detail.empty())
            detail = "HTTP " + std::to_string(http_status);
        if (http_status >= 500 && siliconflow)
        {
            detail += "。这是硅基流动服务端内部错误，模型名 " + model_name + " 本身是官方支持的。";
            if (!trace_id.empty())
                detail += " 追踪 ID：" + trace_id + "。";
        }
        return {{}, "语音识别失败：" + detail};
    }
    try
    {
        return {metasequoia::voice::parse_transcription(response), {}};
    }
    catch (...)
    {
        return {{}, "语音识别返回了无法解析的结果。"};
    }
}

bool ShouldPolish(const std::string &text, const VoiceInputConfig &config)
{
    return config.polish_text && !text.empty() && !VoiceInput::ResolvePolishToken(config).empty() &&
           !config.polish_endpoint.empty() && !VoiceInput::ResolvePolishModel(config).empty();
}

std::string Polish(const std::string &text, const VoiceInputConfig &config)
{
    if (!ShouldPolish(text, config))
        return text;
    const std::string polish_token = VoiceInput::ResolvePolishToken(config);
    // 设置页里的润色服务商可能是协议 id（responses / anthropic / gemini）：
    // Chat 沿用原来的厂商特判，其余三种用统一构造的四协议请求体。
    const auto wire = AiWire::FromId(config.polish_provider);
    std::string payload;
    try
    {
        if (wire == AiWire::Wire::Chat)
            payload = VoiceInput::BuildBatchPolish(text, config);
        else
            payload = AiWire::BuildBody(wire, VoiceInput::ResolvePolishModel(config),
                                        VoiceInput::ResolvePolishSystemPrompt(config),
                                        VoiceInput::WrapAsrUserMessage(text), 512, 0.2)
                          .dump();
    }
    catch (...)
    {
        return text;
    }
    const std::string polish_url =
        AiWire::NormalizeEndpoint(config.polish_endpoint, wire, VoiceInput::ResolvePolishModel(config));
    // Polishing is optional: if it cannot finish promptly, abort the request and
    // let the caller commit the original ASR text returned below.
    const long polish_timeout_ms =
        VoiceInput::UsesDeepSeekThinking(config.polish_provider, config.polish_endpoint, config.polish_thinking)
            ? kPolishThinkingTimeoutMs
            : kPolishTimeoutMs;
    const auto polish_outcome = AiWire::Send(polish_url, wire, polish_token, payload, polish_timeout_ms);
    if (!polish_outcome.ok || polish_outcome.status < 200 || polish_outcome.status >= 300)
        return text;
    try
    {
        return metasequoia::voice::parse_polished_text(polish_outcome.body);
    }
    catch (const std::exception &error)
    {
        // 润色失败或整段都是思考（剥完为空）：回退原始 ASR 文本，原因进诊断日志。
        DIAG_LOGF(L"voice: polish failed ({}), committing original asr text", string_to_wstring(error.what()));
        return text;
    }
}

// Returns the number of characters whose Unicode down/up pair was accepted by
// SendInput. A zero or short count stops injection immediately; callers must
// not replay the text automatically because a prefix may already be in the
// field.
std::size_t SendTextViaSendInput(const std::wstring &text)
{
    return VoiceInput::SendUnicodeTextWith(text, [](wchar_t ch) -> UINT {
        INPUT input[2]{};
        input[0].type = INPUT_KEYBOARD;
        input[0].ki.wScan = ch;
        input[0].ki.dwFlags = KEYEVENTF_UNICODE;
        input[1] = input[0];
        input[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        return SendInput(2, input, sizeof(INPUT));
    });
}

bool CopyUnicodeTextToClipboard(const std::wstring &text)
{
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory)
    {
        return false;
    }
    void *destination = GlobalLock(memory);
    if (!destination)
    {
        GlobalFree(memory);
        return false;
    }
    memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);

    // Allocate and fill before touching the shared clipboard. Open/Empty can
    // still fail, but no previous clipboard content is cleared merely because
    // allocation or locking failed.
    if (!OpenClipboard(nullptr))
    {
        GlobalFree(memory);
        return false;
    }
    if (!EmptyClipboard())
    {
        CloseClipboard();
        GlobalFree(memory);
        return false;
    }
    if (!SetClipboardData(CF_UNICODETEXT, memory))
    {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

// Immediate, no-COM route check used inside the send lock right before
// delivery: the captured client/epoch/focus token must still be the current
// active pipe route. The full UIA capture already ran outside the lock.
bool IsPipeRouteCurrent(const VoiceInput::Target &target)
{
    if (target.client == 0 || target.epoch == 0)
    {
        return false;
    }
    const PipeClientActivation active = GetActivePipeClient();
    return active.client_id == target.client && active.epoch == target.epoch &&
           active.focus_token == target.focus_token;
}

// SendInput fallback guarded by a final no-COM re-validation. On failure the
// caller routes the text to the card; nothing is auto-copied here.
bool FastVerifiedSendInput(const std::wstring &text, const VoiceInput::Target &target)
{
    // Same gate as every other delivery path: a bound route must still be the
    // active client/epoch/focus-token route, not just the same focused control.
    if (!VoiceInput::IsTargetCurrentFast(target) || (VoiceInput::HasBoundRoute(target) && !IsPipeRouteCurrent(target)))
    {
        return false;
    }
    const std::size_t delivered = SendTextViaSendInput(text);
    return !text.empty() && delivered == text.size();
}

// Ctrl+V delivery. The re-verification wait happens before the clipboard is
// touched so a focus change observed during the wait routes to the card without
// ever mutating the clipboard. After the wait, session/target (and the bound
// route when present) are re-verified, then copy and immediately paste; a
// failed copy falls back to the original SendInput delivery, itself re-verified
// before sending. There remains an unavoidably short window between the check
// and the paste where the foreground can change - neither of these is an atomic
// cross-process transaction - so absolute delivery into the exact field cannot
// be guaranteed; in that race the text may be pasted into the newly focused
// field instead of withheld.
// A short SendInput for Ctrl+V leaves only a prefix of the four events in the
// input stream. Release the keys that are still down in reverse order. This is
// best-effort cleanup only; it never turns a failed paste into success.
void ReleasePartiallyInjectedCtrlV(const VoiceInput::CtrlVInjectionOutcome &outcome)
{
    INPUT cleanup[2]{};
    UINT count = 0;
    if (outcome.v_key_down)
    {
        cleanup[count].type = INPUT_KEYBOARD;
        cleanup[count].ki.wVk = 'V';
        cleanup[count].ki.dwFlags = KEYEVENTF_KEYUP;
        ++count;
    }
    if (outcome.ctrl_key_down)
    {
        cleanup[count].type = INPUT_KEYBOARD;
        cleanup[count].ki.wVk = VK_CONTROL;
        cleanup[count].ki.dwFlags = KEYEVENTF_KEYUP;
        ++count;
    }
    if (count != 0)
    {
        SendInput(count, cleanup, sizeof(INPUT));
    }
}

bool CommitCtrlVViaClipboard(const std::wstring &text, const VoiceInput::Target &target, std::uint64_t voice_session)
{
    if (!VoiceInput::IsTargetCurrentFast(target))
    {
        return false;
    }
    // Let the focused app settle, then re-verify before any clipboard mutation.
    Sleep(30);
    const bool target_ok = g_voice_session == voice_session && VoiceInput::IsTargetCurrentFast(target) &&
                           (!VoiceInput::HasBoundRoute(target) || IsPipeRouteCurrent(target));
    if (!target_ok)
    {
        return false;
    }
    if (!CopyUnicodeTextToClipboard(text))
    {
        return FastVerifiedSendInput(text, target);
    }
    INPUT inputs[4]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'V';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    const UINT injected = SendInput(4, inputs, sizeof(INPUT));
    const VoiceInput::CtrlVInjectionOutcome outcome = VoiceInput::ClassifyCtrlVInjection(injected);
    if (outcome.delivered)
    {
        return true;
    }
    ReleasePartiallyInjectedCtrlV(outcome);
    return false;
}

bool ShouldStreamInlinePreedit(const VoiceInputConfig &config)
{
    return config.stream_inline_preedit && VoiceInput::IsDoubaoAsrProvider(config.asr_provider) &&
           config.commit_mode == "tsf";
}

// The TSF client id is a PID<<32|TID of the IME thread inside either the
// foreground host process or the process that owns the focused child window.
// A cross-process embedded Edit is valid: its focus PID/TID may differ from the
// foreground host, but the pair must match exactly. Re-query the live
// foreground and focus identity here so a stale snapshot cannot bind a route;
// a background or foreign TIP stays unbound and the result goes to the card.
bool IsForegroundPipeClient(const VoiceInput::Target &target, std::uint64_t client_id)
{
    if (client_id == 0 || target.process == 0 || target.focus_process == 0 || target.thread == 0 ||
        target.foreground == 0 || target.focus == 0)
    {
        return false;
    }
    const HWND foreground = GetForegroundWindow();
    if (!foreground || foreground != reinterpret_cast<HWND>(target.foreground))
    {
        return false;
    }
    DWORD foreground_process = 0;
    const DWORD foreground_thread = GetWindowThreadProcessId(foreground, &foreground_process);
    if (!foreground_thread || foreground_process == 0)
    {
        return false;
    }
    const HWND focus = reinterpret_cast<HWND>(target.focus);
    if (!IsWindow(focus) || GetAncestor(focus, GA_ROOT) != foreground)
    {
        return false;
    }
    DWORD focus_process = 0;
    const DWORD focus_thread = GetWindowThreadProcessId(focus, &focus_process);
    if (!focus_thread || focus_process == 0 || foreground_process != target.process ||
        focus_process != target.focus_process || focus_thread != target.thread)
    {
        return false;
    }
    // Ask the focus window's own thread. For a cross-process embedded Edit the
    // foreground host thread does not own that focus; the live foreground
    // window and the root-ancestor check above still tie this focus to it.
    GUITHREADINFO info{sizeof(info)};
    if (!GetGUIThreadInfo(focus_thread, &info) || info.hwndFocus != focus)
    {
        return false;
    }
    const std::uint32_t client_pid = static_cast<std::uint32_t>(client_id >> 32);
    const DWORD client_thread = static_cast<DWORD>(client_id & 0xFFFFFFFFull);
    return VoiceInput::PipeClientMatchesForeground(target, client_pid, client_thread, foreground_thread);
}

// Capture a value-only target snapshot and bind the current TSF pipe
// client/epoch/focus token when that client belongs to the captured foreground
// host or the focused child route. The COM/UIA work happens in the platform
// adapter and must always run outside g_send_mutex.
VoiceInput::Target CaptureVoiceTarget()
{
    VoiceInput::Target target = VoiceInput::CaptureTarget();
    const PipeClientActivation active = GetActivePipeClient();
    if (active.client_id != 0 && active.epoch != 0 && IsForegroundPipeClient(target, active.client_id))
    {
        target.client = active.client_id;
        target.epoch = active.epoch;
        target.focus_token = active.focus_token;
    }
    return target;
}

VoiceInput::CommitMode NormalizeCommitMode(const std::string &mode)
{
    if (mode == "sendinput")
    {
        return VoiceInput::CommitMode::SendInput;
    }
    if (mode == "ctrl_v")
    {
        return VoiceInput::CommitMode::CtrlV;
    }
    return VoiceInput::CommitMode::Tsf;
}

// In-lock, no-COM re-validation of a bound route: the owner target must still be
// the current editable focus and its client/epoch/focus token must still be the
// active pipe route. When either fails the composition must not be messaged --
// a stale client/epoch pair could forward the message to a field the user
// clicked into after recording started.
bool OwnerRouteValid(const VoiceInput::Target &owner)
{
    return VoiceInput::IsTargetCurrentFast(owner) && IsPipeRouteCurrent(owner);
}

// Voice-composition messages only ever go to the bound owner route after a
// final fast+route re-check, so a stale owner is never deliberately addressed.
// The check and the pipe write are not one cross-process transaction; a focus
// change in that window can still race the delivery.
bool SendVoiceCompositionToOwner(const VoiceInput::Target &owner, UINT msg_type, const std::wstring &text)
{
    if (owner.client == 0 || owner.epoch == 0 || !OwnerRouteValid(owner))
    {
        return false;
    }
    if (msg_type == Global::DataFromServerMsgTypeToTsfWorkerThread::CancelVoiceComposition)
    {
        return SendToTsfWorkerThreadClientViaNamedpipe(owner.client, owner.epoch, msg_type, L"");
    }
    return SendVoiceCompositionToTsfWorker(owner.client, owner.epoch, msg_type, text);
}

void PushInlinePreedit(const std::wstring &text)
{
    if (!g_stream_inline_this_session.load() || !g_recording.load())
    {
        return;
    }
    if (text.empty() || text == g_last_inline_preedit)
    {
        return;
    }
    // Updates only go to the original composition owner once it still is the
    // current editable focus with a live route; otherwise the stream stops and
    // a new field the user clicked into is never targeted. Keep the owner when
    // the check fails: a Win+. round-trip advances only the generation while
    // the DLL retains the composition, so the owner is still the only route
    // that can cancel it later. Update/Commit stay strictly re-verified.
    if (!OwnerRouteValid(g_inline_owner))
    {
        return;
    }
    if (SendVoiceCompositionToOwner(g_inline_owner,
                                    Global::DataFromServerMsgTypeToTsfWorkerThread::UpdateVoiceComposition, text))
    {
        g_last_inline_preedit = text;
    }
    else
    {
        // The route refused this update. Leave teardown to the cancellation or
        // commit path, which reconciles the captured identity outside the lock.
        g_last_inline_preedit.clear();
    }
}

// Shared teardown of the inline preedit state. Must be called under g_send_mutex.
void ResetInlinePreeditState()
{
    g_inline_owner = VoiceInput::Target{};
    g_inline_owner_session = 0;
    g_last_inline_preedit.clear();
    g_stream_inline_this_session.store(false);
}

// Cancellation-only reconciliation. `current` was captured outside
// g_send_mutex. The focus generation is deliberately ignored here: a Win+.
// round-trip bumps it while the DLL keeps the composition and pipe epoch. Every
// other SameTarget field, including the UIA runtime ID and the full client /
// epoch / focus-token route, must still match, and the captured target must
// still pass the no-COM fast window check with the old route still active.
// Must be called under g_send_mutex. The local owner is cleared on both a sent
// cancel and an identity mismatch, so it can never be reused for a new field.
void CancelInlinePreeditFor(const VoiceInput::Target &current)
{
    if (g_inline_owner.client != 0 && g_inline_owner.epoch != 0 &&
        VoiceInput::SameCompositionOwner(g_inline_owner, current) && VoiceInput::IsTargetCurrentFast(current) &&
        IsPipeRouteCurrent(g_inline_owner))
    {
        SendToTsfWorkerThreadClientViaNamedpipe(g_inline_owner.client, g_inline_owner.epoch,
                                                Global::DataFromServerMsgTypeToTsfWorkerThread::CancelVoiceComposition,
                                                L"");
    }
    ResetInlinePreeditState();
}

// Control-thread cancellation: take one current target outside the lock, then
// re-identify the owner under the lock before messaging its old route.
// `expected_session` == 0 is the explicit cancel path, which owns the current
// composition even after the UI has already advanced the session. A non-zero
// value is the generic invalidation guard. The owner snapshot prevents a newer
// composition bound during the UIA capture from receiving this cancel.
bool CancelInlinePreeditOnControlThread(std::uint64_t expected_session)
{
    VoiceInput::Target owner;
    std::uint64_t owner_session = 0;
    {
        std::lock_guard<std::mutex> lock(g_send_mutex);
        owner = g_inline_owner;
        owner_session = g_inline_owner_session;
    }
    if (owner.client == 0 || owner.epoch == 0)
    {
        return false;
    }
    if (expected_session != 0 && (owner_session != expected_session || g_voice_session != expected_session))
    {
        return false;
    }
    const VoiceInput::Target current = CaptureVoiceTarget();
    std::lock_guard<std::mutex> lock(g_send_mutex);
    if ((expected_session != 0 && g_voice_session != expected_session) || g_inline_owner_session != owner_session ||
        !VoiceInput::SameCompositionOwner(owner, g_inline_owner))
    {
        return false;
    }
    CancelInlinePreeditFor(current);
    return true;
}

// Delivers through the captured commit route, never a freshly picked active
// client: the destination was decided when the target snapshot was taken.
// Returns the number of UTF-16 units successfully delivered. Each chunk starts
// only after the route is re-verified (fast focus + live pipe); when delivery
// stops, everything up to the returned offset is already in the field. The
// caller shows the full original on the card and never replays any part,
// because automatic replay would duplicate the delivered prefix.
// Must be called under g_send_mutex.
std::size_t SendTextViaTsf(const std::wstring &text, const VoiceInput::Target &route)
{
    if (text.empty() || route.client == 0 || route.epoch == 0)
    {
        return 0;
    }
    constexpr size_t maxCharsPerPacket =
        (sizeof(((FanyImeNamedpipeDataToTsfWorkerThread *)nullptr)->data) / sizeof(wchar_t)) - 1;
    size_t offset = 0;
    while (offset < text.size())
    {
        if (!OwnerRouteValid(route))
        {
            break;
        }
        const size_t chunkLen = (std::min)(maxCharsPerPacket, text.size() - offset);
        if (!SendToTsfWorkerThreadClientViaNamedpipe(route.client, route.epoch,
                                                     Global::DataFromServerMsgTypeToTsfWorkerThread::InsertText,
                                                     text.substr(offset, chunkLen)))
        {
            break;
        }
        offset += chunkLen;
    }
    return offset;
}

void CommitRecognizedText(const std::string &utf8, const VoiceInputConfig &config, bool stream_inline,
                          const VoiceInput::Target &stop_target, std::uint64_t voice_session)
{
    const std::wstring text = string_to_wstring(utf8);
    if (text.empty())
    {
        return;
    }
    if (g_voice_session.load() != voice_session)
    {
        return; // Cheap pre-check; the authoritative one runs inside the lock.
    }
    // The final target must be captured outside the send lock: the UIA
    // cross-process query is slow and the lock stays lean (no COM under it).
    const VoiceInput::Target final_target = CaptureVoiceTarget();

    std::lock_guard<std::mutex> send_lock(g_send_mutex);
    if (g_voice_session != voice_session)
    {
        return;
    }

    VoiceInput::VoiceCommitContext ctx;
    ctx.same_target = VoiceInput::CanCommitToTarget(stop_target, final_target);
    // Last in-lock, no-COM gate: the final target must still be the current
    // editable focus and its bound route (when present) still the active pipe
    // route. A target that moved while the UIA capture ran keeps the card.
    ctx.fast_valid = VoiceInput::IsTargetCurrentFast(final_target) &&
                     (!VoiceInput::HasBoundRoute(final_target) || IsPipeRouteCurrent(final_target));
    ctx.stream_inline_this_session = stream_inline;
    ctx.inline_owner_matches = stream_inline && VoiceInput::SameTarget(g_inline_owner, final_target);
    ctx.has_bound_route = IsPipeRouteCurrent(final_target);
    ctx.mode = NormalizeCommitMode(config.commit_mode);

    const auto show_card = [&](const std::wstring &card_text) {
        if (stream_inline)
        {
            CancelInlinePreeditFor(final_target);
        }
        // Both windows share this recording's bottom anchor. Hide the status
        // before showing the card so its entrance never overlaps the old bar.
        g_overlay.hide();
        g_result_card.show(voice_session, card_text);
    };

    switch (VoiceInput::DecideVoiceCommit(ctx))
    {
    case VoiceInput::VoiceCommitAction::ShowCard:
        show_card(text);
        return;
    case VoiceInput::VoiceCommitAction::CommitComposition:
        // Only reached when the final target is still the bound inline owner
        // with a current route; commit through that owner.
        if (SendVoiceCompositionToOwner(g_inline_owner,
                                        Global::DataFromServerMsgTypeToTsfWorkerThread::CommitVoiceComposition, text))
        {
            ResetInlinePreeditState();
            return;
        }
        // The prefix was already streamed into the old field; replaying the
        // full text would duplicate it, so the full result stays in the card.
        // Never SendInput from scratch here.
        show_card(text);
        return;
    case VoiceInput::VoiceCommitAction::CommitTsf:
        // Plain insert on the captured route: cancel any local preedit first
        // (owner mismatch or no streaming); never pick a newer active client.
        if (stream_inline)
        {
            CancelInlinePreeditFor(final_target);
        }
        // This action requires a live captured route. A failed chunk leaves
        // any delivered prefix in the field; keep the full original on the
        // Copy surface without replaying it through another delivery method.
        if (SendTextViaTsf(text, final_target) == text.size())
        {
            return;
        }
        show_card(text);
        return;
    case VoiceInput::VoiceCommitAction::CommitSendInput:
        if (stream_inline)
        {
            CancelInlinePreeditFor(final_target);
        }
        if (FastVerifiedSendInput(text, final_target))
        {
            return;
        }
        show_card(text);
        return;
    case VoiceInput::VoiceCommitAction::CommitCtrlV:
        if (CommitCtrlVViaClipboard(text, final_target, voice_session))
        {
            return;
        }
        show_card(text);
        return;
    }
}

void AudioCallback(const float *samples, std::size_t frames)
{
    if (!samples || !g_recording)
        return;
    double sum = 0.0;
    // Widen before squaring. float * float is evaluated at float precision and only then converted,
    // which throws away the accuracy the double accumulator exists to keep.
    for (std::size_t i = 0; i < frames; ++i)
        sum += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
    const float rms = frames ? static_cast<float>(std::sqrt(sum / frames)) : 0.0f;
    // Compress the visual dynamic range so quiet speech still produces a clear waveform,
    // while keeping low-level room/microphone noise close to rest.
    constexpr float noise_floor = 0.004f;
    const float speech_level = (std::max)(0.0f, rms - noise_floor);
    const float normalized = (std::min)(1.0f, speech_level * 14.0f);
    g_overlay.set_input_level(std::pow(normalized, 0.55f));
    g_recorded_frames += frames;
    if (g_doubao_asr)
    {
        g_doubao_asr->PushFloatSamples(samples, frames);
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_samples.insert(g_samples.end(), samples, samples + frames);
}

// Every session invalidation path (cancel, short audio, start failure, overlay
// cancel, discard) funnels through here: the session advances and the per-era
// card is reset under the send lock, so a late recognition callback can never
// re-show a card or commit into a newer session.
void InvalidateVoiceSession()
{
    const std::uint64_t session = g_voice_session.load();
    // Capture outside the send lock; the helper re-checks the owner snapshot,
    // its session and the live route after re-taking the lock.
    CancelInlinePreeditOnControlThread(session);
    std::lock_guard<std::mutex> send_lock(g_send_mutex);
    if (g_voice_session != session)
    {
        // Another invalidation or a new recording already advanced the
        // session while the cancellation capture ran. Never disturb it.
        return;
    }
    ++g_voice_session;
    g_result_card.reset(g_voice_session);
    g_last_inline_preedit.clear();
    g_stream_inline_this_session.store(false);
}

// UI-thread cancellation can only touch atomics/lock-protected state. It
// invalidates the session immediately so a late recognition callback cannot
// show a card or commit, but leaves the inline owner for the queued
// control-thread cleanup that performs the one UIA capture.
void EnqueueCancelFromUi()
{
    {
        // Keep invalidation and queue insertion atomic with Stop's session
        // snapshot. A Stop queued earlier must never adopt this new cookie.
        std::lock_guard<std::mutex> control_lock(g_control_mutex);
        std::lock_guard<std::mutex> send_lock(g_send_mutex);
        const std::uint64_t session_cookie = ++g_voice_session;
        if (g_starting.load())
            g_abort_start = true;
        g_result_card.reset(g_voice_session);
        g_last_inline_preedit.clear();
        g_stream_inline_this_session.store(false);
        VoiceInput::ControlPolicy::Enqueue(g_control_commands,
                                           ControlRequest{ControlCommand::Cancel, session_cookie, CurrentLockTicket()},
                                           g_recording.load(), g_starting.load());
    }
    g_control_cv.notify_one();
}

void AbortPendingStart(const char *reason)
{
    (void)reason;
    // The control loop owns the startup/completion flags.
    InvalidateVoiceSession();
    if (g_doubao_asr)
    {
        g_doubao_asr->Cancel();
        g_doubao_asr.reset();
    }
}

// 配置类错误仍要告知用户，但绝不能在控制线程上模态等待——弹窗挂着的期间，
// 下一次语音的指令队列整个被卡住（提示条卡死的元凶之一）。挪到独立线程展示，
// 队列照常运转；连点几次也只是多几个各自可关闭的窗口。
void ShowVoiceNotice(std::wstring message, unsigned icon = MB_ICONERROR)
{
    std::thread([message = std::move(message), icon] {
        MessageBoxW(nullptr, message.c_str(), L"水杉 IME", MB_OK | icon);
    }).detach();
}

bool StartRecording()
{
    if (g_recording)
        return true;
    if (!g_ime_active)
        return false;
    if (g_abort_start.exchange(false))
        return false;
    const VoiceInputConfig config = GetConfiguredVoiceInput();
    // Deactivation or a queued Cancel may arrive while this Start waits for
    // configuration; it must not begin a capture after that point.
    const bool aborted_after_config = g_abort_start.exchange(false);
    if (!g_ime_active || aborted_after_config)
        return false;
    if (!config.enabled)
        return false;
    const bool use_doubao = VoiceInput::IsDoubaoAsrProvider(config.asr_provider);
    const std::string asr_token = VoiceInput::ResolveAsrToken(config);
    if (asr_token.empty())
    {
        ShowVoiceNotice(L"请先在设置的“语音输入”分区填写当前 ASR 提供商的 API Token。", MB_ICONINFORMATION);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_samples.clear();
    }
    g_recorded_frames = 0;
    const bool want_inline = use_doubao && ShouldStreamInlinePreedit(config);
    const VoiceOverlayAnchor overlay_anchor = CaptureVoiceOverlayAnchor();
    std::uint64_t voice_session = 0;
    // Clear the previous session's inline composition before the new session
    // starts. A UI cancel has already advanced the cookie, so snapshot cleanup
    // deliberately does not require that cookie to equal the old owner session.
    CancelInlinePreeditOnControlThread(0);
    {
        std::lock_guard<std::mutex> send_lock(g_send_mutex);
        voice_session = ++g_voice_session;
        g_dismissed_overlay_session = 0;
        g_result_card.reset(voice_session);
        g_overlay.hide();
        g_overlay.set_anchor(overlay_anchor);
        g_result_card.set_anchor(overlay_anchor);
    }
    if (want_inline)
    {
        // Pin the inline composition owner to the input target captured at the
        // start of the session; only an editable target with a live, matching
        // TSF route may stream preedit into it. The owner is re-verified under
        // the lock (still current focus + live route) after the capture, and
        // only bound when this session is still the one that captured it.
        const VoiceInput::Target owner = CaptureVoiceTarget();
        std::lock_guard<std::mutex> send_lock(g_send_mutex);
        if (owner.client != 0 && owner.epoch != 0 && g_voice_session == voice_session && OwnerRouteValid(owner))
        {
            g_inline_owner = owner;
            g_inline_owner_session = voice_session;
            g_stream_inline_this_session.store(true);
        }
    }
    g_overlay.set_show_transcript(!g_stream_inline_this_session.load());
    g_overlay.set_compact_status(WaveOverlay::CompactStatus::None);
    // 录音中就把取消/确认按钮亮出来。长按 RAlt 时松手事件可能因为切换输入法、
    // Alt-Tab 或别的低级键盘钩子而丢失，而提示条是 HWND_TOPMOST 的独立窗口，
    // 没有按钮就等于没有任何办法把它关掉。
    g_overlay.set_actions_visible(true);
    g_overlay.set_transcript(L"");
    if (use_doubao)
    {
        g_doubao_asr = std::make_unique<DoubaoAsrClient>(
            config.asr_endpoint, VoiceInput::UsesDoubaoLegacyAuth(config), config.asr_app_key, asr_token,
            config.asr_resource_id, config.doubao_enable_itn, config.doubao_enable_punc, config.doubao_enable_ddc,
            config.doubao_boosting_table_id, [voice_session](const std::string &text) {
                if (g_voice_session != voice_session)
                    return;
                const std::wstring wide = string_to_wstring(text);
                std::lock_guard<std::mutex> send_lock(g_send_mutex);
                if (g_voice_session != voice_session)
                    return;
                if (!g_stream_inline_this_session.load())
                    g_overlay.set_transcript(wide);
                PushInlinePreedit(wide);
            });
        if (!g_doubao_asr->Start())
        {
            AbortPendingStart("asr start failed");
            ShowVoiceNotice(L"无法启动豆包流式语音识别。请检查 config.toml。");
            return false;
        }
    }
    if (!g_ime_active || g_abort_start.exchange(false))
    {
        AbortPendingStart("aborted before capture");
        return false;
    }
    if (!g_capture.start(AudioCallback))
    {
        AbortPendingStart("capture start failed");
        ShowVoiceNotice(L"无法启动麦克风。");
        return false;
    }
    bool aborted_after_capture = false;
    {
        std::lock_guard<std::mutex> send_lock(g_send_mutex);
        if (!g_ime_active || g_abort_start.exchange(false))
        {
            aborted_after_capture = true;
        }
        else
        {
            g_recording = true;
        }
    }
    if (aborted_after_capture)
    {
        g_capture.stop();
        AbortPendingStart("aborted after capture start");
        return false;
    }
    g_overlay.set_input_level(0.0f);
    g_overlay.set_listening(true);
    g_overlay.show();
    if (config.start_sound)
        g_cue_player.play_start();
    MuteSystemAudioIfEnabled(config);
    return true;
}

void StopRecording(std::uint64_t voice_session, std::uintptr_t lock_ticket)
{
    bool stream_inline = false;
    {
        std::lock_guard<std::mutex> send_lock(g_send_mutex);
        if (!g_recording || g_voice_session != voice_session)
        {
            ClearLockForRequest(lock_ticket);
            return;
        }
        ClearRecordingLock(lock_ticket);
        g_recording = false;
        stream_inline = g_stream_inline_this_session.load();
    }
    const VoiceInputConfig config = GetConfiguredVoiceInput();
    // Stop the visible recording state before any blocking capture: the UIA
    // query can take RPC time, and the overlay must not keep showing live input
    // while the recording is already over.
    g_overlay.set_listening(false);
    g_overlay.set_input_level(0.0f);
    // 先收掉提示条再关设备：WASAPI 的 stop() 可能阻塞（设备被独占或驱动挂住），
    // 阻塞时不能让置顶的录音条留在屏幕上。正常路径下 stop 只需几毫秒，这一次
    // hide 不会被看见，下面的识别态会立刻把它重新显示出来。
    g_overlay.hide();
    // Decision snapshot pinned to the input target that owns focus when the
    // recording ends; taken before the capture teardown and transcription so a
    // later focus change never moves the destination. The session is already
    // pinned and the recording flag is already off, so no further preedit is
    // pushed while this query runs. This still happens before capture.stop().
    const VoiceInput::Target stop_target = CaptureVoiceTarget();
    g_capture.stop();
    if (g_capture.callback_failed())
    {
        DiscardRecording(config);
        ShowVoiceNotice(L"录音中断，请重试。");
        return;
    }
    RestoreSystemAudioIfMuted();
    if (config.end_sound)
        g_cue_player.play_end();
    auto doubao_asr = std::move(g_doubao_asr);
    const bool batch_recognition = !doubao_asr;
    if (!stream_inline)
    {
        g_overlay.set_compact_status(batch_recognition ? WaveOverlay::CompactStatus::Recognizing
                                                       : WaveOverlay::CompactStatus::None);
        g_overlay.set_show_transcript(true);
        g_overlay.set_transcript(batch_recognition ? L"" : L"正在识别…");
        g_overlay.set_actions_visible(batch_recognition);
        g_overlay.show();
    }
    else
    {
        g_overlay.set_compact_status(WaveOverlay::CompactStatus::Recognizing);
        g_overlay.set_actions_visible(true);
        g_overlay.show();
    }
    const std::size_t recorded_frames = g_recorded_frames;
    std::vector<float> samples;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        samples.swap(g_samples);
    }
    if (recorded_frames < kSampleRate / 4)
    {
        DIAG_LOGF(L"voice: recording shorter than 0.25s ({} frames), treating as no speech", recorded_frames);
        InvalidateVoiceSession();
        g_overlay.hide();
        g_overlay.set_actions_visible(false);
        g_overlay.set_transcript(L"");
        if (doubao_asr)
        {
            g_network_tasks.emplace_back(
                std::async(std::launch::async, [client = std::move(doubao_asr)]() mutable { client->Cancel(); }));
        }
        return;
    }
    g_network_tasks.erase(std::remove_if(g_network_tasks.begin(), g_network_tasks.end(),
                                         [](std::future<void> &task) {
                                             return task.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                                         }),
                          g_network_tasks.end());
    g_network_tasks.emplace_back(
        std::async(std::launch::async, [samples = std::move(samples), client = std::move(doubao_asr), config,
                                        batch_recognition, stream_inline, voice_session, stop_target]() mutable {
            RecognitionResult recognition;
            if (client)
            {
                recognition.text = client->Finish();
                recognition.error = client->LastError();
            }
            else
                recognition = Recognize(samples, config);
            const bool polishing = recognition.error.empty() && ShouldPolish(recognition.text, config);
            {
                // Session changes and overlay updates share the send lock, so
                // an old callback cannot repaint or hide a newer recording.
                std::lock_guard<std::mutex> send_lock(g_send_mutex);
                if (g_voice_session != voice_session)
                    return;
                if (!batch_recognition && !stream_inline)
                {
                    if (!recognition.error.empty())
                        g_overlay.set_transcript(string_to_wstring(recognition.error));
                    else if (!recognition.text.empty())
                        g_overlay.set_transcript(string_to_wstring(recognition.text));
                }
                if (polishing)
                {
                    g_overlay.set_compact_status(WaveOverlay::CompactStatus::Processing);
                    g_overlay.set_actions_visible(true);
                    if (g_dismissed_overlay_session != voice_session)
                        g_overlay.show();
                }
            }
            // 识别失败（静音、没说出话、服务商对空音频报错等）保持静默：
            // 不弹窗、不提交、提示条照常收起，就像什么都没发生。
            // 但原因要进诊断日志——部署在别的机器上排查时，这是唯一的证据；
            // 只记服务商名和错误文案，绝不记语音内容或转写文本。
            if (!recognition.error.empty())
            {
                DIAG_LOGF(L"voice: asr failed, provider={}, error={}", string_to_wstring(config.asr_provider),
                          string_to_wstring(recognition.error));
            }
            const std::string text = recognition.error.empty() ? Polish(recognition.text, config) : std::string();
            {
                std::lock_guard<std::mutex> send_lock(g_send_mutex);
                if (g_voice_session != voice_session)
                    return;
                if (!batch_recognition && !polishing && !stream_inline && !text.empty())
                    g_overlay.set_transcript(string_to_wstring(text));
            }
            if (!text.empty() && g_voice_session == voice_session)
                CommitRecognizedText(text, config, stream_inline, stop_target, voice_session);
            {
                std::lock_guard<std::mutex> send_lock(g_send_mutex);
                if (g_voice_session != voice_session)
                    return;
                if (batch_recognition || stream_inline || polishing)
                {
                    g_overlay.set_compact_status(WaveOverlay::CompactStatus::None);
                    g_overlay.set_actions_visible(false);
                    g_overlay.hide();
                    if (polishing)
                        g_overlay.set_transcript(L"");
                }
            }
            if (text.empty() && stream_inline)
            {
                // The session is re-checked inside the lock: a cancel that
                // advanced the session during recognition must not tear down a
                // newer session's composition.
                const VoiceInput::Target current = CaptureVoiceTarget();
                std::lock_guard<std::mutex> send_lock(g_send_mutex);
                if (g_voice_session == voice_session)
                {
                    CancelInlinePreeditFor(current);
                }
            }
            // Keep the finalized result visible briefly without delaying the commit.
            Sleep(recognition.error.empty() ? 160 : 1200);
            std::lock_guard<std::mutex> send_lock(g_send_mutex);
            if (g_voice_session == voice_session && !g_recording)
            {
                g_overlay.set_compact_status(WaveOverlay::CompactStatus::None);
                g_overlay.set_actions_visible(false);
                g_overlay.hide();
                g_overlay.set_transcript(L"");
            }
        }));
}

void CancelRecordingForSession(std::uint64_t session_cookie, std::uintptr_t lock_ticket)
{
    if (g_voice_session != session_cookie)
    {
        ClearLockForRequest(lock_ticket);
        return;
    }
    ClearRecordingLock(lock_ticket);
    if (g_recording)
    {
        // This cookie still names the live recording, so the normal cancel
        // owns capture teardown. A stale cookie never reaches this branch.
        CancelRecording(lock_ticket);
        return;
    }

    VoiceInput::Target owner;
    std::uint64_t owner_session = 0;
    {
        std::lock_guard<std::mutex> send_lock(g_send_mutex);
        if (g_voice_session != session_cookie)
            return;
        owner = g_inline_owner;
        owner_session = g_inline_owner_session;
    }

    if (owner.client != 0 && owner.epoch != 0)
    {
        const VoiceInput::Target current = CaptureVoiceTarget();
        std::lock_guard<std::mutex> send_lock(g_send_mutex);
        if (g_voice_session != session_cookie)
            return;
        if (g_inline_owner_session == owner_session && VoiceInput::SameCompositionOwner(owner, g_inline_owner))
        {
            // Reuse the owner/route check; it also clears a stale local owner.
            CancelInlinePreeditFor(current);
        }
    }

    if (g_voice_session != session_cookie)
        return;
    g_overlay.set_compact_status(WaveOverlay::CompactStatus::None);
    g_overlay.set_actions_visible(false);
    g_overlay.hide();
    g_overlay.set_transcript(L"");
}

void CancelRecording(std::uintptr_t lock_ticket)
{
    // Cancel terminates the request it was created for, including one whose
    // Lock message has not reached the control thread yet. Applied locks are
    // always part of the recording being cancelled.
    ClearRecordingLock(lock_ticket);
    if (!g_recording)
    {
        // Explicit cancel owns the current composition even if the UI already
        // advanced the session. Capture once here, on the control thread.
        CancelInlinePreeditOnControlThread(0);
        InvalidateVoiceSession();
        g_overlay.set_compact_status(WaveOverlay::CompactStatus::None);
        g_overlay.set_actions_visible(false);
        g_overlay.hide();
        g_overlay.set_transcript(L"");
        return;
    }
    const VoiceInputConfig config = GetConfiguredVoiceInput();
    g_recording = false;
    DiscardRecording(config);
}

// g_recording 已经置为 false 之后的取消收尾。StopRecording 在录音回调失败时也走这里：
// 那时 g_recording 已清掉，再调 CancelRecording 只会进上面的早退分支，系统静音和豆包连接都收不掉。
void DiscardRecording(const VoiceInputConfig &config)
{
    // Explicit cancellation/abort: reconcile the composition before advancing
    // the session. The capture runs here, never under g_send_mutex.
    CancelInlinePreeditOnControlThread(0);
    InvalidateVoiceSession();
    g_overlay.set_listening(false);
    g_overlay.set_input_level(0.0f);
    g_overlay.set_compact_status(WaveOverlay::CompactStatus::None);
    g_overlay.set_actions_visible(false);
    // 取消是最后一道退出通道，必须先让提示条消失再关设备：g_capture.stop()
    // 一旦阻塞，置顶的窗口就会一直留在屏幕上，而且它没有标题栏、没有任务栏项、
    // 也不接受焦点，用户拿它没有任何办法。
    g_overlay.hide();
    g_overlay.set_transcript(L"");
    g_capture.stop();
    RestoreSystemAudioIfMuted();
    if (config.end_sound)
        g_cue_player.play_end();
    auto doubao_asr = std::move(g_doubao_asr);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_samples.clear();
    if (doubao_asr)
    {
        g_network_tasks.emplace_back(
            std::async(std::launch::async, [client = std::move(doubao_asr)]() mutable { client->Cancel(); }));
    }
}
} // namespace

namespace VoiceInput::Detail
{
LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wparam, LPARAM lparam)
{
    return HandleKeyboardHook(code, wparam, lparam);
}
} // namespace VoiceInput::Detail

bool VoiceInput::Initialize()
{
    if (g_initialized)
        return true;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return false;

    // Failure cleanup for this function only. It never queues Exit unless the
    // control thread actually exists; Shutdown cannot be reused here because it
    // unconditionally enqueues Exit.
    const auto cleanup_failed_init = [&]() {
        if (g_control_thread.joinable())
        {
            EnqueueControlCommand(ControlCommand::Exit);
            g_control_thread.join();
        }
        if (g_keyboard_hook)
        {
            UnhookWindowsHookEx(g_keyboard_hook);
            g_keyboard_hook = nullptr;
        }
        if (g_message_window)
        {
            DestroyWindow(g_message_window);
            g_message_window = nullptr;
        }
        g_cue_player.shutdown();
        g_result_card.shutdown();
        g_overlay.shutdown();
        VoiceInput::ShutdownTargetTracking();
        curl_global_cleanup();
        g_initialized = false;
    };

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!g_overlay.init(instance, [](WaveOverlay::Action action) {
            if (action == WaveOverlay::Action::Cancel)
            {
                // Fence the session now, then queue cleanup directly with the
                // new cookie. This bypasses the message window and the generic
                // Start/Stop filters, so a recognition-state cancel still runs.
                EnqueueCancelFromUi();
                return;
            }
            if (g_recording)
            {
                if (g_message_window)
                    PostRecordingControlMessage(kStopRecordingMessage, CurrentLockTicket());
                return;
            }
            g_dismissed_overlay_session = g_voice_session.load();
            g_overlay.set_actions_visible(false);
            g_overlay.hide();
        }))
    {
        cleanup_failed_init();
        return false;
    }
    // The result card is optional: failure must not disable existing voice
    // delivery to editable targets. Its model still retains unsent results.
    if (!g_result_card.init(instance))
    {
        g_result_card.shutdown();
        DIAG_LOGF(L"[voice] result card initialization failed; normal delivery remains available");
    }
    // Focus tracking hooks are installed on the pumping UI thread. A failed
    // install fails closed: captures report non-editable and results go to the
    // card, so live voice input still works without tracking.
    VoiceInput::InitializeTargetTracking();
    // 开着语音输入时和原先一样立刻打开，第一次录音的提示音不晚到；关着时推迟到第一次播放。
    g_cue_player.init(ResolveCuePath(L"start.mp3"), ResolveCuePath(L"end.mp3"), GetConfiguredVoiceInput().enabled);
    RestoreOtherSystemAudio();

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = VoiceMessageWindowProc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kMessageWindowClass;
    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        cleanup_failed_init();
        return false;
    }
    g_message_window =
        CreateWindowExW(0, kMessageWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (!g_message_window)
    {
        cleanup_failed_init();
        return false;
    }
    g_control_thread = std::thread(ControlLoop);
    if (ShouldInstallKeyboardHook(GetConfiguredVoiceInput()))
    {
        g_keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, VoiceInput::Detail::KeyboardHookProc, instance, 0);
        if (!g_keyboard_hook)
        {
            cleanup_failed_init();
            return false;
        }
    }
    g_initialized = true;
    return true;
}

void VoiceInput::RefreshKeyboardHook()
{
    if (!g_initialized)
        return;

    const VoiceInputConfig config = GetConfiguredVoiceInput();
    const bool release_ralt = g_suppress_ralt_until_up;
    const bool stop_recording = g_active_hold_shortcut != HoldShortcut::None || (!config.enabled && g_recording);

    if (g_keyboard_hook)
    {
        UnhookWindowsHookEx(g_keyboard_hook);
        g_keyboard_hook = nullptr;
    }
    if (!config.enabled && !g_recording)
    {
        // Cancel covers a Start that is still queued or already marked as
        // starting. The control queue filters it when only an ASR result
        // remains, so disabling voice input keeps a finished recognition.
        EnqueueControlCommand(ControlCommand::Cancel);
    }
    else if (stop_recording)
    {
        EnqueueControlCommand(ControlCommand::Stop, CurrentLockTicket());
    }
    ResetKeyboardShortcutState();
    if (release_ralt)
        ForceReleaseRAlt();

    if (ShouldInstallKeyboardHook(config))
    {
        g_keyboard_hook =
            SetWindowsHookExW(WH_KEYBOARD_LL, VoiceInput::Detail::KeyboardHookProc, GetModuleHandleW(nullptr), 0);
    }
}

void VoiceInput::SetImeActive(bool active)
{
    const bool was_active = g_ime_active.exchange(active);
    if (active || !was_active)
        return;

    g_active_hold_shortcut = HoldShortcut::None;
    g_lock_state = 0;
    // Cancel covers a queued or starting capture. The control queue filters it
    // when only an ASR result remains, preserving the finished transcript.
    EnqueueControlCommand(ControlCommand::Cancel);
}

void VoiceInput::ToggleRecording()
{
    EnqueueControlCommand(ControlCommand::Toggle, CurrentLockTicket());
}

void VoiceInput::StartRecording()
{
    EnqueueControlCommand(ControlCommand::Start);
}

void VoiceInput::StopRecording()
{
    EnqueueControlCommand(ControlCommand::Stop, CurrentLockTicket());
}

void VoiceInput::CancelRecording()
{
    EnqueueControlCommand(ControlCommand::Cancel, CurrentLockTicket());
}

bool VoiceInput::IsRecording()
{
    return g_recording;
}

void VoiceInput::Shutdown()
{
    const bool release_ralt = g_suppress_ralt_until_up;
    if (g_keyboard_hook)
    {
        UnhookWindowsHookEx(g_keyboard_hook);
        g_keyboard_hook = nullptr;
    }
    if (g_recording)
        EnqueueControlCommand(ControlCommand::Stop, CurrentLockTicket());
    EnqueueControlCommand(ControlCommand::Exit);
    if (g_control_thread.joinable())
        g_control_thread.join();
    // After the control thread has stopped no new session can start; invalidate
    // before waiting on the network tasks so a late recognition callback can
    // neither show a card nor commit into a stale session during shutdown.
    InvalidateVoiceSession();
    RestoreOtherSystemAudio();
    g_muted_system_audio = false;
    for (auto &task : g_network_tasks)
        task.wait();
    g_network_tasks.clear();
    if (g_message_window)
    {
        DestroyWindow(g_message_window);
        g_message_window = nullptr;
    }
    g_cue_player.shutdown();
    g_overlay.shutdown();
    g_result_card.shutdown();
    VoiceInput::ShutdownTargetTracking();
    ResetKeyboardShortcutState();
    if (release_ralt)
        ForceReleaseRAlt();
    if (g_initialized)
        curl_global_cleanup();
    g_initialized = false;
}
