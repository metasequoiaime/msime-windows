#pragma once

// Input method profile icon: the icon Windows shows for this input method in the Win+Space switcher and in the
// keyboard list of the system settings.
//
// The icon is part of the TSF language profile, registered by windows/src/Register/Register.cpp as a file path in
// HKLM. Changing it needs elevation, so it follows the game compatibility name (game_compat_name.h): the settings
// process relaunches itself elevated with --apply-profile-icon or --reset-profile-icon, and that instance
//
//   1. copies the chosen .ico next to MetasequoiaImeTip.ico in both TSF roots (System32 and SysWOW64
//      \IME\MetasequoiaIME), under a name derived from its content;
//   2. records the file name in HKLM\Software\Metasequoia\MetasequoiaIME\ProfileIconFile, which Register.cpp reads so
//      installs and upgrades keep the choice;
//   3. registers the profile again with the new icon path.
//
// The name changes with the content on purpose: the shell caches profile icons by path, so overwriting
// MetasequoiaImeTip.ico in place would keep showing the old icon until explorer restarts.

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace profile_icon
{

// Choice of the installer's own MetasequoiaImeTip.ico; applying it removes the record.
inline constexpr char kDefaultChoice[] = "default";
// Choice of an image the user imported.
inline constexpr char kCustomChoice[] = "custom";

struct Builtin
{
    const char *id;
    const wchar_t *label;
    // Under <data dir>\assets\settings\, installed from windows/image/profile-icons (see Prepare-PackageFiles.ps1).
    const wchar_t *asset;
};

// The icons the settings page offers, default first.
const std::vector<Builtin> &Builtins();
const Builtin *FindBuiltin(const std::string &id);

struct Status
{
    bool registered = false; // the TSF profile exists
    std::string choice;      // recorded choice; kDefaultChoice when nothing is recorded
    std::wstring iconFile;   // IconFile of the profile as Windows has it
};

Status QueryStatus();

// Called from the normal settings process; ask for elevation (UAC) and wait for the elevated instance. On failure
// `error` is human-readable.
bool ApplyBuiltin(HWND owner, const std::string &id, const std::filesystem::path &assetDirectory, std::string &error);
// Converts `image` (.ico, .png, .jpg, .bmp, ...) into an icon and applies it as the custom choice.
bool ApplyCustom(HWND owner, const std::filesystem::path &image, std::string &error);

// Modal file picker on the window thread; empty when cancelled.
std::filesystem::path PromptForImage(HWND owner);

// Called first thing in wWinMain. Returns the exit code when the command line asks for the elevated apply, otherwise
// -1 and the settings window starts normally.
int RunElevatedApplyIfRequested(const wchar_t *commandLine);

// --- Pure helpers, exposed for tests ---

// Upper bound for an icon file; the largest sensible one (12 PNG frames up to 256 px) is well under 1 MB.
inline constexpr size_t kMaxIconBytes = 4 * 1024 * 1024;

// A well-formed .ico: ICONDIR header, 1-64 entries, every frame inside the file and either PNG or a DIB.
bool IsValidIcon(const std::vector<uint8_t> &bytes);
// MetasequoiaImeTip-<16 hex digits of the content hash>.ico
std::wstring StoredFileName(const std::vector<uint8_t> &bytes);
// Whether `name` is a plain file name StoredFileName could have produced.
bool IsStoredFileName(const std::wstring &name);
// Packs PNG-encoded square frames (edge, bytes) into an .ico, in the given order.
std::vector<uint8_t> PackPngIcon(const std::vector<std::pair<int, std::vector<uint8_t>>> &frames);
// The frame edges a converted icon carries: every taskbar / switcher scale from 100% to 300%, plus 64/128/256.
const std::vector<int> &IconFrameSizes();
// Decodes any WIC-readable image and renders the IconFrameSizes() frames, centered on a transparent square. An .ico
// input is validated and returned unchanged.
bool ConvertToIcon(const std::filesystem::path &image, std::vector<uint8_t> &icon, std::string &error);

} // namespace profile_icon
