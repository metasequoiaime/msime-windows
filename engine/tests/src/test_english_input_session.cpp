#include "../../core/data_path.h"
#include "../../core/input_session.h"
#include "../../user_dictionary/user_dictionary_journal.h"
#include "test_directory_cleanup.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdio>

namespace
{
class Database
{
  public:
    explicit Database(const std::filesystem::path &path)
    {
        if (sqlite3_open(metasequoia::path_to_utf8(path).c_str(), &database_) != SQLITE_OK)
        {
            throw std::runtime_error("Failed to create the English input-session test dictionary.");
        }
    }

    ~Database()
    {
        sqlite3_close(database_);
    }

    void execute(const char *sql)
    {
        char *error = nullptr;
        if (sqlite3_exec(database_, sql, nullptr, nullptr, &error) != SQLITE_OK)
        {
            const std::string message = error == nullptr ? "SQLite operation failed." : error;
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

    std::int64_t query_integer(const char *sql)
    {
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
            sqlite3_step(statement) != SQLITE_ROW)
        {
            sqlite3_finalize(statement);
            throw std::runtime_error("Failed to query the English input-session test dictionary.");
        }
        const std::int64_t value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return value;
    }

  private:
    sqlite3 *database_ = nullptr;
};

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void type(metasequoia::InputSession &session, const std::string &text)
{
    for (const char character : text)
    {
        require(session.handle_character(character).handled, "An English test character was not handled.");
    }
}

void set_data_directory(const std::filesystem::path &directory)
{
#ifdef _WIN32
    if (_wputenv_s(L"METASEQUOIA_IME_DATA_DIR", directory.c_str()) != 0)
#else
    if (setenv("METASEQUOIA_IME_DATA_DIR", metasequoia::path_to_utf8(directory).c_str(), 1) != 0)
#endif
    {
        throw std::runtime_error("Failed to set the English test data directory.");
    }
}

void write_file(const std::filesystem::path &path, const std::string &contents)
{
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
    if (!stream)
    {
        throw std::runtime_error("Failed to prepare the corrupt English dictionary fixture.");
    }
}

void prepare_main_database(const std::filesystem::path &directory, bool two_candidates)
{
    std::filesystem::create_directories(directory);
    Database database(directory / "msime.db");
    database.execute("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
    database.execute("INSERT INTO tbl_1_n VALUES('ni','n','你',200)");
    if (two_candidates)
    {
        database.execute("INSERT INTO tbl_1_n VALUES('ni','n','倪',100)");
    }
}
} // namespace

int run_test()
{
    const auto suffix = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("metasequoia-english-session-" + suffix);
    metasequoia::test::ScopedDataDirectoryCleanup cleanup(root);
    const std::filesystem::path english_mode_directory = root / "english-modes";
    prepare_main_database(english_mode_directory, true);
    {
        Database database(english_mode_directory / "english.db");
        database.execute("CREATE TABLE english_words(word TEXT COLLATE BINARY NOT NULL,display TEXT NOT NULL,"
                         "weight INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(word,display)) WITHOUT ROWID");
        database.execute("CREATE TABLE en_zh_glosses(english TEXT PRIMARY KEY,chinese_gloss TEXT NOT NULL)");
        database.execute("CREATE TABLE zh_en_glosses(chinese TEXT PRIMARY KEY,english_gloss TEXT NOT NULL)");
        database.execute("INSERT INTO english_words VALUES('ni','Ni',300)");
        database.execute("INSERT INTO english_words VALUES('ninja','Ninja',200)");
        database.execute("INSERT INTO english_words VALUES('nimbus','Nimbus',100)");
        database.execute("INSERT INTO english_words VALUES('ni','倪',50)");
        database.execute("INSERT INTO english_words VALUES('hello','Hello',300)");
        database.execute("INSERT INTO english_words VALUES('help','Help',200)");
    }
    set_data_directory(english_mode_directory);

    {
        const auto directory = root / "english-spelling-fallback";
        prepare_main_database(directory, true);
        Database database(directory / "english.db");
        database.execute("CREATE TABLE english_words(word TEXT COLLATE BINARY NOT NULL,display TEXT NOT NULL,"
                         "weight INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(word,display)) WITHOUT ROWID;");
        database.execute("INSERT INTO english_words VALUES('working','working',400),('worked','worked',300);");
        set_data_directory(directory);
        for (int mode = 0; mode < 3; ++mode)
        {
            metasequoia::InputSession session(SchemeType::Quanpin);
            metasequoia::EnglishInputOptions options;
            options.mixed_candidates = true;
            options.minimum_prefix = 2;
            require(session.set_english_input_options(options), "Spelling test options were rejected.");
            if (mode == 1)
                session.set_dedicated_english_mode(true);
            if (mode == 2)
                require(session.handle_character('Y', true).handled, "Temporary English mode did not start.");
            type(session, "wroking");
            const auto &candidates = session.candidates();
            const auto found = std::find_if(candidates.begin(), candidates.end(), [](const WordItem &item) {
                return item.source == CandidateSource::EnglishDictionary && item.word == "working";
            });
            require(found != candidates.end() && found->corrected_from == "wroking",
                    "An English input path lost the corrected candidate.");
            const auto selected = session.select_candidate(static_cast<size_t>(found - candidates.begin()));
            require(selected.handled && selected.commit == "working" && !session.has_composition(),
                    "Selecting a spelling correction did not replace the complete misspelled input.");
        }
        set_data_directory(english_mode_directory);
    }

    metasequoia::EnglishInputOptions mixed_options;
    mixed_options.mixed_candidates = true;
    mixed_options.minimum_prefix = 2;
    metasequoia::InputSession mixed(SchemeType::Quanpin);
    require(mixed.set_english_input_options(mixed_options), "Valid mixed-English options were rejected.");
    type(mixed, "ni");
    require(
        mixed.candidates().size() == 5 && mixed.candidates()[0].word == "你" && mixed.candidates()[1].word == "Ni" &&
            mixed.candidates()[1].source == CandidateSource::EnglishDictionary && mixed.candidates()[2].word == "倪" &&
            mixed.candidates()[3].word == "Ninja" && mixed.candidates()[4].word == "Nimbus",
        "Mixed English candidates did not occupy the Windows-compatible slot after the leading Chinese candidate.");

    {
        const auto directory = root / "mixed-completion-budget";
        prepare_main_database(directory, true);
        Database database(directory / "english.db");
        database.execute("CREATE TABLE english_words(word TEXT COLLATE BINARY NOT NULL,display TEXT NOT NULL,"
                         "weight INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(word,display)) WITHOUT ROWID");
        database.execute("INSERT INTO english_words VALUES"
                         "('nice','NICE',1000),('nice','Nice',999),('nice','nICE',998),"
                         "('nice','nIce',997),('nice','niCe',996),('nice','nice',995),"
                         "('night','night',100),('nine','nine',90),('nick','nick',80),"
                         "('nicked','nicked',70),('nicking','nicking',60),('nipped','nipped',50),"
                         "('nipping','nipping',40);");
        set_data_directory(directory);
        metasequoia::InputSession completions(SchemeType::Quanpin);
        require(completions.set_english_input_options(mixed_options), "Mixed completion options were rejected.");
        type(completions, "ni");
        const auto &candidates = completions.candidates();
        std::vector<std::string> english_words;
        for (const auto &candidate : candidates)
            if (candidate.source == CandidateSource::EnglishDictionary)
                english_words.push_back(candidate.pinyin);
        require(english_words.size() == 12, "Mixed input dropped a previously visible casing display.");
        std::sort(english_words.begin(), english_words.end());
        english_words.erase(std::unique(english_words.begin(), english_words.end()), english_words.end());
        require(english_words.size() == 8,
                "Mixed input still limited completion rows before deduplicating casing variants.");
        for (const std::string form : {"nicked", "nicking", "nipped", "nipping"})
            require(std::any_of(candidates.begin(), candidates.end(),
                                [&](const WordItem &item) {
                                    return item.source == CandidateSource::EnglishDictionary && item.word == form;
                                }),
                    "An attested inflected form was missing from mixed-input completions.");
    }
    set_data_directory(english_mode_directory);

    metasequoia::InputSession threshold(SchemeType::Quanpin);
    mixed_options.minimum_prefix = 3;
    require(threshold.set_english_input_options(mixed_options), "A valid English minimum prefix was rejected.");
    type(threshold, "ni");
    require(std::none_of(threshold.candidates().begin(), threshold.candidates().end(),
                         [](const WordItem &item) { return item.source == CandidateSource::EnglishDictionary; }),
            "Mixed English candidates appeared below the configured minimum prefix.");
    require(threshold.handle_character('n').handled &&
                std::any_of(threshold.candidates().begin(), threshold.candidates().end(),
                            [](const WordItem &item) { return item.source == CandidateSource::EnglishDictionary; }),
            "Mixed English candidates did not appear at the configured minimum prefix.");
    mixed_options.minimum_prefix = 0;
    require(!threshold.set_english_input_options(mixed_options), "An invalid English minimum prefix was accepted.");
    mixed_options.minimum_prefix = 9;
    require(!threshold.set_english_input_options(mixed_options),
            "An out-of-range English minimum prefix was accepted.");

    metasequoia::InputSession dedicated(SchemeType::Quanpin);
    dedicated.set_dedicated_english_mode(true);
    require(dedicated.dedicated_english_mode() && !dedicated.has_composition(),
            "Dedicated English mode did not start cleanly.");
    type(dedicated, "HE");
    require(dedicated.preedit() == "HE" && dedicated.candidates().size() == 2 &&
                dedicated.candidates()[0].word == "Hello" && dedicated.candidates()[1].word == "Help" &&
                std::all_of(dedicated.candidates().begin(), dedicated.candidates().end(),
                            [](const WordItem &item) { return item.source == CandidateSource::EnglishDictionary; }),
            "Dedicated English mode did not expose English-only prefix candidates.");
    const auto selected = dedicated.select_candidate(1);
    require(selected.handled && selected.commit == "Help" && dedicated.dedicated_english_mode() &&
                !dedicated.has_composition(),
            "Dedicated English candidate selection left the mode or committed the wrong text.");

    type(dedicated, "Codex");
    require(dedicated.candidates().size() == 1 && dedicated.candidates().front().word == "Codex" &&
                dedicated.candidates().front().source == CandidateSource::Generated,
            "Dedicated English mode did not expose a raw fallback.");
    const auto learned = dedicated.handle_command(metasequoia::Command::CommitRaw);
    require(learned.handled && learned.commit == "Codex" && dedicated.dedicated_english_mode() &&
                !dedicated.has_composition(),
            "Raw Enter did not commit and retain dedicated English mode.");
    {
        Database database(english_mode_directory / "english.db");
        require(database.query_integer("SELECT COUNT(*) FROM english_words WHERE word='codex' AND display='Codex'") ==
                    1,
                "Raw Enter did not learn the dedicated English word.");
    }
    {
        Database database(english_mode_directory / "msime_user.db");
        require(database.query_integer("SELECT COUNT(*) FROM user_dictionary_operations WHERE dictionary='english' "
                                       "AND key='codex' AND value='Codex' AND operation='upsert'") == 1,
                "Learned dedicated English was not journaled for upgrade replay.");
    }
    {
        // A busy user dictionary must not leave english.db carrying a weight that no journal row can replay: the boost
        // would be reverted at the next dictionary upgrade even though the dictionary already showed it.
        const std::string english_db = metasequoia::path_to_utf8(english_mode_directory / "english.db");
        const std::string user_db = user_dictionary::default_user_db_path();
        const std::vector<WordItem> ranked{WordItem("ninja", "Ninja", 200, CandidateSource::EnglishDictionary),
                                           WordItem("nimbus", "Nimbus", 100, CandidateSource::EnglishDictionary)};
        sqlite3 *blocker = nullptr;
        require(sqlite3_open(user_db.c_str(), &blocker) == SQLITE_OK &&
                    sqlite3_exec(blocker, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK,
                "Failed to hold a write lock on the user dictionary database.");
        bool ranking_changed = true;
        const bool blocked = user_dictionary::adjust_english_candidate_ranking(
            english_db, user_db, "english:ni", ranked, "nimbus", "Nimbus", "promote", 1, 1, true, &ranking_changed);
        sqlite3_exec(blocker, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(blocker);
        require(!blocked && !ranking_changed, "A locked user dictionary reported a persisted English ranking.");
        {
            Database database(english_mode_directory / "english.db");
            require(database.query_integer("SELECT weight FROM english_words WHERE word='nimbus' AND "
                                           "display='Nimbus'") == 100,
                    "An unjournaled English ranking was written into the English dictionary.");
        }
        require(user_dictionary::adjust_english_candidate_ranking(english_db, user_db, "english:ni", ranked, "nimbus",
                                                                  "Nimbus", "promote", 1, 1, true, &ranking_changed) &&
                    ranking_changed,
                "English ranking was not persisted once the user dictionary was writable.");
        {
            Database database(english_mode_directory / "english.db");
            require(database.query_integer("SELECT weight FROM english_words WHERE word='nimbus' AND "
                                           "display='Nimbus'") == 1200,
                    "English ranking did not lift the selected candidate above the candidate list.");
        }
        {
            Database database(english_mode_directory / "msime_user.db");
            require(database.query_integer("SELECT weight FROM user_dictionary_operations WHERE dictionary='english' "
                                           "AND key='nimbus' AND value='Nimbus' AND operation='upsert'") == 1200,
                    "English ranking was not journaled for upgrade replay.");
        }

        // 混输列表里的中文候选不能当英文调频的基准：中文词库的权重是另一个量纲，拿它 + 1000 会让
        // 选过一次的英文永久压过「在」这种高频字。
        const std::vector<WordItem> mixed_ranked{WordItem("ni", "你", 19195987, CandidateSource::Database),
                                                 WordItem("nimbus", "Nimbus", 1200, CandidateSource::EnglishDictionary),
                                                 WordItem("ninja", "Ninja", 200, CandidateSource::EnglishDictionary)};
        require(user_dictionary::adjust_english_candidate_ranking(english_db, user_db, "english:nin", mixed_ranked,
                                                                  "ninja", "Ninja", "promote", 1, 1, false),
                "English ranking in a mixed list failed.");
        {
            Database database(english_mode_directory / "english.db");
            require(database.query_integer("SELECT weight FROM english_words WHERE word='ninja' AND "
                                           "display='Ninja'") == 2200,
                    "English ranking used a Chinese candidate's weight as its baseline.");
        }
    }
    {
        const std::string user_db = user_dictionary::default_user_db_path();
        require(!user_dictionary::english_slot(user_db, "zai").has_value(), "An unlearned English slot was stored.");

        // 补全词默认在首页末位（下标 6）：每选中一次按 promote 的名次规则前移，能一路升到首位。
        const int expected_promotions[] = {4, 3, 2, 1, 0};
        int english_index = 6;
        for (const int expected : expected_promotions)
        {
            require(user_dictionary::learn_english_slot_selection(user_db, "zai", english_index, "promote", 1, 1) &&
                        user_dictionary::english_slot(user_db, "zai") == expected,
                    "Selecting the slotted English candidate did not promote it by rank.");
            english_index = expected;
        }
        // 已在首位：没有可学的。
        require(user_dictionary::learn_english_slot_selection(user_db, "zai", 0, "promote", 1, 1) &&
                    user_dictionary::english_slot(user_db, "zai") == 0,
                "Selecting a first English candidate changed its slot.");

        // 英文在首位时越过它选了第二个候选（「在」）：英文后退一位，不会赶不走。
        require(user_dictionary::learn_english_slot_bypass(user_db, "zai", 0, 1, "promote", 1, 1, 6) &&
                    user_dictionary::english_slot(user_db, "zai") == 1,
                "Bypassing a first English candidate did not demote it.");
        // 选的候选离得太远、调频目标没越过英文，不算越过。
        require(user_dictionary::learn_english_slot_bypass(user_db, "zai", 1, 7, "promote", 1, 1, 6) &&
                    user_dictionary::english_slot(user_db, "zai") == 1,
                "A pick that does not pass the English candidate demoted it.");

        // trigger_count：累计够次数才动。
        require(user_dictionary::learn_english_slot_bypass(user_db, "bus", 1, 2, "promote", 1, 2, 6) &&
                    !user_dictionary::english_slot(user_db, "bus").has_value(),
                "An English slot moved before reaching the trigger count.");
        require(user_dictionary::learn_english_slot_bypass(user_db, "bus", 1, 2, "promote", 1, 2, 6) &&
                    user_dictionary::english_slot(user_db, "bus") == 2,
                "An English slot did not move at the trigger count.");

        // 已在首页末位不再后退；调频关闭时不学。
        require(user_dictionary::learn_english_slot_bypass(user_db, "bus", 6, 7, "promote", 1, 1, 6) &&
                    user_dictionary::english_slot(user_db, "bus") == 2,
                "An English slot moved past the last slot of the first page.");
        require(user_dictionary::set_english_slot(user_db, "github", 3) &&
                    user_dictionary::learn_english_slot_selection(user_db, "github", 3, "disabled", 1, 1) &&
                    user_dictionary::english_slot(user_db, "github") == 3,
                "English slot learning ran with frequency adjustment disabled.");
    }
    {
        const auto reads_as_pinyin = [](SchemeType scheme, const std::string &input) {
            metasequoia::InputSession session(scheme);
            type(session, input);
            return session.reads_as_pinyin();
        };
        require(reads_as_pinyin(SchemeType::Quanpin, "zai"), "A complete syllable did not read as pinyin.");
        require(reads_as_pinyin(SchemeType::Quanpin, "bus"), "A syllable with a jianpin tail did not read as pinyin.");
        require(!reads_as_pinyin(SchemeType::Quanpin, "github"), "An English-only spelling read as pinyin.");
        require(!reads_as_pinyin(SchemeType::Quanpin, "upstream"), "An English-only spelling read as pinyin.");
        require(reads_as_pinyin(SchemeType::Shuangpin, "zd"), "A complete shuangpin code did not read as pinyin.");
        require(reads_as_pinyin(SchemeType::Shuangpin, "zdb"),
                "A shuangpin code with a pending initial did not read as pinyin.");
    }
    dedicated.set_dedicated_english_mode(false);
    require(!dedicated.dedicated_english_mode() && !dedicated.has_composition(),
            "Dedicated English mode did not exit cleanly.");

    const std::filesystem::path corrupt_directory = root / "english-corrupt";
    prepare_main_database(corrupt_directory, false);
    write_file(corrupt_directory / "english.db", "not a sqlite database");
    set_data_directory(corrupt_directory);
    mixed_options = {};
    mixed_options.mixed_candidates = true;
    metasequoia::InputSession corrupt(SchemeType::Quanpin);
    require(corrupt.set_english_input_options(mixed_options),
            "Valid mixed-English options were rejected for failure isolation.");
    type(corrupt, "ni");
    require(!corrupt.candidates().empty() && corrupt.candidates().front().word == "你" &&
                std::none_of(corrupt.candidates().begin(), corrupt.candidates().end(),
                             [](const WordItem &item) { return item.source == CandidateSource::EnglishDictionary; }),
            "An English database failure suppressed local Chinese candidates.");

    const std::filesystem::path missing_directory = root / "english-missing";
    prepare_main_database(missing_directory, false);
    set_data_directory(missing_directory);
    metasequoia::InputSession missing(SchemeType::Quanpin);
    require(missing.set_english_input_options(mixed_options),
            "Valid mixed-English options were rejected for missing-database isolation.");
    type(missing, "ni");
    require(!missing.candidates().empty() && missing.candidates().front().word == "你" &&
                !std::filesystem::exists(missing_directory / "english.db"),
            "A missing English database suppressed Chinese candidates or was created by a read-only query.");

    return 0;
}

int main()
{
    try
    {
        return run_test();
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    catch (...)
    {
        std::fprintf(stderr, "An exception escaped the test body.\n");
        return 1;
    }
}
