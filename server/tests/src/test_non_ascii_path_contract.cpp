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
constexpr const wchar_t *kChineseDirectory = L"陆傲天";
} // namespace

// Narrow strings in this repo are UTF-8, but std::filesystem converts narrow strings through the
// system ANSI code page. The two only agree on ASCII, so every path that crosses the narrow boundary
// has to go through these helpers. This pins that difference down so the reason the helpers exist
// stays visible: path::string() does not merely mangle a Chinese path, it throws.
TEST_CASE(filesystem_narrow_conversions_are_utf8_not_ansi)
{
    const std::filesystem::path path = std::filesystem::path(LR"(C:\tmp)") / kChineseDirectory / L"msime_user.db";

    // 陆 U+9646, 傲 U+50B2, 天 U+5929 in UTF-8.
    const std::string expected = "C:\\tmp\\\xE9\x99\x86\xE5\x82\xB2\xE5\xA4\xA9\\msime_user.db";
    REQUIRE_EQ(test::Utf8(path), expected);

    // The round trip has to be lossless, otherwise a path handed to an engine API cannot come back.
    REQUIRE_EQ(metasequoia::path_from_utf8(test::Utf8(path).c_str()), path);

    // path::string() is not an alternative spelling of the above: it either produces different bytes
    // or, on a code page that cannot represent the characters at all, throws. Both are failures, and
    // a throw here is what fastfailed the replay tool during upgrades.
    bool matched_utf8 = false;
    try
    {
        matched_utf8 = (path.string() == expected);
    }
    catch (const std::exception &)
    {
        matched_utf8 = false;
    }
    if (GetACP() != CP_UTF8)
    {
        REQUIRE(!matched_utf8);
    }
}

// The journal API is the one the replay tool drives, and it takes its paths as UTF-8. Exercised
// in-process here so a regression in the library is caught even if the tool around it is fine.
TEST_CASE(user_dictionary_journal_round_trips_under_non_ascii_path)
{
    namespace fs = std::filesystem;
    const fs::path unique_root = test::TempRoot() / (L"msime-journal-test-" + std::to_wstring(GetCurrentProcessId()));

    std::error_code ec;
    fs::remove_all(unique_root, ec);

    const fs::path data_dir = unique_root / kChineseDirectory / L"metasequoiaime";
    fs::create_directories(data_dir, ec);
    REQUIRE(!ec);

    const std::string user_db = test::Utf8(data_dir / L"msime_user.db");
    REQUIRE(user_dictionary::ensure_user_database(user_db));
    REQUIRE(fs::exists(data_dir / L"msime_user.db"));

    // Opening the database it just created must work through the same UTF-8 spelling.
    REQUIRE(user_dictionary::ensure_user_database(user_db));

    fs::remove_all(unique_root, ec);
}
