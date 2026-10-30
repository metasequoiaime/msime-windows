#pragma once

#include <string>

#include <windows.h>

// Naming of the session-scoped diagnostic logs. It lives apart from the writer
// because installer/msime_setup.iss deletes leftovers by the same prefix: a
// silent change here would strand every previous log in the data directory.
namespace DiagnosticLogNaming
{
// Every diagnostic file starts with this prefix.
inline constexpr wchar_t kFilePrefix[] = L"水杉IME诊断日志-";

// One channel per settings switch, so a user who only needs the TSF trace does
// not have to hand over the Server trace as well.
inline constexpr wchar_t kServerChannel[] = L"server";
inline constexpr wchar_t kTsfChannel[] = L"tsf";

// <data directory>\logs. The directory follows the location chosen during setup.
std::wstring DirectoryFor(const std::wstring &data_directory);

// 水杉IME诊断日志-server-20261029-153012.log. A sequence above 1 is appended
// when a file for the same second already exists, which only happens when two
// processes start logging within the same second.
std::wstring FileNameFor(const wchar_t *channel, const SYSTEMTIME &local_time, unsigned sequence);
} // namespace DiagnosticLogNaming
