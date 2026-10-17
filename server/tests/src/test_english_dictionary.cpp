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
        directory_ = std::filesystem::temp_directory_path() / ("metasequoia_english_test_" + suffix);
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

TEST_CASE(english_spelling_fallback_keeps_exact_prefixes_and_recovers_single_edits)
{
    TemporaryEnglishDatabase database;
    EnglishDictionary dictionary(test::Utf8(database.path()));
    database.execute("INSERT INTO english_words VALUES('working','working',400);"
                     "INSERT INTO english_words VALUES('worked','worked',300);"
                     "INSERT INTO english_words VALUES('receive','receive',200);");
    for (const auto &typo : {"wroking", "workign", "workng", "workking", "workimg", "recieve"})
    {
        const auto words = dictionary.query_candidates(typo, 3, true);
        REQUIRE(!words.empty());
        REQUIRE_EQ(words.front().word, std::string(typo) == "recieve" ? "receive" : "working");
        REQUIRE_EQ(words.front().corrected_from, typo);
    }
    const auto exact = dictionary.query_candidates("work", 3, true);
    REQUIRE(!exact.empty());
    REQUIRE(exact.front().corrected_from.empty());
    REQUIRE(dictionary.query_candidates("wr", 3).empty());
    REQUIRE(dictionary.query_candidates("w0rking", 3).empty());
    REQUIRE(dictionary.query_candidates(std::string(25, 'q'), 3).empty());
    REQUIRE(dictionary.query_candidates("wroking", 0).empty());
    REQUIRE(dictionary.query_candidates("zzzzzzzz", 3).empty());
    REQUIRE(dictionary.query_candidates("wroking", 1).size() == 1);
    const auto capitalized = dictionary.query_candidates("Wroking", 1);
    REQUIRE(!capitalized.empty());
    REQUIRE_EQ(capitalized.front().word, "Working");
    REQUIRE(dictionary.query_candidates("WROKING", 3).empty());
    REQUIRE(dictionary.query_candidates("wRoKing", 3).empty());
    REQUIRE(dictionary.query_candidates("unicornxzy", 3).empty());
    database.execute("INSERT INTO english_words VALUES('unicornxyz','unicornxyz',500);");
    const auto refreshed = dictionary.query_candidates("unicornxzy", 1);
    REQUIRE(!refreshed.empty());
    REQUIRE_EQ(refreshed.front().word, "unicornxyz");
    // Raw Enter may have learned the typo. Explicit reconversion still offers
    // alternatives, but never silently overwrites or deletes that literal.
    database.execute("INSERT INTO english_words VALUES('wroking','wroking',9000);");
    const auto learned = dictionary.query_candidates("wroking", 3);
    REQUIRE_EQ(learned.front().word, "wroking");
    const auto manual = dictionary.query_candidates("wroking", 32, false, true);
    REQUIRE_EQ(manual.front().word, "wroking");
    REQUIRE(manual.front().source == CandidateSource::Generated);
    REQUIRE(std::any_of(manual.begin(), manual.end(),
                        [](const auto &item) { return item.word == "working" && item.corrected_from == "wroking"; }));
}

TEST_CASE(english_spelling_wide_prefix_keeps_global_ranking_and_refreshes_after_writes)
{
    TemporaryEnglishDatabase database;
    EnglishDictionary dictionary(test::Utf8(database.path()));
    // A wide prefix with winners near its far end rules out an arbitrary scan
    // cutoff. Its non-power-of-two row count also exercises the compact tree.
    database.execute("WITH RECURSIVE numbers(n) AS (VALUES(0) UNION ALL SELECT n+1 FROM numbers WHERE n<16383) "
                     "INSERT INTO english_words(word,display,weight) "
                     "SELECT 'ca'||char(97+n/17576)||char(97+n/676%26)||char(97+n/26%26)||char(97+n%26),"
                     "'ca'||char(97+n/17576)||char(97+n/676%26)||char(97+n/26%26)||char(97+n%26),0 FROM numbers;");
    database.execute("INSERT INTO english_words VALUES"
                     "('ca','CA',1),('ca','Ca',1),('ca','ca',1),"
                     "('cab','cab',1000),('cazzzx','cazzzx',1000),"
                     "('cazzzz','cazzzz',1000),('cazzzya','cazzzya',1000);");
    // Whole corrected keys beat completions, including multiple stored displays.
    const auto exact = dictionary.query_candidates("cqa", 3);
    REQUIRE_EQ(exact.size(), size_t{3});
    REQUIRE_EQ(exact[0].word, "CA");
    REQUIRE_EQ(exact[1].word, "Ca");
    REQUIRE_EQ(exact[2].word, "ca");
    database.execute("DELETE FROM english_words WHERE word='ca';");
    const auto completions = dictionary.query_candidates("cqa", 3);
    REQUIRE_EQ(completions.size(), size_t{3});
    REQUIRE_EQ(completions[0].word, "cab");
    REQUIRE_EQ(completions[1].word, "cazzzx");
    REQUIRE_EQ(completions[2].word, "cazzzz");
    // External writes must rebuild both the rows and their ranking tree.
    database.execute("UPDATE english_words SET weight=2000 WHERE word='cazzzya';"
                     "INSERT INTO english_words VALUES('cazzzyb','cazzzyb',3000);");
    const auto refreshed = dictionary.query_candidates("cqa", 3);
    REQUIRE_EQ(refreshed.size(), size_t{3});
    REQUIRE_EQ(refreshed[0].word, "cazzzyb");
    REQUIRE_EQ(refreshed[1].word, "cazzzya");
    REQUIRE_EQ(refreshed[2].word, "cab");
    for (const auto &item : refreshed)
        REQUIRE_EQ(item.corrected_from, "cqa");
}
