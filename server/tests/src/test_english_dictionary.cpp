#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"
#include "engine/english/english_dictionary.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <stdexcept>
#include <string>

namespace
{
class TemporaryEnglishDatabase
{
  public:
    TemporaryEnglishDatabase()
    {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        directory_ = test::TempRoot() / ("metasequoia_english_test_" + suffix);
        std::filesystem::create_directories(directory_);
        path_ = directory_ / "english.db";

        sqlite3 *db = nullptr;
        if (sqlite3_open(test::Utf8(path_).c_str(), &db) != SQLITE_OK)
        {
            const std::string error = db == nullptr ? "unknown error" : sqlite3_errmsg(db);
            if (db != nullptr)
            {
                sqlite3_close(db);
            }
            throw std::runtime_error("Unable to create English test database: " + error);
        }

        constexpr const char *sql = "CREATE TABLE english_words ("
                                    "word TEXT PRIMARY KEY COLLATE BINARY, display TEXT NOT NULL"
                                    ") WITHOUT ROWID;"
                                    "INSERT INTO english_words VALUES ('hel', 'hel');"
                                    "INSERT INTO english_words VALUES ('held', 'held');"
                                    "INSERT INTO english_words VALUES ('hello', 'hello');"
                                    "INSERT INTO english_words VALUES ('help', 'help');"
                                    "INSERT INTO english_words VALUES ('helpful', 'helpful');"
                                    "INSERT INTO english_words VALUES ('hero', 'hero');";
        char *error_message = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &error_message) != SQLITE_OK)
        {
            const std::string error = error_message == nullptr ? "unknown error" : error_message;
            sqlite3_free(error_message);
            sqlite3_close(db);
            throw std::runtime_error("Unable to populate English test database: " + error);
        }
        sqlite3_close(db);
    }

    ~TemporaryEnglishDatabase()
    {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    const std::filesystem::path &path() const
    {
        return path_;
    }

    void execute(const char *sql) const
    {
        sqlite3 *db = nullptr;
        if (sqlite3_open(test::Utf8(path_).c_str(), &db) != SQLITE_OK)
        {
            if (db != nullptr)
                sqlite3_close(db);
            throw std::runtime_error("Unable to open English test database.");
        }
        const int result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
        sqlite3_close(db);
        if (result != SQLITE_OK)
            throw std::runtime_error("Unable to populate English test database.");
    }

  private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
};
} // namespace

TEST_CASE(EnglishDictionaryOrdersByExactMatchThenLengthAndWord)
{
    TemporaryEnglishDatabase database;
    EnglishDictionary dictionary(test::Utf8(database.path()));

    const auto candidates = dictionary.query_prefix("hel", 4);
    REQUIRE_EQ(candidates.size(), static_cast<size_t>(4));
    REQUIRE_EQ(candidates[0].word, std::string("hel"));
    REQUIRE_EQ(candidates[1].word, std::string("held"));
    REQUIRE_EQ(candidates[2].word, std::string("help"));
    REQUIRE_EQ(candidates[3].word, std::string("hello"));
    REQUIRE_EQ(candidates[0].pinyin, std::string("hel"));
    REQUIRE(candidates[0].source == CandidateSource::EnglishDictionary);
}

TEST_CASE(EnglishDictionaryRestrictsResultsToTheRequestedPrefix)
{
    TemporaryEnglishDatabase database;
    EnglishDictionary dictionary(test::Utf8(database.path()));

    const auto candidates = dictionary.query_prefix("help", 10);
    REQUIRE_EQ(candidates.size(), static_cast<size_t>(2));
    REQUIRE_EQ(candidates[0].word, std::string("help"));
    REQUIRE_EQ(candidates[1].word, std::string("helpful"));
    REQUIRE(dictionary.query_prefix("HEL", 5).empty());
}

TEST_CASE(EnglishDictionaryMigratesLegacySchemaAndSupportsMultipleDisplaysPerKey)
{
    TemporaryEnglishDatabase database;
    EnglishDictionary dictionary(test::Utf8(database.path()));

    sqlite3 *db = nullptr;
    REQUIRE_EQ(sqlite3_open(test::Utf8(database.path()).c_str(), &db), SQLITE_OK);
    REQUIRE_EQ(sqlite3_exec(db, "INSERT INTO english_words(word,display,weight) VALUES('hello','Hello',100);", nullptr,
                            nullptr, nullptr),
               SQLITE_OK);
    sqlite3_close(db);

    const auto candidates = dictionary.query_prefix("hello", 10);
    REQUIRE_EQ(candidates.size(), static_cast<size_t>(2));
    REQUIRE_EQ(candidates[0].pinyin, std::string("hello"));
    REQUIRE_EQ(candidates[0].word, std::string("Hello"));
    REQUIRE_EQ(candidates[1].word, std::string("hello"));
}

TEST_CASE(EnglishDictionaryMixedPrefixBudgetsDistinctWordsAndKeepsAttestedForms)
{
    TemporaryEnglishDatabase database;
    EnglishDictionary dictionary(test::Utf8(database.path()));
    database.execute("INSERT INTO english_words(word,display,weight) VALUES"
                     "('walk','WALK',100),('walk','Walk',100),('walk','wALK',99),"
                     "('walk','wAlk',98),('walk','waLk',97),('walk','walk',96),"
                     "('walker','walker',1000),('walks','walks',900),"
                     "('walked','walked',800),('walking','walking',800),('walkable','walkable',0);");

    const auto candidates = dictionary.query_mixed_prefix("walk", 5);
    REQUIRE_EQ(candidates.size(), size_t{9});
    // The exact word stays first even when every completion has greater weight.
    REQUIRE_EQ(candidates[0].pinyin, std::string("walk"));
    REQUIRE_EQ(candidates[0].word, std::string("WALK"));
    REQUIRE_EQ(candidates[1].word, std::string("Walk"));
    REQUIRE_EQ(candidates[2].word, std::string("wALK"));
    REQUIRE_EQ(candidates[3].word, std::string("wAlk"));
    REQUIRE_EQ(candidates[4].word, std::string("waLk"));
    REQUIRE_EQ(candidates[5].word, std::string("walker"));
    REQUIRE_EQ(candidates[6].word, std::string("walks"));
    REQUIRE_EQ(candidates[7].word, std::string("walked"));
    REQUIRE_EQ(candidates[8].word, std::string("walking"));
    REQUIRE(candidates[8].source == CandidateSource::EnglishDictionary);

    // Dedicated English still offers all stored casing variants; no form is generated.
    const auto dedicated = dictionary.query_prefix("walk", 5);
    REQUIRE_EQ(dedicated.size(), size_t{5});
    for (const auto &candidate : dedicated)
        REQUIRE_EQ(candidate.pinyin, std::string("walk"));
    REQUIRE_EQ(dictionary.query_prefix("walk", 32).size(), size_t{11});
    REQUIRE_EQ(dictionary.query_mixed_prefix("walk", 32).size(), size_t{10});
}

TEST_CASE(EnglishDictionaryMixedPrefixRetainsPreviouslyVisibleCasingDisplays)
{
    TemporaryEnglishDatabase database;
    EnglishDictionary dictionary(test::Utf8(database.path()));
    database.execute("INSERT INTO english_words(word,display,weight) VALUES"
                     "('python','Python',100),('python','python',90),('pythons','pythons',80);");

    const auto candidates = dictionary.query_mixed_prefix("py");
    REQUIRE_EQ(candidates.size(), size_t{3});
    REQUIRE_EQ(candidates[0].word, std::string("Python"));
    // This lower-ranked display was previously eligible for a fixed position.
    REQUIRE_EQ(candidates[1].word, std::string("python"));
    REQUIRE_EQ(candidates[2].word, std::string("pythons"));
}

TEST_CASE(EnglishDictionaryMixedPrefixValidatesInputAndResetsBindings)
{
    TemporaryEnglishDatabase database;
    EnglishDictionary dictionary(test::Utf8(database.path()));
    REQUIRE(dictionary.query_mixed_prefix("", 5).empty());
    REQUIRE(dictionary.query_mixed_prefix("HEL", 5).empty());
    REQUIRE(dictionary.query_mixed_prefix("he-l", 5).empty());
    REQUIRE(dictionary.query_mixed_prefix("hel", 0).empty());

    const auto first = dictionary.query_mixed_prefix("hel", 1);
    REQUIRE_EQ(first.size(), size_t{1});
    REQUIRE_EQ(first.front().word, std::string("hel"));
    const auto next = dictionary.query_mixed_prefix("help", 2);
    REQUIRE_EQ(next.size(), size_t{2});
    REQUIRE_EQ(next[0].word, std::string("help"));
    REQUIRE_EQ(next[1].word, std::string("helpful"));
    REQUIRE(dictionary.query_mixed_prefix("missing").empty());
}

TEST_CASE(EnglishDictionaryMixedPrefixCapsTheCompletionBudget)
{
    TemporaryEnglishDatabase database;
    EnglishDictionary dictionary(test::Utf8(database.path()));
    for (char suffix = 'a'; suffix <= 'z'; ++suffix)
    {
        const std::string sql = "INSERT INTO english_words(word,display,weight) VALUES('walla" +
                                std::string(1, suffix) + "','walla" + std::string(1, suffix) + "',1),('wallb" +
                                std::string(1, suffix) + "','wallb" + std::string(1, suffix) + "',1);";
        database.execute(sql.c_str());
    }
    database.execute("INSERT INTO english_words(word,display,weight) VALUES"
                     "('wallaa','WALLAA',100),('wallaa','Wallaa',99),('wallaa','wALLAA',98),"
                     "('wallaa','wAllaa',97),('wallaa','waLlaa',96);");

    const auto candidates = dictionary.query_mixed_prefix("wall");
    REQUIRE(candidates.size() <= EnglishDictionary::kMixedCandidateLimit + 5);
    std::vector<std::string> words;
    for (const auto &candidate : candidates)
        words.push_back(candidate.pinyin);
    std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());
    REQUIRE_EQ(words.size(), EnglishDictionary::kMixedCandidateLimit);
    for (const auto &legacy : dictionary.query_prefix("wall", 5))
        REQUIRE(std::any_of(candidates.begin(), candidates.end(), [&](const WordItem &candidate) {
            return candidate.pinyin == legacy.pinyin && candidate.word == legacy.word;
        }));
    REQUIRE_EQ(dictionary.query_mixed_prefix("wall", 1000).size(), candidates.size());
    REQUIRE_EQ(dictionary.query_prefix("wall", 1000).size(), size_t{57});
}

TEST_CASE(EnglishDictionaryQueriesBothGlossDirections)
{
    TemporaryEnglishDatabase database;
    EnglishDictionary dictionary(test::Utf8(database.path()));

    sqlite3 *db = nullptr;
    REQUIRE_EQ(sqlite3_open(test::Utf8(database.path()).c_str(), &db), SQLITE_OK);
    REQUIRE_EQ(sqlite3_exec(db,
                            "INSERT INTO en_zh_glosses VALUES('implement','实现；执行');"
                            "INSERT INTO zh_en_glosses VALUES('实现','realize; implement');",
                            nullptr, nullptr, nullptr),
               SQLITE_OK);
    sqlite3_close(db);

    REQUIRE_EQ(dictionary.query_chinese_gloss("implement"), std::string("实现；执行"));
    REQUIRE_EQ(dictionary.query_english_gloss("实现"), std::string("realize; implement"));
    REQUIRE(dictionary.query_chinese_gloss("missing").empty());
}

TEST_CASE(EnglishDictionaryPrefersSiblingCustomTranslations)
{
    TemporaryEnglishDatabase database;
    sqlite3 *db = nullptr;
    REQUIRE_EQ(sqlite3_open(test::Utf8(database.path()).c_str(), &db), SQLITE_OK);
    REQUIRE_EQ(sqlite3_exec(db,
                            "CREATE TABLE IF NOT EXISTS zh_en_glosses("
                            "chinese TEXT COLLATE BINARY PRIMARY KEY,english_gloss TEXT NOT NULL) WITHOUT ROWID;"
                            "INSERT INTO zh_en_glosses VALUES('华科','Huazhong University of');",
                            nullptr, nullptr, nullptr),
               SQLITE_OK);
    sqlite3_close(db);

    const auto sidecar = database.path().parent_path() / "custom_translations.txt";
    {
        std::ofstream output(sidecar, std::ios::binary);
        output << "华科\tHuazhong University of Science and Technology\n";
        output << "hust\t华中科技大学\n";
    }

    EnglishDictionary dictionary(test::Utf8(database.path()));
    REQUIRE_EQ(dictionary.query_english_gloss("华科"), std::string("Huazhong University of Science and Technology"));
    REQUIRE_EQ(dictionary.query_chinese_gloss("hust"), std::string("华中科技大学"));
    std::error_code error;
    std::filesystem::remove(sidecar, error);
}

TEST_CASE(EnglishDictionaryUpsertsGlossForCloudFallback)
{
    TemporaryEnglishDatabase database;
    EnglishDictionary dictionary(test::Utf8(database.path()));
    REQUIRE(EnglishDictionary::upsert_gloss(test::Utf8(database.path()), true, "水杉", "metasequoia"));
    REQUIRE_EQ(dictionary.query_english_gloss("水杉"), std::string("metasequoia"));
    REQUIRE(EnglishDictionary::upsert_gloss(test::Utf8(database.path()), false, "metasequoia", "水杉"));
    REQUIRE_EQ(dictionary.query_chinese_gloss("metasequoia"), std::string("水杉"));
}

TEST_CASE(EnglishDictionaryServesEveryTargetLanguageLocally)
{
    TemporaryEnglishDatabase database;
    EnglishDictionary dictionary(test::Utf8(database.path()));

    // 英语仍然走 zh_en_glosses；其余语种落到 zh_glosses。
    REQUIRE(EnglishDictionary::upsert_gloss(test::Utf8(database.path()), true, "水杉", "metasequoia"));
    REQUIRE(EnglishDictionary::upsert_gloss(test::Utf8(database.path()), true, "水杉", "métaséquoia", "fr"));
    REQUIRE(EnglishDictionary::upsert_gloss(test::Utf8(database.path()), true, "水杉", "メタセコイア", "ja"));

    REQUIRE_EQ(dictionary.query_target_gloss("水杉", std::string()), std::string("metasequoia"));
    REQUIRE_EQ(dictionary.query_target_gloss("水杉", "en"), std::string("metasequoia"));
    REQUIRE_EQ(dictionary.query_target_gloss("水杉", "fr"), std::string("métaséquoia"));
    REQUIRE_EQ(dictionary.query_target_gloss("水杉", "ja"), std::string("メタセコイア"));
    // 本地没有这个语种：交给云端。
    REQUIRE(dictionary.query_target_gloss("水杉", "th").empty());
    REQUIRE(dictionary.query_target_gloss("银牌", "fr").empty());
}
