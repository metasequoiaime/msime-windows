#pragma once
#include <string>
#include <Windows.h>

// Valve game compatibility name, recorded by the settings page's elevated helper
// (server/src/settings/game_compat_name.cpp). Empty when the switch is off.
namespace Global
{
inline std::wstring ReadGameCompatProfileName()
{
    WCHAR name[128] = {};
    DWORD bytes = sizeof(name) - sizeof(WCHAR);
    // RRF_SUBKEY_WOW6464KEY: the x86 DLL must read the same value as the x64 one.
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Metasequoia\\MetasequoiaIME", L"GameCompatProfileName",
                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, name, &bytes) != ERROR_SUCCESS)
    {
        return {};
    }
    return name;
}
} // namespace Global
