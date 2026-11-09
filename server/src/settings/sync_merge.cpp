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
    while (sqlite3_step(statement) == SQLITE_ROW)
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
    sqlite3_finalize(statement);
    return true;
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
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}
} // namespace

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

    // 只改 user_dictionary_operations 这一张表：其余调频状态表（固定位置、槽位、字母词）
    // 是每台设备自己的使用习惯，跨机合并没有确定语义，这一版不动它们。
    if (!Exec(local.get(), "BEGIN IMMEDIATE", message))
        return false;
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
    if (has_user_inserted)
        insert_sql += ",user_inserted=excluded.user_inserted";
    if (has_weight_edited)
        insert_sql += ",weight_edited=excluded.weight_edited";
    insert_sql += ",updated_at=excluded.updated_at";

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
    // 解析不了就原样返回本地内容：合并配置的失败绝不能比不合并更糟。
    if (!ime_config_detail::TomlTextIsParseable(local_text) || !ime_config_detail::TomlTextIsParseable(remote_text) ||
        !ime_config_detail::TomlTextIsParseable(template_text))
        return local_text;

    const std::map<std::string, std::string> local_values = ime_config_detail::ParseTomlAssignments(local_text);
    const std::map<std::string, std::string> remote_values = ime_config_detail::ParseTomlAssignments(remote_text);
    const std::map<std::string, std::string> template_values = ime_config_detail::ParseTomlAssignments(template_text);

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
