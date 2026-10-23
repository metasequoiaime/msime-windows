#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include "engine/core/data_path.h"
#include "engine/user_dictionary/user_dictionary_journal.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace
{
// Runs the shipped replay tool against a data directory and reports its exit code. The installer
// launches it exactly like this and aborts the upgrade on any non-zero result.
DWORD RunReplay(const std::filesystem::path &data_dir)
{
    // CMake writes the path as UTF-8, so decode it as such rather than through the ANSI code page.
    const std::filesystem::path exe = metasequoia::path_from_utf8(MSIME_DICTIONARY_REPLAY_EXE);
    std::wstring command_line = L"\"" + exe.wstring() + L"\" --data-dir \"" + data_dir.wstring() + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
    REQUIRE(started);

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    const BOOL got_code = GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    REQUIRE(got_code);
    return exit_code;
}

// Creates a data directory holding a valid journal, so the tool gets past the existence check and
// actually opens the databases through the paths it built.
std::filesystem::path SeedDataDir(const std::filesystem::path &root)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(root, ec);
    REQUIRE(!ec);
    REQUIRE(user_dictionary::ensure_user_database(metasequoia::path_to_utf8(root / L"msime_user.db")));
    REQUIRE(fs::exists(root / L"msime_user.db"));
    return root;
}
} // namespace

// A non-ASCII (e.g. Chinese) user profile path must not change how the replay tool behaves. It used to
// hand the journal API paths converted with path::string(), which goes through the ANSI code page: the
// API expects UTF-8, so a Chinese path arrived as invalid UTF-8 and the conversion threw, fastfailing
// the process with 0xC0000409. The installer treats any non-zero exit as fatal, so upgrading was
// impossible for those users. The exit code must depend on the data, never on the path encoding.
TEST_CASE(dictionary_replay_behaves_identically_under_non_ascii_data_path)
{
    namespace fs = std::filesystem;
    const fs::path unique_root = test::TempRoot() / (L"msime-replay-test-" + std::to_wstring(GetCurrentProcessId()));

    std::error_code ec;
    fs::remove_all(unique_root, ec);

    const DWORD ascii_exit = RunReplay(SeedDataDir(unique_root / L"ascii" / L"metasequoiaime"));
    const DWORD non_ascii_exit = RunReplay(SeedDataDir(unique_root / L"用户目录" / L"metasequoiaime"));

    // 0xC0000409 (STATUS_STACK_BUFFER_OVERRUN) is what MSVC's __fastfail reports for an escaping
    // exception, which is the exact regression being guarded against.
    REQUIRE(non_ascii_exit != 0xC0000409u);
    REQUIRE_EQ(non_ascii_exit, ascii_exit);

    fs::remove_all(unique_root, ec);
}
