#pragma once

#include <windows.h>

namespace VoiceInput::Detail
{
// The low-level keyboard hook entry installed by Initialize(). Tests feed this
// same boundary synthetic KBDLLHOOKSTRUCT events; no OS hook is installed.
LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wparam, LPARAM lparam);
} // namespace VoiceInput::Detail
