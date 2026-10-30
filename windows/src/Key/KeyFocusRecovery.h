#pragma once

// fForeground means this TIP became the thread's keystroke handler, which TSF
// also reports right after AdviseKeyEventSink in a process started in the
// background. Only a thread that holds input focus may start a session.
inline bool ShouldRecoverNamedpipeOnKeyFocus(bool foreground, bool threadFocused, bool connected, bool focusStateOwner)
{
    return foreground && threadFocused && !connected && focusStateOwner;
}
