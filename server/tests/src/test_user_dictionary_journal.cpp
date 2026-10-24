#include "tests/includes/fake_traditional_spelling_index.h"
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"
#include "engine/local_modes/date_time_query.h"
#include "engine/user_dictionary/user_dictionary_journal.h"
#include "ipc/traditional_variant_expansion_policy.h"

#include <sqlite3.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

namespace
{
class TestDatabase
{
  public:
    explicit TestDatabase(const std::filesystem::path &path)
    {
        REQUIRE_EQ(sqlite3_open(test::Utf8(path).c_str(), &db_), SQLITE_OK);
    }
    ~TestDatabase()
    {
        if (db_ != nullptr)
            sqlite3_close(db_);
    }
    void exec(const char *sql)
    {
        REQUIRE_EQ(sqlite3_exec(db_, sql, nullptr, nullptr, nullptr), SQLITE_OK);
    }
    int scalar_int(const char *sql)
    {
        return static_cast<int>(scalar_int64(sql));
    }

    std::int64_t scalar_int64(const char *sql)
    {
        sqlite3_stmt *stmt = nullptr;
        REQUIRE_EQ(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), SQLITE_OK);
        REQUIRE_EQ(sqlite3_step(stmt), SQLITE_ROW);
        const std::int64_t value = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return value;
    }

  private:
    sqlite3 *db_ = nullptr;
};

// TempRoot 下的一个临时目录：构造时清空重建，析构时删掉，断言失败抛异常时也删。
class ScopedTempDirectory
{
  public:
    explicit ScopedTempDirectory(const std::string &name)
        : path_(test::TempRoot() / (name + std::to_string(GetCurrentProcessId())))
    {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~ScopedTempDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    ScopedTempDirectory(const ScopedTempDirectory &) = delete;
    ScopedTempDirectory &operator=(const ScopedTempDirectory &) = delete;
    const std::filesystem::path &path() const
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};
} // namespace

TEST_CASE(DictionaryLockWaitEndsBeforeTheClientGivesUp)
{
    // The wait exists to absorb a lock held for one commit. Anything longer than the client's own reply window cannot
    // end in a reply the client is still listening for, so the reader is better off failing the page than holding a
    // key until the client resets the composition. windows/src/IPC/Ipc.h owns the numbers; keep this the shorter of
    // the two.
    static_assert(user_dictionary::kJournalBusyTimeoutMs < 300,
                  "must fit inside FANY_IME_COMMIT_REPLY_SESSION_WAIT_MS");

    const auto directory = test::TempRoot() / ("msime-lock-wait-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    // A path other than default_user_db_path(): acquire_database only caches the default journal's connection, so this
    // one is opened per call and closed when the operation returns, which is what lets the directory be deleted below.
    REQUIRE(user_dictionary::ensure_user_database(test::Utf8(user_path)));

    {
        // Hold the journal's write lock from a separate connection, the way a learning commit would.
        TestDatabase holder(user_path);
        holder.exec("BEGIN IMMEDIATE");

        const ULONGLONG started = GetTickCount64();
        const bool wrote = user_dictionary::set_fixed_position(test::Utf8(user_path), "ni", "ni", "乙", 1);
        const ULONGLONG waited = GetTickCount64() - started;

        // The write must lose, and must lose quickly, rather than sit on the lock until the client has already given
        // up. A write that somehow succeeded would also mean the lock was not real, so neither branch may pass.
        REQUIRE(!wrote);
        REQUIRE(waited < 300);
        holder.exec("ROLLBACK");
    }

    std::filesystem::remove_all(directory, ec);
}

// The dictionary writer thread commits learned weights while the key thread
// builds candidates from the same files. In rollback-journal mode a commit's
// exclusive lock made those reads wait out the whole commit -- up to a second
// per key on a busy disk, long enough to time out a following commit key and
// drop the composition. In WAL mode the reads see the last committed snapshot.
TEST_CASE(DictionaryReadsDoNotWaitForALearningWrite)
{
    const auto directory = test::TempRoot() / ("msime-wal-reads-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    const auto main_path = directory / "msime.db";
    {
        TestDatabase db(main_path);
        db.exec("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                "INSERT INTO tbl_1_n VALUES('ni','n','甲',100);"
                "INSERT INTO tbl_1_n VALUES('ni','n','乙',90);");
    }
    REQUIRE(user_dictionary::enable_write_ahead_log(test::Utf8(main_path)));
    REQUIRE(user_dictionary::set_fixed_position(test::Utf8(user_path), "ni", "ni", "乙", 1));

    {
        TestDatabase journal_writer(user_path);
        TestDatabase main_writer(main_path);
        REQUIRE_EQ(journal_writer.scalar_int("SELECT COUNT(*) FROM pragma_journal_mode WHERE journal_mode='wal'"), 1);
        journal_writer.exec("BEGIN EXCLUSIVE");
        journal_writer.exec("DELETE FROM fixed_candidate_positions");
        main_writer.exec("BEGIN EXCLUSIVE");
        main_writer.exec("UPDATE tbl_1_n SET weight=1");

        const ULONGLONG started = GetTickCount64();
        std::vector<WordItem> candidates = {{"ni", "甲", 100}, {"ni", "乙", 90}};
        user_dictionary::apply_fixed_positions(test::Utf8(user_path), "ni", candidates, false);
        REQUIRE(std::any_of(candidates.begin(), candidates.end(),
                            [](const WordItem &item) { return item.word == "乙" && item.fixed_position == 1; }));
        TestDatabase reader(main_path);
        REQUIRE_EQ(reader.scalar_int("SELECT weight FROM tbl_1_n WHERE value='甲'"), 100);
        REQUIRE(GetTickCount64() - started < 1000);

        journal_writer.exec("ROLLBACK");
        main_writer.exec("ROLLBACK");
    }
    std::filesystem::remove_all(directory, ec);
}

TEST_CASE(UserDictionaryReplayIsIdempotentAcrossAllSettingsDictionaries)
{
    const auto directory = test::TempRoot() / ("msime-user-dictionary-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    const auto main_path = directory / "msime.db";
    const auto english_path = directory / "english.db";

    {
        TestDatabase main_db(main_path);
        main_db.exec("CREATE TABLE tbl_2_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                     "CREATE TABLE tbl_others_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                     "CREATE TABLE wubi86(key TEXT,value TEXT,weight INTEGER);"
                     "CREATE TABLE quick_parases(key TEXT,value TEXT,weight INTEGER);"
                     "INSERT INTO tbl_2_n VALUES('ni''hao','nh','旧词',1);"
                     "INSERT INTO wubi86 VALUES('abcd','旧五笔',1);");
        TestDatabase english_db(english_path);
        english_db.exec("CREATE TABLE english_words(word TEXT PRIMARY KEY,display TEXT);"
                        "INSERT INTO english_words VALUES('obsolete','obsolete');");
    }

    REQUIRE(user_dictionary::record_upsert(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin, "ni'hao",
                                           "你好", 12000));
    REQUIRE(user_dictionary::record_delete(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin, "ni'hao",
                                           "旧词"));
    REQUIRE(user_dictionary::record_upsert(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin,
                                           "shui'shan'shu'ru'fa'hai'ke'yi", "水杉输入法还可以", 5000));
    REQUIRE(user_dictionary::record_upsert(test::Utf8(user_path), user_dictionary::DictionaryKind::Wubi, "wxyz",
                                           "新五笔", 88));
    REQUIRE(
        user_dictionary::record_delete(test::Utf8(user_path), user_dictionary::DictionaryKind::Wubi, "abcd", "旧五笔"));
    REQUIRE(user_dictionary::record_upsert(test::Utf8(user_path), user_dictionary::DictionaryKind::QuickPhrase, "mail",
                                           "example@example.com", 20));
    REQUIRE(user_dictionary::record_upsert(test::Utf8(user_path), user_dictionary::DictionaryKind::English, "codex",
                                           "codex", 0, "Codex"));
    REQUIRE(user_dictionary::record_delete(test::Utf8(user_path), user_dictionary::DictionaryKind::English, "obsolete",
                                           "obsolete"));

    for (int pass = 0; pass < 2; ++pass)
    {
        const auto replay =
            user_dictionary::replay(test::Utf8(user_path), test::Utf8(main_path), test::Utf8(english_path));
        REQUIRE(replay.error.empty());
        REQUIRE_EQ(replay.failed, 0);
        REQUIRE_EQ(replay.applied, 8);
    }

    {
        TestDatabase main_db(main_path);
        REQUIRE_EQ(main_db.scalar_int("SELECT COUNT(*) FROM tbl_2_n WHERE key='ni''hao' AND value='你好' AND "
                                      "jp='nh' AND weight=12000"),
                   1);
        REQUIRE_EQ(main_db.scalar_int("SELECT COUNT(*) FROM tbl_2_n WHERE value='旧词'"), 0);
        REQUIRE_EQ(
            main_db.scalar_int("SELECT COUNT(*) FROM tbl_others_s WHERE key='shui''shan''shu''ru''fa''hai''ke''yi' "
                               "AND value='水杉输入法还可以' AND jp='sssrfhky' AND weight=5000"),
            1);
        REQUIRE_EQ(main_db.scalar_int("SELECT COUNT(*) FROM wubi86 WHERE key='wxyz' AND value='新五笔' AND weight=88"),
                   1);
        REQUIRE_EQ(main_db.scalar_int("SELECT COUNT(*) FROM wubi86 WHERE value='旧五笔'"), 0);
        REQUIRE_EQ(main_db.scalar_int("SELECT COUNT(*) FROM quick_parases WHERE key='mail' AND weight=20"), 1);
        TestDatabase english_db(english_path);
        REQUIRE_EQ(english_db.scalar_int("SELECT COUNT(*) FROM english_words WHERE word='codex' AND display='Codex'"),
                   1);
        REQUIRE_EQ(english_db.scalar_int("SELECT COUNT(*) FROM english_words WHERE word='obsolete'"), 0);
    }

    std::filesystem::remove_all(directory);
}

// The journal stores the absolute weights a learning write produced against the shipped
// dictionary of that time. After an upgrade that rescaled the dictionary (msime-dictionary
// aligned its pinyin weights with rime-wanxiang: 了/le went from 3.5 million to 52 million),
// replaying 了 at its old learned 6.5 million put 来 and 里 ahead of it on "l". A changed
// dictionary now keeps the new shipped weight for learned rows that sit below it.
TEST_CASE(UserDictionaryReplayKeepsNewShippedWeightsWhenTheDictionaryChanged)
{
    const auto directory = test::TempRoot() / ("msime-replay-rebase-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    const auto main_path = directory / "msime.db";
    const auto english_path = directory / "english.db";
    const auto manifest_path = directory / "dictionary-manifest.json";

    const auto install_dictionary = [&](const char *sha256) {
        std::filesystem::remove(main_path, ec);
        {
            TestDatabase main_db(main_path);
            main_db.exec("CREATE TABLE tbl_1_l(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "CREATE TABLE tbl_2_z(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "CREATE TABLE wubi86(key TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_l VALUES('le','l','了',52009647);"
                         "INSERT INTO tbl_1_l VALUES('le','l','乐',6501204);"
                         "INSERT INTO tbl_1_l VALUES('lai','l','来',9077734);"
                         "INSERT INTO tbl_2_z VALUES('zhi''dao','zd','知道',1807266);"
                         "INSERT INTO tbl_2_z VALUES('zhi''dao','zd','直到',364449);"
                         "INSERT INTO wubi86 VALUES('bnhn','了',900);");
        }
        if (sha256 == nullptr)
        {
            std::filesystem::remove(manifest_path, ec);
            return;
        }
        std::ofstream manifest(manifest_path, std::ios::binary | std::ios::trunc);
        manifest << "{\"files\":{\"msime.db\":{\"sha256\":\"" << sha256 << "\"}}}";
    };
    {
        TestDatabase english_db(english_path);
        english_db.exec("CREATE TABLE english_words(word TEXT PRIMARY KEY,display TEXT);");
    }

    // Learned on the previous dictionary: 了 and 乐 by the old staircase, 直到 promoted past 知道,
    // 了 in wubi. 来 was added by hand and 知道 lowered by hand in settings; both are replayed as
    // written whatever the dictionary says.
    REQUIRE(user_dictionary::record_upsert(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin, "le", "了",
                                           6501704));
    REQUIRE(user_dictionary::record_upsert(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin, "le", "乐",
                                           6501204));
    REQUIRE(user_dictionary::record_upsert(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin, "zhi'dao",
                                           "直到", 5052903));
    REQUIRE(user_dictionary::record_upsert(test::Utf8(user_path), user_dictionary::DictionaryKind::Wubi, "bnhn", "了",
                                           100));
    REQUIRE(user_dictionary::record_user_insert(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin, "lai",
                                                "来", 10000));
    REQUIRE(user_dictionary::record_weight_edit(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin,
                                                "zhi'dao", "知道", 1));

    // No manifest: the shipped dictionary cannot be identified, so every weight is replayed as written.
    install_dictionary(nullptr);
    auto replay = user_dictionary::replay(test::Utf8(user_path), test::Utf8(main_path), test::Utf8(english_path));
    REQUIRE(replay.error.empty());
    REQUIRE_EQ(replay.rebased, 0);
    {
        TestDatabase main_db(main_path);
        REQUIRE_EQ(main_db.scalar_int64("SELECT weight FROM tbl_1_l WHERE value='了'"), 6501704);
    }

    // A dictionary the journal has not been replayed onto yet.
    install_dictionary("a");
    replay = user_dictionary::replay(test::Utf8(user_path), test::Utf8(main_path), test::Utf8(english_path));
    REQUIRE(replay.error.empty());
    REQUIRE_EQ(replay.failed, 0);
    REQUIRE_EQ(replay.applied, 6);
    REQUIRE_EQ(replay.rebased, 2);
    {
        TestDatabase main_db(main_path);
        REQUIRE_EQ(main_db.scalar_int64("SELECT weight FROM tbl_1_l WHERE value='了'"), 52009647);
        REQUIRE_EQ(main_db.scalar_int64("SELECT weight FROM tbl_1_l WHERE value='乐'"), 6501204);
        REQUIRE_EQ(main_db.scalar_int64("SELECT weight FROM tbl_2_z WHERE value='直到'"), 5052903);
        REQUIRE_EQ(main_db.scalar_int64("SELECT weight FROM tbl_2_z WHERE value='知道'"), 1);
        REQUIRE_EQ(main_db.scalar_int64("SELECT weight FROM wubi86 WHERE value='了'"), 900);
        REQUIRE_EQ(main_db.scalar_int64("SELECT weight FROM tbl_1_l WHERE value='来'"), 10000);
        TestDatabase journal(user_path);
        REQUIRE_EQ(journal.scalar_int64("SELECT weight FROM user_dictionary_operations WHERE value='了' AND "
                                        "dictionary='pinyin'"),
                   52009647);
        REQUIRE_EQ(journal.scalar_int64("SELECT weight FROM user_dictionary_operations WHERE value='了' AND "
                                        "dictionary='wubi'"),
                   900);
        REQUIRE_EQ(journal.scalar_int64("SELECT weight FROM user_dictionary_operations WHERE value='直到'"), 5052903);
        REQUIRE_EQ(journal.scalar_int("SELECT COUNT(*) FROM dictionary_state WHERE "
                                      "name='replayed_dictionary_sha256' AND value='a'"),
                   1);
    }

    // Learning on that dictionary, then reinstalling the same dictionary: weights below the
    // shipped ones are the staircase room made for a promotion and must replay exactly.
    REQUIRE(user_dictionary::record_upsert(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin, "le", "乐",
                                           5000000));
    install_dictionary("a");
    replay = user_dictionary::replay(test::Utf8(user_path), test::Utf8(main_path), test::Utf8(english_path));
    REQUIRE(replay.error.empty());
    REQUIRE_EQ(replay.rebased, 0);
    {
        TestDatabase main_db(main_path);
        REQUIRE_EQ(main_db.scalar_int64("SELECT weight FROM tbl_1_l WHERE value='了'"), 52009647);
        REQUIRE_EQ(main_db.scalar_int64("SELECT weight FROM tbl_1_l WHERE value='乐'"), 5000000);
        REQUIRE_EQ(main_db.scalar_int64("SELECT weight FROM tbl_2_z WHERE value='直到'"), 5052903);
    }

    // A learning write over a hand-set weight makes the row learned again.
    REQUIRE(user_dictionary::record_upsert(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin, "zhi'dao",
                                           "知道", 2));
    {
        TestDatabase journal(user_path);
        REQUIRE_EQ(journal.scalar_int("SELECT weight_edited FROM user_dictionary_operations WHERE value='知道'"), 0);
    }

    std::filesystem::remove_all(directory, ec);
}

TEST_CASE(UserDictionaryTracksOnlyExplicitUserInsertionsForExport)
{
    const auto directory = test::TempRoot() / ("msime-user-insert-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";

    {
        TestDatabase legacy_db(user_path);
        legacy_db.exec("CREATE TABLE user_dictionary_operations("
                       "dictionary TEXT NOT NULL,key TEXT NOT NULL,value TEXT NOT NULL,"
                       "operation TEXT NOT NULL,weight INTEGER NOT NULL DEFAULT 0,"
                       "display TEXT NOT NULL DEFAULT '',updated_at INTEGER NOT NULL DEFAULT(unixepoch()),"
                       "PRIMARY KEY(dictionary,key,value));"
                       "INSERT INTO user_dictionary_operations VALUES("
                       "'pinyin','yi','一','upsert',999999,'',unixepoch());");
    }

    REQUIRE(user_dictionary::ensure_user_database(test::Utf8(user_path)));
    REQUIRE(
        !user_dictionary::is_user_inserted(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin, "yi", "一"));

    REQUIRE(user_dictionary::record_upsert(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin, "xi'tong",
                                           "系统", 100));
    REQUIRE(!user_dictionary::is_user_inserted(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin,
                                               "xi'tong", "系统"));

    REQUIRE(user_dictionary::record_user_insert(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin,
                                                "yong'hu", "用户", 10000));
    REQUIRE(user_dictionary::is_user_inserted(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin, "yong'hu",
                                              "用户"));

    REQUIRE(user_dictionary::record_upsert(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin, "yong'hu",
                                           "用户", 20000));
    REQUIRE(user_dictionary::is_user_inserted(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin, "yong'hu",
                                              "用户"));

    std::filesystem::remove_all(directory);
}

TEST_CASE(EnterLearnedEnglishWordsAreValidatedPersistedAndIdempotent)
{
    const auto directory = test::TempRoot() / ("msime-enter-english-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    const auto english_path = directory / "english.db";

    {
        TestDatabase english_db(english_path);
    }

    REQUIRE(user_dictionary::learn_entered_english_word(test::Utf8(english_path), test::Utf8(user_path), "Codex"));
    REQUIRE(user_dictionary::learn_entered_english_word(test::Utf8(english_path), test::Utf8(user_path), "Codex"));
    for (const char *word : {"Kotlin", "Ubuntu", "TypeScript", "Emoji", "Metasequoia", "Java", "YouTube", "Rust"})
        REQUIRE(user_dictionary::learn_entered_english_word(test::Utf8(english_path), test::Utf8(user_path), word));
    REQUIRE(!user_dictionary::learn_entered_english_word(test::Utf8(english_path), test::Utf8(user_path), "ni'hao"));
    REQUIRE(!user_dictionary::learn_entered_english_word(test::Utf8(english_path), test::Utf8(user_path), "hello2"));
    REQUIRE(!user_dictionary::learn_entered_english_word(test::Utf8(english_path), test::Utf8(user_path),
                                                         std::string(65, 'a')));

    {
        TestDatabase english_db(english_path);
        REQUIRE_EQ(english_db.scalar_int(
                       "SELECT COUNT(*) FROM english_words WHERE word='codex' AND display='Codex' AND weight=10"),
                   1);
        REQUIRE_EQ(english_db.scalar_int("SELECT COUNT(*) FROM english_words"), 9);
    }
    REQUIRE(user_dictionary::is_user_inserted(test::Utf8(user_path), user_dictionary::DictionaryKind::English, "codex",
                                              "Codex"));

    const auto main_path = directory / "msime.db";
    {
        TestDatabase main_db(main_path);
        main_db.exec("CREATE TABLE tbl_1_a(key TEXT,jp TEXT,value TEXT,weight INTEGER);");
        std::filesystem::remove(english_path);
        TestDatabase factory_english(english_path);
        factory_english.exec("CREATE TABLE english_words ("
                             "word TEXT PRIMARY KEY COLLATE BINARY, display TEXT NOT NULL"
                             ") WITHOUT ROWID;"
                             "INSERT INTO english_words VALUES('hello','hello');");
    }
    const auto replay = user_dictionary::replay(test::Utf8(user_path), test::Utf8(main_path), test::Utf8(english_path));
    REQUIRE(replay.error.empty());
    REQUIRE_EQ(replay.failed, 0);
    {
        TestDatabase english_db(english_path);
        REQUIRE_EQ(english_db.scalar_int(
                       "SELECT COUNT(*) FROM english_words WHERE word='codex' AND display='Codex' AND weight=10"),
                   1);
        REQUIRE_EQ(english_db.scalar_int("SELECT COUNT(*) FROM english_words WHERE word='metasequoia' AND "
                                         "display='Metasequoia' AND weight=10"),
                   1);
        REQUIRE_EQ(english_db.scalar_int("SELECT COUNT(*) FROM english_words WHERE word='hello' AND display='hello'"),
                   1);
    }

    std::filesystem::remove_all(directory);
}

TEST_CASE(UserDictionarySupportsFixedPositionsAndDeferredSafeRanking)
{
    const auto directory = test::TempRoot() / ("msime-ranking-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    const auto main_path = directory / "msime.db";
    {
        TestDatabase db(main_path);
        db.exec("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                "INSERT INTO tbl_1_n VALUES('ni','n','甲',100);"
                "INSERT INTO tbl_1_n VALUES('ni','n','乙',90);"
                "INSERT INTO tbl_1_n VALUES('ni','n','丙',80);"
                "INSERT INTO tbl_1_n VALUES('ni','n','丁',70);"
                "INSERT INTO tbl_1_n VALUES('ni','n','戊',60);"
                "INSERT INTO tbl_1_n VALUES('ni','n','己',50);");
    }
    std::vector<WordItem> candidates = {{"ni", "甲", 100}, {"ni", "乙", 90}, {"ni", "丙", 80},
                                        {"ni", "丁", 70},  {"ni", "戊", 60}, {"ni", "己", 50}};

    REQUIRE(user_dictionary::set_fixed_position(test::Utf8(user_path), "ni", "ni", "己", 2));
    REQUIRE(!user_dictionary::set_fixed_position(test::Utf8(user_path), "ni", "ni", "己", 6));
    candidates.insert(candidates.begin() + 1, {"ni", "云", 1, CandidateSource::CloudSuggestion});
    candidates.insert(candidates.begin() + 2, {"ni", "AI", 1, CandidateSource::AiSuggestion});
    user_dictionary::apply_fixed_positions(test::Utf8(user_path), "ni", candidates, false);
    REQUIRE_EQ(candidates[1].word, std::string("云"));
    REQUIRE_EQ(candidates[2].word, std::string("AI"));
    REQUIRE_EQ(candidates[3].word, std::string("己"));
    REQUIRE_EQ(candidates[3].fixed_position, 2);

    REQUIRE(user_dictionary::clear_fixed_position(test::Utf8(user_path), "ni", "ni", "己"));

    // With a helpcode active the caller already ordered the suggestions, so they
    // must not be hoisted back to slots 1 and 2.
    candidates = {{"ni", "甲", 100}, {"ni", "乙", 90}, {"ni", "丙", 80}};
    candidates.insert(candidates.begin(), {"ni", "AI", 1, CandidateSource::AiSuggestion});
    candidates.insert(candidates.begin() + 3, {"ni", "云", 1, CandidateSource::CloudSuggestion});
    user_dictionary::apply_fixed_positions(test::Utf8(user_path), "ni", candidates, false, {}, true);
    REQUIRE_EQ(candidates[0].word, std::string("AI"));
    REQUIRE_EQ(candidates[3].word, std::string("云"));

    candidates = {{"ni", "甲", 100}, {"ni", "乙", 90}, {"ni", "丙", 80},
                  {"ni", "丁", 70},  {"ni", "戊", 60}, {"ni", "己", 50}};
    REQUIRE(user_dictionary::adjust_candidate_ranking(test::Utf8(main_path), test::Utf8(user_path), "ni", candidates,
                                                      "ni", "己", "linear", 2, 2, false));
    {
        TestDatabase db(main_path);
        REQUIRE_EQ(db.scalar_int("SELECT weight FROM tbl_1_n WHERE value='己'"), 50);
    }
    REQUIRE(user_dictionary::adjust_candidate_ranking(test::Utf8(main_path), test::Utf8(user_path), "ni", candidates,
                                                      "ni", "己", "linear", 2, 2, false));
    {
        TestDatabase db(main_path);
        REQUIRE(db.scalar_int("SELECT weight FROM tbl_1_n WHERE value='己'") > 70);
    }

    {
        TestDatabase db(main_path);
        db.exec("UPDATE tbl_1_n SET weight=100 WHERE value='甲';"
                "UPDATE tbl_1_n SET weight=99 WHERE value='乙';"
                "UPDATE tbl_1_n SET weight=98 WHERE value='丙';");
    }
    candidates = {{"ni", "甲", 100}, {"ni", "乙", 99}, {"ni", "丙", 98}};
    REQUIRE(user_dictionary::adjust_candidate_ranking(test::Utf8(main_path), test::Utf8(user_path), "ni", candidates,
                                                      "ni", "丙", "linear", 1, 1, false));
    {
        TestDatabase db(main_path);
        REQUIRE_EQ(db.scalar_int("SELECT weight FROM tbl_1_n WHERE value='丙'"), 101);
        REQUIRE_EQ(db.scalar_int("SELECT weight FROM tbl_1_n WHERE value='乙'"), 99);
    }
    std::filesystem::remove_all(directory);
}

TEST_CASE(UserDictionaryRebalanceKeepsWeightsBounded)
{
    const auto directory = test::TempRoot() / ("msime-bounded-ranking-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    const auto main_path = directory / "msime.db";
    {
        TestDatabase db(main_path);
        db.exec("CREATE TABLE tbl_1_y(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                "INSERT INTO tbl_1_y VALUES('yi','y','甲',1000000000000);"
                "INSERT INTO tbl_1_y VALUES('yi','y','乙',999999000000);"
                "INSERT INTO tbl_1_y VALUES('yi','y','丙',999998000000);");
    }
    std::vector<WordItem> candidates = {
        {"yi", "甲", 1000000000000LL},
        {"yi", "乙", 999999000000LL},
        {"yi", "丙", 999998000000LL},
    };

    REQUIRE(user_dictionary::adjust_candidate_ranking(test::Utf8(main_path), test::Utf8(user_path), "yi", candidates,
                                                      "yi", "丙", "pin", 1, 1, true));
    {
        TestDatabase db(main_path);
        REQUIRE_EQ(db.scalar_int("SELECT COUNT(*) FROM tbl_1_y WHERE weight > 100000000"), 0);
        REQUIRE_EQ(db.scalar_int("SELECT COUNT(*) FROM tbl_1_y WHERE value='丙' AND weight=100000000"), 1);
        REQUIRE_EQ(db.scalar_int("SELECT COUNT(*) FROM tbl_1_y WHERE weight < 1"), 0);
    }
    std::filesystem::remove_all(directory);
}

TEST_CASE(UserDictionaryPromoteDoesNotCrushPrefixSinglesFromSeriesQuery)
{
    const auto directory = test::TempRoot() / ("msime-xianwang-ranking-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    const auto main_path = directory / "msime.db";
    {
        TestDatabase db(main_path);
        db.exec("CREATE TABLE tbl_2_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                "CREATE TABLE tbl_1_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                "INSERT INTO tbl_2_x VALUES("
                "'xian''wang','xw','先王',15850),"
                "('xian''wang','xw','先往',4590),"
                "('xian''wang','xw','先汪',1),"
                "('xian''wang','xw','贤王',1),"
                "('xian''wang','xw','先亡',1),"
                "('xian''wang','xw','鲜网',1),"
                "('xian''wang','xw','献王',1),"
                "('xian''wang','xw','宪王',1),"
                "('xian''wang','xw','限网',1),"
                "('xian''wang','xw','现网',1),"
                "('xian''wang','xw','闲望',1),"
                "('xian''wang','xw','线网',1),"
                "('xian''wang','xw','纤网',1),"
                "('xian''wang','xw','宪网',1),"
                "('xian''wang','xw','弦望',1),"
                "('xian''wang','xw','仙王',1);"
                "INSERT INTO tbl_1_x VALUES("
                "'xian','x','先',1662684),"
                "('xian','x','现',1200000),"
                "('xian','x','显',1100000),"
                "('xian','x','线',1000000),"
                "('xian','x','县',1);");
    }

    const std::vector<std::pair<std::string, std::int64_t>> xianwang_words = {
        {"先王", 15850}, {"先往", 4590}, {"先汪", 1}, {"贤王", 1}, {"先亡", 1}, {"鲜网", 1}, {"献王", 1}, {"宪王", 1},
        {"限网", 1},     {"现网", 1},    {"闲望", 1}, {"线网", 1}, {"纤网", 1}, {"宪网", 1}, {"弦望", 1}, {"仙王", 1},
    };
    std::vector<WordItem> candidates;
    for (const auto &[word, weight] : xianwang_words)
        candidates.push_back({"xian'wang", word, weight, CandidateSource::Database, "xian'wang"});
    candidates.push_back({"xian", "先", 1662684, CandidateSource::Database, "xian"});
    candidates.push_back({"xian", "现", 1200000, CandidateSource::Database, "xian"});
    candidates.push_back({"xian", "显", 1100000, CandidateSource::Database, "xian"});
    candidates.push_back({"xian", "线", 1000000, CandidateSource::Database, "xian"});
    candidates.push_back({"xian", "县", 1, CandidateSource::Database, "xian"});

    bool ranking_changed = false;
    REQUIRE(user_dictionary::adjust_candidate_ranking(test::Utf8(main_path), test::Utf8(user_path), "xian'wang",
                                                      candidates, "xian'wang", "现网", "promote", 1, 1, false,
                                                      &ranking_changed));
    REQUIRE(ranking_changed);

    {
        TestDatabase db(main_path);
        REQUIRE(db.scalar_int64("SELECT weight FROM tbl_2_x WHERE value='现网'") > 1);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_1_x WHERE value='先'"), 1662684);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_1_x WHERE value='现'"), 1200000);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_1_x WHERE value='显'"), 1100000);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_1_x WHERE value='线'"), 1000000);
        REQUIRE_EQ(db.scalar_int("SELECT COUNT(*) FROM tbl_1_x WHERE weight < 1"), 0);
        REQUIRE_EQ(db.scalar_int("SELECT COUNT(*) FROM tbl_2_x WHERE weight < 1"), 0);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_2_x WHERE value='先王'"), 15850);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_2_x WHERE value='先往'"), 4590);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_2_x WHERE value='先亡'"), 1);
    }
    {
        TestDatabase user_db(user_path);
        REQUIRE_EQ(user_db.scalar_int("SELECT COUNT(*) FROM user_dictionary_operations WHERE key='xian'"), 0);
        REQUIRE_EQ(user_db.scalar_int("SELECT COUNT(*) FROM user_dictionary_operations WHERE value='先'"), 0);
        REQUIRE_EQ(user_db.scalar_int(
                       "SELECT COUNT(*) FROM user_dictionary_operations WHERE key='xian''wang' AND value='现网' AND "
                       "operation='upsert'"),
                   1);
        REQUIRE(user_db.scalar_int64("SELECT weight FROM user_dictionary_operations WHERE value='现网'") > 1);
    }
    std::filesystem::remove_all(directory);
}

// 回归：调频的权重基准必须取自按权重排好序的比较集，不能按显示顺序取。备选切分词
// （xi'e 的西鄂 w=6）会坐在 xie 显示列表最前面，拿它当基准会把写（原 605147）写成
// 6+500=506，权重反而低于些/血/谢——这就是用户机器上「置顶写之后写还是不在前排」的根因。
// 同音节的多个表仍互相比权，不受影响。
TEST_CASE(UserDictionaryPromoteBasesWeightOnTheHeaviestCandidateNotTheTopmost)
{
    const auto directory = test::TempRoot() / ("msime-cross-table-ranking-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    const auto main_path = directory / "msime.db";
    {
        TestDatabase db(main_path);
        db.exec("CREATE TABLE tbl_1_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                "CREATE TABLE tbl_2_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                "INSERT INTO tbl_1_x VALUES("
                "'xie','x','些',3752167),"
                "('xie','x','血',933230),"
                "('xie','x','谢',623220),"
                "('xie','x','写',605147);"
                "INSERT INTO tbl_2_x VALUES('xi''e','xa','西鄂',6),('xi''e','xa','喜恶',1);");
    }
    // 显示列表照旧把备选切分词放在最前（旧排序的产物）。
    std::vector<WordItem> candidates = {
        {"xi'e", "西鄂", 6, CandidateSource::Database, "xi'e"},
        {"xie", "些", 3752167, CandidateSource::Database, "xie"},
        {"xie", "血", 933230, CandidateSource::Database, "xie"},
        {"xie", "谢", 623220, CandidateSource::Database, "xie"},
        {"xie", "写", 605147, CandidateSource::Database, "xie"},
    };

    bool ranking_changed = false;
    REQUIRE(user_dictionary::adjust_candidate_ranking(test::Utf8(main_path), test::Utf8(user_path), "xie", candidates,
                                                      "xie", "写", "pin", 1, 1, true, &ranking_changed));
    REQUIRE(ranking_changed);
    {
        TestDatabase db(main_path);
        // 置顶写：基准必须是同表的些（3752167），不再是跨表的西鄂 6。
        REQUIRE(db.scalar_int64("SELECT weight FROM tbl_1_x WHERE value='写'") > 3752167);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_2_x WHERE value='西鄂'"), 6);
    }

    // 已经是同表首位的词不再被跨表基准反降（旧逻辑会把些写成 6+500=506）。
    ranking_changed = false;
    REQUIRE(user_dictionary::adjust_candidate_ranking(test::Utf8(main_path), test::Utf8(user_path), "xie", candidates,
                                                      "xie", "些", "pin", 1, 1, true, &ranking_changed));
    REQUIRE(!ranking_changed);
    {
        TestDatabase db(main_path);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_1_x WHERE value='些'"), 3752167);
    }
    std::filesystem::remove_all(directory);
}

// 回归：调频必须能把另一种切分的词调到整个列表的首位。打 jian 时列表里混着 ji'an 的
// 吉安（出货词库权重是 1，压在同组的积案 9420 底下），旧实现按音节数圈比较集，吉安
// 只跟积案/几案比，可学权重封顶一万出头，怎么调都追不上 见 的 3460998——用户看到的就是
// 「调多少次都没用」。比较集不再按音节数切开之后，基准取到 见，一次置顶就越过它。
TEST_CASE(UserDictionaryPromoteLiftsAnAlternativeSegmentationAcrossKeys)
{
    const auto directory = test::TempRoot() / ("msime-cross-key-ranking-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    const auto main_path = directory / "msime.db";
    {
        TestDatabase db(main_path);
        db.exec("CREATE TABLE tbl_1_j(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                "CREATE TABLE tbl_2_j(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                "INSERT INTO tbl_1_j VALUES("
                "'jian','j','见',3460998),"
                "('jian','j','间',3067939),"
                "('jian','j','剑',1151704);"
                "INSERT INTO tbl_2_j VALUES("
                "'ji''an','ja','积案',9420),"
                "('ji''an','ja','几案',5999),"
                "('ji''an','ja','吉安',1);");
    }
    std::vector<WordItem> candidates = {
        {"jian", "见", 3460998, CandidateSource::Database, "jian"},
        {"jian", "间", 3067939, CandidateSource::Database, "jian"},
        {"jian", "剑", 1151704, CandidateSource::Database, "jian"},
        {"jian", "积案", 9420, CandidateSource::Database, "ji'an"},
        {"jian", "几案", 5999, CandidateSource::Database, "ji'an"},
        {"jian", "吉安", 1, CandidateSource::Database, "ji'an"},
    };

    bool ranking_changed = false;
    REQUIRE(user_dictionary::adjust_candidate_ranking(test::Utf8(main_path), test::Utf8(user_path), "jian", candidates,
                                                      "ji'an", "吉安", "pin", 1, 1, true, &ranking_changed));
    REQUIRE(ranking_changed);
    {
        TestDatabase db(main_path);
        // 基准是整个列表最重的 见，不再是 ji'an 这一组的头部 积案。
        REQUIRE(db.scalar_int64("SELECT weight FROM tbl_2_j WHERE value='吉安'") > 3460998);
        // 写入仍只落在 entry_key 的行上：jian 那张表一行都没动（#36 的护栏）。
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_1_j WHERE value='见'"), 3460998);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_1_j WHERE value='间'"), 3067939);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_2_j WHERE value='积案'"), 9420);
    }
    std::filesystem::remove_all(directory);
}

// 回归：不置顶、按 promote 模式连着选几次，也要能把吉安一路顶到首位。每一轮都照着
// 库里的新权重重排候选列表，模拟用户重新打一遍 jian 看到的顺序。
TEST_CASE(UserDictionaryRepeatedPromotionWalksAnAlternativeSegmentationToTheTop)
{
    const auto directory = test::TempRoot() / ("msime-cross-key-walk-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    const auto main_path = directory / "msime.db";
    {
        TestDatabase db(main_path);
        db.exec("CREATE TABLE tbl_1_j(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                "CREATE TABLE tbl_2_j(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                "INSERT INTO tbl_1_j VALUES("
                "'jian','j','见',3460998),"
                "('jian','j','间',3067939),"
                "('jian','j','剑',1151704),"
                "('jian','j','件',935235),"
                "('jian','j','建',598616);"
                "INSERT INTO tbl_2_j VALUES("
                "'ji''an','ja','积案',9420),"
                "('ji''an','ja','几案',5999),"
                "('ji''an','ja','吉安',1);");
    }
    const std::vector<std::pair<std::string, std::string>> rows = {
        {"jian", "见"}, {"jian", "间"},    {"jian", "剑"},    {"jian", "件"},
        {"jian", "建"}, {"ji'an", "积案"}, {"ji'an", "几案"}, {"ji'an", "吉安"},
    };
    const auto current_list = [&]() {
        TestDatabase db(main_path);
        std::vector<WordItem> list;
        for (const auto &row : rows)
        {
            const std::string table = row.first == "jian" ? "tbl_1_j" : "tbl_2_j";
            const auto sql = "SELECT weight FROM " + table + " WHERE value='" + row.second + "'";
            list.push_back({"jian", row.second, db.scalar_int64(sql.c_str()), CandidateSource::Database, row.first});
        }
        std::stable_sort(list.begin(), list.end(),
                         [](const WordItem &lhs, const WordItem &rhs) { return lhs.weight > rhs.weight; });
        return list;
    };
    const auto rank_of = [](const std::vector<WordItem> &list, const std::string &word) {
        for (std::size_t i = 0; i < list.size(); ++i)
            if (list[i].word == word)
                return i;
        return list.size();
    };

    // promote 模式一次最多前进到第 5 位，所以从第 8 位走到首位需要几轮。上限给 8 轮，
    // 断言的是「会收敛」，不是精确的轮数。
    std::size_t rounds = 0;
    while (rank_of(current_list(), "吉安") != 0 && rounds < 8)
    {
        const auto list = current_list();
        REQUIRE(user_dictionary::adjust_candidate_ranking(test::Utf8(main_path), test::Utf8(user_path), "jian", list,
                                                          "ji'an", "吉安", "promote", 1, 1, false));
        ++rounds;
    }
    REQUIRE_EQ(rank_of(current_list(), "吉安"), static_cast<std::size_t>(0));
    {
        TestDatabase db(main_path);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_1_j WHERE value='见'"), 3460998);
    }
    std::filesystem::remove_all(directory);
}

TEST_CASE(UserDictionaryEqualWeightPromoteDoesNotWriteNegatives)
{
    const auto directory = test::TempRoot() / ("msime-equal-weight-ranking-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    const auto main_path = directory / "msime.db";
    {
        TestDatabase db(main_path);
        db.exec("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                "INSERT INTO tbl_1_n VALUES('ni','n','甲',1),"
                "('ni','n','乙',1),"
                "('ni','n','丙',1),"
                "('ni','n','丁',1),"
                "('ni','n','戊',1),"
                "('ni','n','己',1);");
    }
    std::vector<WordItem> candidates = {
        {"ni", "甲", 1}, {"ni", "乙", 1}, {"ni", "丙", 1}, {"ni", "丁", 1}, {"ni", "戊", 1}, {"ni", "己", 1},
    };

    REQUIRE(user_dictionary::adjust_candidate_ranking(test::Utf8(main_path), test::Utf8(user_path), "ni", candidates,
                                                      "ni", "己", "promote", 1, 1, false));
    {
        TestDatabase db(main_path);
        REQUIRE(db.scalar_int64("SELECT weight FROM tbl_1_n WHERE value='己'") > 1);
        REQUIRE_EQ(db.scalar_int("SELECT COUNT(*) FROM tbl_1_n WHERE weight < 1"), 0);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_1_n WHERE value='甲'"), 1);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_1_n WHERE value='乙'"), 1);
    }
    {
        TestDatabase user_db(user_path);
        REQUIRE_EQ(user_db.scalar_int("SELECT COUNT(*) FROM user_dictionary_operations"), 1);
        REQUIRE_EQ(user_db.scalar_int("SELECT COUNT(*) FROM user_dictionary_operations WHERE value='己'"), 1);
    }
    std::filesystem::remove_all(directory);
}

TEST_CASE(UserDictionaryReplaySkipsNonpositivePinyinUpserts)
{
    const auto directory = test::TempRoot() / ("msime-skip-negative-replay-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    const auto main_path = directory / "msime.db";
    const auto english_path = directory / "english.db";
    {
        TestDatabase main_db(main_path);
        main_db.exec("CREATE TABLE tbl_1_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                     "INSERT INTO tbl_1_x VALUES('xian','x','先',1662684);");
        TestDatabase english_db(english_path);
        english_db.exec("CREATE TABLE english_words(word TEXT PRIMARY KEY,display TEXT);");
    }

    REQUIRE(user_dictionary::record_upsert(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin, "xian", "先",
                                           -12999));
    REQUIRE(user_dictionary::record_upsert(test::Utf8(user_path), user_dictionary::DictionaryKind::Pinyin, "xian", "现",
                                           5000));

    const auto replay = user_dictionary::replay(test::Utf8(user_path), test::Utf8(main_path), test::Utf8(english_path));
    REQUIRE(replay.error.empty());
    REQUIRE_EQ(replay.failed, 0);
    REQUIRE_EQ(replay.skipped, 1);
    REQUIRE_EQ(replay.applied, 1);
    {
        TestDatabase db(main_path);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_1_x WHERE value='先'"), 1662684);
        REQUIRE_EQ(db.scalar_int("SELECT COUNT(*) FROM tbl_1_x WHERE value='现' AND weight=5000"), 1);
    }

    REQUIRE(user_dictionary::ensure_user_database(test::Utf8(user_path)));
    {
        TestDatabase user_db(user_path);
        REQUIRE_EQ(user_db.scalar_int("SELECT COUNT(*) FROM user_dictionary_operations WHERE value='先'"), 0);
        REQUIRE_EQ(user_db.scalar_int("SELECT COUNT(*) FROM user_dictionary_operations WHERE value='现'"), 1);
    }
    std::filesystem::remove_all(directory);
}

// #869：旧版日文模式调频时，日文候选以罗马音为键进了拼音日志；msime.db 里没有那些行，
// UPDATE 改不到任何东西，但日志照记，升级回放时 apply_pinyin 就把日文词插进了拼音表。
TEST_CASE(UserDictionaryRankingDoesNotJournalRowsMissingFromTheDictionary)
{
    const auto directory = test::TempRoot() / ("msime-ranking-missing-rows-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    const auto main_path = directory / "msime.db";
    {
        TestDatabase db(main_path);
        db.exec("CREATE TABLE tbl_1_p(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                "INSERT INTO tbl_1_p VALUES('pan','p','盘',5000);");
    }
    std::vector<WordItem> candidates = {
        {"pan", "パン", 980000, CandidateSource::Database, "pan"},
        {"pan", "パン粉", 970000, CandidateSource::Database, "pan"},
    };
    REQUIRE(user_dictionary::adjust_candidate_ranking(test::Utf8(main_path), test::Utf8(user_path), "pan", candidates,
                                                      "pan", "パン粉", "promote", 1, 1, false));
    {
        TestDatabase db(main_path);
        REQUIRE_EQ(db.scalar_int("SELECT COUNT(*) FROM tbl_1_p"), 1);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_1_p WHERE value='盘'"), 5000);
    }
    {
        TestDatabase user_db(user_path);
        REQUIRE_EQ(user_db.scalar_int("SELECT COUNT(*) FROM user_dictionary_operations"), 0);
    }
    std::filesystem::remove_all(directory);
}

// 固定位置作用在已展开的列表上（混输英文回来后重新应用固定位置）：真实的 apply_fixed_positions 经
// ReorderAsOneItemPerWord 只看到每组一项，固定词的每种写法都留着，插回的位置按组算，不切开别的组。
TEST_CASE(UserDictionaryFixedPositionsOnAnExpandedListKeepEverySpellingGroupWhole)
{
    const ScopedTempDirectory directory("msime-fixed-positions-spellings-");
    const std::string user_path = test::Utf8(directory.path() / "msime_user.db");
    const test::FakeTraditionalSpellingIndex index({{"台", {"臺", "台", "檯", "颱"}}, {"为", {"為", "爲"}}});
    const auto expand = [&](std::vector<WordItem> items) {
        return FanyImeIpc::ExpandTraditionalVariants(std::move(items), index, FanyImeIpc::TraditionalRegion::Taiwan);
    };
    const auto apply_fixed = [&](std::vector<WordItem> &items) {
        user_dictionary::apply_fixed_positions(user_path, "ni", items, false);
    };
    const auto display = [](const std::vector<WordItem> &items) {
        std::vector<std::string> texts;
        for (const auto &item : items)
            texts.push_back(item.output_text);
        return texts;
    };

    REQUIRE(user_dictionary::set_fixed_position(user_path, "ni", "ni", "为", 1));
    const auto to_front = FanyImeIpc::ReorderAsOneItemPerWord(
        expand({{"ni", "你", 9}, {"tai", "台", 8}, {"wei", "为", 7}}), apply_fixed, expand);
    REQUIRE_EQ(display(to_front), (std::vector<std::string>{"為", "爲", "你", "臺", "台", "檯", "颱"}));
    REQUIRE_EQ(to_front[0].fixed_position, 1);
    REQUIRE_EQ(to_front[1].fixed_position, 1);

    REQUIRE(user_dictionary::set_fixed_position(user_path, "ni", "ni", "为", 3));
    const auto third = FanyImeIpc::ReorderAsOneItemPerWord(
        expand({{"ni", "你", 9}, {"wei", "为", 8}, {"tai", "台", 7}}), apply_fixed, expand);
    REQUIRE_EQ(display(third), (std::vector<std::string>{"你", "臺", "台", "檯", "颱", "為", "爲"}));
}

// 已经写进日志的那些残留：升级回放要跳过，打开日志时清掉；用户在设置页手动加的带假名词不能动。
TEST_CASE(UserDictionaryReplaySkipsLearnedKanaPinyinUpserts)
{
    const auto directory = test::TempRoot() / ("msime-skip-kana-replay-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    const auto main_path = directory / "msime.db";
    const auto english_path = directory / "english.db";
    {
        TestDatabase main_db(main_path);
        main_db.exec("CREATE TABLE tbl_1_p(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                     "CREATE TABLE tbl_1_a(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                     "CREATE TABLE tbl_1_o(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                     "CREATE TABLE tbl_1_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                     "INSERT INTO tbl_1_x VALUES('xian','x','现',1);");
        TestDatabase english_db(english_path);
        english_db.exec("CREATE TABLE english_words(word TEXT PRIMARY KEY,display TEXT);");
    }

    const auto user = test::Utf8(user_path);
    REQUIRE(user_dictionary::record_upsert(user, user_dictionary::DictionaryKind::Pinyin, "pan", "パン", 980000));
    REQUIRE(user_dictionary::record_upsert(user, user_dictionary::DictionaryKind::Pinyin, "amarini", "余りに", 900000));
    REQUIRE(
        user_dictionary::record_upsert(user, user_dictionary::DictionaryKind::Pinyin, "omoimasu", "思います", 900000));
    REQUIRE(user_dictionary::record_upsert(user, user_dictionary::DictionaryKind::Pinyin, "xian", "现", 5000));
    REQUIRE(user_dictionary::record_user_insert(user, user_dictionary::DictionaryKind::Pinyin, "pen", "ペン", 100));

    const auto replay = user_dictionary::replay(user, test::Utf8(main_path), test::Utf8(english_path));
    REQUIRE(replay.error.empty());
    REQUIRE_EQ(replay.failed, 0);
    REQUIRE_EQ(replay.skipped, 3);
    REQUIRE_EQ(replay.applied, 2);
    {
        TestDatabase db(main_path);
        REQUIRE_EQ(db.scalar_int("SELECT COUNT(*) FROM tbl_1_p WHERE value='パン'"), 0);
        REQUIRE_EQ(db.scalar_int("SELECT COUNT(*) FROM tbl_1_a"), 0);
        REQUIRE_EQ(db.scalar_int("SELECT COUNT(*) FROM tbl_1_o"), 0);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_1_x WHERE value='现'"), 5000);
        REQUIRE_EQ(db.scalar_int("SELECT COUNT(*) FROM tbl_1_p WHERE value='ペン'"), 1);
    }

    REQUIRE(user_dictionary::ensure_user_database(user));
    {
        TestDatabase user_db(user_path);
        REQUIRE_EQ(user_db.scalar_int("SELECT COUNT(*) FROM user_dictionary_operations WHERE user_inserted=0 AND "
                                      "value IN ('パン','余りに','思います')"),
                   0);
        REQUIRE_EQ(user_db.scalar_int("SELECT COUNT(*) FROM user_dictionary_operations WHERE value IN ('现','ペン')"),
                   2);
    }
    std::filesystem::remove_all(directory);
}

TEST_CASE(UserDictionaryLocalRebalanceKeepsPositiveSameKeyWeights)
{
    const auto directory = test::TempRoot() / ("msime-local-rebalance-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    const auto main_path = directory / "msime.db";
    {
        TestDatabase db(main_path);
        db.exec("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                "INSERT INTO tbl_1_n VALUES('ni','n','甲',20000),"
                "('ni','n','乙',19000),"
                "('ni','n','丙',18000),"
                "('ni','n','丁',17000),"
                "('ni','n','戊',16000),"
                "('ni','n','己',15000);");
    }
    std::vector<WordItem> candidates = {
        {"ni", "甲", 20000}, {"ni", "乙", 20000}, {"ni", "丙", 20000},
        {"ni", "丁", 20000}, {"ni", "戊", 20000}, {"ni", "己", 20000},
    };
    REQUIRE(user_dictionary::adjust_candidate_ranking(test::Utf8(main_path), test::Utf8(user_path), "ni", candidates,
                                                      "ni", "己", "promote", 1, 1, false));
    {
        TestDatabase db(main_path);
        REQUIRE_EQ(db.scalar_int("SELECT COUNT(*) FROM tbl_1_n WHERE weight < 1"), 0);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_1_n WHERE value='甲'"), 20000);
        REQUIRE(db.scalar_int64("SELECT weight FROM tbl_1_n WHERE value='己'") >
                db.scalar_int64("SELECT weight FROM tbl_1_n WHERE value='戊'"));
        REQUIRE(db.scalar_int64("SELECT weight FROM tbl_1_n WHERE value='己'") < 20000);
    }
    std::filesystem::remove_all(directory);
}

// 候选没带 canonical_pinyin 时，键要从 context_key 按字数截出来（先 -> xian，
// 现网 -> xian'wang）。较短的那个键现在也参与比权，但写入必须只落在 entry_key 的行上。
TEST_CASE(UserDictionaryRankingKeepsWritesOnTheEntryKeyWhenKeysAreReconstructed)
{
    const auto directory =
        test::TempRoot() / ("msime-reconstructed-key-ranking-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    const auto main_path = directory / "msime.db";
    {
        TestDatabase db(main_path);
        db.exec("CREATE TABLE tbl_2_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                "CREATE TABLE tbl_1_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                "INSERT INTO tbl_2_x VALUES('xian''wang','xw','现网',1),('xian''wang','xw','先亡',1);"
                "INSERT INTO tbl_1_x VALUES('xian','x','先',1662684);");
    }
    std::vector<WordItem> candidates = {
        {"xianwang", "先亡", 1},
        {"xianwang", "现网", 1},
        {"xianwang", "先", 1662684},
    };
    REQUIRE(user_dictionary::adjust_candidate_ranking(test::Utf8(main_path), test::Utf8(user_path), "xian'wang",
                                                      candidates, "xian'wang", "现网", "promote", 1, 1, false));
    {
        TestDatabase db(main_path);
        REQUIRE(db.scalar_int64("SELECT weight FROM tbl_2_x WHERE value='现网'") > 1);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_1_x WHERE value='先'"), 1662684);
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_2_x WHERE value='先亡'"), 1);
    }
    std::filesystem::remove_all(directory);
}

TEST_CASE(UserDictionaryWubiPromoteUpdatesWubiTable)
{
    const auto directory = test::TempRoot() / ("msime-wubi-ranking-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto user_path = directory / "msime_user.db";
    const auto main_path = directory / "msime.db";
    {
        TestDatabase db(main_path);
        db.exec("CREATE TABLE wubi86(key TEXT,value TEXT,weight INTEGER);"
                "CREATE TABLE tbl_1_a(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                "INSERT INTO wubi86 VALUES('aaaa','工',100),('aaaa','或',50);"
                "INSERT INTO tbl_1_a VALUES('aaaa','a','啊',1);");
    }
    std::vector<WordItem> candidates = {
        {"aaaa", "工", 100},
        {"aaaa", "或", 50},
    };
    bool ranking_changed = false;
    REQUIRE(user_dictionary::adjust_candidate_ranking(test::Utf8(main_path), test::Utf8(user_path), "aaaa", candidates,
                                                      "aaaa", "或", "promote", 1, 1, false, &ranking_changed,
                                                      user_dictionary::DictionaryKind::Wubi));
    REQUIRE(ranking_changed);
    {
        TestDatabase db(main_path);
        REQUIRE(db.scalar_int64("SELECT weight FROM wubi86 WHERE value='或'") >
                db.scalar_int64("SELECT weight FROM wubi86 WHERE value='工'"));
        REQUIRE_EQ(db.scalar_int64("SELECT weight FROM tbl_1_a WHERE value='啊'"), 1);
    }
    {
        TestDatabase user_db(user_path);
        REQUIRE_EQ(user_db.scalar_int(
                       "SELECT COUNT(*) FROM user_dictionary_operations WHERE dictionary='wubi' AND value='或'"),
                   1);
        REQUIRE_EQ(user_db.scalar_int("SELECT COUNT(*) FROM user_dictionary_operations WHERE dictionary='pinyin'"), 0);
    }
    std::filesystem::remove_all(directory);
}

// 日期时间格式按格式 ID 调频：日期文本每天都变，学到的顺序和固定位置都不能按文字记。
TEST_CASE(DateTimeFormatOrderIsLearnedPinnedAndFixedByFormatId)
{
    const auto directory = test::TempRoot() / ("msime-date-time-order-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const std::string user_path = test::Utf8(directory / "msime_user.db");
    const metasequoia::local_modes::LocalDateTime now{2026, 8, 9, 0, 14, 30, 0};
    const auto default_ids = metasequoia::local_modes::date_time_format_ids("rq");
    const auto ordered = [&](const char *keyword, bool learned_order = true) {
        auto items = metasequoia::local_modes::query_date_time(keyword, &now);
        user_dictionary::apply_date_time_order(user_path, metasequoia::local_modes::date_time_category(keyword), items,
                                               learned_order);
        return items;
    };
    // REQUIRE_EQ 按引用接住参数，取首位 ID 要按值返回，不能引用临时列表里的元素。
    const auto first = [&](const char *keyword, bool learned_order = true) {
        return ordered(keyword, learned_order)[0].pinyin;
    };
    const auto index_of = [](const std::vector<WordItem> &items, const std::string &format_id) {
        return static_cast<size_t>(
            std::find_if(items.begin(), items.end(), [&](const WordItem &item) { return item.pinyin == format_id; }) -
            items.begin());
    };

    REQUIRE_EQ(first("rq"), std::string("date:ymd_cn"));

    // 已在首位的格式没有可学的；第二位的按 pin 模式一次到顶。
    REQUIRE(user_dictionary::learn_date_time_selection(user_path, "date", default_ids, "date:ymd_cn", "pin", 1, 1));
    REQUIRE_EQ(first("rq"), std::string("date:ymd_cn"));
    REQUIRE(user_dictionary::learn_date_time_selection(user_path, "date", default_ids, "date:ymd_dash", "pin", 1, 1));
    auto items = ordered("rq");
    REQUIRE_EQ(items[0].pinyin, std::string("date:ymd_dash"));
    REQUIRE_EQ(items[0].word, std::string("2026-08-09"));
    REQUIRE_EQ(items[1].pinyin, std::string("date:ymd_cn"));
    // rq / riqi / date 共用一份顺序，时间那组不受影响。
    REQUIRE_EQ(first("riqi"), std::string("date:ymd_dash"));
    REQUIRE_EQ(first("sj"), std::string("time:hm"));

    // 计数到 trigger_count 才挪，linear 每次前移 linear_step 位。
    const size_t lunar = index_of(ordered("rq"), "date:lunar");
    REQUIRE(user_dictionary::learn_date_time_selection(user_path, "date", default_ids, "date:lunar", "linear", 3, 2));
    REQUIRE_EQ(index_of(ordered("rq"), "date:lunar"), lunar);
    REQUIRE(user_dictionary::learn_date_time_selection(user_path, "date", default_ids, "date:lunar", "linear", 3, 2));
    REQUIRE_EQ(index_of(ordered("rq"), "date:lunar"), lunar - 3);

    // 调频关闭时不学，也不用学到的顺序。
    REQUIRE(user_dictionary::learn_date_time_selection(user_path, "date", default_ids, "date:md_cn", "disabled", 1, 1));
    REQUIRE_EQ(first("rq"), std::string("date:ymd_dash"));
    REQUIRE_EQ(first("rq", false), std::string("date:ymd_cn"));

    // 置顶不计数。
    REQUIRE(user_dictionary::pin_date_time_format(user_path, "date", default_ids, "date:md_cn"));
    REQUIRE_EQ(first("rq"), std::string("date:md_cn"));
    // Shift+T 后面写的指定日期沿用同一组格式 ID，学到的顺序跟着走；只写年月时只有年月几种，不受影响。
    const auto christmas = ordered("20241225");
    REQUIRE_EQ(christmas[0].pinyin, std::string("date:md_cn"));
    REQUIRE_EQ(christmas[0].word, std::string("12月25日"));
    REQUIRE_EQ(first("2024/12"), std::string("date:ym_cn"));

    // 固定位置按格式 ID 摆，压过学到的顺序；取消后回到学到的顺序。
    const std::string context = user_dictionary::date_time_fixed_position_context("date");
    REQUIRE(user_dictionary::set_fixed_position(user_path, context, "date:ymd_slash", "date:ymd_slash", 1));
    items = ordered("rq");
    REQUIRE_EQ(items[0].pinyin, std::string("date:ymd_slash"));
    REQUIRE_EQ(items[0].fixed_position, 1);
    REQUIRE_EQ(items[1].pinyin, std::string("date:md_cn"));
    // 固定位置只挪位置，不多不少。格式表里还有唤醒词给不出的年月几种，所以拿未排序的结果比。
    REQUIRE_EQ(items.size(), metasequoia::local_modes::query_date_time("rq", &now).size());
    REQUIRE(user_dictionary::clear_fixed_position(user_path, context, "date:ymd_slash", "date:ymd_slash"));
    REQUIRE_EQ(first("rq"), std::string("date:md_cn"));
    items = ordered("rq");
    REQUIRE_EQ(items[0].fixed_position, 0);
    std::filesystem::remove_all(directory);
}