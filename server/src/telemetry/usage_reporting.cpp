#include "telemetry/usage_reporting.h"

#include "config/ime_config.h"
#include "msime_version.h"
#include "telemetry/usage_reporting_store.h"
#include "utils/common_utils.h"
#include "utils/network_proxy.h"

#include <Windows.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace usage_reporting
{
namespace
{
constexpr wchar_t kHost[] = L"api.msime.app";
constexpr wchar_t kEventsPath[] = L"/v1/telemetry/events";
constexpr wchar_t kDirectoryName[] = L"MetasequoiaImeTelemetry";
constexpr DWORD kTimeoutMs = 10000;
constexpr auto kFlushInterval = std::chrono::minutes(30);
// 开关由任意一个重载配置的线程更新，后台线程每分钟看一次：关掉后最迟一分钟清空队列，打开后最迟一分钟补发。
// 关掉之后的发送由 Flush 在每次请求前再查一次开关挡住，不依赖这个间隔。
constexpr auto kConsentPollInterval = std::chrono::minutes(1);

std::atomic<bool> g_started{false};

std::string ReadRetryAfter(HINTERNET request)
{
    wchar_t value[64]{};
    DWORD size = sizeof(value);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX, value, &size,
                             WINHTTP_NO_HEADER_INDEX))
        return {};
    std::string text;
    for (const wchar_t *ch = value; *ch; ++ch)
    {
        // 秒数形式只有 ASCII 数字；其他字符原样换成占位，交给 ParseRetryAfter 判为不可用。
        text.push_back(*ch < 0x80 ? static_cast<char>(*ch) : '?');
    }
    return text;
}

// POST 一条事件。不带任何凭据和 Cookie，不跟随重定向。
SendResult PostEvent(const std::string &body)
{
    SendResult result;
    HINTERNET session = NetworkProxy::OpenWinHttpSession(L"MetasequoiaImeServer/1.0");
    if (!session)
        return result;
    WinHttpSetTimeouts(session, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    HINTERNET connection = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"POST", kEventsPath, nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                   : nullptr;
    if (request)
    {
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
        DWORD no_cookies = WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &no_cookies, sizeof(no_cookies));
    }
    static constexpr wchar_t kHeaders[] = L"Content-Type: application/json\r\n";
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    const DWORD length = static_cast<DWORD>(body.size());
    if (request &&
        WinHttpSendRequest(request, kHeaders, static_cast<DWORD>(-1L), const_cast<char *>(body.data()), length, length,
                           0) &&
        WinHttpReceiveResponse(request, nullptr) &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
    {
        result = ClassifyStatus(status, ParseRetryAfter(ReadRetryAfter(request)));
    }
    if (request)
        WinHttpCloseHandle(request);
    if (connection)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}

void Run(std::filesystem::path directory)
{
    Store store(std::move(directory));
    const std::string version = MSIME_PRODUCT_VERSION;
    // 第一次循环时不知道上次进程退出前开关是什么状态。
    // 关着就先清一次，覆盖 Server 没在跑时被关掉的情况。
    bool cleared = false;
    bool was_enabled = false;
    auto next_flush = std::chrono::steady_clock::now();
    for (;;)
    {
        const bool enabled = GetConfiguredUsageReportingEnabled();
        if (enabled)
        {
            cleared = false;
            // 刚打开时立刻补发，和一次 Server 启动等价。
            if (!was_enabled || std::chrono::steady_clock::now() >= next_flush)
            {
                const auto now = std::chrono::system_clock::now();
                store.RecordActive(version, now);
                store.Flush(PostEvent, now, [] { return GetConfiguredUsageReportingEnabled(); });
                next_flush = std::chrono::steady_clock::now() + kFlushInterval;
            }
        }
        else if (!cleared)
        {
            cleared = store.Clear();
        }
        was_enabled = enabled;
        std::this_thread::sleep_for(kConsentPollInterval);
    }
}
} // namespace

std::filesystem::path DefaultDirectory()
{
    const std::wstring local_app_data = CommonUtils::get_local_appdata_path_w();
    if (local_app_data.empty())
        return {};
    return std::filesystem::path(local_app_data) / kDirectoryName;
}

void Start()
{
    if (g_started.exchange(true))
        return;
    std::filesystem::path directory = DefaultDirectory();
    if (directory.empty() || !directory.is_absolute())
        return;
    // 线程跟着进程活着，退出时直接 detach：没发出去的事件留在队列里，下次启动再发。
    std::thread(Run, std::move(directory)).detach();
}
} // namespace usage_reporting
