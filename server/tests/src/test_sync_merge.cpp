#include "settings/sync_merge.h"
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include <sqlite3.h>

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
