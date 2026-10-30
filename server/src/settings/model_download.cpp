#include "model_download.h"

#include "utils/network_proxy.h"

#include <Windows.h>
#include <ShObjIdl.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")

namespace model_download
{

namespace
{

std::string Sha256Hex(const std::filesystem::path &file)
{
    std::ifstream input(file, std::ios::binary);
    if (!input)
        return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return {};
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0)
    {
        bool ok = true;
        std::vector<char> buffer(1 << 20);
        while (ok && input)
        {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize read = input.gcount();
            if (read > 0)
                ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(read), 0) == 0;
        }
        unsigned char digest[32] = {};
        if (ok && input.eof() && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
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
    return hex;
}

std::string Utf8FromWide(const wchar_t *wide)
{
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string narrow(bytes > 0 ? static_cast<size_t>(bytes) - 1 : 0, '\0');
    if (bytes > 1)
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow.data(), bytes, nullptr, nullptr);
    return narrow;
}

bool IsBusy(bool downloading, bool importing)
{
    return downloading || importing;
}

} // namespace

std::filesystem::path ModelDownloader::Directory() const
{
    return ModelFile().parent_path();
}

std::filesystem::path ModelDownloader::PartFile() const
{
    // 同一卷上的临时名，校验通过后原子 rename 到位。
    return Directory() / (std::string(spec_.file) + ".part");
}

void ModelDownloader::SetState(State state, int progress, std::string error)
{
    std::lock_guard lock(mutex_);
    state_ = state;
    progress_ = progress;
    error_ = std::move(error);
}

void ModelDownloader::Fail(std::string error)
{
    SetState(State::Error, 0, std::move(error));
    std::error_code cleanup;
    std::filesystem::remove(PartFile(), cleanup);
}

// PartFile 写好之后的公共尾段：校验 -> 原子落位 -> NOTICE。下载与导入都走这里，
// 所以「文件存在 = 曾通过校验」对两条来源同样成立。
void ModelDownloader::InstallPart(bool imported)
{
    std::string verify_error;
    if (!Verify(PartFile(), verify_error))
    {
        Fail(verify_error);
        return;
    }
    std::error_code rename_error;
    std::filesystem::rename(PartFile(), ModelFile(), rename_error);
    if (rename_error)
    {
        Fail("模型落位失败");
        return;
    }
    std::ofstream notice(Directory() / "NOTICE.md", std::ios::binary | std::ios::trunc);
    if (notice)
    {
        notice << "# " << spec_.title << "\n\n"
               << "- 来源：" << SourceUrl() << (imported ? "（用户自行下载后本地导入）" : "") << "\n"
               << "- 许可：" << spec_.license << "\n"
               << "- 文件：" << spec_.file << "（" << spec_.size << " 字节，SHA256 " << spec_.sha256 << "）\n";
        if (spec_.notice[0] != '\0')
            notice << "\n" << spec_.notice << "\n";
    }
    SetState(State::Idle, 100, {});
}

void ModelDownloader::DownloadThread()
{
    std::error_code fs_error;
    std::filesystem::create_directories(Directory(), fs_error);
    if (fs_error)
    {
        SetState(State::Error, 0, "无法创建模型目录");
        return;
    }

    HINTERNET session = NetworkProxy::OpenWinHttpSession(L"MetasequoiaImeServer/1.0");
    if (session == nullptr)
    {
        SetState(State::Error, 0, "无法初始化网络");
        return;
    }
    WinHttpSetTimeouts(session, 30000, 30000, 30000, 0);
    HINTERNET connection = WinHttpConnect(session, spec_.host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = nullptr;
    if (connection != nullptr)
    {
        request = WinHttpOpenRequest(connection, L"GET", spec_.path, nullptr, WINHTTP_NO_REFERER,
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
        out.open(PartFile(), std::ios::binary | std::ios::trunc);
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
        if (received > spec_.size)
        {
            ok = false;
            break;
        }
        // 总长是钉死的，进度不必依赖响应是否声明 Content-Length。
        SetState(State::Downloading, static_cast<int>((std::min)(100LL, received * 100LL / spec_.size)), {});
    }
    if (request != nullptr)
        WinHttpCloseHandle(request);
    if (connection != nullptr)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    out.close();

    if (!ok)
    {
        Fail(status != 0 && status != HTTP_STATUS_OK ? "下载响应异常" : "下载中断");
        return;
    }
    InstallPart(false);
}

// 先复制再校验，而不是直接校验原文件：落位的 rename 必须在同一卷上原子完成，原文件
// 可能在别的盘，也不该被挪走。
void ModelDownloader::ImportThread(std::filesystem::path source)
{
    std::error_code fs_error;
    std::filesystem::create_directories(Directory(), fs_error);
    if (fs_error)
    {
        SetState(State::Error, 0, "无法创建模型目录");
        return;
    }
    std::filesystem::copy_file(source, PartFile(), std::filesystem::copy_options::overwrite_existing, fs_error);
    if (fs_error)
    {
        Fail("无法读取所选文件");
        return;
    }
    InstallPart(true);
}

// 下载与导入的公共前置。占位成功返回 true；已就绪或同类任务已在进行时 satisfied 置 true。
bool ModelDownloader::ClaimSlot(State state, bool &satisfied)
{
    satisfied = false;
    std::error_code fs_error;
    // 删除待重启时文件仍被 Server 映射着，新文件落不了位，重启前不接新的下载或导入。
    if (std::filesystem::exists(Directory() / kPendingDeleteMarker, fs_error))
        return false;
    // 落位只在校验之后发生，文件存在即曾通过校验，重下只会白白覆盖一份好文件。
    if (std::filesystem::exists(ModelFile(), fs_error) && !fs_error)
    {
        satisfied = true;
        return false;
    }
    std::lock_guard lock(mutex_);
    if (IsBusy(state_ == State::Downloading, state_ == State::Importing))
    {
        satisfied = state_ == state;
        return false;
    }
    state_ = state;
    progress_ = 0;
    error_.clear();
    return true;
}

std::string ModelDownloader::SourceUrl() const
{
    return "https://" + Utf8FromWide(spec_.host) + Utf8FromWide(spec_.path);
}

bool ModelDownloader::Verify(const std::filesystem::path &file, std::string &error) const
{
    std::error_code fs_error;
    const auto size = std::filesystem::file_size(file, fs_error);
    if (fs_error || static_cast<long long>(size) != spec_.size)
    {
        error = std::string("文件大小与 ") + spec_.source + " 不符";
        return false;
    }
    if (Sha256Hex(file) != spec_.sha256)
    {
        error = std::string("文件校验和与 ") + spec_.source + " 不符";
        return false;
    }
    return true;
}

ModelStatus ModelDownloader::GetStatus()
{
    {
        // runtime 的下载与错误态优先于磁盘：刚失败的一次比旧文件还在更值得看见。
        std::lock_guard lock(mutex_);
        if (state_ == State::Downloading)
            return {"downloading", progress_, {}};
        if (state_ == State::Importing)
            return {"importing", 0, {}};
        if (state_ == State::Error)
            return {"error", 0, error_};
    }
    std::error_code fs_error;
    if (std::filesystem::exists(Directory() / kPendingDeleteMarker, fs_error))
        return {"absent", 0, {}, true};
    if (std::filesystem::exists(ModelFile(), fs_error) && !fs_error)
        return {"ready", 100, {}};
    return {"absent", 0, {}};
}

bool ModelDownloader::StartDownload()
{
    bool satisfied = false;
    if (!ClaimSlot(State::Downloading, satisfied))
        return satisfied;
    std::thread(&ModelDownloader::DownloadThread, this).detach();
    return true;
}

bool ModelDownloader::Import(const std::filesystem::path &source)
{
    std::error_code fs_error;
    if (!std::filesystem::is_regular_file(source, fs_error) || fs_error)
        return false;
    bool satisfied = false;
    if (!ClaimSlot(State::Importing, satisfied))
        return satisfied;
    std::thread(&ModelDownloader::ImportThread, this, source).detach();
    return true;
}

std::filesystem::path ModelDownloader::PromptForFile(HWND owner) const
{
    // 设置窗口的 UI 线程启动时已初始化过 STA，这里的 CoInitializeEx 只是配平引用计数。
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::filesystem::path selected;
    IFileOpenDialog *dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        const COMDLG_FILTERSPEC filters[] = {{spec_.filter, spec_.pattern}, {L"所有文件", L"*.*"}};
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        dialog->SetTitle(spec_.prompt);
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

bool ModelDownloader::Delete()
{
    {
        std::lock_guard lock(mutex_);
        if (IsBusy(state_ == State::Downloading, state_ == State::Importing))
            return false;
        // 删除也清掉上一次的错误态，页面回到「未下载」。
        state_ = State::Idle;
        progress_ = 0;
        error_.clear();
    }
    std::error_code error;
    std::filesystem::remove_all(Directory(), error);
    if (!std::filesystem::exists(ModelFile(), error))
        return true;
    if (!spec_.deferred_delete)
        return false;
    // 文件被 Server 映射着：留标记，Server 立即停用它，下次启动时删掉。
    std::ofstream marker(Directory() / kPendingDeleteMarker, std::ios::binary | std::ios::trunc);
    return static_cast<bool>(marker);
}

} // namespace model_download
