#include "candidate_diag_log.h"

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "config/ime_config.h"
#include "diagnostic_log_naming.h"
#include "msime_version.h"
#include "utils/common_utils.h"

namespace
{
constexpr unsigned long long kMaxLogBytes = 4ull * 1024ull * 1024ull;

// Shared budget for both channels. The folder is a diagnostic aid, not an
// archive, and every session adds one or two files.
constexpr size_t kMaxLogFiles = 10;

// How many names to try when another process already wrote a file for the same
// second before giving up and keeping the name of the last attempt.
constexpr unsigned kMaxNameAttempts = 100;

struct ChannelState
{
    bool resolved = false;
    std::wstring path;
};

std::mutex g_log_mutex;
bool g_directory_resolved = false;
std::wstring g_log_directory;
ChannelState g_server_channel;
ChannelState g_tsf_channel;

std::wstring EnsureDirectory(const std::wstring &path)
{
    if (path.empty())
        return {};
    if (!CreateDirectoryW(path.c_str(), nullptr))
    {
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return {};
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            return {};
    }
    return path;
}

const std::wstring &ResolveLogDirectoryUnlocked()
{
    if (g_directory_resolved)
        return g_log_directory;
    g_directory_resolved = true;

    // The data directory follows the location picked during setup, so the logs
    // stay next to the dictionaries instead of in the user's own folders. It is
    // deliberately not the Desktop: that folder belongs to the user, may be
    // synced to OneDrive and hides when it is redirected.
    const std::wstring data_directory = CommonUtils::get_ime_data_path_w();
    if (!data_directory.empty())
    {
        // A development profile can point the data directory at a folder that
        // does not exist yet; create it before the logs subfolder.
        EnsureDirectory(data_directory);
        g_log_directory = EnsureDirectory(DiagnosticLogNaming::DirectoryFor(data_directory));
        if (!g_log_directory.empty())
            return g_log_directory;
    }

    // An offline redirected profile can leave the data directory unavailable;
    // diagnostics must still land somewhere the user can find and send.
    wchar_t temp_path[MAX_PATH]{};
    const DWORD temp_length = GetTempPathW(MAX_PATH, temp_path);
    if (temp_length != 0 && temp_length < MAX_PATH)
        g_log_directory = EnsureDirectory(std::wstring(temp_path) + L"metasequoiaime-logs");
    return g_log_directory;
}

bool RotateIfOversizedUnlocked(const std::wstring &path)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
        return false;
    const unsigned long long size =
        (static_cast<unsigned long long>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    if (size < kMaxLogBytes)
        return false;

    const std::wstring previous = path + L".1";
    DeleteFileW(previous.c_str());
    return MoveFileW(path.c_str(), previous.c_str()) != FALSE;
}

std::wstring Timestamp()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buffer[40]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u", static_cast<unsigned>(now.wYear),
               static_cast<unsigned>(now.wMonth), static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour),
               static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond),
               static_cast<unsigned>(now.wMilliseconds));
    return buffer;
}

void AppendRecordUnlocked(const std::wstring &path, const std::wstring &line)
{
    const std::wstring record = Timestamp() + L" [p" + std::to_wstring(GetCurrentProcessId()) + L":t" +
                                std::to_wstring(GetCurrentThreadId()) + L"] " + line + L"\r\n";
    const std::string utf8 = wstring_to_string(record);

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    if (GetLastError() != ERROR_ALREADY_EXISTS)
    {
        const char bom[] = "\xEF\xBB\xBF";
        DWORD written = 0;
        WriteFile(file, bom, 3, &written, nullptr);
    }

    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
}

std::wstring SessionHeaderLine()
{
    const std::wstring server_state = GetConfiguredDiagnosticLogEnabled() ? L"on" : L"off";
    const std::wstring tsf_state = GetConfiguredTsfDiagnosticLogEnabled() ? L"on" : L"off";
    return L"[session] version=" + string_to_wstring(MSIME_PRODUCT_VERSION) + L" server_log=" + server_state +
           L" tsf_log=" + tsf_state;
}

// Deletes the oldest files once the folder holds more than the budget. Only
// names carrying our prefix are touched; dictionary-build.log and anything the
// user put there stays. Files still in use by either channel are skipped:
// removing one would silently restart that session's file.
void PruneOldLogsUnlocked(const std::wstring &directory)
{
    WIN32_FIND_DATAW entry{};
    const std::wstring pattern = directory + L"\\" + DiagnosticLogNaming::kFilePrefix + L"*";
    HANDLE find = FindFirstFileW(pattern.c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE)
        return;

    std::vector<std::wstring> names;
    do
    {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            continue;
        names.emplace_back(entry.cFileName);
    } while (FindNextFileW(find, &entry));
    FindClose(find);

    // The current file is not on disk yet when a channel opens, so leaving one
    // slot for it keeps the folder at the budget after the first write.
    if (names.size() < kMaxLogFiles)
        return;
    std::sort(names.begin(), names.end());
    size_t excess = names.size() + 1 - kMaxLogFiles;
    for (const std::wstring &name : names)
    {
        if (excess == 0)
            break;
        const std::wstring path = directory + L"\\" + name;
        if (path == g_server_channel.path || path == g_tsf_channel.path)
            continue;
        if (DeleteFileW(path.c_str()))
            --excess;
    }
}

std::wstring ResolveChannelPathUnlocked(ChannelState &state, const wchar_t *channel)
{
    if (state.resolved)
        return state.path;
    state.resolved = true;

    const std::wstring &directory = ResolveLogDirectoryUnlocked();
    if (directory.empty())
        return state.path;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    for (unsigned sequence = 1; sequence <= kMaxNameAttempts; ++sequence)
    {
        const std::wstring candidate = directory + L"\\" + DiagnosticLogNaming::FileNameFor(channel, now, sequence);
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            state.path = candidate;
            break;
        }
    }
    if (state.path.empty())
        return state.path;

    PruneOldLogsUnlocked(directory);
    return state.path;
}

void WriteToChannelUnlocked(ChannelState &state, const wchar_t *channel, const std::wstring &line)
{
    const bool opening = !state.resolved;
    const std::wstring path = ResolveChannelPathUnlocked(state, channel);
    if (path.empty())
        return;
    // A rotated file starts empty, so it needs the session header again.
    const bool rotated = RotateIfOversizedUnlocked(path);
    if (opening || rotated)
        AppendRecordUnlocked(path, SessionHeaderLine());
    AppendRecordUnlocked(path, line);
}
} // namespace

namespace DiagnosticLog
{
bool IsEnabled()
{
    return GetConfiguredDiagnosticLogEnabled();
}

void Write(const std::wstring &line)
{
    try
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        WriteToChannelUnlocked(g_server_channel, DiagnosticLogNaming::kServerChannel, line);
    }
    catch (...)
    {
        // Diagnostics must never affect the input path.
    }
}

void WriteTsf(const std::wstring &line)
{
    try
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        WriteToChannelUnlocked(g_tsf_channel, DiagnosticLogNaming::kTsfChannel, line);
    }
    catch (...)
    {
        // Diagnostics must never affect the input path.
    }
}
} // namespace DiagnosticLog
