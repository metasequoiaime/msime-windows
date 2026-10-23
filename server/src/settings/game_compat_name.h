#pragma once

// Valve game compatibility name.
//
// CS2 and Dota 2 (Source 2, imemanager.dll) only handle IME messages for input methods whose registered TSF profile
// name is on a whitelist compiled into the game. For any other input method the composition falls through to
// Windows' default composition window (CiceroUIWndFrame): a box in the top-left corner and, in exclusive fullscreen,
// enough to throw the game out of fullscreen. Registering under a whitelisted name makes the game draw the candidates
// itself. The cost is real: the language bar and Windows settings show that name too, so this is an opt-in switch.
//
// The choice is machine-wide and lives in HKLM, so changing it needs elevation. The settings process relaunches
// itself elevated with --apply-game-compat-name; that instance records the name for the TSF DLL's registration
// (windows/src/Register/Register.cpp reads it so installs and upgrades keep it) and rewrites the profile Description
// that games and the language bar read. The game only reads the name when it starts.
//
// Description alone was not enough before: the profile was enabled by default in HKLM but never installed into the
// user's keyboard list, so the name the user saw stayed at the old cached one. The installer now installs it as the
// original user (input_method_install.h), and an existing install that predates that is brought up to date the first
// time the switch is applied.

#include <Windows.h>

#include <string>

namespace game_compat_name
{

// Entry 90 of the whitelist, in the "Unknown" branch. Verified in CS2 and Dota 2 by WindInput; contains no spaces,
// brackets or hyphens, which is how hand-copied whitelist entries usually go wrong.
inline constexpr wchar_t kDefaultName[] = L"拼音输入法";

struct Status
{
    bool enabled = false;        // a compatibility name is recorded
    std::wstring name;           // the recorded name, empty when off
    std::wstring registeredName; // the profile name Windows currently has, empty when the TIP is not registered
};

Status QueryStatus();

// A name the game could match: 1-64 characters, no control characters, double quotes or backslashes.
bool IsValidName(const std::wstring &name);

// Called from the normal settings process. Asks for elevation (UAC) and waits for the elevated instance; on failure
// `error` is human-readable.
bool Apply(HWND owner, bool enabled, const std::wstring &name, std::string &error);

// Called first thing in wWinMain. Returns the exit code when the command line asks for the elevated apply, otherwise
// -1 and the settings window starts normally.
int RunElevatedApplyIfRequested(const wchar_t *commandLine);

} // namespace game_compat_name
