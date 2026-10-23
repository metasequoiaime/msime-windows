#include "voice-input/voice_target.h"
#include <windows.h>
#include <chrono>
#include <future>
#include <iostream>
#include <string>

namespace
{
constexpr UINT kFocus = WM_APP + 20;
LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_APP + 21)
    {
        HWND background = CreateWindowW(L"Static", L"background notification", WS_OVERLAPPED, 0, 0, 10, 10, nullptr,
                                        nullptr, GetModuleHandleW(nullptr), nullptr);
        NotifyWinEvent(EVENT_OBJECT_FOCUS, background, OBJID_CLIENT, 0);
        DestroyWindow(background);
        return 0;
    }
    if (m == kFocus)
    {
        DestroyCaret();
        if (w == 6)
        {
            SetFocus(GetDlgItem(h, 3));
            const HWND sibling = GetDlgItem(h, 1);
            CreateCaret(sibling, nullptr, 2, 18);
            ShowCaret(sibling);
        }
        else
            SetFocus(GetDlgItem(h, static_cast<int>(w)));
        return 0;
    }
    if (m == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
void Pump(unsigned ms = 50)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    do
    {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(1);
    } while (std::chrono::steady_clock::now() < end);
}
VoiceInput::Target Capture()
{
    auto f = std::async(std::launch::async, [] { return VoiceInput::CaptureTarget(); });
    while (f.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        Pump(5);
    return f.get();
}
int Host(const wchar_t *title)
{
    auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = HostProc;
    wc.lpszClassName = L"VoiceTargetReviewFixture";
    RegisterClassW(&wc);
    HWND root = CreateWindowW(wc.lpszClassName, title, WS_OVERLAPPEDWINDOW, 100, 100, 440, 310, nullptr, nullptr,
                              instance, nullptr);
    CreateWindowW(L"Edit", L"synthetic editable", WS_CHILD | WS_VISIBLE | WS_BORDER, 20, 20, 330, 30, root,
                  reinterpret_cast<HMENU>(1), instance, nullptr);
    CreateWindowW(L"Edit", L"readonly fixture", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY, 20, 65, 330, 30, root,
                  reinterpret_cast<HMENU>(2), instance, nullptr);
    CreateWindowW(L"Button", L"non-editor fixture", WS_CHILD | WS_VISIBLE, 20, 110, 200, 30, root,
                  reinterpret_cast<HMENU>(3), instance, nullptr);
    LoadLibraryW(L"Msftedit.dll");
    CreateWindowW(L"RICHEDIT50W", L"rich fixture", WS_CHILD | WS_VISIBLE | WS_BORDER, 20, 155, 330, 30, root,
                  reinterpret_cast<HMENU>(4), instance, nullptr);
    CreateWindowW(L"RICHEDIT50W", L"readonly rich", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY, 20, 200, 330, 30,
                  root, reinterpret_cast<HMENU>(5), instance, nullptr);
    ShowWindow(root, SW_SHOW);
    SetForegroundWindow(root);
    SetFocus(GetDlgItem(root, 1));
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
} // namespace
int wmain(int argc, wchar_t **argv)
{
    if (argc > 2 && std::wstring(argv[1]) == L"--host")
        return Host(argv[2]);
    if (argc > 2 && std::wstring(argv[1]) == L"--embedded")
    {
        const auto parent = reinterpret_cast<HWND>(std::stoull(argv[2]));
        HWND edit = CreateWindowW(L"Edit", L"cross-process editor", WS_CHILD | WS_VISIBLE | WS_BORDER, 20, 245, 330, 25,
                                  parent, reinterpret_cast<HMENU>(7), GetModuleHandleW(nullptr), nullptr);
        if (!edit)
            return 3;
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        DestroyWindow(edit);
        return 0;
    }
    // Opt in explicitly: this integration test changes foreground focus.
    wchar_t opt_in[2]{};
    if (GetEnvironmentVariableW(L"MSIME_RUN_INTERACTIVE_VOICE_TESTS", opt_in, 2) != 1 || opt_in[0] != L'1')
    {
        std::cout << "SKIP: set MSIME_RUN_INTERACTIVE_VOICE_TESTS=1 to run focus integration tests\n";
        return 77;
    }
    HWND original = GetForegroundWindow();
    if (!VoiceInput::InitializeTargetTracking())
        return 1;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring title = L"Voice target test " + std::to_wstring(GetCurrentProcessId());
    std::wstring command = L"\"" + std::wstring(path) + L"\" --host \"" + title + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return 1;
    AllowSetForegroundWindow(pi.dwProcessId);
    HWND root = nullptr;
    for (int i = 0; i < 100 && !root; ++i)
    {
        Pump(10);
        root = FindWindowW(L"VoiceTargetReviewFixture", title.c_str());
    }
    Pump(150);
    if (!root || GetForegroundWindow() != root)
    {
        DWORD host_exit = 0;
        GetExitCodeProcess(pi.hProcess, &host_exit);
        std::cout << "SKIP fixture could not acquire foreground; root=" << root
                  << " foreground=" << GetForegroundWindow() << " host_exit=" << host_exit << "\n";
        if (root)
            PostMessageW(root, WM_CLOSE, 0, 0);
        VoiceInput::ShutdownTargetTracking();
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 77;
    }
    int failures = 0;
    auto check = [&](bool ok, const char *s) {
        std::cout << (ok ? "PASS " : "FAIL ") << s << "\n";
        failures += !ok;
    };
    auto focus = [&](int id) {
        SendMessageW(root, kFocus, id, 0);
        Pump();
    };
    focus(1);
    const auto first = Capture();
    check(first.editable, "editable Edit");
    check(VoiceInput::IsTargetCurrentFast(first), "fast recheck accepts original");
    check(VoiceInput::SameTarget(first, Capture()), "unchanged target matches");
    SendMessageW(root, WM_APP + 21, 0, 0);
    Pump();
    check(VoiceInput::SameTarget(first, Capture()), "background focus event preserves current input target");
    focus(2);
    check(!Capture().editable, "readonly Edit with caret rejected");
    check(!VoiceInput::IsTargetCurrentFast(first), "fast recheck rejects different field");
    focus(1);
    const auto returned = Capture();
    check(returned.editable && returned.generation != first.generation,
          "external focus leave/return advances generation");
    check(!VoiceInput::SameTarget(first, returned), "return to same field invalidates old snapshot");
    focus(3);
    check(!Capture().editable, "button rejected through UIA");
    focus(6);
    GUITHREADINFO gui{sizeof(gui)};
    GetGUIThreadInfo(GetWindowThreadProcessId(root, nullptr), &gui);
    check(gui.hwndFocus == GetDlgItem(root, 3) && gui.hwndCaret == GetDlgItem(root, 1),
          "fixture establishes focused button with sibling caret");
    check(!Capture().editable, "sibling caret cannot make non-editor focus editable");
    focus(4);
    check(Capture().editable, "editable RichEdit");
    focus(5);
    check(!Capture().editable, "readonly RichEdit rejected");
    std::wstring embedded_command =
        L"\"" + std::wstring(path) + L"\" --embedded " + std::to_wstring(reinterpret_cast<std::uintptr_t>(root));
    PROCESS_INFORMATION embedded{};
    const bool spawned = CreateProcessW(nullptr, embedded_command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                                        &si, &embedded) != FALSE;
    HWND child = nullptr;
    if (spawned)
        for (int i = 0; i < 100 && !child; ++i)
        {
            Pump(10);
            child = GetDlgItem(root, 7);
        }
    check(spawned && child, "cross-process embedded Edit created");
    if (child)
    {
        focus(7);
        const auto cross = Capture();
        check(cross.editable && cross.process == pi.dwProcessId && cross.focus_process == embedded.dwProcessId &&
                  cross.process != cross.focus_process,
              "capture separates host and focused child process");
        check(VoiceInput::IsTargetCurrentFast(cross), "fast recheck accepts cross-process embedded Edit");
        SendMessageW(child, EM_SETREADONLY, TRUE, 0);
        check(!Capture().editable && !VoiceInput::IsTargetCurrentFast(cross), "cross-process readonly change rejected");
        SendMessageW(child, EM_SETREADONLY, FALSE, 0);
        focus(1);
        check(!VoiceInput::IsTargetCurrentFast(cross), "cross-process target rejected after focus moves");
    }
    if (spawned)
    {
        PostThreadMessageW(embedded.dwThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(embedded.hProcess, 1500);
        CloseHandle(embedded.hThread);
        CloseHandle(embedded.hProcess);
    }
    VoiceInput::ShutdownTargetTracking();
    check(!Capture().editable, "shutdown fails closed");
    PostMessageW(root, WM_CLOSE, 0, 0);
    WaitForSingleObject(pi.hProcess, 1500);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (original)
        SetForegroundWindow(original);
    std::cout << failures << " failures\n";
    return failures ? 1 : 0;
}
