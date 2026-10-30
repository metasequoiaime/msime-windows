#include "diagnostic_log_naming.h"

#include <cstdio>

namespace DiagnosticLogNaming
{
std::wstring DirectoryFor(const std::wstring &data_directory)
{
    return data_directory + L"\\logs";
}

std::wstring FileNameFor(const wchar_t *channel, const SYSTEMTIME &local_time, unsigned sequence)
{
    wchar_t buffer[128]{};
    if (sequence <= 1)
    {
        swprintf_s(buffer, L"%s%s-%04u%02u%02u-%02u%02u%02u.log", kFilePrefix, channel,
                   static_cast<unsigned>(local_time.wYear), static_cast<unsigned>(local_time.wMonth),
                   static_cast<unsigned>(local_time.wDay), static_cast<unsigned>(local_time.wHour),
                   static_cast<unsigned>(local_time.wMinute), static_cast<unsigned>(local_time.wSecond));
    }
    else
    {
        swprintf_s(buffer, L"%s%s-%04u%02u%02u-%02u%02u%02u-%u.log", kFilePrefix, channel,
                   static_cast<unsigned>(local_time.wYear), static_cast<unsigned>(local_time.wMonth),
                   static_cast<unsigned>(local_time.wDay), static_cast<unsigned>(local_time.wHour),
                   static_cast<unsigned>(local_time.wMinute), static_cast<unsigned>(local_time.wSecond), sequence);
    }
    return buffer;
}
} // namespace DiagnosticLogNaming
