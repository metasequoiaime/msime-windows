#include "neural_model.h"

#include "engine/core/data_path.h"
#include "utils/network_proxy.h"

#include <Windows.h>
#include <winhttp.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace neural_model
{

namespace
{

// 模型文件位置（不加扩展名，直接是目录路径）
std::filesystem::path ModelDirectory()
{
    return metasequoia::data_directory() / "models" / kDesktopModelId;
}

std::filesystem::path ModelFile()
{
    // 效果优先模型（24.9M），从 release 获取后保存为标准文件名
    return ModelDirectory() / "sentence-model-desktop.safetensors";
}

std::filesystem::path PartFile()
{
    // 下载中的临时文件，完成后 rename 到位
    return ModelDirectory() / "model.onnx.part";
}

enum class DownloadState
{
    Idle,
    Downloading,
    Error,
};

struct RuntimeState
{
    DownloadState state = DownloadState::Idle;
    int progress = 0;
    std::string error;
};

std::mutex g_mutex;
RuntimeState g_state;

void SetState(DownloadState state, int progress, std::string error)
{
    std::lock_guard lock(g_mutex);
    g_state.state = state;
    g_state.progress = progress;
    g_state.error = std::move(error);
}

// 后台线程：下载 -> 原子落位
void DownloadThread()
{
    SetState(DownloadState::Downloading, 0, {});

    std::error_code fs_error;
    const std::filesystem::path directory = ModelDirectory();
    std::filesystem::create_directories(directory, fs_error);
    if (fs_error)
    {
        SetState(DownloadState::Error, 0, "无法创建模型目录");
        return;
    }

    // 从 https://github.com/metasequoiaime/chinese-ime-lm/releases/download/model-v1/
    // 下载效果优先模型（24.9M）
    const wchar_t *host = L"github.com";
    const wchar_t *path = L"/metasequoiaime/chinese-ime-lm/releases/download/model-v1/sentence-model-desktop.safetensors";

    HINTERNET session = NetworkProxy::OpenWinHttpSession(L"MetasequoiaImeServer/1.0");
    if (session == nullptr)
    {
        SetState(DownloadState::Error, 0, "无法初始化网络");
        return;
    }
    WinHttpSetTimeouts(session, 30000, 30000, 30000, 0);
    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = nullptr;
    if (connection != nullptr)
    {
        request = WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (request != nullptr)
        {
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

    long long expected = 0;
    if (ok)
    {
        DWORD length = 0;
        DWORD length_size = sizeof(length);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &length, &length_size, WINHTTP_NO_HEADER_INDEX))
        {
            expected = length;
        }
    }

    std::ofstream out;
    if (ok)
    {
        out.open(PartFile(), std::ios::binary | std::ios::trunc);
        ok = static_cast<bool>(out);
    }

    long long received = 0;
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
        std::vector<char> buffer(available);
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
        if (expected > 0)
        {
            const int progress = static_cast<int>((std::min)(100LL, received * 100LL / expected));
            SetState(DownloadState::Downloading, progress, {});
        }
    }
    if (request != nullptr)
        WinHttpCloseHandle(request);
    if (connection != nullptr)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    out.close();

    if (!ok)
    {
        SetState(DownloadState::Error, 0,
                 status != 0 && status != HTTP_STATUS_OK ? "下载响应异常" : "下载中断");
        std::error_code cleanup;
        std::filesystem::remove(PartFile(), cleanup);
        return;
    }

    if (expected > 0 && received != expected)
    {
        SetState(DownloadState::Error, 0, "下载字节数与响应声明不符");
        std::error_code cleanup;
        std::filesystem::remove(PartFile(), cleanup);
        return;
    }

    // 原子落位
    std::error_code rename_error;
    std::filesystem::rename(PartFile(), ModelFile(), rename_error);
    if (rename_error)
    {
        SetState(DownloadState::Error, 0, "模型落位失败");
        std::error_code cleanup;
        std::filesystem::remove(PartFile(), cleanup);
        return;
    }

    SetState(DownloadState::Idle, 0, {});
}

} // namespace

ModelStatus GetModelStatus()
{
    std::lock_guard lock(g_mutex);
    ModelStatus status;
    status.state = "absent";
    status.progress = 0;
    status.error = "";

    // 磁盘优先：文件存在就是就绪
    if (std::filesystem::exists(ModelFile()))
    {
        status.state = "ready";
        return status;
    }

    // 否则返回运行时状态（可能在下载中或出过错）
    switch (g_state.state)
    {
    case DownloadState::Downloading:
        status.state = "downloading";
        status.progress = g_state.progress;
        break;
    case DownloadState::Error:
        status.state = "error";
        status.error = g_state.error;
        break;
    default:
        // Idle 且文件不存在 = absent（已上面返回过）
        break;
    }

    return status;
}

bool StartDownload()
{
    std::lock_guard lock(g_mutex);

    // 已下载或正在下载时不重复启动
    if (std::filesystem::exists(ModelFile()))
        return true;
    if (g_state.state == DownloadState::Downloading)
        return true;

    // 启动后台下载线程
    g_state.state = DownloadState::Downloading;
    g_state.progress = 0;
    g_state.error.clear();
    std::thread(DownloadThread).detach();

    return true;
}

bool DeleteModel()
{
    std::lock_guard lock(g_mutex);

    // 下载中不删除
    if (g_state.state == DownloadState::Downloading)
        return false;

    std::error_code error;
    if (std::filesystem::exists(ModelFile(), error))
    {
        std::filesystem::remove(ModelFile(), error);
        if (error)
            return false;
    }

    // 清除模型目录
    if (std::filesystem::exists(ModelDirectory(), error))
    {
        std::filesystem::remove_all(ModelDirectory(), error);
        if (error)
            return false;
    }

    g_state.state = DownloadState::Idle;
    g_state.progress = 0;
    g_state.error.clear();

    return true;
}

} // namespace neural_model
