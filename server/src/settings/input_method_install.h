#pragma once

#include <Windows.h>

// Puts the TSF profile into the current user's keyboard list. That list is what registers the profile as an installed
// input method for the user; the profile can be switched to without it (it is enabled by default in HKLM), but the
// registry entry the language bar and the Windows settings page show is then the one the list holds, not the one the
// profile was registered with.
//
// This writes HKCU and therefore has to run as the interactive user, never from the elevated registration:
// DllRegisterServer runs elevated, and its HKCU is the administrator's hive, not the user's. The installer starts
// MetasequoiaImeSettings.exe --register-profile as the original user to run this; the settings page also calls the
// same helper before applying the game compat name, for machines installed before this existed.

namespace input_method_install
{

// Idempotent: does nothing when the profile is already in the user's keyboard list. False means the entry is still
// missing afterwards, whether input.dll could not be loaded, the export was absent, or the call did not take.
bool EnsureInKeyboardList();

// Called first thing in wWinMain. Returns the exit code when the command line is --register-profile, otherwise -1 and
// the settings window starts normally.
int RunRegisterProfileIfRequested(const wchar_t *commandLine);

} // namespace input_method_install
