#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

// The Direct2D settings window. It adopts the HWND that settings_app.cpp creates; the
// window class, single-instance activation, lingering and the worker that applies config
// writes live there, and only painting and input are handled here.
namespace native_settings
{
class SettingsWindowImpl;

class SettingsWindow
{
  public:
    struct Callbacks
    {
        // A client message of the webview contract (engine/contracts/webview), serialized.
        std::function<void(const std::string &message)> postToHost;
    };

    SettingsWindow(HWND hwnd, Callbacks callbacks, const std::string &initialPage);
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow &) = delete;
    SettingsWindow &operator=(const SettingsWindow &) = delete;

    // Messages the window handles itself; returns true with `result` set when the host
    // must not process the message further.
    bool HandleMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT &result);
    // A host message (configSnapshot, dictionaryResponse, ...), JSON.
    void OnHostMessage(const std::wstring &message);
    void SetLightTheme(bool light);
    // appearance.settings_launch_animation, read before the first frame. Off, the page
    // is covered by a brief plain fill instead, without the gear and the fade.
    void SetLaunchAnimation(bool enabled);
    void ShowPage(const std::string &pageId);
    // The in-window toast pages use, for host-side results such as a failed config write.
    void ShowToast(const std::wstring &message, bool success);
    // Drops hover and pressed states before the window is hidden, so the frame kept
    // for the next show does not flash a highlighted caption button.
    void ResetTransientState();

  private:
    std::unique_ptr<SettingsWindowImpl> impl_;
};
} // namespace native_settings
