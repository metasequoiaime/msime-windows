#include "settings/account/msime_account_client.h"

#include "utils/common_utils.h"

#include <nlohmann/json.hpp>

// The import library and the boundary header are staged next to the build by
// scripts/ci/build-host-api.ps1; the CMake target links msime_host_api.dll.lib.
#include "msime_client.h"

#include <windows.h>

#include <filesystem>
#include <vector>

namespace msime_account
{
namespace
{
namespace fs = std::filesystem;

constexpr int EntryAccount = 0;
constexpr int EntryCloudSync = 1;

// Converts a UTF-8 JSON response from the Rust side into the envelope struct.
// The returned pointer is a NUL-terminated UTF-8 JSON string owned by the
// library; copy, then free through the same ABI.
CallResult ParseEnvelope(char *response)
{
    CallResult result;
    if (response == nullptr)
    {
        result.error = "account_unavailable";
        return result;
    }
    std::string text(response);
    msime_client_string_free(response);

    try
    {
        const nlohmann::json envelope = nlohmann::json::parse(text);
        result.ok = envelope.value("ok", false);
        if (envelope.contains("error"))
        {
            result.error = envelope["error"].get<std::string>();
        }
        if (envelope.contains("retry_after_seconds"))
        {
            result.retry_after_seconds = envelope["retry_after_seconds"].get<long>();
        }
        if (result.ok && envelope.contains("value") && !envelope["value"].is_null())
        {
            result.value_json = envelope["value"].dump();
        }
    }
    catch (const nlohmann::json::exception &)
    {
        result.ok = false;
        result.error = "account_unavailable";
    }
    return result;
}

std::string NarrowPath(const std::wstring &path)
{
    // Paths cross the ABI as UTF-8 JSON strings.
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
    std::string result(bytes > 0 ? bytes : 0, '\0');
    if (bytes > 0)
    {
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), result.data(), bytes, nullptr,
                            nullptr);
    }
    return result;
}
} // namespace

CallResult CallAccount(const std::string &action_json)
{
    nlohmann::json request;
    request["directory"] = NarrowPath(AccountDirectory());
    const std::wstring legacy = LegacyAccountDirectory();
    if (!legacy.empty())
    {
        request["legacy_directory"] = NarrowPath(legacy);
    }
    request["client_agent"] = ClientAgent();
    request["action"] = nlohmann::json::parse(action_json);
    return ParseEnvelope(
        msime_client_account(reinterpret_cast<const uint8_t *>(request.dump().data()), request.dump().size()));
}

CallResult CallCloudSync(const std::string &action_json)
{
    nlohmann::json request;
    request["directory"] = NarrowPath(AccountDirectory());
    request["state_directory"] = NarrowPath(CloudSyncStateDirectory());
    request["preferences_directory"] = NarrowPath(PreferencesDirectory());
    request["action"] = nlohmann::json::parse(action_json);
    const std::string text = request.dump();
    return ParseEnvelope(msime_client_cloud_sync(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
}

std::wstring AccountDirectory()
{
    const std::wstring dir = CommonUtils::get_ime_data_path_w() + L"\\account";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::wstring LegacyAccountDirectory()
{
    // The Tauri-era shared app stored the session under its identifier; adopt
    // it on first use so users keep their login across the migration.
    const std::wstring base = CommonUtils::get_local_appdata_path_w() + L"\\app.msime.windows";
    std::error_code ec;
    if (fs::exists(fs::path(base) / L"account", ec))
    {
        return base + L"\\account";
    }
    if (fs::exists(fs::path(base) / L"account-session.json", ec))
    {
        return base;
    }
    return {};
}

std::wstring CloudSyncStateDirectory()
{
    const std::wstring dir = CommonUtils::get_ime_data_path_w();
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::wstring PreferencesDirectory()
{
    return CommonUtils::get_ime_config_dir_w();
}

std::string ClientAgent()
{
    // Shape required by host-api: msime-<platform>/<version>[ (...)]. ASCII
    // only; the version comes from this executable's VERSIONINFO resource.
    wchar_t module_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    DWORD handle = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(module_path, &handle);
    if (bytes == 0)
    {
        return "msime-windows/0.0.0";
    }
    std::vector<BYTE> data(bytes);
    if (!GetFileVersionInfoW(module_path, 0, bytes, data.data()))
    {
        return "msime-windows/0.0.0";
    }
    VS_FIXEDFILEINFO *info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID *>(&info), &info_size) || !info)
    {
        return "msime-windows/0.0.0";
    }
    wchar_t assembled[128];
    swprintf_s(assembled, L"msime-windows/%lu.%lu.%lu", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
               HIWORD(info->dwFileVersionLS));
    // Pure ASCII by construction; a plain narrowing copy is exact.
    return std::string(assembled, assembled + wcslen(assembled));
}
} // namespace msime_account
