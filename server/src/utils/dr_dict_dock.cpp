#include "utils/dr_dict_dock.h"

#include "utils/dr_dict_dock_policy.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace DrDictDock
{
namespace
{
std::mutex g_mutex;
std::condition_variable g_cv;
std::thread g_thread;
bool g_running = false;
// 界面线程想要的状态，和最后一次真正送到 DR Dict 的状态。没送到（DR Dict 没运行、旧版不认）时
// 后者保持「收起」，DR Dict 之后启动了，下一次变化会照常发过去。
State g_wanted;
State g_sent;
bool g_pending = false;

bool Send(const State &state)
{
    HWND popup = FindWindowW(DrDictLookup::kPopupWindowClass, nullptr);
    if (popup == nullptr)
        return false;
    const std::string payload = BuildPayload(state);
    COPYDATASTRUCT data{static_cast<ULONG_PTR>(kDockRequest), static_cast<DWORD>(payload.size()),
                        const_cast<char *>(payload.data())};
    DWORD_PTR handled = 0;
    // DR Dict 只把查询排进它的工作线程就返回。旧版不认 DOCK，回 FALSE。
    return SendMessageTimeoutW(popup, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 500,
                               &handled) &&
           handled;
}

void ThreadMain()
{
    std::unique_lock lock(g_mutex);
    while (true)
    {
        g_cv.wait(lock, [] { return !g_running || g_pending; });
        if (!g_running)
            break;
        g_pending = false;
        const State wanted = g_wanted;
        if (SameState(wanted, g_sent))
            continue;
        lock.unlock();
        const bool sent = Send(wanted);
        lock.lock();
        g_sent = sent ? wanted : State{};
    }
    // 退出时收起停靠中的弹窗，不留在屏幕上。
    if (g_sent.visible)
    {
        lock.unlock();
        Send(State{});
        lock.lock();
        g_sent = State{};
    }
}

void Want(State state)
{
    {
        std::lock_guard lock(g_mutex);
        if (!g_running)
            return;
        g_wanted = std::move(state);
        g_pending = true;
    }
    g_cv.notify_one();
}
} // namespace

void Start()
{
    std::lock_guard lock(g_mutex);
    if (g_running)
        return;
    g_running = true;
    try
    {
        g_thread = std::thread(ThreadMain);
    }
    catch (...)
    {
        g_running = false;
    }
}

void Stop()
{
    {
        std::lock_guard lock(g_mutex);
        if (!g_running)
            return;
        g_running = false;
    }
    g_cv.notify_all();
    if (g_thread.joinable())
        g_thread.join();
}

void Show(const std::wstring &word, const RECT &card)
{
    if (word.empty() || card.right <= card.left || card.bottom <= card.top)
    {
        Hide();
        return;
    }
    Want(State{true, word, DrDictLookup::CaretRect{card.left, card.top, card.right, card.bottom}});
}

void Hide()
{
    Want(State{});
}
} // namespace DrDictDock
