#include "voice-input/voice_target.h"

#include <windows.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <atomic>
#include <cwchar>

namespace VoiceInput
{
namespace
{
using Microsoft::WRL::ComPtr;

// Monotonic focus epoch. Foreground changes and focus events in that window advance it; a
// background thread stamps the value before the UIA query and the result is
// discarded if the epoch moved meanwhile (focus changed during the query).
std::atomic<std::uint64_t> g_focus_generation{1};
std::atomic<bool> g_tracking{false};
// Hook handles are owned by the Server UI thread; CaptureTarget only reads the
// two atomics above.
HWINEVENTHOOK g_foreground_hook = nullptr;
HWINEVENTHOOK g_focus_hook = nullptr;

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND window, LONG, LONG, DWORD, DWORD)
{
    // Accessibility providers also report focus events for background windows.
    // Those do not change the input destination and must not invalidate an ASR
    // result or stop an otherwise healthy inline composition.
    if (event == EVENT_OBJECT_FOCUS && (!window || GetAncestor(window, GA_ROOT) != GetForegroundWindow()))
    {
        return;
    }
    g_focus_generation.fetch_add(1, std::memory_order_relaxed);
}

struct ComScope
{
    // S_OK and S_FALSE both increment the thread's COM apartment reference
    // (S_OK created it, S_FALSE means the thread was already in an MTA); each
    // successful init must be matched by a CoUninitialize. A failed init
    // (including RPC_E_CHANGED_MODE for an STA thread) owns nothing.
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool initialized = SUCCEEDED(result);
    ~ComScope()
    {
        if (initialized)
        {
            CoUninitialize();
        }
    }
};

constexpr DWORD kUiAProviderTimeoutMs = 100;
constexpr LONG kMaxRuntimeIdElements = 1024;

// Asks UIA for the element with keyboard focus and classifies whether it is
// editable. `target` already carries the foreground-host identity and
// `target.focus_process` is the process that owns the focused child window.
// `known` says whether the verdict is
// definitive:
//  * Provider unavailable (COM init, CUIAutomation creation or
//    GetFocusedElement fails, or there is no focused element): `known` stays
//    false and the caller may fall back to the native caret.
//  * Once an element is acquired, a failed runtime-ID or property read, a
//    background-process element, a non-focused element, a disabled element, and
//    a read-only element are all fail-closed (`known` true, editable false):
//    the native-caret fallback must never override them.
//  * The native-caret supplement is reached only when the element identity
//    (runtime ID) resolved and neither the Value nor the Text pattern
//    classifies editability (patterns genuinely absent, not a read failure).
bool ReadAutomation(Target &target, bool &known)
{
    ComScope com;
    if (FAILED(com.result))
    {
        return false;
    }
    ComPtr<IUIAutomation2> automation;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))))
    {
        return false;
    }
    // The connection/transaction timeouts bound each cross-process provider
    // call individually; they do not impose one deadline over the whole query.
    automation->put_ConnectionTimeout(kUiAProviderTimeoutMs);
    automation->put_TransactionTimeout(kUiAProviderTimeoutMs);

    ComPtr<IUIAutomationElement> element;
    if (FAILED(automation->GetFocusedElement(&element)) || !element)
    {
        return false;
    }

    // From this point the element is in hand; any failure below is fail-closed
    // so an ambiguous or contradictory result never claims editability.
    const auto fail_closed = [&target, &known]() {
        known = true;
        target.editable = false;
        return false;
    };

    int process = 0;
    if (FAILED(element->get_CurrentProcessId(&process)))
    {
        return fail_closed();
    }
    if (process != static_cast<int>(target.focus_process))
    {
        // The focused element does not belong to the process that owns the
        // focused child window. Treat a confirmed background element as
        // non-editable, never as "unknown".
        known = true;
        target.editable = false;
        return false;
    }
    BOOL focused = FALSE;
    if (FAILED(element->get_CurrentHasKeyboardFocus(&focused)))
    {
        return fail_closed();
    }
    if (!focused)
    {
        known = true;
        target.editable = false;
        return false;
    }
    // A reported native handle must belong to the captured foreground window
    // tree and to the focused side of it. This ties the UIA element to the
    // actual focus HWND instead of accepting any element of the same process.
    // Providers may omit the handle; keyboard focus, process and the runtime ID
    // below still pin those elements.
    UIA_HWND native_window = nullptr;
    if (FAILED(element->get_CurrentNativeWindowHandle(&native_window)))
    {
        return fail_closed();
    }
    if (native_window)
    {
        const HWND native = static_cast<HWND>(native_window);
        const HWND foreground = reinterpret_cast<HWND>(target.foreground);
        const HWND focus = reinterpret_cast<HWND>(target.focus);
        DWORD native_process = 0;
        GetWindowThreadProcessId(native, &native_process);
        if (!IsWindow(native) || native_process != static_cast<DWORD>(process) ||
            GetAncestor(native, GA_ROOT) != foreground ||
            (native != focus && !IsChild(focus, native) && !IsChild(native, focus)))
        {
            return fail_closed();
        }
    }
    BOOL enabled = FALSE;
    if (FAILED(element->get_CurrentIsEnabled(&enabled)))
    {
        return fail_closed();
    }
    if (!enabled)
    {
        known = true;
        target.editable = false;
        return false;
    }

    // Runtime ID is the field identity inside a shared HWND (Chromium, OWA,
    // ...). Different fields of the same window must compare unequal, so a
    // missing or invalid runtime ID is fail-closed: editability cannot be
    // claimed and the result keeps the card.
    target.runtime_id.clear();
    SAFEARRAY *id = nullptr;
    bool id_ok = false;
    if (SUCCEEDED(element->GetRuntimeId(&id)) && id)
    {
        LONG first = 0;
        LONG last = -1;
        if (SUCCEEDED(SafeArrayGetLBound(id, 1, &first)) && SUCCEEDED(SafeArrayGetUBound(id, 1, &last)) &&
            last >= first && last - first < kMaxRuntimeIdElements)
        {
            target.runtime_id.reserve(static_cast<size_t>(last - first + 1));
            bool valid = true;
            for (LONG index = first; index <= last; ++index)
            {
                int value = 0;
                if (FAILED(SafeArrayGetElement(id, &index, &value)))
                {
                    valid = false;
                    break;
                }
                target.runtime_id.push_back(value);
            }
            if (!valid || target.runtime_id.empty())
            {
                target.runtime_id.clear();
            }
            else
            {
                id_ok = true;
            }
        }
        SafeArrayDestroy(id);
    }
    if (!id_ok)
    {
        return fail_closed();
    }

    // Value pattern. A live pattern is authoritative and a failed read of the
    // property is fail-closed; only a genuinely absent pattern may defer to
    // the native caret.
    ComPtr<IUIAutomationValuePattern> value;
    HRESULT value_result = element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value));
    if (SUCCEEDED(value_result) && value)
    {
        BOOL read_only = TRUE;
        if (FAILED(value->get_CurrentIsReadOnly(&read_only)))
        {
            return fail_closed();
        }
        known = true;
        target.editable = read_only == FALSE;
        return target.editable;
    }
    if (FAILED(value_result) && value_result != UIA_E_NOTSUPPORTED && value_result != E_NOINTERFACE)
    {
        return fail_closed();
    }

    // Text pattern, same policy: a live pattern is authoritative, a failed read
    // is fail-closed, and only a genuinely absent pattern may defer to the caret.
    ComPtr<IUIAutomationTextPattern> text;
    HRESULT text_result = element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text));
    if (SUCCEEDED(text_result) && text)
    {
        ComPtr<IUIAutomationTextRange> range;
        if (FAILED(text->get_DocumentRange(&range)) || !range)
        {
            return fail_closed();
        }
        VARIANT read_only{};
        VariantInit(&read_only);
        const HRESULT read_result = range->GetAttributeValue(UIA_IsReadOnlyAttributeId, &read_only);
        const bool readable = SUCCEEDED(read_result) && read_only.vt == VT_BOOL;
        const bool not_read_only = readable && read_only.boolVal == VARIANT_FALSE;
        VariantClear(&read_only);
        if (!readable)
        {
            return fail_closed();
        }
        known = true;
        target.editable = not_read_only;
        return target.editable;
    }
    if (FAILED(text_result) && text_result != UIA_E_NOTSUPPORTED && text_result != E_NOINTERFACE)
    {
        return fail_closed();
    }

    // Both patterns are genuinely absent but the element identity resolved: the
    // caller's native-caret supplement remains enabled for this control.
    return false;
}

bool IsStandardEditClass(const wchar_t *class_name)
{
    return _wcsicmp(class_name, L"Edit") == 0 || _wcsnicmp(class_name, L"RichEdit", 8) == 0;
}
} // namespace

bool InitializeTargetTracking()
{
    if (g_tracking.load(std::memory_order_relaxed))
    {
        return true;
    }
    g_foreground_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnWinEvent, 0, 0,
                                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_focus_hook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, OnWinEvent, 0, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_tracking.store(g_foreground_hook && g_focus_hook, std::memory_order_relaxed);
    if (!g_tracking.load(std::memory_order_relaxed))
    {
        ShutdownTargetTracking();
    }
    return g_tracking.load(std::memory_order_relaxed);
}

void ShutdownTargetTracking()
{
    g_tracking.store(false, std::memory_order_relaxed);
    if (g_foreground_hook)
    {
        UnhookWinEvent(g_foreground_hook);
    }
    if (g_focus_hook)
    {
        UnhookWinEvent(g_focus_hook);
    }
    g_foreground_hook = nullptr;
    g_focus_hook = nullptr;
    g_focus_generation.fetch_add(1, std::memory_order_relaxed);
}

Target CaptureTarget()
{
    Target target;
    target.generation = g_focus_generation.load(std::memory_order_relaxed);
    if (!g_tracking.load(std::memory_order_relaxed))
    {
        return target;
    }

    const HWND foreground = GetForegroundWindow();
    DWORD foreground_process = 0;
    const DWORD foreground_thread = GetWindowThreadProcessId(foreground, &foreground_process);
    GUITHREADINFO info{sizeof(info)};
    if (!foreground || !foreground_thread || !GetGUIThreadInfo(foreground_thread, &info) || !info.hwndFocus ||
        GetAncestor(info.hwndFocus, GA_ROOT) != foreground)
    {
        return target;
    }

    DWORD focus_process = 0;
    const DWORD focus_thread = GetWindowThreadProcessId(info.hwndFocus, &focus_process);
    if (!foreground_process || !focus_thread || !focus_process)
    {
        return target;
    }
    target.foreground = reinterpret_cast<std::uintptr_t>(foreground);
    target.focus = reinterpret_cast<std::uintptr_t>(info.hwndFocus);
    target.process = foreground_process;
    target.focus_process = focus_process;
    target.thread = focus_thread;

    wchar_t class_name[128]{};
    GetClassNameW(info.hwndFocus, class_name, 128);
    if (IsStandardEditClass(class_name))
    {
        // Authoritative for standard Edit / RichEdit: enabled and not
        // ES_READONLY. A disabled or read-only control is never editable.
        const bool enabled = IsWindowEnabled(info.hwndFocus) != FALSE;
        const bool read_only = (GetWindowLongPtrW(info.hwndFocus, GWL_STYLE) & ES_READONLY) != 0;
        target.editable = enabled && !read_only;
    }
    else
    {
        // A definitive non-editable verdict (known) never reaches the caret
        // branch; an unknown verdict (provider unavailable or patterns absent)
        // falls back to the native caret.
        bool known = false;
        const bool editable = ReadAutomation(target, known);
        if (known)
        {
            target.editable = editable;
        }
        else
        {
            // The caret is evidence only for the focused control itself, not
            // for a sibling/leftover caret elsewhere in the same top-level
            // window: require the caret HWND to be the focus HWND or a child of
            // it. Explicit read-only/disabled verdicts never reach this branch.
            target.editable = info.hwndCaret != nullptr && IsWindowEnabled(info.hwndFocus) != FALSE &&
                              (info.hwndCaret == info.hwndFocus || IsChild(info.hwndFocus, info.hwndCaret)) &&
                              GetAncestor(info.hwndCaret, GA_ROOT) == foreground;
        }
    }

    // The provider query may have pumped messages or focus may have moved while
    // it ran; the epoch tracks foreground focus events. Any observed change invalidates
    // the snapshot so the caller keeps the card instead of committing elsewhere.
    GUITHREADINFO after{sizeof(after)};
    const bool stable = g_focus_generation.load(std::memory_order_relaxed) == target.generation &&
                        GetForegroundWindow() == foreground && GetGUIThreadInfo(foreground_thread, &after) &&
                        after.hwndFocus == info.hwndFocus;
    if (!stable)
    {
        target.editable = false;
    }
    return target;
}

bool IsTargetCurrentFast(const Target &target)
{
    // Only an editable snapshot can be "current" for a commit route; a snapshot
    // taken without live tracking or with no focus routes to the card instead.
    if (!target.editable || !g_tracking.load(std::memory_order_relaxed))
    {
        return false;
    }
    if (g_focus_generation.load(std::memory_order_relaxed) != target.generation)
    {
        return false;
    }
    const HWND foreground = reinterpret_cast<HWND>(target.foreground);
    const HWND focus = reinterpret_cast<HWND>(target.focus);
    if (!foreground || !focus || GetForegroundWindow() != foreground || target.process == 0 ||
        target.focus_process == 0 || target.thread == 0)
    {
        return false;
    }
    DWORD foreground_process = 0;
    const DWORD foreground_thread = GetWindowThreadProcessId(foreground, &foreground_process);
    DWORD focus_process = 0;
    const DWORD focus_thread = GetWindowThreadProcessId(focus, &focus_process);
    if (!foreground_thread || foreground_process != target.process || focus_thread != target.thread ||
        focus_process != target.focus_process || GetAncestor(focus, GA_ROOT) != foreground)
    {
        return false;
    }
    GUITHREADINFO info{sizeof(info)};
    if (!GetGUIThreadInfo(focus_thread, &info) || info.hwndFocus != focus)
    {
        return false;
    }
    // Native Edit/RichEdit fast path: re-check enabled/read-only at decision
    // time so a control disabled or made read-only during the ASR wait no
    // longer counts as committable. Non-Edit targets are identity-checked only;
    // editability re-verification for those needs UIA and stays with the full
    // CaptureTarget call outside the lock.
    wchar_t class_name[128]{};
    GetClassNameW(focus, class_name, 128);
    if (IsStandardEditClass(class_name))
    {
        const bool enabled = IsWindowEnabled(focus) != FALSE;
        const bool read_only = (GetWindowLongPtrW(focus, GWL_STYLE) & ES_READONLY) != 0;
        if (!enabled || read_only)
        {
            return false;
        }
    }
    return true;
}
} // namespace VoiceInput
