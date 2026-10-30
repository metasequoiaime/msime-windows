#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "log/diagnostic_log_naming.h"
#include "tests/includes/test_framework.h"

#include <windows.h>

namespace
{
SYSTEMTIME FixedTime()
{
    SYSTEMTIME time{};
    time.wYear = 2026;
    time.wMonth = 10;
    time.wDay = 29;
    time.wHour = 15;
    time.wMinute = 30;
    time.wSecond = 12;
    return time;
}
} // namespace

TEST_CASE(diagnostic_log_file_name_carries_channel_and_generation_time)
{
    const std::wstring server = DiagnosticLogNaming::FileNameFor(DiagnosticLogNaming::kServerChannel, FixedTime(), 1);
    REQUIRE(server == L"水杉IME诊断日志-server-20261029-153012.log");

    const std::wstring tsf = DiagnosticLogNaming::FileNameFor(DiagnosticLogNaming::kTsfChannel, FixedTime(), 1);
    REQUIRE(tsf == L"水杉IME诊断日志-tsf-20261029-153012.log");
    REQUIRE(server != tsf);
}

TEST_CASE(diagnostic_log_file_name_keeps_the_prefix_the_installer_deletes_by)
{
    // installer/msime_setup.iss removes 水杉IME诊断日志-* when the data directory is not ours.
    const std::wstring name = DiagnosticLogNaming::FileNameFor(DiagnosticLogNaming::kServerChannel, FixedTime(), 3);
    REQUIRE(name.rfind(DiagnosticLogNaming::kFilePrefix, 0) == 0);
    REQUIRE(name == L"水杉IME诊断日志-server-20261029-153012-3.log");
}

TEST_CASE(diagnostic_log_directory_follows_the_data_directory)
{
    REQUIRE(DiagnosticLogNaming::DirectoryFor(L"C:\\data\\metasequoiaime") == L"C:\\data\\metasequoiaime\\logs");
}
