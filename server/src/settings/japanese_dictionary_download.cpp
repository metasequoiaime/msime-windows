#include "japanese_dictionary_download.h"

#include "engine/core/data_path.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <utility>

namespace japanese_dictionary
{
namespace
{
std::mutex g_mutex;
struct BuildProcess
{
    HANDLE handle = nullptr;
    std::string error;
    ~BuildProcess()
    {
        if (handle)
            CloseHandle(handle);
    }
} g_process;

std::filesystem::path LogFile()
{
    return metasequoia::data_directory() / "japanese-dictionary-build.log";
}

void PollProcess()
{
    if (!g_process.handle)
        return;
    DWORD code = STILL_ACTIVE;
    if (!GetExitCodeProcess(g_process.handle, &code) || code == STILL_ACTIVE)
        return;
    CloseHandle(std::exchange(g_process.handle, nullptr));
    if (code != 0)
    {
        std::ifstream log(LogFile(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
        const auto end = text.find_last_not_of("\r\n");
        if (end != std::string::npos)
        {
            text.resize(end + 1);
            const auto start = text.find_last_of("\r\n");
            g_process.error = text.substr(start == std::string::npos ? 0 : start + 1);
        }
        if (g_process.error.empty())
            g_process.error = "词库构建失败，请查看数据目录中的 japanese-dictionary-build.log";
    }
}

bool LaunchPython(const std::wstring &python, const std::wstring &arguments, HANDLE log)
{
    std::wstring command = python + L" -X utf8 " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log;
    startup.hStdError = log;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process))
        return false;
    CloseHandle(process.hThread);
    g_process.handle = process.hProcess;
    return true;
}
} // namespace

BuildStatus GetStatus()
{
    std::lock_guard lock(g_mutex);
    PollProcess();
    if (g_process.handle)
        return {"building", "正在下载并构建词库…"};
    if (!g_process.error.empty())
        return {"error", g_process.error};
    const auto directory = metasequoia::data_directory();
    std::error_code error;
    if (std::filesystem::exists(directory / "japanese.db.pending", error))
        return {"ready", "已构建，重启输入法后生效"};
    if (std::filesystem::exists(directory / "japanese.db", error))
        return {"ready", "日语词库已安装"};
    return {"absent", "尚未下载日语词库"};
}

void StartBuild()
{
    std::lock_guard lock(g_mutex);
    PollProcess();
    if (g_process.handle)
        return;
    g_process.error.clear();
    const auto directory = metasequoia::data_directory();
    const auto script = directory / "assets" / "dictionaries" / "build_japanese_dictionary.py";
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE log = CreateFileW(LogFile().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE)
    {
        g_process.error = "无法写入数据目录。请先等待后台构建结束；"
                          "若仍失败，检查权限并清理残留的 msime-japanese-* 临时源目录后重试。";
        return;
    }
    const std::wstring arguments = L"\"" + script.wstring() + L"\" --output-root \"" + directory.wstring() + L"\\.\"";
    const bool launched = LaunchPython(L"py.exe -3", arguments, log) || LaunchPython(L"python.exe", arguments, log);
    CloseHandle(log);
    if (!launched)
        g_process.error = "需要安装 Python 3.10 或以上，并将 Python 加入 PATH 后重新打开设置";
}
} // namespace japanese_dictionary
