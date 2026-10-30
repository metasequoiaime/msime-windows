// 光标位置兜底的查询线程，取位顺序参考 FullIME 的 caret_helper：系统光标 → MSAA OBJID_CARET → UIA。
// 不取屏幕中心之类的猜测位置：查不到就保持隐藏，和没有兜底时一样。
#include "window/caret_fallback.h"

#include <oleacc.h>
#include <UIAutomation.h>

#include <condition_variable>
#include <mutex>
#include <thread>

#include <fmt/xchar.h>

#pragma comment(lib, "oleacc.lib")

namespace CaretFallback
{
namespace
{
// UIA 默认的连接超时是 2 秒；宿主卡住时宁可放弃兜底，也不让候选窗晚这么久才出来
constexpr DWORD kUiaConnectionTimeoutMs = 300;
constexpr DWORD kUiaTransactionTimeoutMs = 300;

struct PendingRequest
{
    HWND notifyWindow = nullptr;
    UINT notifyMessage = 0;
    uint32_t generation = 0;
    DWORD clientProcessId = 0;
    ULONGLONG requestedTick = 0;
};

// 线程随进程常驻，状态有意不析构，免得进程退出时静态析构与查询线程互相等待
struct State
{
    std::mutex mutex;
    std::condition_variable wake;
    bool hasPending = false;
    PendingRequest pending;
    bool hasResult = false;
    Result result;
    bool started = false;
};

State &GetState()
{
    static State *state = new State();
    return *state;
}

template <typename T> void SafeRelease(T *&p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

// 候选窗只认落在前台窗口里的光标；(0,0)、零高度或跑出窗口的结果都当作没取到
bool IsPlausibleCaret(HWND foreground, LONG left, LONG top, LONG height)
{
    if (height <= 0 || (left == 0 && top == 0))
    {
        return false;
    }
    RECT window{};
    if (!GetWindowRect(foreground, &window))
    {
        return false;
    }
    const POINT bottom{left, top + height};
    return left >= window.left && left <= window.right && top >= window.top && bottom.y <= window.bottom &&
           MonitorFromPoint(bottom, MONITOR_DEFAULTTONULL) != nullptr;
}

bool TrySystemCaret(HWND foreground, const GUITHREADINFO &info, Result *result)
{
    if (!info.hwndCaret)
    {
        result->trace += L" system=no-caret";
        return false;
    }
    POINT topLeft{info.rcCaret.left, info.rcCaret.top};
    if (!ClientToScreen(info.hwndCaret, &topLeft))
    {
        result->trace += L" system=client-to-screen-failed";
        return false;
    }
    const LONG height = info.rcCaret.bottom - info.rcCaret.top;
    result->trace += fmt::format(L" system=({},{} h={})", topLeft.x, topLeft.y, height);
    if (!IsPlausibleCaret(foreground, topLeft.x, topLeft.y, height))
    {
        return false;
    }
    result->source = Source::SystemCaret;
    result->anchor = {topLeft.x, topLeft.y + height};
    return true;
}

bool TryMsaaCaret(HWND foreground, HWND focus, Result *result)
{
    IAccessible *caret = nullptr;
    const HRESULT hr = AccessibleObjectFromWindow(focus, static_cast<DWORD>(OBJID_CARET), IID_IAccessible,
                                                  reinterpret_cast<void **>(&caret));
    if (FAILED(hr) || !caret)
    {
        result->trace += fmt::format(L" msaa=0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }
    VARIANT self;
    VariantInit(&self);
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    long x = 0, y = 0, width = 0, height = 0;
    const HRESULT locationHr = caret->accLocation(&x, &y, &width, &height, self);
    SafeRelease(caret);
    if (FAILED(locationHr))
    {
        result->trace += fmt::format(L" msaa_location=0x{:08X}", static_cast<unsigned long>(locationHr));
        return false;
    }
    result->trace += fmt::format(L" msaa=({},{} h={})", x, y, height);
    if (!IsPlausibleCaret(foreground, x, y, height))
    {
        return false;
    }
    result->source = Source::MsaaCaret;
    result->anchor = {x, y + height};
    return true;
}

// GetBoundingRectangles 返回 [left, top, width, height] 的序列；取最后一个矩形，
// 选区跨行时那才是光标所在的一行
bool ReadLastRectangle(IUIAutomationTextRange *range, RECT *rect)
{
    SAFEARRAY *rectangles = nullptr;
    if (FAILED(range->GetBoundingRectangles(&rectangles)) || !rectangles)
    {
        return false;
    }
    bool found = false;
    double *values = nullptr;
    LONG lower = 0, upper = -1;
    if (SUCCEEDED(SafeArrayGetLBound(rectangles, 1, &lower)) && SUCCEEDED(SafeArrayGetUBound(rectangles, 1, &upper)) &&
        upper - lower + 1 >= 4 && SUCCEEDED(SafeArrayAccessData(rectangles, reinterpret_cast<void **>(&values))))
    {
        const LONG count = (upper - lower + 1) / 4;
        const double *last = values + (count - 1) * 4;
        rect->left = static_cast<LONG>(last[0]);
        rect->top = static_cast<LONG>(last[1]);
        rect->right = static_cast<LONG>(last[0] + last[2]);
        rect->bottom = static_cast<LONG>(last[1] + last[3]);
        found = true;
        SafeArrayUnaccessData(rectangles);
    }
    SafeArrayDestroy(rectangles);
    return found;
}

// 折叠的光标区间经常没有矩形；这时往前扩一个字符，取那个字符的右边缘当光标位置
bool ReadCaretRectangle(IUIAutomationTextRange *range, RECT *rect)
{
    if (ReadLastRectangle(range, rect) && rect->bottom > rect->top)
    {
        rect->left = rect->right;
        return true;
    }
    IUIAutomationTextRange *previousCharacter = nullptr;
    if (FAILED(range->Clone(&previousCharacter)) || !previousCharacter)
    {
        return false;
    }
    int moved = 0;
    bool found = false;
    if (SUCCEEDED(
            previousCharacter->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character, -1, &moved)) &&
        moved != 0 && ReadLastRectangle(previousCharacter, rect) && rect->bottom > rect->top)
    {
        rect->left = rect->right;
        found = true;
    }
    SafeRelease(previousCharacter);
    return found;
}

bool TryUia(IUIAutomation *automation, HWND foreground, DWORD clientProcessId, Result *result)
{
    if (!automation)
    {
        result->trace += L" uia=unavailable";
        return false;
    }
    IUIAutomationElement *focused = nullptr;
    HRESULT hr = automation->GetFocusedElement(&focused);
    if (FAILED(hr) || !focused)
    {
        result->trace += fmt::format(L" uia_focus=0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }
    int processId = 0;
    if (FAILED(focused->get_CurrentProcessId(&processId)) || static_cast<DWORD>(processId) != clientProcessId)
    {
        result->trace += fmt::format(L" uia_focus_pid={}", processId);
        SafeRelease(focused);
        return false;
    }

    bool found = false;
    RECT rect{};
    IUIAutomationTextPattern2 *pattern2 = nullptr;
    if (SUCCEEDED(focused->GetCurrentPatternAs(UIA_TextPattern2Id, IID_PPV_ARGS(&pattern2))) && pattern2)
    {
        BOOL active = FALSE;
        IUIAutomationTextRange *caretRange = nullptr;
        hr = pattern2->GetCaretRange(&active, &caretRange);
        if (SUCCEEDED(hr) && caretRange)
        {
            found = ReadCaretRectangle(caretRange, &rect);
            result->trace += fmt::format(L" uia_caret=({},{},{},{} found={})", rect.left, rect.top, rect.right,
                                         rect.bottom, found ? 1 : 0);
            if (found && IsPlausibleCaret(foreground, rect.left, rect.top, rect.bottom - rect.top))
            {
                result->source = Source::UiaCaretRange;
            }
            else
            {
                found = false;
            }
        }
        else
        {
            result->trace += fmt::format(L" uia_caret=0x{:08X}", static_cast<unsigned long>(hr));
        }
        SafeRelease(caretRange);
    }
    else
    {
        result->trace += L" uia_pattern2=none";
    }

    if (!found)
    {
        IUIAutomationTextPattern *pattern = nullptr;
        if (pattern2)
        {
            pattern = pattern2;
            pattern->AddRef();
        }
        else if (FAILED(focused->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&pattern))))
        {
            pattern = nullptr;
        }
        if (pattern)
        {
            IUIAutomationTextRangeArray *selections = nullptr;
            int length = 0;
            IUIAutomationTextRange *selection = nullptr;
            if (SUCCEEDED(pattern->GetSelection(&selections)) && selections &&
                SUCCEEDED(selections->get_Length(&length)) && length > 0 &&
                SUCCEEDED(selections->GetElement(0, &selection)) && selection)
            {
                found = ReadCaretRectangle(selection, &rect);
                result->trace += fmt::format(L" uia_selection=({},{},{},{} found={})", rect.left, rect.top, rect.right,
                                             rect.bottom, found ? 1 : 0);
                if (found && IsPlausibleCaret(foreground, rect.left, rect.top, rect.bottom - rect.top))
                {
                    result->source = Source::UiaSelection;
                }
                else
                {
                    found = false;
                }
            }
            else
            {
                result->trace += fmt::format(L" uia_selection=none len={}", length);
            }
            SafeRelease(selection);
            SafeRelease(selections);
            SafeRelease(pattern);
        }
        else
        {
            result->trace += L" uia_text_pattern=none";
        }
    }
    SafeRelease(pattern2);
    SafeRelease(focused);

    if (found)
    {
        result->anchor = {rect.left, rect.bottom};
    }
    return found;
}

IUIAutomation *CreateAutomation()
{
    IUIAutomation *automation = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))))
    {
        return nullptr;
    }
    IUIAutomation2 *automation2 = nullptr;
    if (SUCCEEDED(automation->QueryInterface(IID_PPV_ARGS(&automation2))) && automation2)
    {
        automation2->put_ConnectionTimeout(kUiaConnectionTimeoutMs);
        automation2->put_TransactionTimeout(kUiaTransactionTimeoutMs);
        SafeRelease(automation2);
    }
    return automation;
}

Result Resolve(IUIAutomation *automation, const PendingRequest &request)
{
    Result result;
    result.generation = request.generation;
    const HWND foreground = GetForegroundWindow();
    DWORD processId = 0;
    const DWORD threadId = foreground ? GetWindowThreadProcessId(foreground, &processId) : 0;
    if (threadId == 0 || processId != request.clientProcessId)
    {
        result.trace = fmt::format(L"foreground_pid={} client_pid={}", processId, request.clientProcessId);
        return result;
    }
    GUITHREADINFO info{sizeof(info)};
    GetGUIThreadInfo(threadId, &info);
    const HWND focus = info.hwndFocus ? info.hwndFocus : foreground;
    if (!TrySystemCaret(foreground, info, &result) && !TryMsaaCaret(foreground, focus, &result))
    {
        TryUia(automation, foreground, request.clientProcessId, &result);
    }
    return result;
}

void WorkerLoop()
{
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation *automation = SUCCEEDED(comHr) ? CreateAutomation() : nullptr;
    State &state = GetState();
    for (;;)
    {
        PendingRequest request;
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.wake.wait(lock, [&state] { return state.hasPending; });
            request = state.pending;
            state.hasPending = false;
        }
        Result result = Resolve(automation, request);
        result.elapsedMs = GetTickCount64() - request.requestedTick;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.result = std::move(result);
            state.hasResult = true;
        }
        PostMessageW(request.notifyWindow, request.notifyMessage, request.generation, 0);
    }
}
} // namespace

void Request(HWND notifyWindow, UINT notifyMessage, uint32_t generation, DWORD clientProcessId)
{
    State &state = GetState();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.pending = {notifyWindow, notifyMessage, generation, clientProcessId, GetTickCount64()};
        state.hasPending = true;
        if (!state.started)
        {
            state.started = true;
            std::thread(WorkerLoop).detach();
        }
    }
    state.wake.notify_one();
}

bool TakeResult(uint32_t generation, Result *result)
{
    State &state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.hasResult || state.result.generation != generation)
    {
        return false;
    }
    *result = std::move(state.result);
    state.hasResult = false;
    return true;
}

const wchar_t *SourceName(Source source)
{
    switch (source)
    {
    case Source::SystemCaret:
        return L"system-caret";
    case Source::MsaaCaret:
        return L"msaa-caret";
    case Source::UiaCaretRange:
        return L"uia-caret-range";
    case Source::UiaSelection:
        return L"uia-selection";
    default:
        return L"none";
    }
}
} // namespace CaretFallback
