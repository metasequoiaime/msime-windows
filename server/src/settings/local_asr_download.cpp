#include "local_asr_download.h"

#include "utils/network_proxy.h"
#include "voice-input/local_asr_catalog.h"

#include <Windows.h>
#include <ShObjIdl.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")

namespace local_asr_download
{
namespace
{

namespace fs = std::filesystem;
using local_asr::ModelEntry;
using local_asr::RemoteFile;

enum class Component
{
    Runtime,
    Vad,
    Model,
};

struct Task
{
    Component component;
    const RemoteFile *file;
    const ModelEntry *model; // Component::Model 时有效
};

enum class State
{
    Idle,
    Downloading,
    Extracting,
    Importing,
};

std::mutex g_mutex;
State g_state = State::Idle;
std::string g_active_id;
int g_progress = 0;
std::map<std::string, std::string> g_errors;

class Handle
{
  public:
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle)
    {
    }
    ~Handle()
    {
        if (valid())
            CloseHandle(handle_);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    HANDLE get() const
    {
        return handle_;
    }
    bool valid() const
    {
        return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
    }

  private:
    HANDLE handle_;
};

// 以拒绝写入、删除的共享方式打开：校验到解压结束之间，文件换不掉。
HANDLE OpenLocked(const fs::path &path, bool inheritable = false)
{
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, inheritable ? TRUE : FALSE};
    return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, &attributes, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

std::string Sha256Hex(HANDLE file)
{
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN))
        return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return {};
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0)
    {
        bool ok = true;
        std::vector<unsigned char> buffer(1 << 20);
        for (;;)
        {
            DWORD read = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            {
                ok = false;
                break;
            }
            if (read == 0)
                break;
            if (BCryptHashData(hash, buffer.data(), read, 0) != 0)
            {
                ok = false;
                break;
            }
        }
        unsigned char digest[32] = {};
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
        {
            static constexpr char kDigits[] = "0123456789abcdef";
            for (const unsigned char byte : digest)
            {
                hex.push_back(kDigits[byte >> 4]);
                hex.push_back(kDigits[byte & 0x0f]);
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
    return hex;
}

long long FileSize(HANDLE file)
{
    LARGE_INTEGER size{};
    return GetFileSizeEx(file, &size) ? size.QuadPart : -1;
}

bool Matches(HANDLE file, long long size, const char *sha256)
{
    return FileSize(file) == size && Sha256Hex(file) == sha256;
}

void SetState(State state, int progress)
{
    std::lock_guard lock(g_mutex);
    g_state = state;
    g_progress = progress;
}

void Finish(const std::string &id, std::string error)
{
    std::lock_guard lock(g_mutex);
    g_state = State::Idle;
    g_progress = 0;
    g_active_id.clear();
    if (error.empty())
        g_errors.erase(id);
    else
        g_errors[id] = std::move(error);
}

fs::path DownloadDirectory()
{
    return local_asr::Root() / ".download";
}

fs::path StagingFor(const fs::path &target)
{
    return target.parent_path() / (target.filename().wstring() + L".staging");
}

void WriteMarker(const fs::path &directory, const char *sha256)
{
    std::ofstream marker(directory / local_asr::kInstalledMarker, std::ios::binary | std::ios::trunc);
    marker << sha256 << "\n";
}

// staging 已装好、写过标记：换掉旧目录（若有）并改名到位。
bool Promote(const fs::path &staging, const fs::path &target, std::string &error)
{
    std::error_code ec;
    fs::remove_all(target, ec);
    fs::rename(staging, target, ec);
    if (ec)
    {
        error = "安装落位失败，请关闭占用该目录的程序后重试";
        return false;
    }
    return true;
}

// 用系统自带的 tar.exe（Windows 10 1803 起随系统提供，bsdtar，支持 bzip2）解出指定成员。压缩包经
// 标准输入喂给它、工作目录设为目标目录，命令行里只剩 ASCII 的成员名，数据目录含中文也不受控制台代码页影响。
bool RunTar(HANDLE archive, const fs::path &destination, int strip, const std::vector<std::string> &members,
            std::string &error)
{
    wchar_t system_directory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        error = "找不到系统目录";
        return false;
    }
    const fs::path tar = fs::path(system_directory) / L"tar.exe";
    std::error_code ec;
    if (!fs::is_regular_file(tar, ec))
    {
        error = "系统缺少 tar.exe（需要 Windows 10 1803 或更新版本）";
        return false;
    }
    std::wstring command = L"\"" + tar.wstring() + L"\" -x -j -f - --strip-components " + std::to_wstring(strip);
    for (const std::string &member : members)
        command += L" \"" + std::wstring(member.begin(), member.end()) + L"\"";

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    Handle nul(
        CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));
    if (!nul.valid() || !SetHandleInformation(archive, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
    {
        error = "无法启动解压";
        return false;
    }
    LARGE_INTEGER zero{};
    SetFilePointerEx(archive, zero, nullptr, FILE_BEGIN);

    // 只让子进程继承这两个句柄。
    HANDLE inherited[] = {archive, nul.get()};
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<unsigned char> attribute_buffer(attribute_size);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer.data());
    bool ok = InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size) &&
              UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                        nullptr, nullptr);
    PROCESS_INFORMATION process{};
    if (ok)
    {
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = archive;
        startup.StartupInfo.hStdOutput = nul.get();
        startup.StartupInfo.hStdError = nul.get();
        startup.lpAttributeList = attributes;
        ok = CreateProcessW(tar.c_str(), command.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, destination.c_str(),
                            &startup.StartupInfo, &process) != FALSE;
        DeleteProcThreadAttributeList(attributes);
    }
    SetHandleInformation(archive, HANDLE_FLAG_INHERIT, 0);
    if (!ok)
    {
        error = "无法启动解压";
        return false;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    if (exit_code != 0)
    {
        error = "解压失败，压缩包可能不完整";
        return false;
    }
    return true;
}

bool ResetStaging(const fs::path &staging, std::string &error)
{
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec)
    {
        error = "无法创建模型目录";
        return false;
    }
    return true;
}

bool InstallRuntime(HANDLE archive, std::string &error)
{
    const fs::path target = local_asr::RuntimeDirectory();
    const fs::path staging = StagingFor(target);
    if (!ResetStaging(staging, error))
        return false;
    std::vector<std::string> members;
    for (const local_asr::RuntimeLibrary &library : local_asr::RuntimeLibraries())
        members.push_back(std::string(local_asr::kRuntimeArchiveLibDirectory) + "/" + library.name);
    if (!RunTar(archive, staging, 2, members, error))
        return false;
    for (const local_asr::RuntimeLibrary &library : local_asr::RuntimeLibraries())
    {
        Handle file(OpenLocked(staging / library.name));
        if (!file.valid() || !Matches(file.get(), library.size, library.sha256))
        {
            error = std::string("运行库 ") + library.name + " 校验失败";
            return false;
        }
    }
    {
        std::ofstream notice(staging / "NOTICE.md", std::ios::binary | std::ios::trunc);
        notice << "# sherpa-onnx 1.13.8 运行库（本地语音识别）\n\n"
               << "- 来源：" << local_asr::SourceUrl(local_asr::RuntimeArchive()) << "\n"
               << "- sherpa-onnx：Apache-2.0（https://github.com/k2-fsa/sherpa-onnx）\n"
               << "- ONNX Runtime：MIT（https://github.com/microsoft/onnxruntime）\n";
    }
    WriteMarker(staging, local_asr::RuntimeArchive().sha256);
    return Promote(staging, target, error);
}

bool InstallVad(HANDLE file, std::string &error)
{
    const fs::path target = local_asr::VadDirectory();
    const fs::path staging = StagingFor(target);
    if (!ResetStaging(staging, error))
        return false;
    // 从已校验、已锁住的句柄复制，不重新按路径打开源文件。
    {
        std::ofstream out(staging / local_asr::VadModel().name, std::ios::binary | std::ios::trunc);
        LARGE_INTEGER zero{};
        SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
        std::vector<char> buffer(1 << 16);
        for (;;)
        {
            DWORD read = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            {
                error = "无法读取 silero_vad.onnx";
                return false;
            }
            if (read == 0)
                break;
            out.write(buffer.data(), read);
        }
        if (!out)
        {
            error = "无法写入 silero_vad.onnx";
            return false;
        }
    }
    {
        std::ofstream notice(staging / "NOTICE.md", std::ios::binary | std::ios::trunc);
        notice << "# Silero VAD（本地语音识别断句）\n\n"
               << "- 来源：" << local_asr::SourceUrl(local_asr::VadModel()) << "\n"
               << "- 许可：MIT（https://github.com/snakers4/silero-vad）\n";
    }
    WriteMarker(staging, local_asr::VadModel().sha256);
    return Promote(staging, target, error);
}

bool InstallModel(const ModelEntry &model, HANDLE archive, bool imported, std::string &error)
{
    const fs::path target = local_asr::ModelDirectory(model);
    const fs::path staging = StagingFor(target);
    if (!ResetStaging(staging, error))
        return false;
    std::vector<std::string> members;
    for (const local_asr::ModelFileEntry &file : model.files)
        members.push_back(std::string(model.root) + "/" + file.path);
    if (!RunTar(archive, staging, 1, members, error))
        return false;
    for (const local_asr::ModelFileEntry &file : model.files)
    {
        std::error_code ec;
        if (!fs::exists(staging / file.path, ec))
        {
            error = std::string("压缩包里缺少 ") + file.path;
            return false;
        }
    }
    {
        std::ofstream notice(staging / "NOTICE.md", std::ios::binary | std::ios::trunc);
        notice << "# " << model.title << "（本地语音识别模型）\n\n"
               << "- 来源：" << local_asr::SourceUrl(model.archive) << (imported ? "（用户自行下载后本地导入）" : "")
               << "\n"
               << "- 许可：" << model.license << "\n"
               << "- 压缩包：" << model.archive.name << "（" << model.archive.size << " 字节，SHA256 "
               << model.archive.sha256 << "）\n\n"
               << model.notice << "\n";
    }
    WriteMarker(staging, model.archive.sha256);
    return Promote(staging, target, error);
}

bool Install(const Task &task, HANDLE file, bool imported, std::string &error)
{
    bool installed = false;
    fs::path target;
    switch (task.component)
    {
    case Component::Runtime:
        target = local_asr::RuntimeDirectory();
        installed = InstallRuntime(file, error);
        break;
    case Component::Vad:
        target = local_asr::VadDirectory();
        installed = InstallVad(file, error);
        break;
    case Component::Model:
        target = local_asr::ModelDirectory(*task.model);
        installed = InstallModel(*task.model, file, imported, error);
        break;
    }
    if (!installed)
    {
        // 半成品不留：staging 从不带完成标记，留着也不会被当成已安装，但白占几百 MB。
        std::error_code ec;
        fs::remove_all(StagingFor(target), ec);
    }
    return installed;
}

std::vector<Task> MissingComponents(const ModelEntry &model)
{
    std::vector<Task> tasks;
    if (!local_asr::RuntimeInstalled())
        tasks.push_back({Component::Runtime, &local_asr::RuntimeArchive(), nullptr});
    if (local_asr::NeedsVad(model) && !local_asr::VadInstalled())
        tasks.push_back({Component::Vad, &local_asr::VadModel(), nullptr});
    if (!local_asr::ModelFilesInstalled(model))
        tasks.push_back({Component::Model, &model.archive, &model});
    return tasks;
}

// WinHTTP 下载到 destination，边下边报字节数。超出钉死的大小立即中止。
bool DownloadFile(const RemoteFile &file, const fs::path &destination,
                  const std::function<void(long long received)> &on_progress, std::string &error)
{
    HINTERNET session = NetworkProxy::OpenWinHttpSession(L"MetasequoiaImeSettings/1.0");
    if (session == nullptr)
    {
        error = "无法初始化网络";
        return false;
    }
    WinHttpSetTimeouts(session, 30000, 30000, 60000, 60000);
    HINTERNET connection = WinHttpConnect(session, file.host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = nullptr;
    if (connection != nullptr)
    {
        request = WinHttpOpenRequest(connection, L"GET", file.path, nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (request != nullptr)
        {
            // release 资产经 302 跳到 CDN。
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        }
    }
    bool ok = connection != nullptr && request != nullptr &&
              WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (ok)
    {
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
        ok = status == HTTP_STATUS_OK;
    }
    std::ofstream out;
    if (ok)
    {
        out.open(destination, std::ios::binary | std::ios::trunc);
        ok = static_cast<bool>(out);
    }
    long long received = 0;
    std::vector<char> buffer;
    while (ok)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
        {
            ok = false;
            break;
        }
        if (available == 0)
            break;
        buffer.resize(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read) || read == 0)
        {
            ok = false;
            break;
        }
        out.write(buffer.data(), static_cast<std::streamsize>(read));
        if (!out)
        {
            ok = false;
            break;
        }
        received += read;
        if (received > file.size)
        {
            ok = false;
            break;
        }
        on_progress(received);
    }
    if (request != nullptr)
        WinHttpCloseHandle(request);
    if (connection != nullptr)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    out.close();
    if (!ok)
    {
        error = status != 0 && status != HTTP_STATUS_OK ? "下载响应异常" : "下载中断";
        return false;
    }
    return true;
}

void DownloadThread(const ModelEntry *model)
{
    const std::string id = model->id;
    const std::vector<Task> tasks = MissingComponents(*model);
    long long total = 0;
    for (const Task &task : tasks)
        total += task.file->size;
    total = (std::max)(total, 1LL);
    std::error_code ec;
    fs::create_directories(DownloadDirectory(), ec);
    if (ec)
    {
        Finish(id, "无法创建模型目录");
        return;
    }
    long long done = 0;
    for (const Task &task : tasks)
    {
        const fs::path part = DownloadDirectory() / (std::string(task.file->name) + ".part");
        std::string error;
        SetState(State::Downloading, static_cast<int>(done * 100 / total));
        const bool downloaded = DownloadFile(
            *task.file, part,
            [done, total](long long received) {
                SetState(State::Downloading, static_cast<int>((std::min)(100LL, (done + received) * 100 / total)));
            },
            error);
        bool installed = false;
        if (downloaded)
        {
            Handle file(OpenLocked(part));
            if (!file.valid() || !Matches(file.get(), task.file->size, task.file->sha256))
            {
                error = std::string(task.file->name) + " 的大小或校验和与发布文件不符";
            }
            else
            {
                SetState(State::Extracting, 0);
                installed = Install(task, file.get(), false, error);
            }
        }
        fs::remove(part, ec);
        if (!installed)
        {
            fs::remove_all(DownloadDirectory(), ec);
            Finish(id, error.empty() ? "安装失败" : error);
            return;
        }
        done += task.file->size;
    }
    fs::remove_all(DownloadDirectory(), ec);
    Finish(id, {});
}

void ImportThread(std::string id, fs::path source)
{
    Handle file(OpenLocked(source));
    if (!file.valid())
    {
        Finish(id, "无法读取所选文件（是否正被其他程序写入？）");
        return;
    }
    const long long size = FileSize(file.get());
    // 先按大小筛掉明显不是的文件，免得对任意大文件整份算哈希。
    std::vector<Task> candidates;
    if (size == local_asr::RuntimeArchive().size)
        candidates.push_back({Component::Runtime, &local_asr::RuntimeArchive(), nullptr});
    if (size == local_asr::VadModel().size)
        candidates.push_back({Component::Vad, &local_asr::VadModel(), nullptr});
    for (const ModelEntry &model : local_asr::Catalog())
    {
        if (size == model.archive.size)
            candidates.push_back({Component::Model, &model.archive, &model});
    }
    const std::string sha256 = candidates.empty() ? std::string() : Sha256Hex(file.get());
    for (const Task &task : candidates)
    {
        if (sha256 != task.file->sha256)
            continue;
        SetState(State::Extracting, 0);
        std::string error;
        if (!Install(task, file.get(), true, error))
        {
            Finish(id, error);
            return;
        }
        Finish(id, {});
        return;
    }
    Finish(id, "所选文件不是本页列出的模型压缩包、运行库压缩包或 silero_vad.onnx（大小或校验和不符）");
}

// 占用单槽。已在为同一 id 下载时 satisfied 置 true。
bool Claim(const std::string &id, State state, bool &satisfied)
{
    satisfied = false;
    std::lock_guard lock(g_mutex);
    if (g_state != State::Idle)
    {
        satisfied = g_active_id == id && g_state == state;
        return false;
    }
    g_state = state;
    g_progress = 0;
    g_active_id = id;
    g_errors.erase(id);
    return true;
}

std::string MissingDetail(const ModelEntry &model)
{
    std::vector<std::string> missing;
    if (!local_asr::ModelFilesInstalled(model))
        missing.push_back("模型");
    if (!local_asr::RuntimeInstalled())
        missing.push_back("运行库");
    if (local_asr::NeedsVad(model) && !local_asr::VadInstalled())
        missing.push_back("断句模型 silero_vad");
    // 全缺就是「未下载」，不必列。
    if (missing.size() == (local_asr::NeedsVad(model) ? 3u : 2u))
        return {};
    std::string detail = "还缺";
    for (size_t i = 0; i < missing.size(); ++i)
        detail += (i == 0 ? "" : "、") + missing[i];
    return detail;
}

} // namespace

std::map<std::string, ModelStatus> GetStatuses()
{
    std::map<std::string, ModelStatus> statuses;
    State state;
    std::string active;
    int progress;
    std::map<std::string, std::string> errors;
    {
        std::lock_guard lock(g_mutex);
        state = g_state;
        active = g_active_id;
        progress = g_progress;
        errors = g_errors;
    }
    for (const ModelEntry &model : local_asr::Catalog())
    {
        ModelStatus status;
        status.model_installed = local_asr::ModelFilesInstalled(model);
        if (state != State::Idle && active == model.id)
        {
            status.state = state == State::Downloading  ? "downloading"
                           : state == State::Extracting ? "extracting"
                                                        : "importing";
            status.progress = progress;
        }
        else if (const auto found = errors.find(model.id); found != errors.end())
        {
            status.state = "error";
            status.error = found->second;
        }
        else if (local_asr::ModelReady(model))
        {
            status.state = "ready";
            status.progress = 100;
        }
        else
        {
            status.state = "absent";
            status.detail = MissingDetail(model);
        }
        statuses[model.id] = std::move(status);
    }
    return statuses;
}

bool StartDownload(const std::string &model_id)
{
    const ModelEntry *model = local_asr::FindModel(model_id);
    if (!model)
        return false;
    if (local_asr::ModelReady(*model))
        return true;
    bool satisfied = false;
    if (!Claim(model_id, State::Downloading, satisfied))
        return satisfied;
    std::thread(DownloadThread, model).detach();
    return true;
}

bool Import(const std::string &model_id, const std::filesystem::path &source)
{
    if (!local_asr::FindModel(model_id))
        return false;
    std::error_code ec;
    if (!fs::is_regular_file(source, ec) || ec)
        return false;
    bool satisfied = false;
    if (!Claim(model_id, State::Importing, satisfied))
        return false;
    std::thread(ImportThread, model_id, source).detach();
    return true;
}

std::filesystem::path PromptForFile(HWND owner)
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::filesystem::path selected;
    IFileOpenDialog *dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        const COMDLG_FILTERSPEC filters[] = {{L"模型或运行库 (*.tar.bz2; *.onnx)", L"*.tar.bz2;*.onnx"},
                                             {L"所有文件", L"*.*"}};
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        dialog->SetTitle(L"选择已下载的模型压缩包、运行库压缩包或 silero_vad.onnx");
        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options)))
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item)))
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                selected = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
        dialog->Release();
    }
    if (SUCCEEDED(init))
        CoUninitialize();
    return selected;
}

bool Delete(const std::string &model_id)
{
    const ModelEntry *model = local_asr::FindModel(model_id);
    if (!model)
        return false;
    {
        std::lock_guard lock(g_mutex);
        if (g_state != State::Idle && g_active_id == model_id)
            return false;
        g_errors.erase(model_id);
    }
    const fs::path directory = local_asr::ModelDirectory(*model);
    std::error_code ec;
    // 先删标记：即使后面有文件删不掉，这个目录也不再被当成已安装的模型。
    fs::remove(directory / local_asr::kInstalledMarker, ec);
    fs::remove_all(directory, ec);
    fs::remove_all(StagingFor(directory), ec);
    return !fs::exists(directory / local_asr::kInstalledMarker, ec);
}

} // namespace local_asr_download
