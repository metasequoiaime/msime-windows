#pragma once

#include <string>

std::wstring GetCurrentProcessName();

// Registry paths shared by the language-bar theme decision and the theme
// watcher (MetasequoiaIME_WindowProc.cpp). Keep the single spelling here.
inline constexpr wchar_t kRegKeyPersonalizeTheme[] = L"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize";
inline constexpr wchar_t kRegKeyDwm[] = L"Software\Microsoft\Windows\DWM";
inline constexpr wchar_t kRegKeyExplorerAccent[] = L"Software\Microsoft\Windows\Explorer\Accent";

// Reads a DWORD value from HKCU; returns fallback when the key or value is absent.
DWORD ReadHkcuDword(LPCWSTR subKey, LPCWSTR valueName, DWORD fallback);
