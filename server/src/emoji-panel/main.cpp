#include "EmojiPanel.h"
#include "emoji_panel_splash.h"

#include "msimeui/Application.h"
#include "msimeui/Scene.h"
#include "msimeui/Theme.h"
#include "msimeui/Window.h"
#include "utils/dwm_attributes.h"
#include "utils/single_instance.h"
#include "utils/surface_theme_config.h"

#include <dwmapi.h>
#include <memory>

#pragma comment(lib, "dwmapi.lib")

namespace
{
constexpr wchar_t kWindowClassName[] = L"msimeui.EmojiPanel";

void ApplyHostChrome(HWND hwnd, bool lightTheme)
{
    if (!hwnd)
    {
        return;
    }
    // DWORD instead of the SDK's DWM_WINDOW_CORNER_PREFERENCE typedef: the
    // compat header (utils/dwm_attributes.h) declares the values as an
    // anonymous enum so the Win7 target compiles without the Win10 SDK block.
    DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const BOOL dark = lightTheme ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

void ActivateExistingInstance()
{
    if (const HWND existing = FindWindowW(kWindowClassName, nullptr))
    {
        if (IsIconic(existing))
        {
            ShowWindow(existing, SW_RESTORE);
        }
        else
        {
            ShowWindow(existing, SW_SHOW);
        }
        SetForegroundWindow(existing);
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int nCmdShow)
{
    CommonUtils::SingleInstanceGuard single_instance(L"Local\\MetasequoiaImeEmojiPanel.SingleInstance");
    if (!single_instance.is_valid())
    {
        return -1;
    }
    if (single_instance.already_running())
    {
        ActivateExistingInstance();
        return 0;
    }

    if (!msimeui::Application::Initialize())
    {
        return -1;
    }

    const bool lightTheme = SurfaceThemeConfig::IsLight("theme_emoji");
    msimeui::Theme theme;
    if (!lightTheme)
    {
        theme.windowBackground = D2D1::ColorF(0x202027);
        theme.surface = D2D1::ColorF(0x202027);
        theme.borderStrong = D2D1::ColorF(0x45454F);
        theme.primary = D2D1::ColorF(0x8C55A2);
        theme.primaryFocusStrong = D2D1::ColorF(0xD88BDE);
        theme.textPrimary = D2D1::ColorF(0xF5F5F7);
        theme.textSecondary = D2D1::ColorF(0xC9C9D0);
    }
    else
    {
        theme.primary = D2D1::ColorF(0x7A3E91);
        theme.primaryFocusStrong = D2D1::ColorF(0x9A62AD);
    }
    msimeui::ThemeManager::SetCurrent(std::move(theme));

    // Window sizes are physical pixels; the panel renders its design surface at 2/3 scale.
    msimeui::Window window(kWindowClassName, L"Emoji and more", 550, 610);
    window.SetWindowStyle(WS_POPUP, WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
    window.SetDragRegionHeight(56.0f * 2.0f / 3.0f);
    window.SetRoundedCorners(true);
    if (!window.Create())
    {
        msimeui::Application::Shutdown();
        return -1;
    }

    const HWND hwnd = window.GetHandle();
    ApplyHostChrome(hwnd, lightTheme);

    // Show the host immediately, then cover it with the Settings-style splash while the panel
    // loads its catalog and completes the first Direct2D paint.
    ShowWindow(hwnd, nCmdShow == SW_HIDE ? SW_SHOWNORMAL : nCmdShow);
    UpdateWindow(hwnd);
    EmojiPanelSplash::Show(hwnd, lightTheme);
    EmojiPanelSplash::Pump();

    auto scene = std::make_unique<msimeui::Scene>();
    scene->SetRoot(std::make_shared<msimeui::EmojiPanel>(lightTheme));
    EmojiPanelSplash::Pump();
    window.SetScene(std::move(scene));
    window.Relayout();
    InvalidateRect(hwnd, nullptr, FALSE);
    UpdateWindow(hwnd);
    EmojiPanelSplash::Pump();

    EmojiPanelSplash::Dismiss();

    const int result = window.Run(SW_SHOW);
    msimeui::Application::Shutdown();
    return result;
}
