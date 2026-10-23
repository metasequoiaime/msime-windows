#pragma once

#include "engine/core/data_path.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace test
{
// Every dictionary/engine API takes its paths as UTF-8. path::string() converts through the ANSI
// code page instead, which corrupts a non-ASCII path and throws outright on a code page that cannot
// represent the characters -- and the fixtures using this all live under the temp directory, which
// sits inside the user profile. Tests must therefore convert with this, never with path::string().
inline std::string Utf8(const std::filesystem::path &path)
{
    return metasequoia::path_to_utf8(path);
}

// Overridable temp root. On hosts where the system temp directory refuses new files and
// directories (sandboxes, hardened TEMP ACLs) every database-creating test fails for a
// reason that has nothing to do with the code under test. METASEQUOIA_IME_TEST_TEMP
// points them at a writable directory instead. CI leaves it unset.
inline std::filesystem::path TempRoot()
{
    if (const char *override_dir = std::getenv("METASEQUOIA_IME_TEST_TEMP"))
    {
        if (override_dir[0] != '\0')
        {
            return std::filesystem::u8path(override_dir);
        }
    }
    return std::filesystem::temp_directory_path();
}

} // namespace test
