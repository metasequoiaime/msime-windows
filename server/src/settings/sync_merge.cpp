#include "settings/sync_merge.h"

#include "config/ime_config_internal.h"

#include <bcrypt.h>
#include <sqlite3.h>
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace user_data_sync
{
namespace
{
using DbHandle = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

std::string Utf8(const std::filesystem::path &path)
{
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char *>(utf8.data()), utf8.size()};
}

DbHandle OpenDatabase(const std::filesystem::path &path, int flags, std::string &message)
{
    sqlite3 *raw = nullptr;
    const std::string utf8_path = Utf8(path);
    if (sqlite3_open_v2(utf8_path.c_str(), &raw, flags | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
    {
        message = std::string("无法打开数据库：") + (raw ? sqlite3_errmsg(raw) : "未知错误");
        if (raw)
            sqlite3_close(raw);
        return {nullptr, &sqlite3_close};
    }
    sqlite3_busy_timeout(raw, 5000);
    return {raw, &sqlite3_close};
}

bool TableExists(sqlite3 *db, const char *table)
{
    sqlite3_stmt *statement = nullptr;
    const std::string sql = std::string("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1");
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
        return false;
    bool found = false;
    if (sqlite3_bind_text(statement, 1, table, -1, SQLITE_TRANSIENT) == SQLITE_OK)
        found = sqlite3_step(statement) == SQLITE_ROW;
    sqlite3_finalize(statement);
    return found;
}

bool HasColumn(sqlite3 *db, const char *table, const char *column)
{
    sqlite3_stmt *statement = nullptr;
    const std::string sql = std::string("PRAGMA table_info(") + table + ")";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
        return false;
    bool found = false;
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        const unsigned char *name = sqlite3_column_text(statement, 1);
        if (name && std::string(reinterpret_cast<const char *>(name)) == column)
        {
            found = true;
            break;
        }
    }
    sqlite3_finalize(statement);
    return found;
}

bool Exec(sqlite3 *db, const std::string &sql, std::string &message)
{
    char *error = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK)
    {
        message = error ? error : "SQL 执行失败";
        sqlite3_free(error);
        return false;
    }
    return true;
}

// 词库日志的一行。字段顺序与 user_dictionary_operations 的表结构对齐。
struct OperationRow
{
    std::string dictionary;
    std::string key;
    std::string value;
    std::string operation;
    std::int64_t weight = 0;
    std::string display;
    bool user_inserted = false;
    bool weight_edited = false;
    std::int64_t updated_at = 0;
};

bool ReadOperations(sqlite3 *db, std::vector<OperationRow> &rows, std::string &message)
{
    // 老版本的日志没有 user_inserted / weight_edited 两列；缺列时按默认值读，不能因此失败。
    const bool has_user_inserted = HasColumn(db, "user_dictionary_operations", "user_inserted");
    const bool has_weight_edited = HasColumn(db, "user_dictionary_operations", "weight_edited");
    std::string sql = "SELECT dictionary,key,value,operation,weight,display,";
    sql += has_user_inserted ? "user_inserted," : "0,";
    sql += has_weight_edited ? "weight_edited," : "0,";
    sql += "updated_at FROM user_dictionary_operations";

    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
    {
        message = std::string("读取用户词库日志失败：") + sqlite3_errmsg(db);
        return false;
    }
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW)
    {
        const auto text = [&](int column) {
            const unsigned char *value = sqlite3_column_text(statement, column);
            return value ? std::string(reinterpret_cast<const char *>(value)) : std::string();
        };
        OperationRow row;
        row.dictionary = text(0);
        row.key = text(1);
        row.value = text(2);
        row.operation = text(3);
        row.weight = sqlite3_column_int64(statement, 4);
        row.display = text(5);
        row.user_inserted = sqlite3_column_int(statement, 6) != 0;
        row.weight_edited = sqlite3_column_int(statement, 7) != 0;
        row.updated_at = sqlite3_column_int64(statement, 8);
        rows.push_back(std::move(row));
    }
    // 读到一半出错（SQLITE_CORRUPT、IO 错误）时只拿到了部分行。拿它去合并，远端的旧行会
    // 被当成"本地没有"而写进来，必须判失败。
    const bool done = step == SQLITE_DONE;
    if (!done)
        message = std::string("读取用户词库日志失败：") + sqlite3_errmsg(db);
    sqlite3_finalize(statement);
    return done;
}

// dictionary_state 里「日志上次回放到哪份出厂词库」那一行，与 engine 的 kReplayedDictionaryState 同名。
constexpr const char *kReplayedDictionaryState = "replayed_dictionary_sha256";

// 没有表或没有这一行都返回空串：对合并来说两者都等于"不知道这份日志是按哪份词库记的"。
std::string ReplayedDictionaryIdentity(sqlite3 *db)
{
    if (!TableExists(db, "dictionary_state"))
        return {};
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT value FROM dictionary_state WHERE name=?1", -1, &statement, nullptr) !=
        SQLITE_OK)
        return {};
    std::string value;
    if (sqlite3_bind_text(statement, 1, kReplayedDictionaryState, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW)
    {
        const unsigned char *text = sqlite3_column_text(statement, 0);
        if (text)
            value = reinterpret_cast<const char *>(text);
    }
    sqlite3_finalize(statement);
    return value;
}

// 用户在设置页加的字母词。主键 (key,value)，没有删除墓碑：删除就是删行。
struct LatinWordRow
{
    std::string key;
    std::string value;
    std::int64_t weight = 0;
    int exact_only = 0;
    std::int64_t updated_at = 0;
};

bool ReadLatinWords(sqlite3 *db, std::vector<LatinWordRow> &rows, std::string &message)
{
    if (!TableExists(db, "user_latin_words"))
        return true; // 旧日志还没有这张表，等于没有字母词
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT key,value,weight,exact_only,updated_at FROM user_latin_words", -1, &statement,
                           nullptr) != SQLITE_OK)
    {
        message = std::string("读取用户字母词失败：") + sqlite3_errmsg(db);
        return false;
    }
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW)
    {
        LatinWordRow row;
        const unsigned char *key = sqlite3_column_text(statement, 0);
        const unsigned char *value = sqlite3_column_text(statement, 1);
        row.key = key ? reinterpret_cast<const char *>(key) : "";
        row.value = value ? reinterpret_cast<const char *>(value) : "";
        row.weight = sqlite3_column_int64(statement, 2);
        row.exact_only = sqlite3_column_int(statement, 3);
        row.updated_at = sqlite3_column_int64(statement, 4);
        rows.push_back(std::move(row));
    }
    const bool done = result == SQLITE_DONE;
    if (!done)
        message = std::string("读取用户字母词失败：") + sqlite3_errmsg(db);
    sqlite3_finalize(statement);
    return done;
}

std::string OperationId(const OperationRow &row)
{
    // 与主键 (dictionary,key,value) 一致。用不可见分隔符拼，避免键里本身含分隔字符时串味。
    return row.dictionary + '\x1f' + row.key + '\x1f' + row.value;
}

// 较新者胜出；时间相同时本地胜出并计入 conflicts（两边同时改过同一词的情况很少，
// 让本机行为可预期比让远端覆盖更稳妥）。
bool RemoteWins(const OperationRow &local, const OperationRow &remote, int &conflicts)
{
    if (remote.updated_at > local.updated_at)
        return true;
    if (remote.updated_at < local.updated_at)
        return false;
    if (remote.operation != local.operation || remote.weight != local.weight || remote.display != local.display ||
        remote.user_inserted != local.user_inserted || remote.weight_edited != local.weight_edited)
    {
        ++conflicts;
    }
    return false;
}

// 32 位小写十六进制，UUID v4 形状但不带连字符：够唯一，也不依赖任何平台 UUID API。
std::string GenerateDeviceId()
{
    unsigned char bytes[16]{};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return {};
    // 版本位与变体位，保持 UUID v4 的形状，方便将来换成真正的 UUID 存储。
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (const unsigned char byte : bytes)
    {
        out.push_back(kHex[(byte >> 4) & 0x0F]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

// 从 sync_state.json 里取 device_id。文件很小，且只由本进程写，用最小的字符串搜索即可，
// 不值得为它引入 JSON 解析。
std::string ReadDeviceId(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::string needle = "\"device_id\"";
    const std::size_t key = text.find(needle);
    if (key == std::string::npos)
        return {};
    const std::size_t quote = text.find('"', text.find(':', key));
    if (quote == std::string::npos)
        return {};
    const std::size_t end = text.find('"', quote + 1);
    if (end == std::string::npos)
        return {};
    return text.substr(quote + 1, end - quote - 1);
}

bool WriteDeviceId(const std::filesystem::path &path, const std::string &device_id)
{
    const std::filesystem::path temporary = path.parent_path() / (path.filename().wstring() + L".tmp");
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        file << "{\n  \"device_id\": \"" << device_id << "\"\n}\n";
        if (!file)
            return false;
    }
    // 不能先删目标再改名：MoveFileEx 失败时旧文件已经没了，下次启动会生成新设备标识，
    // 云端就会多出一台"新设备"。MOVEFILE_REPLACE_EXISTING 本身就是原子的替换。
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE)
        return true;
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
}
} // namespace

bool IsDeviceLocalSection(const std::string &section)
{
    return section == "sync" || section == "network";
}

bool IsValidDeviceId(const std::string &device_id)
{
    if (device_id.size() != 32)
        return false;
    return std::all_of(device_id.begin(), device_id.end(),
                       [](unsigned char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
}

std::string LoadOrCreateDeviceId(const std::filesystem::path &data_dir, std::string &message)
{
    message.clear();
    if (data_dir.empty())
    {
        message = "数据目录不可用";
        return {};
    }
    const std::filesystem::path path = data_dir / L"sync_state.json";
    const std::string existing = ReadDeviceId(path);
    if (IsValidDeviceId(existing))
        return existing;

    const std::string generated = GenerateDeviceId();
    if (!IsValidDeviceId(generated))
    {
        message = "无法生成设备标识";
        return {};
    }
    std::error_code error;
    std::filesystem::create_directories(data_dir, error);
    if (!WriteDeviceId(path, generated))
    {
        message = "无法保存设备标识";
        return {};
    }
    return generated;
}

bool MergeUserDictionary(const std::filesystem::path &local_db, const std::filesystem::path &remote_db,
                         DictionaryMergeReport &report, std::string &message)
{
    report = {};
    message.clear();
    std::error_code error;
    if (!std::filesystem::is_regular_file(local_db, error))
    {
        message = "本地用户词库不存在";
        return false;
    }
    if (!std::filesystem::is_regular_file(remote_db, error))
    {
        message = "远端用户词库不存在";
        return false;
    }

    DbHandle remote = OpenDatabase(remote_db, SQLITE_OPEN_READONLY, message);
    if (!remote)
        return false;
    if (!TableExists(remote.get(), "user_dictionary_operations"))
    {
        message = "远端用户词库结构无法识别";
        return false;
    }
    std::vector<OperationRow> remote_rows;
    if (!ReadOperations(remote.get(), remote_rows, message))
        return false;
    std::vector<LatinWordRow> remote_latin;
    if (!ReadLatinWords(remote.get(), remote_latin, message))
        return false;
    const std::string remote_identity = ReplayedDictionaryIdentity(remote.get());
    remote.reset();

    DbHandle local = OpenDatabase(local_db, SQLITE_OPEN_READWRITE, message);
    if (!local)
        return false;
    if (!TableExists(local.get(), "user_dictionary_operations"))
    {
        message = "本地用户词库结构无法识别";
        return false;
    }
    std::vector<OperationRow> local_rows;
    if (!ReadOperations(local.get(), local_rows, message))
        return false;

    std::map<std::string, const OperationRow *> local_by_id;
    for (const OperationRow &row : local_rows)
        local_by_id.emplace(OperationId(row), &row);

    // 合并 user_dictionary_operations 与 user_latin_words。字母词是用户在设置页亲手加的词，
    // 有日志时归档里的文本词库会被整批跳过，不在这里合并它就永远同步不过来。
    // 其余调频状态表（固定位置、槽位、日期格式顺序）是每台设备自己的使用习惯，跨机合并
    // 没有确定语义，这一版不动它们。
    if (!Exec(local.get(), "BEGIN IMMEDIATE", message))
        return false;
    const std::string local_identity = ReplayedDictionaryIdentity(local.get());
    // 老版本的日志没有 user_inserted / weight_edited 两列；缺列时既不写也不读，
    // 保持那些行的默认值，不能因为缺列而合并失败。
    const bool has_user_inserted = HasColumn(local.get(), "user_dictionary_operations", "user_inserted");
    const bool has_weight_edited = HasColumn(local.get(), "user_dictionary_operations", "weight_edited");

    std::string insert_sql = "INSERT INTO user_dictionary_operations(dictionary,key,value,operation,weight,display";
    if (has_user_inserted)
        insert_sql += ",user_inserted";
    if (has_weight_edited)
        insert_sql += ",weight_edited";
    insert_sql += ",updated_at) VALUES(?1,?2,?3,?4,?5,?6";
    int next_parameter = 7;
    if (has_user_inserted)
        insert_sql += ",?" + std::to_string(next_parameter++);
    if (has_weight_edited)
        insert_sql += ",?" + std::to_string(next_parameter++);
    insert_sql += ",?" + std::to_string(next_parameter) + ")";
    insert_sql +=
        " ON CONFLICT(dictionary,key,value) DO UPDATE SET operation=excluded.operation,weight=excluded.weight,"
        "display=excluded.display";
    // user_inserted 是粘性标记：engine 的 upsert / delete 都不清它。远端那一行是 0 只说明
    // 那台机器没手动加过，不能拿它把本机"用户手动加的词"降级成自动调频（降级后带假名的词
    // 会在回放时被跳过并删掉）。
    if (has_user_inserted)
        insert_sql += ",user_inserted=MAX(user_inserted,excluded.user_inserted)";
    if (has_weight_edited)
        insert_sql += ",weight_edited=excluded.weight_edited";
    insert_sql += ",updated_at=excluded.updated_at";
    // RemoteWins 已经只放行更新的行，这里在数据库层再守一道，防止本地快照与表不一致时用旧行盖新行。
    insert_sql += " WHERE excluded.updated_at>user_dictionary_operations.updated_at";

    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(local.get(), insert_sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
    {
        message = std::string("准备合并语句失败：") + sqlite3_errmsg(local.get());
        (void)Exec(local.get(), "ROLLBACK", message);
        return false;
    }

    bool ok = true;
    for (const OperationRow &row : remote_rows)
    {
        const auto found = local_by_id.find(OperationId(row));
        if (found != local_by_id.end() && !RemoteWins(*found->second, row, report.conflicts))
        {
            ++report.kept_local;
            continue;
        }
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
        bool bound = sqlite3_bind_text(statement, 1, row.dictionary.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                     sqlite3_bind_text(statement, 2, row.key.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                     sqlite3_bind_text(statement, 3, row.value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                     sqlite3_bind_text(statement, 4, row.operation.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                     sqlite3_bind_int64(statement, 5, row.weight) == SQLITE_OK &&
                     sqlite3_bind_text(statement, 6, row.display.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
        int next = 7;
        if (has_user_inserted)
            bound = bound && sqlite3_bind_int(statement, next++, row.user_inserted ? 1 : 0) == SQLITE_OK;
        if (has_weight_edited)
            bound = bound && sqlite3_bind_int(statement, next++, row.weight_edited ? 1 : 0) == SQLITE_OK;
        bound = bound && sqlite3_bind_int64(statement, next, row.updated_at) == SQLITE_OK;
        if (!bound || sqlite3_step(statement) != SQLITE_DONE)
        {
            message = std::string("写入合并结果失败：") + sqlite3_errmsg(local.get());
            ok = false;
            break;
        }
        if (found == local_by_id.end())
            ++report.added;
        else
            ++report.taken_from_remote;
    }
    sqlite3_finalize(statement);

    // 字母词：没有墓碑，只能做并集——远端有、本地没有的补进来，两边都有的取较新的权重。
    // 代价是本机删掉、远端还在的字母词会在恢复后回来；恢复是用户主动拉别的设备的数据，
    // 这比"远端加的字母词永远过不来"好接受。
    if (ok && !remote_latin.empty())
    {
        if (!Exec(local.get(),
                  "CREATE TABLE IF NOT EXISTS user_latin_words("
                  "key TEXT NOT NULL,value TEXT NOT NULL,weight INTEGER NOT NULL DEFAULT 0,"
                  "exact_only INTEGER NOT NULL DEFAULT 0,updated_at INTEGER NOT NULL DEFAULT(unixepoch()),"
                  "PRIMARY KEY(key,value))",
                  message))
        {
            ok = false;
        }
        sqlite3_stmt *latin = nullptr;
        if (ok && sqlite3_prepare_v2(local.get(),
                                     "INSERT INTO user_latin_words(key,value,weight,exact_only,updated_at) "
                                     "VALUES(?1,?2,?3,?4,?5) ON CONFLICT(key,value) DO UPDATE SET "
                                     "weight=excluded.weight,exact_only=excluded.exact_only,"
                                     "updated_at=excluded.updated_at "
                                     "WHERE excluded.updated_at>user_latin_words.updated_at",
                                     -1, &latin, nullptr) != SQLITE_OK)
        {
            message = std::string("准备字母词合并语句失败：") + sqlite3_errmsg(local.get());
            ok = false;
        }
        for (const LatinWordRow &row : remote_latin)
        {
            if (!ok)
                break;
            sqlite3_reset(latin);
            sqlite3_clear_bindings(latin);
            const bool bound = sqlite3_bind_text(latin, 1, row.key.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                               sqlite3_bind_text(latin, 2, row.value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                               sqlite3_bind_int64(latin, 3, row.weight) == SQLITE_OK &&
                               sqlite3_bind_int(latin, 4, row.exact_only) == SQLITE_OK &&
                               sqlite3_bind_int64(latin, 5, row.updated_at) == SQLITE_OK;
            if (!bound || sqlite3_step(latin) != SQLITE_DONE)
            {
                message = std::string("写入字母词合并结果失败：") + sqlite3_errmsg(local.get());
                ok = false;
                break;
            }
            report.latin_words += sqlite3_changes(local.get()) > 0 ? 1 : 0;
        }
        if (latin)
            sqlite3_finalize(latin);
    }

    // 远端的行是按远端回放过的那份出厂词库的量级记的。两边词库不是同一份（或远端没记）时，
    // 这些绝对权重原样回放会把新词库的词频压下去（「了 le」被压到旧值就是这么来的）。
    // 删掉本地"已回放到哪份词库"的记录，下次回放就会走 engine 的 rebase：自动调频行只在
    // 高过出厂权重时写回，用户手动加的词与亲手改的权重不受影响。
    const bool took_remote_rows = report.added > 0 || report.taken_from_remote > 0;
    if (ok && took_remote_rows && remote_identity != local_identity && TableExists(local.get(), "dictionary_state"))
    {
        sqlite3_stmt *state = nullptr;
        ok = sqlite3_prepare_v2(local.get(), "DELETE FROM dictionary_state WHERE name=?1", -1, &state, nullptr) ==
                 SQLITE_OK &&
             sqlite3_bind_text(state, 1, kReplayedDictionaryState, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_step(state) == SQLITE_DONE;
        if (!ok)
            message = std::string("重置词库回放状态失败：") + sqlite3_errmsg(local.get());
        if (state)
            sqlite3_finalize(state);
    }

    if (!ok || !Exec(local.get(), "COMMIT", message))
    {
        std::string ignored;
        (void)Exec(local.get(), "ROLLBACK", ignored);
        return false;
    }
    return true;
}

std::string MergeConfig(const std::string &local_text, const std::string &remote_text, const std::string &template_text)
{
    // 本地或远端解析不了就原样返回本地内容：合并配置的失败绝不能比不合并更糟。
    // 模板可以为空，那表示没有出厂基线，只跳过"远端是不是还是默认值"这一层判断。
    if (!ime_config_detail::TomlTextIsParseable(local_text) || !ime_config_detail::TomlTextIsParseable(remote_text))
        return local_text;
    if (!template_text.empty() && !ime_config_detail::TomlTextIsParseable(template_text))
        return local_text;

    const std::map<std::string, std::string> local_values = ime_config_detail::ParseTomlAssignments(local_text);
    const std::map<std::string, std::string> remote_values = ime_config_detail::ParseTomlAssignments(remote_text);
    // 空模板时 template_values 为空，下面的模板比较自然全部落空，等于"远端与本地不同就采纳"。
    const std::map<std::string, std::string> template_values =
        template_text.empty() ? std::map<std::string, std::string>{}
                              : ime_config_detail::ParseTomlAssignments(template_text);

    std::string merged = local_text;
    for (const auto &[id, remote_value] : remote_values)
    {
        const std::size_t separator = id.find('\x01');
        if (separator == std::string::npos)
            continue;
        const std::string section = id.substr(0, separator);
        const std::string key = id.substr(separator + 1);
        // 凭据永不跨机：远端那份是别的账户的 DPAPI 密文，抄过来也解不开。
        if (ime_config_detail::IsCredentialKey(key))
            continue;
        // 设备本地的段整段不合并。[sync] 是这台机器连云端的方式：各机常用各自的 IAM 密钥或
        // WebDAV 账号，只换 access_key / 用户名而 secret 还是本机的，签名就全错了。
        // [network] 的代理随所在网络而定，公司电脑的代理抄到家里电脑上只会让它连不上。
        if (IsDeviceLocalSection(section))
            continue;
        const auto template_value = template_values.find(id);
        // 远端还是出厂默认值，说明对方没动过这一项，不该拿它盖掉本地的选择。
        if (template_value != template_values.end() && template_value->second == remote_value)
            continue;
        const auto local_value = local_values.find(id);
        if (local_value != local_values.end() && local_value->second == remote_value)
            continue;
        if (ime_config_detail::ReplaceTomlValuePreservingFormatting(merged, section, key, remote_value))
            continue;
        (void)ime_config_detail::InsertTomlValuePreservingFormatting(merged, section, key, remote_value);
    }
    return merged;
}

std::string StripCredentials(const std::string &text)
{
    if (!ime_config_detail::TomlTextIsParseable(text))
        return text;
    std::string stripped = text;
    // 收集后再倒序替换，避免前面的替换让后面的偏移失效。
    struct Patch
    {
        std::size_t begin;
        std::size_t end;
    };
    std::vector<Patch> patches;
    ime_config_detail::ForEachTomlAssignment(
        stripped, [&](const std::string &section, const std::string &key, std::size_t begin, std::size_t end) {
            (void)section;
            if (ime_config_detail::IsCredentialKey(key))
                patches.push_back({begin, end});
        });
    for (auto patch = patches.rbegin(); patch != patches.rend(); ++patch)
        stripped.replace(patch->begin, patch->end - patch->begin, "\"\"");
    return stripped;
}
} // namespace user_data_sync
