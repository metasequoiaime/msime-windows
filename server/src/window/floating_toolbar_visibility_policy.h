#pragma once

namespace FanyImeUi
{
// A temporary thread-focus suspension (for example Win+.) does not change
// ime_active. Only terminal TIP activation/deactivation changes that bit, so
// the toolbar stays resident across auxiliary Windows input surfaces but is
// hidden after the user switches to another input method. An auto-hidden
// toolbar stays hidden through focus changes until an input state change or a
// settings change clears auto_hidden.
//
// foreground_is_ime_client covers the one case where that lifecycle reports
// nothing at all: with "Let me use a different input method for each app window"
// enabled, the text service stays activated in the window the user left, so
// focusing a window that uses another IME never produces a terminal
// deactivation. The toolbar describes the focused window, not the thread that
// still holds the stale activation, so that window has to agree.
constexpr bool ShouldShowFloatingToolbar(bool configured_enabled, bool fullscreen, bool ime_active, bool auto_hidden,
                                         bool foreground_is_ime_client)
{
    return configured_enabled && !fullscreen && ime_active && !auto_hidden && foreground_is_ime_client;
}
} // namespace FanyImeUi
