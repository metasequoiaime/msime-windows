#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"
#include "english/japanese_dictionary.h"
#include "ipc/candidate_translation_policy.h"

#include <chrono>
#include <filesystem>
#include <sqlite3.h>

namespace
{
void Execute(const std::filesystem::path &path, const char *sql)
{
    sqlite3 *db = nullptr;
    REQUIRE_EQ(sqlite3_open(test::Utf8(path).c_str(), &db), SQLITE_OK);
    const int status = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    REQUIRE_EQ(status, SQLITE_OK);
}

class GlossFixture
{
  public:
    GlossFixture()
    {
        directory = std::filesystem::temp_directory_path() /
                    (L"msime-日本語-" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        japanese = directory / "japanese.db";
        Execute(japanese, "CREATE TABLE zh_glosses(chinese TEXT PRIMARY KEY, gloss TEXT, reading TEXT, source TEXT);"
                          "INSERT INTO zh_glosses VALUES ('学习', '習う', 'ならう', 'fixture');"
                          "INSERT INTO zh_glosses VALUES ('电脑', 'コンピュータ', '', 'fixture');");
    }
    ~GlossFixture()
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    std::filesystem::path directory;
    std::filesystem::path japanese;
};
} // namespace

TEST_CASE(japanese_dictionary_missing_file_does_not_create_a_database)
{
    const auto missing =
        std::filesystem::temp_directory_path() /
        ("msime-missing-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    JapaneseDictionary dictionary(test::Utf8(missing));
    REQUIRE(!dictionary.available());
    REQUIRE(dictionary.lookup("学习").gloss.empty());
    REQUIRE(!std::filesystem::exists(missing));
}

// These content expectations are tied to GlossFixture's dictionary, created locally
// for this test. Custom dictionaries may have different translations, readings or
// headwords, so these assertions may not apply unchanged.
TEST_CASE(japanese_dictionary_returns_gloss_and_reading)
{
    GlossFixture fixture;
    JapaneseDictionary dictionary(test::Utf8(fixture.japanese));
    const auto result = dictionary.lookup("学习");
    REQUIRE_EQ(result.gloss, std::string("習う"));
    REQUIRE_EQ(result.reading, std::string("ならう"));
    REQUIRE_EQ(FanyImeIpc::AppendJapaneseReading(result.gloss, result.reading), std::string("習う（ならう）"));
    const auto kana = dictionary.lookup("电脑");
    REQUIRE_EQ(kana.gloss, std::string("コンピュータ"));
    REQUIRE(kana.reading.empty());
    REQUIRE(dictionary.lookup("未收录的词条").gloss.empty());
}
