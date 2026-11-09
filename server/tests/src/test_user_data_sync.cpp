#include "settings/remote_store.h"
#include "settings/remote_store_internal.h"
#include "settings/sync_merge.h"
#include "settings/sync_service.h"
#include "settings/user_data_backup.h"
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

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

// 内存里的假 store，用来测编排层而不碰网络。键值都留在内存。
class FakeStore final : public user_data_sync::RemoteStore
{
  public:
    bool Put(const std::string &key, const fs::path &file, const user_data_sync::ProgressCallback &,
             std::string &message) override
    {
        std::error_code error;
        if (!fs::is_regular_file(file, error))
        {
            message = "missing file";
            return false;
        }
        objects_[key] = fs::file_size(file, error);
        return true;
    }
    bool Get(const std::string &key, const fs::path &file, const user_data_sync::ProgressCallback &,
             std::string &message) override
    {
        const auto found = objects_.find(key);
        if (found == objects_.end())
        {
            message = "云端没有这个文件";
            return false;
        }
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "fake";
        return true;
    }
    bool Stat(const std::string &key, user_data_sync::RemoteObject &object, std::string &) override
    {
        const auto found = objects_.find(key);
        if (found == objects_.end())
            return false;
        object.key = key;
        object.size = found->second;
        return true;
    }
    bool List(const std::string &prefix, std::vector<user_data_sync::RemoteObject> &objects, std::string &) override
    {
        objects.clear();
        for (const auto &[key, size] : objects_)
        {
            if (key.rfind(prefix, 0) == 0)
                objects.push_back({key, size, {}, 0});
        }
        return true;
    }
    bool Delete(const std::string &key, std::string &) override
    {
        objects_.erase(key);
        return true;
    }
    bool Probe(std::string &) override
    {
        return true;
    }
    bool EnsureDirectory(const std::string &, std::string &) override
    {
        return true;
    }

  private:
    std::map<std::string, std::uint64_t> objects_;
};

// 造一个最小可用的 msime_user.db（带日志表），供合并与导入测试使用。
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

// AWS 官方文档里的两个 SigV4 测试向量。签名逻辑错一位，真实 S3 会直接 403，
// 而本地没有 S3 可连，所以这两个向量是唯一能离线验证正确性的手段。
TEST_CASE(sigv4_matches_aws_documented_get_object_vector)
{
    user_data_sync::S3Options options;
    options.region = "us-east-1";
    options.access_key = "AKIAIOSFODNN7EXAMPLE";
    options.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    options.path_style = false;
    options.bucket = "examplebucket";

    const std::vector<std::pair<std::string, std::string>> headers = {
        {"host", "examplebucket.s3.amazonaws.com"},
        {"range", "bytes=0-9"},
        {"x-amz-content-sha256", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"x-amz-date", "20130524T000000Z"},
    };
    const user_data_sync::S3Signature signature =
        user_data_sync::S3Sign(options, "GET", "/test.txt", "", headers,
                               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 1369353600);
    REQUIRE_EQ(signature.signature, std::string("f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41"));
    REQUIRE_EQ(signature.amz_date, std::string("20130524T000000Z"));
}

TEST_CASE(sigv4_matches_aws_documented_put_object_vector)
{
    user_data_sync::S3Options options;
    options.region = "us-east-1";
    options.access_key = "AKIAIOSFODNN7EXAMPLE";
    options.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    options.path_style = false;
    options.bucket = "examplebucket";

    const std::string payload = "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072";
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"date", "Fri, 24 May 2013 00:00:00 GMT"},
        {"host", "examplebucket.s3.amazonaws.com"},
        {"x-amz-content-sha256", payload},
        {"x-amz-date", "20130524T000000Z"},
    };
    const user_data_sync::S3Signature signature =
        user_data_sync::S3Sign(options, "PUT", "/test%24file.text", "", headers, payload, 1369353600);
    REQUIRE_EQ(signature.signature, std::string("1f0f03a2f9e50fde0c0a81574e56504b7c8a90148de788439fd654821bcab381"));
}

TEST_CASE(empty_payload_hash_is_sha256_of_nothing)
{
    REQUIRE_EQ(user_data_sync::EmptyPayloadSha256(),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

TEST_CASE(uri_encode_matches_sigv4_rules)
{
    // SigV4 要求空格编成 %20 而不是 +，斜杠在对象 key 里保留。
    REQUIRE_EQ(user_data_sync::detail::UriEncode("a b", true), std::string("a%20b"));
    REQUIRE_EQ(user_data_sync::detail::UriEncode("a/b", false), std::string("a/b"));
    REQUIRE_EQ(user_data_sync::detail::UriEncode("a/b", true), std::string("a%2Fb"));
    REQUIRE_EQ(user_data_sync::detail::UriEncode("A-z_0.9~", true), std::string("A-z_0.9~"));
    REQUIRE_EQ(user_data_sync::detail::UriEncode("水", true), std::string("%E6%B0%B4"));
}

TEST_CASE(canonical_query_sorts_by_encoded_name)
{
    const std::vector<std::pair<std::string, std::string>> params = {{"prefix", "a b"}, {"list-type", "2"}};
    REQUIRE_EQ(user_data_sync::detail::CanonicalQuery(params), std::string("list-type=2&prefix=a%20b"));
}

TEST_CASE(split_url_handles_ports_and_ipv6)
{
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
    REQUIRE(user_data_sync::detail::SplitUrl("https://s3.example.com/bucket/key", scheme, host, port, path));
    REQUIRE_EQ(scheme, std::string("https"));
    REQUIRE_EQ(host, std::string("s3.example.com"));
    REQUIRE_EQ(port, std::string("443"));
    REQUIRE_EQ(path, std::string("/bucket/key"));

    REQUIRE(user_data_sync::detail::SplitUrl("http://127.0.0.1:9000/b", scheme, host, port, path));
    REQUIRE_EQ(host, std::string("127.0.0.1"));
    REQUIRE_EQ(port, std::string("9000"));

    // IPv6 字面量里的冒号不能被当成端口分隔。
    REQUIRE(user_data_sync::detail::SplitUrl("https://[::1]:8443/b", scheme, host, port, path));
    REQUIRE_EQ(host, std::string("[::1]"));
    REQUIRE_EQ(port, std::string("8443"));

    REQUIRE(user_data_sync::detail::SplitUrl("https://[::1]/b", scheme, host, port, path));
    REQUIRE_EQ(host, std::string("[::1]"));
    REQUIRE_EQ(port, std::string("443"));

    // 域名里的端口必须从 host 里剥掉，否则 S3 的 canonical host 会错、签名对不上。
    REQUIRE(user_data_sync::detail::SplitUrl("https://minio.local:9000", scheme, host, port, path));
    REQUIRE_EQ(host, std::string("minio.local"));
    REQUIRE_EQ(port, std::string("9000"));
    REQUIRE_EQ(path, std::string("/"));
}

TEST_CASE(split_url_rejects_non_http_schemes)
{
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
    REQUIRE(!user_data_sync::detail::SplitUrl("file:///C:/x", scheme, host, port, path));
    REQUIRE(!user_data_sync::detail::IsSupportedUrl("file:///C:/x"));
    REQUIRE(user_data_sync::detail::IsSupportedUrl("https://a"));
}

TEST_CASE(join_url_does_not_double_slash)
{
    REQUIRE_EQ(user_data_sync::detail::JoinUrl("https://dav.example.com/msime/", "devices/a.json"),
               std::string("https://dav.example.com/msime/devices/a.json"));
    REQUIRE_EQ(user_data_sync::detail::JoinUrl("https://dav.example.com/msime", "devices/a.json"),
               std::string("https://dav.example.com/msime/devices/a.json"));
}

TEST_CASE(device_and_snapshot_keys_round_trip)
{
    const std::string device = "0123456789abcdef0123456789abcdef";
    REQUIRE_EQ(user_data_sync::DeviceInfoKey(device), std::string("devices/") + device + ".json");
    REQUIRE_EQ(user_data_sync::DeviceIdFromInfoKey(user_data_sync::DeviceInfoKey(device)), device);

    const std::string snapshot = user_data_sync::SnapshotKey(device, "1700000000-1234");
    REQUIRE_EQ(snapshot, std::string("devices/") + device + "/1700000000-1234.zip");
    REQUIRE_EQ(user_data_sync::DeviceIdFromSnapshotKey(snapshot), device);

    // 不是我们生成的形状一律拒绝，避免远端返回的内容被拼进本地路径。
    REQUIRE_EQ(user_data_sync::DeviceIdFromInfoKey("devices/../evil.json"), std::string());
    REQUIRE_EQ(user_data_sync::DeviceIdFromSnapshotKey("devices/x/../../y.zip"), std::string());
    REQUIRE_EQ(user_data_sync::DeviceIdFromSnapshotKey("devices/" + device + "/a/b.zip"), std::string());
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

// 没有出厂模板时也要能合并：空模板表示"没有基线"，只跳过默认值判断，不能整段放弃。
TEST_CASE(merge_config_with_empty_template_still_takes_remote_changes)
{
    const std::string local_text = "[appearance]\ntheme_mode = \"dark\"\n";
    const std::string remote_text = "[appearance]\ntheme_mode = \"light\"\n";
    const std::string merged = user_data_sync::MergeConfig(local_text, remote_text, "");
    REQUIRE(merged.find("theme_mode = \"light\"") != std::string::npos);
}

// 从云端恢复不能清空本机凭据。同步归档里的 config.toml 在上传前被剥离过凭据，
// 整包覆盖会把 token、语音密钥、同步密码全部写成空串。
TEST_CASE(restore_keeps_local_credentials_and_merges_changed_keys)
{
    const std::string local_text = "[ai_assistant]\n"
                                   "token = \"dpapi:LOCAL\"\n"
                                   "model = \"deepseek-chat\"\n"
                                   "\n"
                                   "[appearance]\n"
                                   "theme_mode = \"dark\"\n";
    const std::string archived_text = "[ai_assistant]\n"
                                      "token = \"\"\n"
                                      "model = \"deepseek-reasoner\"\n"
                                      "\n"
                                      "[appearance]\n"
                                      "theme_mode = \"light\"\n";
    const std::string merged = SettingsUserDataBackup::MergeArchivedConfig(local_text, archived_text, "");
    REQUIRE(merged.find("dpapi:LOCAL") != std::string::npos);
    REQUIRE(merged.find("deepseek-reasoner") != std::string::npos);
    REQUIRE(merged.find("theme_mode = \"light\"") != std::string::npos);
}

// 远端仍是出厂默认的键不该覆盖本机：对方没动过，拿它盖掉本地没有道理。
TEST_CASE(restore_does_not_take_remote_defaults_over_local_changes)
{
    const std::string template_text = "[appearance]\n"
                                      "theme_mode = \"system\"\n"
                                      "page_size = 8\n";
    const std::string local_text = "[appearance]\n"
                                   "theme_mode = \"dark\"\n"
                                   "page_size = 8\n";
    const std::string archived_text = "[appearance]\n"
                                      "theme_mode = \"system\"\n"
                                      "page_size = 9\n";
    const std::string merged = SettingsUserDataBackup::MergeArchivedConfig(local_text, archived_text, template_text);
    REQUIRE(merged.find("theme_mode = \"dark\"") != std::string::npos);
    REQUIRE(merged.find("page_size = 9") != std::string::npos);
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

TEST_CASE(fake_store_upload_and_download_round_trip)
{
    const fs::path root = TempDir("sync-service");
    auto store = std::make_unique<FakeStore>();
    user_data_sync::SyncOptions options;
    user_data_sync::SyncService service(std::move(store), root, options);

    const user_data_sync::UploadResult uploaded = service.Upload(
        [](const fs::path &destination, std::string &message) {
            std::ofstream file(destination, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                message = "write failed";
                return false;
            }
            file << "payload";
            return true;
        },
        {});
    REQUIRE(uploaded.ok);
    REQUIRE(!uploaded.revision.empty());

    std::vector<user_data_sync::DeviceInfo> devices;
    std::string message;
    REQUIRE(service.ListDevices(devices, message));
    REQUIRE_EQ(devices.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(devices.front().device_id, service.DeviceId());
    REQUIRE_EQ(devices.front().revision, uploaded.revision);
}
