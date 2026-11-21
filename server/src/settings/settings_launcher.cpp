#include "settings_launcher.h"

#include <Windows.h>
#include <filesystem>

#pragma comment(lib, "Shell32.lib")

namespace
{
constexpr wchar_t kSettingsWindowClass[] = L"MetasequoiaImeSettingsWindow";
constexpr wchar_t kEmojiPanelWindowClass[] = L"msimeui.EmojiPanel";
constexpr wchar_t kKeyboardPanelWindowClass[] = L"msimeui.KeyboardDemo";
constexpr wchar_t kHandwritingPanelWindowClass[] = L"msimeui.HandwritingDemo";
constexpr UINT kActivateSettings = WM_APP + 1;
constexpr UINT kOpenSettingsAbout = WM_APP + 2;
constexpr UINT kQuitSettings = WM_APP + 5;
constexpr UINT kOpenSettingsVoice = WM_APP + 7;

bool OpenSiblingApplication(const wchar_t *executable_name, const wchar_t *window_class)
{
    if (window_class)
    {
        if (const HWND existing_window = FindWindowW(window_class, nullptr))
        {
            ShowWindow(existing_window, SW_SHOWNORMAL);
            return SetForegroundWindow(existing_window) != FALSE;
        }
    }

    std::wstring module_path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (length == 0 || length >= module_path.size())
    {
        return false;
    }
    module_path.resize(length);

    const std::filesystem::path application_path = std::filesystem::path(module_path).parent_path() / executable_name;
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", application_path.c_str(), nullptr,
                                           application_path.parent_path().c_str(), SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool CloseApplication(const wchar_t *window_class)
{
    const HWND application_window = FindWindowW(window_class, nullptr);
    return !application_window || PostMessageW(application_window, WM_CLOSE, 0, 0) != FALSE;
}

// 设置窗口被关闭后只是隐藏，进程会再留一段时间。重新打开必须把消息投给它自己处理，
// 不能在这边直接 ShowWindow：那样它的延迟退出定时器不会取消，窗口会在用户面前被销毁，
// 而且 SW_SHOWNORMAL 还会把之前最大化的窗口还原掉。
bool ActivateSettingsWindow(HWND window, UINT message)
{
    PostMessageW(window, message, 0, 0);
    SetForegroundWindow(window);
    return true;
}
} // namespace

bool OpenSettingsApplication()
{
    if (const HWND existing_window = FindWindowW(kSettingsWindowClass, nullptr))
        return ActivateSettingsWindow(existing_window, kActivateSettings);

    return OpenSiblingApplication(L"MetasequoiaImeSettings.exe", nullptr);
}

// 打开设置并停在某一页：已在运行就投对应消息，否则带参数启动（参数与消息由 settings_app.cpp 解释）。
static bool OpenSettingsSection(UINT message, const wchar_t *argument)
{
    if (const HWND existing_window = FindWindowW(kSettingsWindowClass, nullptr))
        return ActivateSettingsWindow(existing_window, message);

    std::wstring module_path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (length == 0 || length >= module_path.size())
        return false;
    module_path.resize(length);

    const std::filesystem::path application_path =
        std::filesystem::path(module_path).parent_path() / L"MetasequoiaImeSettings.exe";
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", application_path.c_str(), argument,
                                           application_path.parent_path().c_str(), SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool OpenSettingsAboutApplication()
{
    return OpenSettingsSection(kOpenSettingsAbout, L"--about");
}

bool OpenSettingsVoiceApplication()
{
    return OpenSettingsSection(kOpenSettingsVoice, L"--voice");
}

// 这是真正要求退出的通道（Server 自己也要终止），所以发 kQuitSettings 而不是 WM_CLOSE
// ——后者现在只把窗口隐藏起来，会把设置进程留在 Server 之后。
bool CloseSettingsApplication()
{
    const HWND application_window = FindWindowW(kSettingsWindowClass, nullptr);
    return !application_window || PostMessageW(application_window, kQuitSettings, 0, 0) != FALSE;
}

bool OpenEmojiPanelApplication()
{
    return OpenSiblingApplication(L"MetasequoiaImeEmojiPanel.exe", kEmojiPanelWindowClass);
}

bool CloseEmojiPanelApplication()
{
    return CloseApplication(kEmojiPanelWindowClass);
}

bool OpenKeyboardPanelApplication()
{
    return OpenSiblingApplication(L"MetasequoiaImeKeyboardPanel.exe", kKeyboardPanelWindowClass);
}

bool CloseKeyboardPanelApplication()
{
    return CloseApplication(kKeyboardPanelWindowClass);
}

bool OpenHandwritingPanelApplication()
{
    return OpenSiblingApplication(L"MetasequoiaImeHandwritingPanel.exe", kHandwritingPanelWindowClass);
}

bool CloseHandwritingPanelApplication()
{
    return CloseApplication(kHandwritingPanelWindowClass);
}
