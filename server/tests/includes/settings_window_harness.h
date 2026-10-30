#pragma once

#include "msimeui/Scene.h"
#include "msimeui/Window.h"

#include <windows.h>

#include <cmath>
#include <memory>

// Drives settings-window controls the way msimeui::Window delivers input: mouse and key
// messages go through Window::DispatchImportedMessage, so hit testing, capture, focus and
// popup dismissal are the real ones. Points are given in DIPs and sent as client pixels at
// the window's DPI, which comes from an override. The window is a real HWND that is never
// shown: text boxes need one, and COM, for their text services.
class SettingsWindowHarness
{
  public:
    explicit SettingsWindowHarness(float dpi, msimeui::SizeF viewport = {800.0f, 900.0f})
        : comInitialized_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))),
          window_(std::make_unique<msimeui::Window>(L"settings-window-test", L"", 800, 900)), dpi_(dpi),
          viewport_(viewport)
    {
        window_->SetDpiOverride(dpi);
        window_->SetStealFocusOnClick(false);
        window_->SetQuitOnDestroy(false);
        window_->Create();
    }

    ~SettingsWindowHarness()
    {
        if (window_->GetHandle())
            DestroyWindow(window_->GetHandle());
        // The scene's text services go before the apartment they were created in.
        window_.reset();
        if (comInitialized_)
            CoUninitialize();
    }

    SettingsWindowHarness(const SettingsWindowHarness &) = delete;
    SettingsWindowHarness &operator=(const SettingsWindowHarness &) = delete;

    void SetRoot(std::shared_ptr<msimeui::Visual> root)
    {
        auto scene = std::make_unique<msimeui::Scene>();
        scene->SetRoot(std::move(root));
        window_->SetScene(std::move(scene));
        Layout();
    }

    // A real window lays out before every paint; tests do it after every input.
    void Layout()
    {
        window_->GetScene()->Layout(viewport_);
    }

    msimeui::Window &Window()
    {
        return *window_;
    }

    msimeui::Visual *VisualAt(float x, float y)
    {
        return window_->GetScene()->FindVisualAt({x, y});
    }

    POINT Pixels(float x, float y) const
    {
        const float scale = dpi_ / 96.0f;
        return {static_cast<LONG>(std::lround(x * scale)), static_cast<LONG>(std::lround(y * scale))};
    }

    void Press(float x, float y)
    {
        Send(WM_LBUTTONDOWN, MK_LBUTTON, x, y);
    }

    void MoveHeld(float x, float y)
    {
        Send(WM_MOUSEMOVE, MK_LBUTTON, x, y);
    }

    void Release(float x, float y)
    {
        Send(WM_LBUTTONUP, 0, x, y);
    }

    void Click(float x, float y)
    {
        Press(x, y);
        Release(x, y);
    }

    // WM_MOUSEWHEEL carries screen coordinates, which need an HWND to convert; the scene
    // takes the client point the window would hand it.
    void Wheel(float x, float y, short delta)
    {
        window_->GetScene()->OnMouseWheel(Pixels(x, y), delta, 0);
        Layout();
    }

    // The test window is never shown, so it never has keyboard focus, and Window only tells
    // a visual it is focused while the window is. This delivers the focus the window of a
    // real session has.
    void GiveWindowKeyboardFocus()
    {
        window_->DispatchImportedMessage(WM_SETFOCUS, 0, 0);
        Layout();
    }

    void Key(WPARAM key)
    {
        window_->DispatchImportedMessage(WM_KEYDOWN, key, 0);
        Layout();
    }

    void Type(wchar_t ch)
    {
        window_->DispatchImportedMessage(WM_CHAR, ch, 0);
        Layout();
    }

  private:
    void Send(UINT message, WPARAM keyState, float x, float y)
    {
        const POINT point = Pixels(x, y);
        window_->DispatchImportedMessage(message, keyState, MAKELPARAM(point.x, point.y));
        Layout();
    }

    bool comInitialized_ = false;
    std::unique_ptr<msimeui::Window> window_;
    float dpi_ = 96.0f;
    msimeui::SizeF viewport_;
};
