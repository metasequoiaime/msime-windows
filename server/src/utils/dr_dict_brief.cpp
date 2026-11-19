#include "utils/dr_dict_brief.h"

#include "utils/dr_dict_brief_policy.h"
#include "utils/dr_dict_lookup_policy.h"

#include <Windows.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace DrDictBrief
{
namespace
{
constexpr wchar_t kWindowClass[] = L"MetasequoiaImeDrDictBrief";
constexpr UINT kSendLatest = WM_APP + 1;
// 同时挂着的请求上限：DR Dict 没回的请求（它在查询时被关掉）不能让表一直长下去。
constexpr std::size_t kMaxPending = 64;

std::mutex g_mutex;
std::condition_variable g_ready_cv;
std::thread g_thread;
HWND g_window = nullptr;
bool g_started = false;
bool g_thread_ready = false;
Callback g_callback;
struct PendingRequest
{
    std::string word;
    bool brief = false;
    bool phonetics = false;
};
PendingRequest g_latest;
// 只在本模块的线程上读写。
std::uint64_t g_next_id = 0;
std::unordered_map<std::uint64_t, PendingRequest> g_pending;

void SendLatest(HWND window)
{
    PendingRequest request;
    {
        std::lock_guard lock(g_mutex);
        request = std::exchange(g_latest, PendingRequest());
    }
    if (request.word.empty() || (!request.brief && !request.phonetics))
        return;
    HWND popup = FindWindowW(DrDictLookup::kPopupWindowClass, nullptr);
    if (popup == nullptr)
        return;
    if (g_pending.size() >= kMaxPending)
        g_pending.clear();
    const std::uint64_t id = ++g_next_id;
    const std::string payload =
        BuildRequestPayload(request.word, static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(window)), id,
                            kMaxChars, request.brief, request.phonetics);
    COPYDATASTRUCT data{static_cast<ULONG_PTR>(kBriefRequest), static_cast<DWORD>(payload.size()),
                        const_cast<char *>(payload.data())};
    DWORD_PTR handled = 0;
    // DR Dict 只把请求排进它的查询线程就返回，这里等的只是消息本身。旧版 DR Dict 不认这种请求，回 FALSE。
    // SMTO_BLOCK：等待期间不处理别的发来的消息，否则回复可能赶在下面记下请求号之前到达而被丢掉。
    if (SendMessageTimeoutW(popup, WM_COPYDATA, reinterpret_cast<WPARAM>(window), reinterpret_cast<LPARAM>(&data),
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &handled) &&
        handled)
    {
        g_pending.emplace(id, std::move(request));
    }
}

void ReceiveReply(const COPYDATASTRUCT &data)
{
    if (data.dwData != kBriefReply || data.lpData == nullptr)
        return;
    const auto reply = ParseReply(std::string_view(static_cast<const char *>(data.lpData), data.cbData));
    if (!reply)
        return;
    // 只认自己发出去、还没回过的请求，词也要对得上：别的程序冒充的回复不显示。
    const auto pending = g_pending.find(reply->id);
    if (pending == g_pending.end() || pending->second.word != reply->word)
        return;
    Answer answer;
    answer.word = std::move(pending->second.word);
    answer.asked_brief = pending->second.brief;
    answer.asked_phonetics = pending->second.phonetics;
    g_pending.erase(pending);
    answer.text = reply->text;
    answer.has_phonetics = reply->has_phonetics;
    answer.uk = reply->uk;
    answer.us = reply->us;
    Callback callback;
    {
        std::lock_guard lock(g_mutex);
        callback = g_callback;
    }
    if (callback)
        callback(std::move(answer));
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case kSendLatest:
        SendLatest(window);
        return 0;
    case WM_COPYDATA:
        if (const auto *data = reinterpret_cast<const COPYDATASTRUCT *>(lparam))
            ReceiveReply(*data);
        return TRUE;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

void ThreadMain()
{
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kWindowClass;
    RegisterClassW(&window_class);
    HWND window =
        CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, window_class.hInstance, nullptr);
    if (window != nullptr)
    {
        // 服务端带 uiAccess，普通进程（DR Dict）发来的 WM_COPYDATA 会被 UIPI 拦下，回复要专门放行。
        ChangeWindowMessageFilterEx(window, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    }
    {
        std::lock_guard lock(g_mutex);
        g_window = window;
        g_thread_ready = true;
    }
    g_ready_cv.notify_all();
    if (window == nullptr)
        return;

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DestroyWindow(window);
    g_pending.clear();
}
} // namespace

void Start(Callback callback)
{
    std::unique_lock lock(g_mutex);
    if (g_started)
        return;
    g_started = true;
    g_thread_ready = false;
    g_callback = std::move(callback);
    try
    {
        g_thread = std::thread(ThreadMain);
    }
    catch (...)
    {
        g_started = false;
        g_callback = {};
        return;
    }
    // 等窗口建好（或建不了），之后的 Request 才有地方投递。
    g_ready_cv.wait(lock, [] { return g_thread_ready; });
}

void Stop()
{
    HWND window = nullptr;
    {
        std::lock_guard lock(g_mutex);
        if (!g_started)
            return;
        g_started = false;
        window = g_window;
        g_window = nullptr;
        g_callback = {};
    }
    if (window != nullptr)
        PostMessageW(window, WM_QUIT, 0, 0);
    if (g_thread.joinable())
        g_thread.join();
}

void Request(const std::string &word, bool brief, bool phonetics)
{
    HWND window = nullptr;
    {
        std::lock_guard lock(g_mutex);
        if (!g_started || g_window == nullptr || word.empty() || (!brief && !phonetics))
            return;
        g_latest = {word, brief, phonetics};
        window = g_window;
    }
    PostMessageW(window, kSendLatest, 0, 0);
}
} // namespace DrDictBrief
