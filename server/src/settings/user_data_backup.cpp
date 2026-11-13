#include "settings/user_data_backup.h"

#include "config/ime_config.h"
#include "config/credential_protection.h"
#include "config/ime_config_internal.h"
#include "engine/user_dictionary/user_dictionary_journal.h"
#include "settings/dictionary_manager.h"
#include "settings/sync_merge.h"
#include "settings/zip_archive.h"
#include "utils/common_utils.h"

#include <boost/json.hpp>
#include <sqlite3.h>
#include <toml++/toml.h>
#include <windows.h>

#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <system_error>
#include <vector>

namespace SettingsUserDataBackup
{
namespace
{
constexpr char kFormat[] = "metasequoia-user-data";
constexpr int kFormatVersion = 1;
constexpr char kManifestPath[] = "manifest.json";
constexpr std::uint64_t kMaxEntrySize = 4ull * 1024 * 1024 * 1024 - 1;
// Matches the writer, which patches 32-bit local and central directory fields and
// therefore cannot produce an archive larger than this.
constexpr std::uint64_t kMaxTotalSize = 4ull * 1024 * 1024 * 1024 - 1;
constexpr std::size_t kMaxEntries = 20000;
constexpr std::uint64_t kMaxManifestBytes = 16 * 1024 * 1024;
constexpr DWORD kProcessExitTimeoutMs = 15000;
// A Watchdog that is already running but has no Server yet is mid-start; five
// seconds covers that window without inheriting the full exit-wait timeout and
// guaranteeing a failure.
constexpr DWORD kServerStartTimeoutMs = 5000;
constexpr std::uint64_t kMaxDictionaryBytes = 512ull * 1024 * 1024;
const char *const kDictionaries[] = {"quanpin", "wubi", "english", "quick"};

struct ManifestItem
{
    std::string path;
    std::uint64_t size = 0;
    std::uint32_t crc = 0;
};

std::string LowerAscii(std::string value)
{
    for (char &ch : value)
    {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    return value;
}

// Case folding for collision checks. Windows file systems compare names
// case-insensitively beyond ASCII, so a ZIP with skins/Ä and skins/ä would
// extract onto the same path; fold through the OS mapping instead of ASCII only.
std::string CaseFoldPath(const std::string &utf8)
{
    if (utf8.empty())
        return {};
    const int wide_length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wide_length <= 0)
        return LowerAscii(utf8);
    std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), wide_length);
    const int folded_length = LCMapStringW(LOCALE_INVARIANT, LCMAP_LOWERCASE, wide.data(), wide_length, nullptr, 0);
    if (folded_length <= 0)
        return LowerAscii(utf8);
    std::wstring folded(static_cast<std::size_t>(folded_length), L'\0');
    LCMapStringW(LOCALE_INVARIANT, LCMAP_LOWERCASE, wide.data(), wide_length, folded.data(), folded_length);
    const int result_length =
        WideCharToMultiByte(CP_UTF8, 0, folded.data(), folded_length, nullptr, 0, nullptr, nullptr);
    if (result_length <= 0)
        return LowerAscii(utf8);
    std::string result(static_cast<std::size_t>(result_length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, folded.data(), folded_length, result.data(), result_length, nullptr, nullptr);
    return result;
}

bool HasPrefix(const std::string &value, const char *prefix)
{
    return value.rfind(prefix, 0) == 0;
}

std::string PathToUtf8(const std::filesystem::path &path)
{
    return path.u8string();
}

std::filesystem::path Utf8Path(const std::string &path)
{
    return std::filesystem::u8path(path);
}

std::uint32_t CurrentProcessId32()
{
    return static_cast<std::uint32_t>(GetCurrentProcessId());
}

std::string ErrorSuffix(DWORD error)
{
    return "（错误码 " + std::to_string(error) + "）";
}

bool EnsureDirectory(const std::filesystem::path &path, std::string &message)
{
    if (path.empty())
        return true;
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec)
    {
        message = "无法创建目录：" + PathToUtf8(path) + "（" + ec.message() + "）";
        return false;
    }
    return true;
}

void ClearReadOnly(const std::filesystem::path &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0)
        SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
}

bool CopyFileAtomic(const std::filesystem::path &source, const std::filesystem::path &target, std::string &message)
{
    if (!EnsureDirectory(target.parent_path(), message))
        return false;
    const std::filesystem::path temporary = target.parent_path() / (target.filename().wstring() + L".import-" +
                                                                    std::to_wstring(CurrentProcessId32()) + L".tmp");
    if (!CopyFileW(source.c_str(), temporary.c_str(), FALSE))
    {
        message = "无法复制文件：" + PathToUtf8(source) + ErrorSuffix(GetLastError());
        return false;
    }
    ClearReadOnly(target);
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const DWORD error = GetLastError();
        DeleteFileW(temporary.c_str());
        message = "无法替换文件：" + PathToUtf8(target) + ErrorSuffix(error);
        return false;
    }
    return true;
}

bool IsReparsePoint(const std::filesystem::path &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool SameNormalizedPath(const std::filesystem::path &left, const std::filesystem::path &right)
{
    std::error_code error;
    if (std::filesystem::equivalent(left, right, error) && !error)
        return true;
    return CaseFoldPath(left.lexically_normal().u8string()) == CaseFoldPath(right.lexically_normal().u8string());
}

std::filesystem::path CurrentExecutableDirectory()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size())
        {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        if (buffer.size() >= 32768)
            return {};
        buffer.resize(buffer.size() * 2);
    }
}

std::vector<DWORD> FindProcesses(const std::filesystem::path &image_path)
{
    std::vector<DWORD> result;
    const std::wstring target_name = image_path.filename().wstring();
    const std::wstring target_path = image_path.wstring();
    if (target_name.empty())
        return result;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, target_name.c_str()) != 0)
                continue;
            if (entry.th32ProcessID == GetCurrentProcessId())
                continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process)
            {
                // The Server may run with uiAccess at a higher integrity level than the
                // settings process. Falling back to the process name keeps StopServer from
                // silently writing files while an unqueryable Server is still running.
                result.push_back(entry.th32ProcessID);
                continue;
            }
            std::wstring path(32768, L'\0');
            DWORD size = static_cast<DWORD>(path.size());
            if (QueryFullProcessImageNameW(process, 0, path.data(), &size))
            {
                path.resize(size);
                if (CompareStringOrdinal(path.c_str(), -1, target_path.c_str(), -1, TRUE) == CSTR_EQUAL)
                    result.push_back(entry.th32ProcessID);
            }
            else
            {
                result.push_back(entry.th32ProcessID);
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool ProcessStillRunning(DWORD process_id)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (entry.th32ProcessID == process_id)
            {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool WaitForProcessesToExit(const std::vector<DWORD> &process_ids, DWORD timeout_ms, std::string &message)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (const DWORD process_id : process_ids)
    {
        while (ProcessStillRunning(process_id))
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                message = "等待输入法进程退出超时。请稍后重试。";
                return false;
            }
            Sleep(100);
        }
    }
    return true;
}

bool WaitForServerToStart(const std::filesystem::path &image, DWORD timeout_ms, std::string &message)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        if (!FindProcesses(image).empty())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
        {
            message = "等待输入法后台启动超时。请稍后重试。";
            return false;
        }
        Sleep(100);
    }
}

bool StopServer(std::string &message)
{
    const std::filesystem::path directory = CurrentExecutableDirectory();
    if (directory.empty())
    {
        message = "无法确定设置程序所在目录。";
        return false;
    }
    const std::filesystem::path server_image = directory / L"MetasequoiaImeServer.exe";
    const std::filesystem::path watchdog_image = directory / L"MetasequoiaImeWatchdog.exe";
    std::vector<DWORD> server_processes = FindProcesses(server_image);
    const std::vector<DWORD> watchdog_processes = FindProcesses(watchdog_image);
    if (server_processes.empty() && watchdog_processes.empty())
        return true;

    if (server_processes.empty())
    {
        // The Watchdog is starting the Server right now; stopping the Watchdog alone
        // would leave that Server orphaned, and the later exit wait would time out.
        std::string wait_message;
        if (!WaitForServerToStart(server_image, kServerStartTimeoutMs, wait_message))
        {
            message = "输入法后台正在启动，请稍后重试。";
            return false;
        }
        server_processes = FindProcesses(server_image);
    }

    const bool notified = NotifyImeServerStopForBackup();
    if (!notified && !server_processes.empty())
    {
        message = "无法通知输入法后台停止，导入未开始。";
        return false;
    }
    if (!WaitForProcessesToExit(server_processes, kProcessExitTimeoutMs, message))
        return false;
    if (!WaitForProcessesToExit(watchdog_processes, kProcessExitTimeoutMs, message))
        return false;
    return true;
}

bool StartWatchdog(std::string &message)
{
    const std::filesystem::path directory = CurrentExecutableDirectory();
    if (directory.empty())
    {
        message = "无法确定设置程序所在目录，输入法未自动启动。";
        return false;
    }
    const std::filesystem::path image = directory / L"MetasequoiaImeWatchdog.exe";
    const std::filesystem::path server_image = directory / L"MetasequoiaImeServer.exe";
    if (!std::filesystem::is_regular_file(image))
    {
        message = "找不到 MetasequoiaImeWatchdog.exe，输入法未自动启动。";
        return false;
    }
    if (FindProcesses(image).empty())
    {
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.lpVerb = L"open";
        info.lpFile = image.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info))
        {
            message = "启动输入法失败" + ErrorSuffix(GetLastError()) + "，请手动启动水杉输入法。";
            return false;
        }
        if (info.hProcess)
            CloseHandle(info.hProcess);
    }
    std::string wait_message;
    if (!WaitForServerToStart(server_image, kProcessExitTimeoutMs, wait_message))
    {
        message = "输入法后台没有启动，请手动启动水杉输入法。";
        return false;
    }
    return true;
}

bool ExportDictionaryViaSettings(const std::string &dictionary, std::string &content, std::string &message)
{
    content.clear();
    try
    {
        boost::json::object request;
        request["action"] = "export";
        request["dictionary"] = dictionary;
        const boost::json::object response = SettingsDictionary::HandleRequest(request);
        const boost::json::value *ok = response.if_contains("ok");
        if (!ok || !ok->is_bool() || !ok->as_bool())
        {
            const boost::json::value *error = response.if_contains("message");
            message = error && error->is_string() ? std::string(error->as_string()) : std::string("词库导出失败");
            return false;
        }
        const boost::json::value *value = response.if_contains("content");
        if (!value || !value->is_string())
        {
            message = "词库导出没有返回内容";
            return false;
        }
        content.assign(value->as_string().data(), value->as_string().size());
        return true;
    }
    catch (...)
    {
        message = "词库导出失败";
        return false;
    }
}

bool ImportDictionaryViaSettings(const std::string &dictionary, const std::string &content, std::string &message)
{
    try
    {
        boost::json::object request;
        request["action"] = "import";
        request["dictionary"] = dictionary;
        request["content"] = content;
        const boost::json::object response = SettingsDictionary::HandleRequest(request);
        const boost::json::value *ok = response.if_contains("ok");
        const boost::json::value *response_message = response.if_contains("message");
        message =
            response_message && response_message->is_string()
                ? std::string(response_message->as_string())
                : (ok && ok->is_bool() && ok->as_bool() ? std::string("词库导入成功") : std::string("词库导入失败"));
        return ok && ok->is_bool() && ok->as_bool();
    }
    catch (...)
    {
        message = "词库导入失败";
        return false;
    }
}

bool SnapshotSqlite(const std::filesystem::path &source, const std::filesystem::path &destination,
                    const char *source_name, std::string &message)
{
    {
        std::ifstream header(source, std::ios::binary);
        std::array<char, 16> magic{};
        header.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        if (header.gcount() != static_cast<std::streamsize>(magic.size()) ||
            std::memcmp(magic.data(), "SQLite format 3\0", magic.size()) != 0)
        {
            message = std::string(source_name) + "不是有效的 SQLite 文件，未导出备份。";
            return false;
        }
    }

    sqlite3 *raw_source = nullptr;
    sqlite3 *raw_destination = nullptr;
    const std::string source_path = PathToUtf8(source);
    const std::string destination_path = PathToUtf8(destination);
    if (!EnsureDirectory(destination.parent_path(), message))
        return false;
    std::error_code ec;
    std::filesystem::remove(destination, ec);

    if (sqlite3_open_v2(source_path.c_str(), &raw_source, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) !=
        SQLITE_OK)
    {
        const std::string error = raw_source ? sqlite3_errmsg(raw_source) : std::string("无法打开") + source_name;
        if (raw_source)
            sqlite3_close(raw_source);
        message = std::string("打开") + source_name + "失败：" + error;
        return false;
    }
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw_source, &sqlite3_close);
    if (sqlite3_open_v2(destination_path.c_str(), &raw_destination,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
    {
        const std::string error =
            raw_destination ? sqlite3_errmsg(raw_destination) : std::string("无法创建") + source_name + "快照";
        if (raw_destination)
            sqlite3_close(raw_destination);
        message = std::string("创建") + source_name + "快照失败：" + error;
        return false;
    }
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> snapshot(raw_destination, &sqlite3_close);
    sqlite3_busy_timeout(raw_source, 5000);

    sqlite3_backup *backup = sqlite3_backup_init(raw_destination, "main", raw_source, "main");
    if (!backup)
    {
        message = std::string(source_name) + "快照初始化失败：" + sqlite3_errmsg(raw_destination);
        return false;
    }
    int result = SQLITE_OK;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do
    {
        result = sqlite3_backup_step(backup, 256);
        if (result == SQLITE_BUSY || result == SQLITE_LOCKED)
        {
            // The live stats writer may hold a short transaction. Retry briefly,
            // but never spin forever on a database that stays locked.
            if (std::chrono::steady_clock::now() >= deadline)
                break;
            Sleep(20);
        }
    } while (result == SQLITE_OK || result == SQLITE_BUSY || result == SQLITE_LOCKED);
    const int finish_result = sqlite3_backup_finish(backup);
    if (result != SQLITE_DONE || finish_result != SQLITE_OK)
    {
        message = std::string(source_name) + "快照失败：" + sqlite3_errmsg(raw_destination);
        std::filesystem::remove(destination, ec);
        return false;
    }
    return true;
}

// stats.db 与 msime_user.db 都是 WAL，而 Server 是用 ExitProcess 退出的，-wal 会留在
// 磁盘上。放着旧 WAL 去替换主库，SQLite 下次打开会把旧页回放到新库里。这里先把 WAL
// checkpoint 进主库并清掉 sidecar，随后的副本才是完整的，预导入备份也才有意义。
bool CheckpointLiveDatabase(const std::filesystem::path &path, const char *label, std::string &message)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error))
    {
        // 孤立的 WAL/journal 会挂到即将落位的新文件上，主库不在时先清掉 sidecar。
        for (const wchar_t *suffix : {L"-wal", L"-shm", L"-journal"})
        {
            std::filesystem::path sidecar = path;
            sidecar += suffix;
            std::error_code remove_error;
            std::filesystem::remove(sidecar, remove_error);
        }
        return true;
    }
    sqlite3 *raw = nullptr;
    const std::string path_utf8 = PathToUtf8(path);
    if (sqlite3_open_v2(path_utf8.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
    {
        const std::string reason = raw ? sqlite3_errmsg(raw) : std::string("无法打开") + label;
        if (raw)
            sqlite3_close(raw);
        message = std::string("无法整理现有") + label + "：" + reason;
        return false;
    }
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, &sqlite3_close);
    sqlite3_busy_timeout(raw, 5000);
    char *error_text = nullptr;
    const int result = sqlite3_exec(raw, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, &error_text);
    if (result != SQLITE_OK)
    {
        message = std::string("无法整理现有") + label + "：" + (error_text ? error_text : sqlite3_errmsg(raw));
    }
    sqlite3_free(error_text);
    return result == SQLITE_OK;
}

bool ReadSmallFile(const std::filesystem::path &path, std::uint64_t max_size, std::string &content,
                   std::string &message)
{
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(path, ec);
    if (ec || size > max_size)
    {
        message = "文件过大或无法读取：" + PathToUtf8(path);
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        message = "无法读取文件：" + PathToUtf8(path);
        return false;
    }
    content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool ParseManifest(const std::string &text, std::vector<ManifestItem> &items, std::string &message)
{
    boost::system::error_code error;
    const boost::json::value root = boost::json::parse(text, error);
    if (error)
    {
        message = "manifest.json 不是有效的 JSON。";
        return false;
    }
    if (!root.is_object())
    {
        message = "manifest.json 顶层不是对象。";
        return false;
    }
    const boost::json::object &object = root.as_object();
    if (object.size() != 3)
    {
        message = "manifest.json 包含未知字段。";
        return false;
    }
    const boost::json::value *format = object.if_contains("format");
    const boost::json::value *version = object.if_contains("version");
    const boost::json::value *list = object.if_contains("items");
    if (!format || !format->is_string() || std::string(format->as_string()) != kFormat)
    {
        message = "不是水杉用户数据备份。";
        return false;
    }
    if (!version || !version->is_int64() || version->as_int64() != kFormatVersion)
    {
        message = "备份格式版本不受支持。";
        return false;
    }
    if (!list || !list->is_array() || list->as_array().size() > kMaxEntries)
    {
        message = "manifest.json 的 items 不合法。";
        return false;
    }

    std::map<std::string, bool> seen;
    std::uint64_t total_size = 0;
    items.clear();
    for (const boost::json::value &value : list->as_array())
    {
        if (!value.is_object() || value.as_object().size() != 3)
        {
            message = "manifest.json 的条目格式不合法。";
            return false;
        }
        const boost::json::object &item = value.as_object();
        const boost::json::value *path_value = item.if_contains("path");
        const boost::json::value *size_value = item.if_contains("size");
        const boost::json::value *crc_value = item.if_contains("crc32");
        if (!path_value || !path_value->is_string() || !size_value ||
            (!size_value->is_int64() && !size_value->is_uint64()) || !crc_value ||
            (!crc_value->is_int64() && !crc_value->is_uint64()))
        {
            message = "manifest.json 的条目字段不合法。";
            return false;
        }
        const std::string path(path_value->as_string().data(), path_value->as_string().size());
        if (!IsAllowedDataPath(path))
        {
            message = "备份包含不允许的路径：" + path;
            return false;
        }
        const std::int64_t declared_size =
            size_value->is_int64() ? size_value->as_int64() : static_cast<std::int64_t>(size_value->as_uint64());
        const std::int64_t declared_crc =
            crc_value->is_int64() ? crc_value->as_int64() : static_cast<std::int64_t>(crc_value->as_uint64());
        if (declared_size < 0 || static_cast<std::uint64_t>(declared_size) > kMaxEntrySize || declared_crc < 0 ||
            static_cast<std::uint64_t>(declared_crc) > 0xFFFFFFFFull)
        {
            message = "manifest.json 的条目大小或校验值不合法。";
            return false;
        }
        const std::uint64_t size = static_cast<std::uint64_t>(declared_size);
        if (size > kMaxTotalSize - total_size)
        {
            message = "备份解压后总大小超过上限。";
            return false;
        }
        total_size += size;
        if (!seen.emplace(CaseFoldPath(path), true).second)
        {
            message = "manifest.json 包含重复路径：" + path;
            return false;
        }
        items.push_back({path, size, static_cast<std::uint32_t>(declared_crc)});
    }
    return true;
}

std::string SerializeManifest(const std::vector<ManifestItem> &items)
{
    boost::json::array list;
    list.reserve(items.size());
    for (const ManifestItem &item : items)
    {
        boost::json::object entry;
        entry["path"] = item.path;
        entry["size"] = item.size;
        entry["crc32"] = item.crc;
        list.push_back(std::move(entry));
    }
    boost::json::object manifest;
    manifest["format"] = kFormat;
    manifest["version"] = kFormatVersion;
    manifest["items"] = std::move(list);
    return boost::json::serialize(manifest);
}

bool ValidateArchive(const std::filesystem::path &archive, std::vector<ManifestItem> &items, std::string &message,
                     bool verify_contents = true)
{
    user_data_zip::Limits limits;
    limits.max_entry_size = kMaxEntrySize;
    limits.max_total_size = kMaxTotalSize;
    limits.max_entries = kMaxEntries;
    user_data_zip::Reader reader(archive, limits);
    if (!reader.Open(message))
        return false;

    const user_data_zip::Entry *manifest_entry = reader.Find(kManifestPath);
    if (!manifest_entry)
    {
        message = "备份缺少 manifest.json。";
        return false;
    }
    std::string manifest_text;
    if (!reader.ExtractToMemory(*manifest_entry, manifest_text, kMaxManifestBytes, message))
        return false;
    std::vector<ManifestItem> parsed;
    if (!ParseManifest(manifest_text, parsed, message))
        return false;

    std::map<std::string, const user_data_zip::Entry *> entries;
    std::size_t manifest_count = 0;
    for (const user_data_zip::Entry &entry : reader.Entries())
    {
        if (CaseFoldPath(entry.path) == kManifestPath)
        {
            if (entry.path != kManifestPath)
            {
                message = "备份包含大小写冲突的 manifest 路径。";
                return false;
            }
            ++manifest_count;
            continue;
        }
        if (!IsAllowedDataPath(entry.path))
        {
            message = "备份包含不允许的路径：" + entry.path;
            return false;
        }
        const std::string key = CaseFoldPath(entry.path);
        if (!entries.emplace(key, &entry).second)
        {
            message = "备份包含重复或大小写冲突的路径：" + entry.path;
            return false;
        }
    }
    if (manifest_count != 1)
    {
        message = "备份包含重复的 manifest.json。";
        return false;
    }
    if (entries.size() != parsed.size())
    {
        message = "manifest.json 与备份实际内容不一致。";
        return false;
    }

    for (const ManifestItem &item : parsed)
    {
        const auto found = entries.find(CaseFoldPath(item.path));
        if (found == entries.end())
        {
            message = "备份缺少 manifest 中列出的文件：" + item.path;
            return false;
        }
        const user_data_zip::Entry &entry = *found->second;
        if (entry.size != item.size || entry.crc != item.crc)
        {
            message = "备份条目与 manifest 记录不一致：" + item.path;
            return false;
        }
        // Import skips this full pass because it extracts every entry into the
        // staging directory before the Server is stopped, and ExtractTo verifies
        // the CRC as it writes. Validate() is the standalone API and keeps it.
        if (verify_contents && !reader.Verify(entry, message))
            return false;
    }

    for (const auto &path_entry : entries)
    {
        const std::string &path = path_entry.first;
        std::size_t position = 0;
        while ((position = path.find('/', position)) != std::string::npos)
        {
            if (entries.find(path.substr(0, position)) != entries.end())
            {
                message = "备份包含文件与目录同名冲突：" + path;
                return false;
            }
            ++position;
        }
    }

    items = std::move(parsed);
    return true;
}

bool IsDataPath(const std::string &path)
{
    return path == "config.toml" || path == "stats.db" || path == "msime_user.db" || HasPrefix(path, "skins/") ||
           HasPrefix(path, "models/");
}

std::filesystem::path TargetPath(const Context &context, const std::string &path)
{
    if (path == "config.toml")
        return context.config_dir / L"config.toml";
    return context.data_dir / Utf8Path(path);
}

bool HasSuffix(const std::string &value, const char *suffix)
{
    const std::size_t length = std::strlen(suffix);
    return value.size() >= length && value.compare(value.size() - length, length, suffix) == 0;
}

// A model directory keeps download scratch files (.part), atomic-write scratch
// files (.tmp) and the logical-deletion marker (.trash) next to the payload.
// Scratch bytes are never restorable, and .trash is runtime deletion bookkeeping
// rather than user payload, so all three stay out of the archive. Pending updates
// (.gram.new / .remote.new) and the remote identities are real state and stay.
bool IsModelTransientFile(const std::filesystem::path &path)
{
    const std::string name = LowerAscii(path.filename().u8string());
    return name == ".trash" || HasSuffix(name, ".part") || HasSuffix(name, ".tmp");
}

bool IsSkippedPath(const std::filesystem::path &path, const std::vector<std::filesystem::path> &skip)
{
    for (const std::filesystem::path &candidate : skip)
    {
        if (!candidate.empty() && SameNormalizedPath(path, candidate))
            return true;
    }
    return false;
}

bool AddDirectoryToArchive(user_data_zip::Writer &writer, const std::filesystem::path &root, const std::string &prefix,
                           bool skip_default_skin, bool skip_model_transients, std::vector<ManifestItem> &items,
                           std::string &message, const std::vector<std::filesystem::path> &skip = {})
{
    std::error_code error;
    const bool present = std::filesystem::exists(root, error);
    if (error)
    {
        // An unreadable directory must not look like a missing one: the archive
        // would silently lose every skin or model below it.
        message = "无法读取备份目录：" + PathToUtf8(root) + "（" + error.message() + "）";
        return false;
    }
    if (!present)
        return true;
    if (!std::filesystem::is_directory(root, error))
    {
        message = "备份源不是目录：" + PathToUtf8(root);
        return false;
    }
    std::filesystem::recursive_directory_iterator iterator(root, std::filesystem::directory_options::none, error);
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end)
    {
        if (error)
        {
            message = "读取目录失败：" + PathToUtf8(root) + "（" + error.message() + "）";
            return false;
        }
        const std::filesystem::directory_entry &entry = *iterator;
        const std::filesystem::file_status status = entry.symlink_status(error);
        if (error)
        {
            message = "读取文件状态失败：" + PathToUtf8(entry.path());
            return false;
        }
        if (std::filesystem::is_symlink(status) || IsReparsePoint(entry.path()))
        {
            if (std::filesystem::is_directory(status))
                iterator.disable_recursion_pending();
            iterator.increment(error);
            continue;
        }
        if (skip_default_skin && iterator.depth() == 0 && CaseFoldPath(entry.path().filename().u8string()) == "default")
        {
            iterator.disable_recursion_pending();
            iterator.increment(error);
            continue;
        }
        if (!entry.is_regular_file(error))
        {
            if (error)
            {
                message = "读取文件状态失败：" + PathToUtf8(entry.path());
                return false;
            }
            iterator.increment(error);
            continue;
        }
        // The fixed layout only knows skins/<name>/... and models/<id>/...; a stray file
        // directly under either directory is not part of a skin or a model. Adding it
        // would make the archive fail its own path whitelist at the final validation.
        if (iterator.depth() == 0)
        {
            iterator.increment(error);
            continue;
        }
        // The export writes its temporary ZIP next to the user's chosen destination.
        // A destination inside models/ or skins/ must not swallow its own partial
        // file, and a destination from an earlier export must not be archived again.
        if (IsSkippedPath(entry.path(), skip))
        {
            iterator.increment(error);
            continue;
        }
        if (skip_model_transients && IsModelTransientFile(entry.path()))
        {
            iterator.increment(error);
            continue;
        }
        const std::string relative = entry.path().lexically_relative(root).generic_u8string();
        const std::string archive_path = prefix + relative;
        if (!user_data_zip::IsSafeArchivePath(archive_path))
        {
            message = "文件名无法安全写入备份：" + archive_path;
            return false;
        }
        user_data_zip::Entry added;
        if (!writer.AddFile(archive_path, entry.path(), message, &added))
            return false;
        items.push_back({added.path, added.size, added.crc});
        iterator.increment(error);
    }
    if (error)
    {
        message = "读取目录失败：" + PathToUtf8(root) + "（" + error.message() + "）";
        return false;
    }
    return true;
}

std::filesystem::path TemporaryDirectory(const std::filesystem::path &data_dir, const wchar_t *prefix)
{
    return data_dir / (std::wstring(prefix) + std::to_wstring(CurrentProcessId32()));
}

struct DirectoryCleanup
{
    ~DirectoryCleanup()
    {
        if (!path.empty())
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
    std::filesystem::path path;
};

bool ReadArchiveFile(const user_data_zip::Entry &entry, const std::filesystem::path &staging, std::string &content,
                     std::string &message)
{
    const std::filesystem::path path = staging / Utf8Path(entry.path);
    return ReadSmallFile(path, kMaxDictionaryBytes, content, message);
}

const ManifestItem *FindItem(const std::vector<ManifestItem> &items, const std::string &path)
{
    for (const ManifestItem &item : items)
    {
        if (item.path == path)
            return &item;
    }
    return nullptr;
}
} // namespace

bool IsAllowedDataPath(const std::string &path)
{
    if (!user_data_zip::IsSafeArchivePath(path))
        return false;
    if (path == "config.toml" || path == "stats.db" || path == "msime_user.db")
        return true;
    if (HasPrefix(path, "skins/"))
    {
        const std::string rest = path.substr(6);
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size())
            return false;
        if (CaseFoldPath(rest.substr(0, slash)) == "default")
            return false;
        // skins/<name>/<file...>: the name must be a directory, not a file.
        return true;
    }
    if (HasPrefix(path, "models/"))
    {
        const std::string rest = path.substr(7);
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size())
            return false;
        // models/<id>/<file...>: the id must be a directory, not a file.
        return true;
    }
    if (HasPrefix(path, "dictionary/"))
    {
        const std::string rest = path.substr(11);
        for (const char *dictionary : kDictionaries)
        {
            if (rest == std::string(dictionary) + ".txt")
                return true;
        }
        return false;
    }
    return false;
}

Context DefaultContext()
{
    return {std::filesystem::path(CommonUtils::get_ime_data_path_w()),
            std::filesystem::path(CommonUtils::get_ime_config_dir_w())};
}

Hooks ProductionHooks()
{
    Hooks hooks;
    hooks.stop_server = [](std::string &message) { return StopServer(message); };
    hooks.start_server = [](std::string &message) { return StartWatchdog(message); };
    hooks.export_dictionary = [](const std::string &dictionary, std::string &content, std::string &message) {
        return ExportDictionaryViaSettings(dictionary, content, message);
    };
    hooks.import_dictionary = [](const std::string &dictionary, const std::string &content, std::string &message) {
        return ImportDictionaryViaSettings(dictionary, content, message);
    };
    return hooks;
}

bool Validate(const std::filesystem::path &archive, std::string &message)
{
    std::vector<ManifestItem> items;
    return ValidateArchive(archive, items, message, true);
}

bool Export(const std::filesystem::path &destination, bool include_models, const Hooks &hooks, std::string &message)
{
    return Export(DefaultContext(), destination, include_models, hooks, message);
}

namespace
{
bool ExportImpl(const Context &context, const std::filesystem::path &destination, bool include_models,
                const Hooks &hooks, const ConfigTransform &transform_config, bool include_user_database,
                std::string &message)
{
    message.clear();
    if (destination.empty() || context.data_dir.empty())
    {
        message = "备份路径无效。";
        return false;
    }
    // Never let a backup replace the live configuration or statistics database; the
    // save dialog starts elsewhere, but the user can navigate anywhere.
    if (SameNormalizedPath(destination, context.data_dir / L"stats.db") ||
        SameNormalizedPath(destination, context.data_dir / L"msime_user.db") ||
        SameNormalizedPath(destination, context.config_dir / L"config.toml"))
    {
        message = "不能把备份写到正在使用的配置文件、统计数据库或用户词库上。";
        return false;
    }
    if (!EnsureDirectory(destination.parent_path(), message))
        return false;

    DirectoryCleanup staging;
    staging.path = TemporaryDirectory(context.data_dir, L".user-data-export-");
    std::error_code error;
    std::filesystem::remove_all(staging.path, error);
    if (!EnsureDirectory(staging.path, message))
        return false;

    const std::filesystem::path temporary = destination.parent_path() / (destination.filename().wstring() + L".tmp-" +
                                                                         std::to_wstring(CurrentProcessId32()));
    std::filesystem::remove(temporary, error);

    user_data_zip::Writer writer(temporary);
    std::vector<ManifestItem> items;
    user_data_zip::Entry added;

    const std::filesystem::path config = context.config_dir / L"config.toml";
    if (std::filesystem::is_regular_file(config, error) && !error)
    {
        if (transform_config)
        {
            // 同步上传时用清洗后的版本：内容先进内存，再作为字节写入归档。
            std::string text;
            if (!ReadSmallFile(config, 16 * 1024 * 1024, text, message))
                return false;
            const std::string transformed = transform_config(text);
            if (!writer.AddBytes("config.toml", transformed, message, &added))
                return false;
        }
        else if (!writer.AddFile("config.toml", config, message, &added))
        {
            return false;
        }
        items.push_back({added.path, added.size, added.crc});
    }
    error.clear();

    const std::filesystem::path stats = context.data_dir / L"stats.db";
    if (std::filesystem::is_regular_file(stats, error) && !error)
    {
        const std::filesystem::path snapshot = staging.path / L"stats.db";
        if (!SnapshotSqlite(stats, snapshot, "统计数据库", message))
            return false;
        if (!writer.AddFile("stats.db", snapshot, message, &added))
            return false;
        items.push_back({added.path, added.size, added.crc});
    }
    error.clear();

    // 同步恢复要靠日志里的删除墓碑与 weight_edited，所以同步导出要把整份用户词库一起打包。
    // 普通备份不带它：那份文件由安装器当作不可替代的用户数据保护，塞进可覆盖的归档里
    // 反而增加误覆盖面。
    if (include_user_database)
    {
        const std::filesystem::path user_database = context.data_dir / L"msime_user.db";
        if (std::filesystem::is_regular_file(user_database, error) && !error)
        {
            const std::filesystem::path snapshot = staging.path / L"msime_user.db";
            if (!SnapshotSqlite(user_database, snapshot, "用户词库", message))
                return false;
            if (!writer.AddFile("msime_user.db", snapshot, message, &added))
                return false;
            items.push_back({added.path, added.size, added.crc});
        }
        error.clear();
    }

    if (hooks.export_dictionary)
    {
        for (const char *dictionary : kDictionaries)
        {
            std::string content;
            std::string dictionary_message;
            if (!hooks.export_dictionary(dictionary, content, dictionary_message))
            {
                // "Nothing to export" is a normal empty dictionary. Any other
                // failure means the archive would silently lose entries the user
                // asked to back up, so fail the export instead.
                if (dictionary_message.rfind("当前没有可导出", 0) == 0)
                    continue;
                message = std::string("导出") + dictionary + "词库失败：" +
                          (dictionary_message.empty() ? "未知错误" : dictionary_message);
                return false;
            }
            if (content.empty())
                continue;
            if (!writer.AddBytes(std::string("dictionary/") + dictionary + ".txt", content, message, &added))
                return false;
            items.push_back({added.path, added.size, added.crc});
        }
    }

    const std::vector<std::filesystem::path> exported_paths{temporary, destination};
    if (!AddDirectoryToArchive(writer, context.data_dir / L"skins", "skins/", true, false, items, message,
                               exported_paths))
        return false;
    if (include_models && !AddDirectoryToArchive(writer, context.data_dir / L"models", "models/", false, true, items,
                                                 message, exported_paths))
        return false;

    std::sort(items.begin(), items.end(),
              [](const ManifestItem &left, const ManifestItem &right) { return left.path < right.path; });
    if (!writer.AddBytes(kManifestPath, SerializeManifest(items), message, nullptr))
        return false;
    if (!writer.Finish(message))
        return false;

    if (!Validate(temporary, message))
    {
        std::filesystem::remove(temporary, error);
        return false;
    }

    ClearReadOnly(destination);
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const DWORD move_error = GetLastError();
        std::filesystem::remove(temporary, error);
        message = "无法写入备份文件：" + PathToUtf8(destination) + ErrorSuffix(move_error);
        return false;
    }
    if (!Validate(destination, message))
    {
        std::filesystem::remove(destination, error);
        return false;
    }
    message = "已导出用户数据备份（" + std::to_string(items.size()) + " 个文件）。";
    return true;
}
} // namespace

bool Export(const Context &context, const std::filesystem::path &destination, bool include_models, const Hooks &hooks,
            std::string &message)
{
    return ExportImpl(context, destination, include_models, hooks, {}, false, message);
}

bool ExportSanitized(const Context &context, const std::filesystem::path &destination, bool include_models,
                     const Hooks &hooks, const ConfigTransform &transform_config, std::string &message)
{
    // 同步导出：配置清洗 + 带上用户词库，让恢复端能按日志合并。
    return ExportImpl(context, destination, include_models, hooks, transform_config, true, message);
}

bool Import(const std::filesystem::path &source, const Hooks &hooks, std::string &message)
{
    return Import(DefaultContext(), source, hooks, message);
}

namespace
{
// 从归档里取 config.toml，和本地那份按键合并。凭据键保留本地值：
// 同步归档在上传前被剥离过凭据，整包覆盖会清空本机的 token 与密码。
std::string MergeArchivedConfigImpl(const Context &context, const std::string &local_text,
                                    const std::string &archived_text)
{
    // 合并靠 template 判断「远端这一项是不是还是出厂默认」。设置进程的配置目录里就有
    // 出厂模板，读不到就传空串：MergeConfig 会退化成「远端与本地不同就采纳远端」，
    // 凭据键仍然被跳过，比整包覆盖安全。
    std::string template_text;
    const std::filesystem::path template_path = context.config_dir / L"config.default.toml";
    std::error_code error;
    if (std::filesystem::is_regular_file(template_path, error))
    {
        std::string ignored;
        if (!ReadSmallFile(template_path, 16 * 1024 * 1024, template_text, ignored))
            template_text.clear();
    }
    if (!ime_config_detail::TomlTextIsParseable(template_text))
        template_text.clear();
    return user_data_sync::MergeConfig(local_text, archived_text, template_text);
}

// 归档里的凭据字面量在本机能不能用：空串、出厂占位符、以及解不开的 DPAPI 密文都算不能用。
// 换机恢复时归档带的是另一台机器的密文，本机 Unprotect 会失败；从没填过 token 的机器
// 导出的则是 <YOUR_...> / FAKESECRET_ 这类占位符，照抄过来等于把本机的真 token 换掉。
bool ArchivedCredentialIsUsable(const std::string &literal)
{
    if (literal.size() < 2 || literal.front() != '"' || literal.back() != '"')
        return false;
    const std::string value = literal.substr(1, literal.size() - 2);
    if (value.empty())
        return false;
    // 与 VoiceInput::IsPlaceholderToken 同一套判据。这里不直接调它：那个函数在
    // voice-input 目标里，本文件不该为了一个字符串判断多拉一个依赖。
    if (value.front() == '<' && value.back() == '>')
        return false;
    if (value.rfind("FAKESECRET_", 0) == 0)
        return false;
    if (CredentialProtection::IsProtected(value))
        return CredentialProtection::Unprotect(value).has_value();
    return true;
}

// 普通备份导入用：把归档里为空或解不开的凭据键替换成本机的值，其余键保持归档原样。
// 这样"整体恢复"的语义不变，同时不会因为归档被清洗过、或来自另一台机器而清空 token。
std::string PreserveLocalCredentials(const std::string &local_text, const std::string &archived_text)
{
    if (local_text.empty() || !ime_config_detail::TomlTextIsParseable(local_text) ||
        !ime_config_detail::TomlTextIsParseable(archived_text))
        return archived_text;

    const std::map<std::string, std::string> local_values = ime_config_detail::ParseTomlAssignments(local_text);
    const std::map<std::string, std::string> archived_values = ime_config_detail::ParseTomlAssignments(archived_text);
    std::string patched = archived_text;
    for (const auto &[id, local_value] : local_values)
    {
        const std::size_t separator = id.find('\x01');
        if (separator == std::string::npos)
            continue;
        const std::string key = id.substr(separator + 1);
        if (!ime_config_detail::IsCredentialKey(key))
            continue;
        const auto archived = archived_values.find(id);
        if (archived == archived_values.end())
            continue;
        // 归档里的值能解开、且不是空串，就尊重归档（用户可能就是要恢复旧 token）。
        if (ArchivedCredentialIsUsable(archived->second))
            continue;
        if (local_value.empty() || local_value == "\"\"")
            continue;
        (void)ime_config_detail::ReplaceTomlValuePreservingFormatting(patched, id.substr(0, separator), key,
                                                                      local_value);
    }
    return patched;
}

// 把用户词库日志回放到 msime.db。合并只改日志，不回放的话删除墓碑与远端权重
// 在下次安装前都不影响候选。
//
// 两个主库都必须已经在磁盘上：replay 内部对 english.db 用的是 SQLITE_OPEN_CREATE，
// 文件不在时会顺手建一个空库并返回成功，英语词条就此变成「装着但没词」。
// 缺库时把 report.replay_skipped 置真并返回 true：恢复本身是成功的，只是回放留到
// 下次安装；真正的回放失败才返回 false。
bool ReplayUserDictionary(const Context &context, MergeImportReport *report, std::string &message)
{
    const std::filesystem::path main_database = context.data_dir / L"msime.db";
    const std::filesystem::path english_database = context.data_dir / L"english.db";
    std::error_code error;
    if (!std::filesystem::is_regular_file(main_database, error) ||
        !std::filesystem::is_regular_file(english_database, error))
    {
        if (report)
            report->replay_skipped = true;
        message.clear();
        return true;
    }
    const std::string user_db = PathToUtf8(context.data_dir / L"msime_user.db");
    const user_dictionary::ReplayResult result =
        user_dictionary::replay(user_db, PathToUtf8(main_database), PathToUtf8(english_database));
    if (result.failed || !result.error.empty())
    {
        message = "词库日志已合并，但回放失败：" + result.error;
        return false;
    }
    message.clear();
    return true;
}

// Import 的实现体。merge 为真时走同步恢复的合并语义：配置按键合并、凭据保留本地值。
// 普通备份文件导入用 merge = false，配置与统计整包覆盖。
// 用户词库不受 merge 影响：只要归档带 msime_user.db，两种模式都按日志合并。
bool ImportImpl(const Context &context, const std::filesystem::path &source, const Hooks &hooks, bool merge,
                MergeImportReport *report, std::string &message);
} // namespace

std::string MergeArchivedConfig(const std::string &local_text, const std::string &archived_text,
                                const std::string &template_text)
{
    return user_data_sync::MergeConfig(local_text, archived_text, template_text);
}

bool ImportMerging(const std::filesystem::path &source, const Hooks &hooks, MergeImportReport &report,
                   std::string &message)
{
    return ImportMerging(DefaultContext(), source, hooks, report, message);
}

bool ImportMerging(const Context &context, const std::filesystem::path &source, const Hooks &hooks,
                   MergeImportReport &report, std::string &message)
{
    report = {};
    return ImportImpl(context, source, hooks, true, &report, message);
}

bool Import(const Context &context, const std::filesystem::path &source, const Hooks &hooks, std::string &message)
{
    return ImportImpl(context, source, hooks, false, nullptr, message);
}

namespace
{
bool ImportImpl(const Context &context, const std::filesystem::path &source, const Hooks &hooks, bool merge,
                MergeImportReport *report, std::string &message)
{
    message.clear();
    std::vector<ManifestItem> items;
    // Import extracts every entry into staging before stopping the Server, and
    // ExtractTo verifies the CRC while writing, so this pass only has to check the
    // structure and the manifest. Reading every model twice would double the cost.
    if (!ValidateArchive(source, items, message, false))
        return false;

    DirectoryCleanup staging;
    staging.path = TemporaryDirectory(context.data_dir, L".user-data-import-");
    std::error_code error;
    std::filesystem::remove_all(staging.path, error);
    if (!EnsureDirectory(staging.path, message))
        return false;

    user_data_zip::Limits limits;
    limits.max_entry_size = kMaxEntrySize;
    limits.max_total_size = kMaxTotalSize;
    limits.max_entries = kMaxEntries;
    user_data_zip::Reader reader(source, limits);
    if (!reader.Open(message))
        return false;
    for (const user_data_zip::Entry &entry : reader.Entries())
    {
        if (CaseFoldPath(entry.path) == kManifestPath)
            continue;
        const std::filesystem::path target = staging.path / Utf8Path(entry.path);
        if (!EnsureDirectory(target.parent_path(), message))
            return false;
        if (!reader.ExtractTo(entry, target, message))
            return false;
    }

    const std::filesystem::path staged_config = staging.path / L"config.toml";
    if (std::filesystem::is_regular_file(staged_config, error) && !error)
    {
        std::string text;
        if (!ReadSmallFile(staged_config, 16 * 1024 * 1024, text, message))
            return false;
        try
        {
            (void)toml::parse(text);
        }
        catch (const toml::parse_error &)
        {
            message = "config.toml 不是有效的 TOML 配置，导入已取消。";
            return false;
        }
        if (merge)
        {
            // 同步恢复：和本地配置按键合并，凭据键保留本地值。归档里的 config.toml
            // 在上传前被剥离过凭据，整包覆盖会清空本机的 token 与同步密码。
            std::string local_text;
            const std::filesystem::path live_config = context.config_dir / L"config.toml";
            std::error_code local_error;
            if (std::filesystem::is_regular_file(live_config, local_error) && !local_error)
            {
                std::string ignored;
                if (!ReadSmallFile(live_config, 16 * 1024 * 1024, local_text, ignored))
                    local_text.clear();
            }
            const std::string merged = MergeArchivedConfigImpl(context, local_text, text);
            // 合并结果可用就用它。不可用时分两种：
            // - 本地配置是好的：合并本身出了问题（比如插入键时撞上格式特殊的节头）。保留本地
            //   原样，不能退回归档原文——那份的凭据在上传前已被清空，退回去本机 token 全没了。
            // - 本地配置缺失或损坏：新机器（或配置刚被删）只能用归档那份，否则永远无法从
            //   云端恢复；仍尽量用本地残存的凭据补上空值。
            const bool local_usable = !local_text.empty() && ime_config_detail::TomlTextIsParseable(local_text);
            std::string result;
            if (!merged.empty() && ime_config_detail::TomlTextIsParseable(merged))
                result = merged;
            else if (local_usable)
                result = local_text;
            else
            {
                const std::string patched = PreserveLocalCredentials(local_text, text);
                result = patched.empty() ? text : patched;
            }
            {
                std::ofstream out(staged_config, std::ios::binary | std::ios::trunc);
                if (!out)
                {
                    message = "无法写入合并后的配置。";
                    return false;
                }
                out << result;
            }
            if (report)
                report->config_merged = true;
        }
        else
        {
            // 普通备份导入是"整体恢复"，但凭据不能一起恢复：用户如果把同步归档从浏览器
            // 下载回来手动导入，那份 config.toml 是剥离过凭据的，覆盖会把 token 清空。
            // 只用本机的非空凭据补上归档里的空值，其余键照旧整体覆盖。
            std::string local_text;
            const std::filesystem::path live_config = context.config_dir / L"config.toml";
            std::error_code local_error;
            if (std::filesystem::is_regular_file(live_config, local_error) && !local_error)
            {
                std::string ignored;
                if (!ReadSmallFile(live_config, 16 * 1024 * 1024, local_text, ignored))
                    local_text.clear();
            }
            const std::string patched = PreserveLocalCredentials(local_text, text);
            if (!patched.empty() && patched != text)
            {
                // 写不进去就中止：继续往下会把剥过凭据的原文落位，本机 token 被清空。
                std::ofstream out(staged_config, std::ios::binary | std::ios::trunc);
                if (out)
                    out << patched;
                if (!out)
                {
                    message = "无法写入保留本机凭据后的配置，导入已取消。";
                    return false;
                }
            }
        }
    }
    error.clear();

    const std::filesystem::path staged_stats = staging.path / L"stats.db";
    if (std::filesystem::is_regular_file(staged_stats, error) && !error)
    {
        std::ifstream header(staged_stats, std::ios::binary);
        std::array<char, 16> magic{};
        header.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        if (header.gcount() != static_cast<std::streamsize>(magic.size()) ||
            std::memcmp(magic.data(), "SQLite format 3\0", magic.size()) != 0)
        {
            message = "stats.db 不是有效的 SQLite 文件，导入已取消。";
            return false;
        }
    }
    error.clear();

    // 用户词库的合并放到停 Server 之后（见下）。这里只确认归档里那一份是有效的 SQLite，
    // 免得等到停了 Server 才发现备份坏了。
    const std::filesystem::path staged_user_db = staging.path / L"msime_user.db";
    const bool has_user_database = std::filesystem::is_regular_file(staged_user_db, error) && !error;
    // 归档里带 msime_user.db 时，无论普通导入还是同步恢复，这一份都只能合并、不能覆盖。
    // 它是操作日志：整份覆盖会把本机独有的词条从日志里抹掉。那些词条此时还在 msime.db，
    // 但下次安装重建词库并回放时就静默消失了，而 .backup-before-import 只留到下一次导入。
    // 用户把同步包从浏览器下载回来手动导入，走的正是普通导入这条路径。
    const bool merge_user_dictionary = has_user_database;
    if (has_user_database)
    {
        std::ifstream header(staged_user_db, std::ios::binary);
        std::array<char, 16> magic{};
        header.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        if (header.gcount() != static_cast<std::streamsize>(magic.size()) ||
            std::memcmp(magic.data(), "SQLite format 3\0", magic.size()) != 0)
        {
            message = "备份里的 msime_user.db 不是有效的 SQLite 文件，恢复已取消。";
            return false;
        }
    }
    error.clear();

    bool stop_attempted = false;
    if (hooks.stop_server)
    {
        stop_attempted = true;
        std::string stop_message;
        if (!hooks.stop_server(stop_message))
        {
            if (hooks.start_server)
            {
                std::string ignored;
                (void)hooks.start_server(ignored);
            }
            message = stop_message.empty() ? "停止输入法失败，导入未开始。" : stop_message;
            return false;
        }
    }

    // Only touch the live databases when the archive replaces them. The checkpoint must run
    // after the Server stopped and before the pre-import copy: stats.db and msime_user.db are
    // both WAL, and the Server exits with ExitProcess, so their -wal survives the stop. Replacing
    // the database while that WAL is still on disk would let SQLite replay the old pages into it.
    if (FindItem(items, "stats.db") != nullptr)
    {
        if (!CheckpointLiveDatabase(context.data_dir / L"stats.db", "统计数据库", message))
        {
            if (stop_attempted && hooks.start_server)
            {
                std::string ignored;
                (void)hooks.start_server(ignored);
            }
            return false;
        }
    }
    if (FindItem(items, "msime_user.db") != nullptr)
    {
        if (!CheckpointLiveDatabase(context.data_dir / L"msime_user.db", "用户词库", message))
        {
            if (stop_attempted && hooks.start_server)
            {
                std::string ignored;
                (void)hooks.start_server(ignored);
            }
            return false;
        }
    }

    const std::filesystem::path backup_root = context.data_dir / L".backup-before-import";
    error.clear();
    if (std::filesystem::exists(backup_root, error))
    {
        std::filesystem::remove_all(backup_root, error);
        if (error)
        {
            message = "无法清理旧的导入前副本：" + PathToUtf8(backup_root) + "（" + error.message() + "）";
            if (stop_attempted && hooks.start_server)
            {
                std::string ignored;
                (void)hooks.start_server(ignored);
            }
            return false;
        }
    }
    if (!EnsureDirectory(backup_root, message))
    {
        if (stop_attempted && hooks.start_server)
        {
            std::string ignored;
            (void)hooks.start_server(ignored);
        }
        return false;
    }

    std::vector<std::string> failures;
    for (const ManifestItem &item : items)
    {
        if (!IsDataPath(item.path))
            continue;
        const std::filesystem::path target = TargetPath(context, item.path);
        if (std::filesystem::is_regular_file(target, error))
        {
            if (!CopyFileAtomic(target, backup_root / Utf8Path(item.path), message))
            {
                failures.push_back(message);
                break;
            }
        }
        else if (std::filesystem::exists(target, error))
        {
            failures.push_back("目标路径不是普通文件：" + PathToUtf8(target));
            break;
        }
        error.clear();
    }

    // 恢复流程会 replay 改 msime.db 与 english.db；普通导入在归档带用户词库时也 replay。
    // 这两个主库不在归档里、也不会被落位，但同样需要能回滚，所以单独补一份进预导入副本。
    // 用 SnapshotSqlite 而不是 CopyFileAtomic：它们是 WAL，Server 又是 ExitProcess 退出，
    // 直接拷主文件会漏掉 -wal 里已提交的页面。
    //
    // 拿不到这份副本就中止恢复：replay 会直接写这两个主库，没有副本的写入是不可回滚的。
    // 宁可这次恢复不做，也不留下一个「失败之后只能靠重装」的状态。
    const bool will_replay = has_user_database;
    if (failures.empty() && will_replay)
    {
        for (const wchar_t *name : {L"msime.db", L"english.db"})
        {
            const std::filesystem::path live_path = context.data_dir / name;
            std::error_code copy_error;
            if (!std::filesystem::is_regular_file(live_path, copy_error))
                continue;
            const char *label = name[0] == L'm' ? "拼音/五笔词库" : "英文词库";
            if (!SnapshotSqlite(live_path, backup_root / name, label, message))
            {
                failures.push_back(message);
                break;
            }
        }
    }

    if (failures.empty())
    {
        for (const ManifestItem &item : items)
        {
            if (!IsDataPath(item.path))
                continue;
            // 归档带用户词库日志时不能由它覆盖本地那份：一旦覆盖，后面的
            // MergeUserDictionary 就是在拿远端快照跟自己合并，本机独有的词条、
            // weight_edited 与删除墓碑会被静默丢掉。普通导入与同步恢复都一样。
            if (merge_user_dictionary && item.path == "msime_user.db")
                continue;
            // 同步恢复不动 stats.db：统计还没有按设备分账，整份替换就是用别的设备的统计
            // 抹掉本机的。分账落地之前，统计只随普通导入（用户自己选的完整备份）恢复。
            if (merge && item.path == "stats.db")
                continue;
            const std::filesystem::path staged = staging.path / Utf8Path(item.path);
            const std::filesystem::path target = TargetPath(context, item.path);
            std::string apply_message;
            if (!CopyFileAtomic(staged, target, apply_message))
                failures.push_back(apply_message);
        }
    }

    // 归档带用户词库时按日志合并进本地那份，而不是覆盖。普通导入也走这里：
    // 同步包被手动导入时，用户同样不该丢掉本机独有的词条。
    //
    // 顺序很关键。合并必须发生在「停 Server 之后、备份之后」：
    // - 之前合并要打开正在被 Server 写入的库，引擎自己的 busy_timeout 只有 200ms，
    //   合并期间的调频写入会被直接丢弃；
    // - 备份之后合并，被改动的词库已经进了 .backup-before-import，失败时可以拿
    //   那份副本手动恢复。
    // 合并结果还要回放到 msime.db 才会影响候选，所以这里接着调 replay。
    bool replay_skipped = false;
    if (failures.empty() && merge_user_dictionary)
    {
        const std::filesystem::path live_user_db = context.data_dir / L"msime_user.db";
        std::error_code user_error;
        user_data_sync::DictionaryMergeReport merge_report;
        bool merged_ok = true;
        if (!std::filesystem::is_regular_file(live_user_db, user_error))
        {
            // 本地还没有用户词库：直接落位即可，没有需要合并的。
            merged_ok = CopyFileAtomic(staged_user_db, live_user_db, message);
        }
        else
        {
            merged_ok = user_data_sync::MergeUserDictionary(live_user_db, staged_user_db, merge_report, message);
            if (report && merged_ok)
            {
                report->dictionary_taken_from_remote = merge_report.taken_from_remote;
                report->dictionary_kept_local = merge_report.kept_local;
                report->dictionary_added = merge_report.added;
                report->dictionary_conflicts = merge_report.conflicts;
                report->latin_words = merge_report.latin_words;
            }
        }
        if (!merged_ok)
        {
            failures.push_back(message);
        }
        else
        {
            if (report)
                report->dictionary_merged = true;
            // 日志合并完还要回放，否则删除墓碑与远端权重在下次安装前都不生效。
            // 普通导入没有 report 参数，用一个本地对象接 replay_skipped，好在
            // 成功文案里把「要等下次安装」说清楚。
            MergeImportReport local_report;
            MergeImportReport *replay_report = report ? report : &local_report;
            std::string replay_message;
            if (!ReplayUserDictionary(context, replay_report, replay_message))
                failures.push_back(replay_message);
            else
                replay_skipped = replay_report->replay_skipped;
        }
    }

    if (failures.empty())
    {
        // 归档带日志时跳过 dictionary/*.txt：那批文本带着远端设备的权重，导入会用
        // record_user_insert 把刚按 updated_at 判过的新旧关系冲掉。日志已经能完整表达。
        // 只有当归档确实带着日志时才跳过；没带日志的归档仍然要靠这批文本恢复词条。
        if (!merge_user_dictionary)
        {
            for (const char *dictionary : kDictionaries)
            {
                const std::string archive_path = std::string("dictionary/") + dictionary + ".txt";
                if (!FindItem(items, archive_path))
                    continue;
                const user_data_zip::Entry *entry = reader.Find(archive_path);
                std::string content;
                std::string dictionary_message;
                if (!entry || !ReadArchiveFile(*entry, staging.path, content, dictionary_message))
                {
                    failures.push_back(dictionary_message);
                    continue;
                }
                if (!hooks.import_dictionary)
                {
                    failures.push_back(std::string("缺少词库导入接口：") + dictionary);
                    continue;
                }
                if (!hooks.import_dictionary(dictionary, content, dictionary_message))
                    failures.push_back(dictionary_message.empty() ? std::string("词库导入失败：") + dictionary
                                                                  : dictionary_message);
            }
        }
    }

    bool start_ok = true;
    std::string start_message;
    if (stop_attempted)
    {
        if (!hooks.start_server)
        {
            // A stop without a matching start leaves the input method down; that
            // must never be reported as a successful import.
            start_ok = false;
            start_message = "导入后无法重新启动输入法，请手动启动水杉输入法。";
        }
        else
        {
            start_ok = hooks.start_server(start_message);
        }
    }

    if (start_ok && failures.empty())
    {
        message = "用户数据已导入。";
        if (replay_skipped)
            message += "部分词条改动要等下次安装时才生效。";
        return true;
    }

    if (!start_ok)
        failures.push_back(start_message.empty() ? "输入法未自动启动，请手动启动水杉输入法。" : start_message);
    message = "导入未完全成功：";
    for (std::size_t index = 0; index < failures.size(); ++index)
    {
        if (index)
            message += "；";
        message += failures[index];
    }
    message += "。导入前副本保留在 " + PathToUtf8(backup_root) + "。";
    return false;
}
} // namespace

} // namespace SettingsUserDataBackup
