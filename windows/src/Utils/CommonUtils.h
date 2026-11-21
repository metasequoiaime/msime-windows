#pragma once

#include <Windows.h>
#include <string>

std::wstring GetCurrentProcessName();

// Registry paths shared by the language-bar theme decision and the theme
// watcher (MetasequoiaIME_WindowProc.cpp). Keep the single spelling here.
inline constexpr wchar_t kRegKeyPersonalizeTheme[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
inline constexpr wchar_t kRegKeyDwm[] = L"Software\\Microsoft\\Windows\\DWM";
inline constexpr wchar_t kRegKeyExplorerAccent[] = L"Software\\Microsoft\\Windows\\Explorer\\Accent";
// Under HKLM; carries UBR (the patch revision), which RtlGetVersion does not report.
inline constexpr wchar_t kRegKeyNtCurrentVersion[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion";

// Reads a DWORD value from HKCU; returns fallback when the key or value is absent.
DWORD ReadHkcuDword(LPCWSTR subKey, LPCWSTR valueName, DWORD fallback);

// Same as ReadHkcuDword, but rooted at HKLM.
DWORD ReadHklmDword(LPCWSTR subKey, LPCWSTR valueName, DWORD fallback);
