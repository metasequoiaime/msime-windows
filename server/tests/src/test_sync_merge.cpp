#include "settings/sync_merge.h"
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"
#include "engine/user_dictionary/user_dictionary_journal.h"

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// 合并语义的离线验证。真实导入流程（停 Server、落位、回放）由
// test_user_data_backup.cpp 覆盖，这里只测「两条数据怎么合成一条」。

namespace
{
namespace fs = std::filesystem;

fs::path TempDir(const char *name)
{
    const fs::path root = test::TempRoot() / "user-data-sync" / name;
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root, error);
    return root;
}

// 造一个最小可用的 msime_user.db（带日志表）。
void CreateUserDatabase(const fs::path &path, const std::vector<std::string> &inserts)
{
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(test::Utf8(path).c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db,
                         "CREATE TABLE user_dictionary_operations(dictionary TEXT NOT NULL,key TEXT NOT NULL,"
                         "value TEXT NOT NULL,operation TEXT NOT NULL,weight INTEGER NOT NULL DEFAULT 0,"
                         "display TEXT NOT NULL DEFAULT '',user_inserted INTEGER NOT NULL DEFAULT 0,"
                         "weight_edited INTEGER NOT NULL DEFAULT 0,updated_at INTEGER NOT NULL DEFAULT 0,"
                         "PRIMARY KEY(dictionary,key,value));",
                         nullptr, nullptr, nullptr) == SQLITE_OK);
    for (const std::string &sql : inserts)
        REQUIRE(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
}

int UserDatabaseRowCount(const fs::path &path)
{
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(test::Utf8(path).c_str(), &db) == SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM user_dictionary_operations", -1, &statement, nullptr) ==
            SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    const int count = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return count;
}
} // namespace

// 凭据永远不能进同步归档：远端那份是别的账户的 DPAPI 密文，传上去既解不开又多一份泄露面。
TEST_CASE(strip_credentials_blanks_only_credential_values)
{
    const std::string text = "[ai_assistant]\n"
                             "endpoint = \"https://api.example.com\"\n"
                             "token = \"dpapi:AAAA\"\n"
                             "token_deepseek = \"dpapi:BBBB\"\n"
                             "\n"
                             "[voice_input]\n"
                             "asr_token = \"dpapi:CCCC\"\n"
                             "language = \"zh-cn\"\n";
    const std::string stripped = user_data_sync::StripCredentials(text);
    REQUIRE(stripped.find("dpapi:AAAA") == std::string::npos);
    REQUIRE(stripped.find("dpapi:BBBB") == std::string::npos);
    REQUIRE(stripped.find("dpapi:CCCC") == std::string::npos);
    REQUIRE(stripped.find("https://api.example.com") != std::string::npos);
    REQUIRE(stripped.find("zh-cn") != std::string::npos);
    // 保留行结构与节名，不能被整段重排。
    REQUIRE(stripped.find("[ai_assistant]") != std::string::npos);
    REQUIRE(stripped.find("[voice_input]") != std::string::npos);
}

// 远端仍是出厂默认的键不该覆盖本地：对方没动过这一项，拿它盖掉本机选择没有道理。
TEST_CASE(merge_config_only_takes_values_the_remote_actually_changed)
{
    const std::string template_text = "[appearance]\n"
                                      "theme_mode = \"system\"\n"
                                      "page_size = 8\n";
    const std::string local_text = "[appearance]\n"
                                   "theme_mode = \"dark\"\n"
                                   "page_size = 8\n";
    const std::string remote_text = "[appearance]\n"
                                    "theme_mode = \"system\"\n"
                                    "page_size = 9\n";
    const std::string merged = user_data_sync::MergeConfig(local_text, remote_text, template_text);
    REQUIRE(merged.find("theme_mode = \"dark\"") != std::string::npos);
    REQUIRE(merged.find("page_size = 9") != std::string::npos);
}

TEST_CASE(merge_config_never_imports_credentials)
{
    const std::string template_text = "[ai_assistant]\n"
                                      "token = \"\"\n"
                                      "model = \"deepseek-chat\"\n";
    const std::string local_text = "[ai_assistant]\n"
                                   "token = \"dpapi:LOCAL\"\n"
                                   "model = \"deepseek-chat\"\n";
    const std::string remote_text = "[ai_assistant]\n"
                                    "token = \"dpapi:REMOTE\"\n"
                                    "model = \"deepseek-reasoner\"\n";
    const std::string merged = user_data_sync::MergeConfig(local_text, remote_text, template_text);
    REQUIRE(merged.find("dpapi:LOCAL") != std::string::npos);
    REQUIRE(merged.find("dpapi:REMOTE") == std::string::npos);
    REQUIRE(merged.find("deepseek-reasoner") != std::string::npos);
}

TEST_CASE(merge_config_returns_local_when_remote_is_unparseable)
{
    const std::string local_text = "[appearance]\ntheme_mode = \"dark\"\n";
    const std::string merged = user_data_sync::MergeConfig(local_text, "this is not toml = = =", "");
    REQUIRE_EQ(merged, local_text);
}

// 没有出厂模板时也要能合并：空模板表示「没有基线」，只跳过默认值判断，不能整段放弃。
TEST_CASE(merge_config_with_empty_template_still_takes_remote_changes)
{
    const std::string local_text = "[appearance]\ntheme_mode = \"dark\"\n";
    const std::string remote_text = "[appearance]\ntheme_mode = \"light\"\n";
    const std::string merged = user_data_sync::MergeConfig(local_text, remote_text, "");
    REQUIRE(merged.find("theme_mode = \"light\"") != std::string::npos);
}

// 用户词库合并的核心语义：较新者胜出、删除墓碑保留。
TEST_CASE(merge_user_dictionary_takes_newer_rows_and_keeps_tombstones)
{
    const fs::path root = TempDir("merge-dictionary");
    const fs::path local_db = root / "local.db";
    const fs::path remote_db = root / "remote.db";
    std::string message;

    // 本地：ni'hao 权重 10，更新于 100。远端：ni'hao 权重 20，更新于 200（较新，应胜出）；
    // 另有一条本地没有的删除墓碑 shi'jie（应被合并进来，否则同步会把删掉的词复活）。
    CreateUserDatabase(
        local_db, {"INSERT INTO user_dictionary_operations VALUES('pinyin','ni''hao','你好','upsert',10,'',1,0,100)"});
    CreateUserDatabase(
        remote_db, {"INSERT INTO user_dictionary_operations VALUES('pinyin','ni''hao','你好','upsert',20,'',1,0,200)",
                    "INSERT INTO user_dictionary_operations VALUES('pinyin','shi''jie','世界','delete',0,'',0,0,150)"});

    user_data_sync::DictionaryMergeReport report;
    REQUIRE(user_data_sync::MergeUserDictionary(local_db, remote_db, report, message));
    REQUIRE_EQ(report.added, 1);
    REQUIRE_EQ(report.taken_from_remote, 1);
    REQUIRE_EQ(report.kept_local, 0);
    REQUIRE_EQ(UserDatabaseRowCount(local_db), 2);

    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(test::Utf8(local_db).c_str(), &db) == SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "SELECT weight FROM user_dictionary_operations WHERE key='ni''hao'", -1, &statement,
                               nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    REQUIRE_EQ(sqlite3_column_int(statement, 0), 20);
    sqlite3_finalize(statement);
    REQUIRE(sqlite3_prepare_v2(db, "SELECT operation FROM user_dictionary_operations WHERE key='shi''jie'", -1,
                               &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    REQUIRE_EQ(std::string(reinterpret_cast<const char *>(sqlite3_column_text(statement, 0))), std::string("delete"));
    sqlite3_finalize(statement);
    sqlite3_close(db);
}

TEST_CASE(merge_user_dictionary_preserves_temporary_state_and_permanent_confirmation)
{
    const fs::path root = TempDir("merge-dictionary-learning-state");
    const fs::path local_db = root / "local.db";
    const fs::path remote_db = root / "remote.db";
    CreateUserDatabase(local_db, {});
    CreateUserDatabase(remote_db, {});
    for (const auto &path : {local_db, remote_db})
    {
        sqlite3 *db = nullptr;
        REQUIRE(sqlite3_open(test::Utf8(path).c_str(), &db) == SQLITE_OK);
        REQUIRE(sqlite3_exec(
                    db,
                    "ALTER TABLE user_dictionary_operations ADD COLUMN learning_count INTEGER NOT NULL DEFAULT 0;"
                    "ALTER TABLE user_dictionary_operations ADD COLUMN last_learned_at INTEGER NOT NULL DEFAULT 0;",
                    nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }
    {
        sqlite3 *db = nullptr;
        REQUIRE(sqlite3_open(test::Utf8(local_db).c_str(), &db) == SQLITE_OK);
        REQUIRE(sqlite3_exec(db,
                             "INSERT INTO user_dictionary_operations(dictionary,key,value,operation,weight,display,"
                             "user_inserted,weight_edited,learning_count,last_learned_at,updated_at)"
                             " VALUES('pinyin','yi','一','upsert',10000,'',1,0,0,0,100),"
                             "('pinyin','hao','好','upsert',1000,'',1,0,1,500,500)",
                             nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }
    {
        sqlite3 *db = nullptr;
        REQUIRE(sqlite3_open(test::Utf8(remote_db).c_str(), &db) == SQLITE_OK);
        REQUIRE(sqlite3_exec(db,
                             "INSERT INTO user_dictionary_operations(dictionary,key,value,operation,weight,display,"
                             "user_inserted,weight_edited,learning_count,last_learned_at,updated_at) VALUES"
                             "('pinyin','yi','一','upsert',1000,'',1,0,1,50,200),"
                             "('pinyin','ni','你','upsert',1000,'',1,0,1,300,300),"
                             "('pinyin','hao','好','upsert',10000,'',1,0,0,0,100)",
                             nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }
    std::string message;
    user_data_sync::DictionaryMergeReport report;
    REQUIRE(user_data_sync::MergeUserDictionary(local_db, remote_db, report, message));
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(test::Utf8(local_db).c_str(), &db) == SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(db,
                               "SELECT learning_count,last_learned_at FROM user_dictionary_operations"
                               " WHERE key='yi'",
                               -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    REQUIRE_EQ(sqlite3_column_int(statement, 0), 0);
    REQUIRE_EQ(sqlite3_column_int64(statement, 1), 0);
    sqlite3_finalize(statement);
    REQUIRE(sqlite3_prepare_v2(db,
                               "SELECT learning_count,last_learned_at FROM user_dictionary_operations"
                               " WHERE key='ni'",
                               -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    REQUIRE_EQ(sqlite3_column_int(statement, 0), 1);
    REQUIRE_EQ(sqlite3_column_int64(statement, 1), 300);
    sqlite3_finalize(statement);
    // 远端较旧的正式行盖过本地较新的临时行：状态取正式，但本地时间戳不能被拨回去。
    REQUIRE(sqlite3_prepare_v2(db,
                               "SELECT learning_count,weight,updated_at FROM user_dictionary_operations"
                               " WHERE key='hao'",
                               -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    REQUIRE_EQ(sqlite3_column_int(statement, 0), 0);
    REQUIRE_EQ(sqlite3_column_int64(statement, 1), 10000);
    REQUIRE_EQ(sqlite3_column_int64(statement, 2), 500);
    sqlite3_finalize(statement);
    sqlite3_close(db);
}

// 本地更新的行不能被远端旧数据覆盖。
TEST_CASE(merge_user_dictionary_keeps_local_when_local_is_newer)
{
    const fs::path root = TempDir("merge-dictionary-newer-local");
    const fs::path local_db = root / "local.db";
    const fs::path remote_db = root / "remote.db";
    std::string message;

    CreateUserDatabase(
        local_db, {"INSERT INTO user_dictionary_operations VALUES('pinyin','ni''hao','你好','upsert',30,'',1,0,500)"});
    CreateUserDatabase(
        remote_db, {"INSERT INTO user_dictionary_operations VALUES('pinyin','ni''hao','你好','upsert',20,'',1,0,200)"});

    user_data_sync::DictionaryMergeReport report;
    REQUIRE(user_data_sync::MergeUserDictionary(local_db, remote_db, report, message));
    REQUIRE_EQ(report.taken_from_remote, 0);
    REQUIRE_EQ(report.kept_local, 1);

    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(test::Utf8(local_db).c_str(), &db) == SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "SELECT weight FROM user_dictionary_operations WHERE key='ni''hao'", -1, &statement,
                               nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    REQUIRE_EQ(sqlite3_column_int(statement, 0), 30);
    sqlite3_finalize(statement);
    sqlite3_close(db);
}

namespace
{
std::int64_t QueryInt(const fs::path &path, const char *sql)
{
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(test::Utf8(path).c_str(), &db) == SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    const std::int64_t value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return value;
}

void Exec(const fs::path &path, const char *sql)
{
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(test::Utf8(path).c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
}

constexpr const char *kLatinTable = "CREATE TABLE user_latin_words(key TEXT NOT NULL,value TEXT NOT NULL,"
                                    "weight INTEGER NOT NULL DEFAULT 0,exact_only INTEGER NOT NULL DEFAULT 0,"
                                    "updated_at INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(key,value));";
constexpr const char *kStateTable = "CREATE TABLE dictionary_state(name TEXT PRIMARY KEY,value TEXT NOT NULL);";
} // namespace

// 种子词库：来源按 id 合并、较新者胜出；词条取并集；删除墓碑清掉两边的词条，再合并同一份旧归档也不复活。
// 种子词条不进个人日志。
TEST_CASE(merge_user_dictionary_syncs_seed_sources_and_entries)
{
    const fs::path root = TempDir("merge-seed-sources");
    const fs::path local_db = root / "local.db";
    const fs::path remote_db = root / "remote.db";
    CreateUserDatabase(local_db, {user_dictionary::kSeedJournalTablesSql,
                                  "INSERT INTO dictionary_sources VALUES('a','本地名','rime',1,0,1,100),"
                                  "('local-only','本地','rime',1,0,1,100);"
                                  "INSERT INTO seed_entries VALUES('a','pinyin','xin','本地词',10,0),"
                                  "('local-only','pinyin','ben','本',10,0);"});
    CreateUserDatabase(remote_db, {user_dictionary::kSeedJournalTablesSql,
                                   "INSERT INTO dictionary_sources VALUES('a','远端名','rime',0,0,1,200),"
                                   "('remote-only','远端','rime',1,0,1,150);"
                                   "INSERT INTO seed_entries VALUES('a','pinyin','xin','远端词',10,0),"
                                   "('a','latin','a','A股',5,0),('remote-only','pinyin','yuan','远',10,0);"});

    user_data_sync::DictionaryMergeReport report;
    std::string message;
    REQUIRE(user_data_sync::MergeUserDictionary(local_db, remote_db, report, message));
    REQUIRE_EQ(QueryInt(local_db, "SELECT COUNT(*) FROM dictionary_sources"), 3);
    REQUIRE_EQ(QueryInt(local_db, "SELECT enabled FROM dictionary_sources WHERE id='a'"), 0);
    REQUIRE_EQ(QueryInt(local_db, "SELECT COUNT(*) FROM seed_entries WHERE source_id='a'"), 3);
    REQUIRE_EQ(QueryInt(local_db, "SELECT COUNT(*) FROM seed_entries"), 5);
    REQUIRE_EQ(QueryInt(local_db, "SELECT COUNT(*) FROM user_dictionary_operations"), 0);

    // 本机较新的删除：词条清掉，再合并同一份远端归档也不回来。
    Exec(local_db, "UPDATE dictionary_sources SET deleted=1,enabled=0,updated_at=300 WHERE id='a';"
                   "DELETE FROM seed_entries WHERE source_id='a'");
    REQUIRE(user_data_sync::MergeUserDictionary(local_db, remote_db, report, message));
    REQUIRE_EQ(QueryInt(local_db, "SELECT deleted FROM dictionary_sources WHERE id='a'"), 1);
    REQUIRE_EQ(QueryInt(local_db, "SELECT COUNT(*) FROM seed_entries WHERE source_id='a'"), 0);
}

// 同一秒两边都改过：删除优先、其次停用优先，两个方向合并结果一致。
TEST_CASE(merge_user_dictionary_seed_source_ties_converge)
{
    const fs::path root = TempDir("merge-seed-ties");
    const fs::path first = root / "first.db";
    const fs::path second = root / "second.db";
    const std::vector<std::string> first_rows = {user_dictionary::kSeedJournalTablesSql,
                                                 "INSERT INTO dictionary_sources VALUES"
                                                 "('del','删','rime',1,1,1,100),('off','停','rime',1,0,1,100);"
                                                 "INSERT INTO seed_entries VALUES('del','pinyin','shan','删',10,0);"};
    const std::vector<std::string> second_rows = {user_dictionary::kSeedJournalTablesSql,
                                                  "INSERT INTO dictionary_sources VALUES"
                                                  "('del','删','rime',1,0,1,100),('off','停','rime',0,0,1,100);"
                                                  "INSERT INTO seed_entries VALUES('del','pinyin','shan','删',10,0);"};
    for (const bool forward : {true, false})
    {
        std::error_code error;
        fs::remove(first, error);
        fs::remove(second, error);
        CreateUserDatabase(first, first_rows);
        CreateUserDatabase(second, second_rows);
        const fs::path &local = forward ? first : second;
        const fs::path &remote = forward ? second : first;
        user_data_sync::DictionaryMergeReport report;
        std::string message;
        REQUIRE(user_data_sync::MergeUserDictionary(local, remote, report, message));
        REQUIRE_EQ(QueryInt(local, "SELECT deleted FROM dictionary_sources WHERE id='del'"), 1);
        REQUIRE_EQ(QueryInt(local, "SELECT enabled FROM dictionary_sources WHERE id='off'"), 0);
        REQUIRE_EQ(QueryInt(local, "SELECT COUNT(*) FROM seed_entries"), 0);
    }
}

// 远端是还没有种子表的旧归档：本机的种子词库原样保留。
TEST_CASE(merge_user_dictionary_old_archive_keeps_local_seed_sources)
{
    const fs::path root = TempDir("merge-seed-old-remote");
    const fs::path local_db = root / "local.db";
    const fs::path remote_db = root / "remote.db";
    CreateUserDatabase(local_db, {user_dictionary::kSeedJournalTablesSql,
                                  "INSERT INTO dictionary_sources VALUES('a','来源','rime',1,0,1,100);"
                                  "INSERT INTO seed_entries VALUES('a','pinyin','xin','来源词',10,0);"});
    CreateUserDatabase(remote_db, {});
    user_data_sync::DictionaryMergeReport report;
    std::string message;
    REQUIRE(user_data_sync::MergeUserDictionary(local_db, remote_db, report, message));
    REQUIRE_EQ(QueryInt(local_db, "SELECT COUNT(*) FROM dictionary_sources"), 1);
    REQUIRE_EQ(QueryInt(local_db, "SELECT COUNT(*) FROM seed_entries"), 1);
}

// user_inserted 是粘性标记：远端较新的自动调频行（0）不能把本机手动加的词（1）降级，
// 否则带假名的手加词会在回放时被当成学来的假名拼音跳过并删掉。
TEST_CASE(merge_user_dictionary_keeps_user_inserted_sticky)
{
    const fs::path root = TempDir("merge-dictionary-user-inserted");
    const fs::path local_db = root / "local.db";
    const fs::path remote_db = root / "remote.db";
    std::string message;

    CreateUserDatabase(
        local_db, {"INSERT INTO user_dictionary_operations VALUES('pinyin','a''ri','アリ','upsert',10,'',1,0,100)"});
    CreateUserDatabase(
        remote_db, {"INSERT INTO user_dictionary_operations VALUES('pinyin','a''ri','アリ','upsert',50,'',0,0,200)"});

    user_data_sync::DictionaryMergeReport report;
    REQUIRE(user_data_sync::MergeUserDictionary(local_db, remote_db, report, message));
    REQUIRE_EQ(report.taken_from_remote, 1);
    // 权重取远端较新的值，user_inserted 保持本机的 1。
    REQUIRE_EQ(QueryInt(local_db, "SELECT weight FROM user_dictionary_operations WHERE key='a''ri'"), 50);
    REQUIRE_EQ(QueryInt(local_db, "SELECT user_inserted FROM user_dictionary_operations WHERE key='a''ri'"), 1);
}

// 字母词没有墓碑，按并集合并：远端有、本地没有的补进来，两边都有的取较新的。
TEST_CASE(merge_user_dictionary_merges_latin_words)
{
    const fs::path root = TempDir("merge-dictionary-latin");
    const fs::path local_db = root / "local.db";
    const fs::path remote_db = root / "remote.db";
    std::string message;

    CreateUserDatabase(local_db, {kLatinTable, "INSERT INTO user_latin_words VALUES('e a','鄂A',5,0,300)"});
    CreateUserDatabase(remote_db, {kLatinTable, "INSERT INTO user_latin_words VALUES('e a','鄂A',9,0,100)",
                                   "INSERT INTO user_latin_words VALUES('h b','沪B',7,1,100)"});

    user_data_sync::DictionaryMergeReport report;
    REQUIRE(user_data_sync::MergeUserDictionary(local_db, remote_db, report, message));
    REQUIRE_EQ(report.latin_words, 1);
    REQUIRE_EQ(QueryInt(local_db, "SELECT COUNT(*) FROM user_latin_words"), 2);
    // 本地那条更新，不被远端旧值覆盖。
    REQUIRE_EQ(QueryInt(local_db, "SELECT weight FROM user_latin_words WHERE key='e a'"), 5);
    REQUIRE_EQ(QueryInt(local_db, "SELECT exact_only FROM user_latin_words WHERE key='h b'"), 1);
}

// 本地还没有 user_latin_words 表（旧日志）时，合并要能把表建出来再补词。
TEST_CASE(merge_user_dictionary_creates_latin_table_for_old_local_journal)
{
    const fs::path root = TempDir("merge-dictionary-latin-old");
    const fs::path local_db = root / "local.db";
    const fs::path remote_db = root / "remote.db";
    std::string message;

    CreateUserDatabase(local_db, {});
    CreateUserDatabase(remote_db, {kLatinTable, "INSERT INTO user_latin_words VALUES('h b','沪B',7,0,100)"});

    user_data_sync::DictionaryMergeReport report;
    REQUIRE(user_data_sync::MergeUserDictionary(local_db, remote_db, report, message));
    REQUIRE_EQ(QueryInt(local_db, "SELECT COUNT(*) FROM user_latin_words"), 1);
}

// 远端日志按另一份出厂词库记的：采纳远端行后必须让下次回放走 rebase，
// 否则旧词库量级的绝对权重会把新词库的词频压下去。
TEST_CASE(merge_user_dictionary_forces_rebase_when_dictionaries_differ)
{
    const fs::path root = TempDir("merge-dictionary-rebase");
    const fs::path local_db = root / "local.db";
    const fs::path remote_db = root / "remote.db";
    std::string message;

    CreateUserDatabase(local_db,
                       {kStateTable, "INSERT INTO dictionary_state VALUES('replayed_dictionary_sha256','new')"});
    CreateUserDatabase(
        remote_db, {kStateTable, "INSERT INTO dictionary_state VALUES('replayed_dictionary_sha256','old')",
                    "INSERT INTO user_dictionary_operations VALUES('pinyin','le','了','upsert',6500000,'',0,0,200)"});

    user_data_sync::DictionaryMergeReport report;
    REQUIRE(user_data_sync::MergeUserDictionary(local_db, remote_db, report, message));
    REQUIRE_EQ(report.added, 1);
    REQUIRE_EQ(QueryInt(local_db, "SELECT COUNT(*) FROM dictionary_state WHERE name='replayed_dictionary_sha256'"), 0);
}

// 同一份词库时不必重算，状态原样保留。
TEST_CASE(merge_user_dictionary_keeps_replay_state_for_same_dictionary)
{
    const fs::path root = TempDir("merge-dictionary-same-dictionary");
    const fs::path local_db = root / "local.db";
    const fs::path remote_db = root / "remote.db";
    std::string message;

    CreateUserDatabase(local_db,
                       {kStateTable, "INSERT INTO dictionary_state VALUES('replayed_dictionary_sha256','x')"});
    CreateUserDatabase(remote_db,
                       {kStateTable, "INSERT INTO dictionary_state VALUES('replayed_dictionary_sha256','x')",
                        "INSERT INTO user_dictionary_operations VALUES('pinyin','le','了','upsert',65,'',0,0,200)"});

    user_data_sync::DictionaryMergeReport report;
    REQUIRE(user_data_sync::MergeUserDictionary(local_db, remote_db, report, message));
    REQUIRE_EQ(QueryInt(local_db, "SELECT COUNT(*) FROM dictionary_state WHERE name='replayed_dictionary_sha256'"), 1);
}

// [sync] 与 [network] 是设备本地的：恢复别的设备时，本机连云端的账号和代理都不能被换掉。
TEST_CASE(merge_config_skips_device_local_sections)
{
    const std::string template_text = "[sync]\n"
                                      "s3_access_key = \"\"\n"
                                      "keep_snapshots = 5\n"
                                      "[network]\n"
                                      "proxy_mode = \"system\"\n"
                                      "[appearance]\n"
                                      "page_size = 8\n";
    const std::string local_text = "[sync]\n"
                                   "s3_access_key = \"LOCALKEY\"\n"
                                   "keep_snapshots = 5\n"
                                   "[network]\n"
                                   "proxy_mode = \"system\"\n"
                                   "[appearance]\n"
                                   "page_size = 8\n";
    const std::string remote_text = "[sync]\n"
                                    "s3_access_key = \"REMOTEKEY\"\n"
                                    "keep_snapshots = 9\n"
                                    "[network]\n"
                                    "proxy_mode = \"custom\"\n"
                                    "[appearance]\n"
                                    "page_size = 9\n";
    const std::string merged = user_data_sync::MergeConfig(local_text, remote_text, template_text);
    REQUIRE(merged.find("LOCALKEY") != std::string::npos);
    REQUIRE(merged.find("REMOTEKEY") == std::string::npos);
    REQUIRE(merged.find("keep_snapshots = 5") != std::string::npos);
    REQUIRE(merged.find("proxy_mode = \"system\"") != std::string::npos);
    REQUIRE(merged.find("page_size = 9") != std::string::npos);
}

TEST_CASE(device_id_is_validated_and_stable)
{
    const fs::path root = TempDir("device-id");
    std::string message;
    const std::string first = user_data_sync::LoadOrCreateDeviceId(root, message);
    REQUIRE(user_data_sync::IsValidDeviceId(first));
    // 第二次读必须拿到同一个值，否则每台机器每次启动都会变成新设备。
    const std::string second = user_data_sync::LoadOrCreateDeviceId(root, message);
    REQUIRE_EQ(first, second);

    REQUIRE(!user_data_sync::IsValidDeviceId(""));
    REQUIRE(!user_data_sync::IsValidDeviceId("ABCDEF"));
    REQUIRE(!user_data_sync::IsValidDeviceId(std::string(32, 'g')));
}
