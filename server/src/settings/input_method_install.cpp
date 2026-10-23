#include "input_method_install.h"

#include <string>

namespace input_method_install
{
namespace
{

// Same CLSID and profile GUID as windows/src/Global/Globals.cpp and default_input_method.cpp.
constexpr wchar_t kProfileId[] = L"0804:{e3062e9a-d834-4637-8958-ed8cfa427d01}{4d59b1b4-d503-44ae-9259-bad9bb2778ab}";
constexpr wchar_t kUserProfileKey[] = L"Control Panel\\International\\User Profile";
constexpr wchar_t kLanguage[] = L"zh-Hans-CN";
// Used by the installer, which starts the settings program as the original user with this switch.
constexpr wchar_t kRegisterProfileSwitch[] = L"--register-profile";

// The keyboard list keeps one value per input method under the language key, named after the profile and holding the
// position. Reading the value back also proves the entry is the one Windows settings wrote: a stray string under the
// same name would not be an installed keyboard.
bool AlreadyListed()
{
    const std::wstring subkey = std::wstring(kUserProfileKey) + L"\\" + kLanguage;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0;
    DWORD order = 0;
    DWORD bytes = sizeof(order);
    const LSTATUS status = RegQueryValueExW(key, kProfileId, nullptr, &type, reinterpret_cast<BYTE *>(&order), &bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_DWORD;
}

} // namespace

bool EnsureInKeyboardList()
{
    if (AlreadyListed())
        return true;
    // input.dll is not linked directly: the exports are documented for LoadLibrary and GetProcAddress only.
    HMODULE input = LoadLibraryExW(L"input.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (input == nullptr)
        return false;
    typedef BOOL(WINAPI * InstallLayoutOrTipFn)(LPCWSTR, DWORD);
    const auto install = reinterpret_cast<InstallLayoutOrTipFn>(GetProcAddress(input, "InstallLayoutOrTip"));
    const bool installed = install != nullptr && install(kProfileId, 0) != FALSE;
    FreeLibrary(input);
    // Trust the keyboard list, not the return value: InstallLayoutOrTip reports success for profiles it then ignores.
    return installed && AlreadyListed();
}

int RunRegisterProfileIfRequested(const wchar_t *commandLine)
{
    // Parsed, not compared with wcscmp: the caller passes a bare parameter today, and anything that adds quotes or
    // spaces would otherwise silently open the settings window instead, which the installer waits on forever.
    if (commandLine == nullptr || *commandLine == L'\0')
        return -1;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(commandLine, &argc);
    if (argv == nullptr)
        return -1;
    const bool requested = argc == 1 && wcscmp(argv[0], kRegisterProfileSwitch) == 0;
    LocalFree(argv);
    if (!requested)
        return -1;
    return EnsureInKeyboardList() ? 0 : 1;
}

} // namespace input_method_install
