// [sync] 段：用户数据同步的目标与开关。默认关闭；密码与密钥按 IsCredentialKey 走 DPAPI 加密。
#include "config/ime_config_internal.h"

#include <mutex>
#include <set>
#include <string>

namespace ime_config_detail
{
std::mutex g_sync_mutex;
SyncConfig g_sync;
} // namespace ime_config_detail

using namespace ime_config_detail;

SyncConfig GetConfiguredSync()
{
    std::lock_guard<std::mutex> lock(g_sync_mutex);
    return g_sync;
}

bool SetConfiguredSyncBool(const std::string &key, bool value)
{
    if (key != "enabled" && key != "include_models" && key != "s3_path_style")
        return false;
    if (!WriteConfiguredValue("sync", key, value ? "true" : "false"))
        return false;
    std::lock_guard<std::mutex> lock(g_sync_mutex);
    if (key == "enabled")
        g_sync.enabled = value;
    else if (key == "include_models")
        g_sync.include_models = value;
    else
        g_sync.s3_path_style = value;
    return true;
}

bool SetConfiguredSyncInt(const std::string &key, int value)
{
    if (key == "auto_upload_minutes")
    {
        // 0 表示只在设置页手动触发；上限一天一次到每小时一次都算合理，超过 1440 按 1440 处理。
        if (value < 0 || value > 1440)
            return false;
    }
    else if (key == "keep_snapshots")
    {
        if (value < 1 || value > 20)
            return false;
    }
    else
    {
        return false;
    }
    if (!WriteConfiguredValue("sync", key, std::to_string(value)))
        return false;
    std::lock_guard<std::mutex> lock(g_sync_mutex);
    if (key == "auto_upload_minutes")
        g_sync.auto_upload_minutes = value;
    else
        g_sync.keep_snapshots = value;
    return true;
}

bool SetConfiguredSyncString(const std::string &key, const std::string &value)
{
    static const std::set<std::string> kKeys = {"backend",       "webdav_url",   "webdav_username", "webdav_password",
                                                "s3_endpoint",   "s3_region",    "s3_bucket",       "s3_prefix",
                                                "s3_access_key", "s3_secret_key"};
    if (kKeys.count(key) == 0)
        return false;
    if (key == "backend" && value != "off" && value != "webdav" && value != "s3")
        return false;
    // 地址必须是 http/https 的绝对地址，空串表示未填写。协议在这里就挡掉，免得
    // file:// 之类的值一路走到传输层才失败。
    if ((key == "webdav_url" || key == "s3_endpoint") && !value.empty())
    {
        if (value.rfind("https://", 0) != 0 && value.rfind("http://", 0) != 0)
            return false;
    }
    if (!WriteConfiguredValue("sync", key, EscapeTomlBasicString(value)))
        return false;
    std::lock_guard<std::mutex> lock(g_sync_mutex);
    if (key == "backend")
        g_sync.backend = value;
    else if (key == "webdav_url")
        g_sync.webdav_url = value;
    else if (key == "webdav_username")
        g_sync.webdav_username = value;
    else if (key == "webdav_password")
        g_sync.webdav_password = value;
    else if (key == "s3_endpoint")
        g_sync.s3_endpoint = value;
    else if (key == "s3_region")
        g_sync.s3_region = value.empty() ? "us-east-1" : value; // 与加载时的归一化一致，空 region 签不出名
    else if (key == "s3_bucket")
        g_sync.s3_bucket = value;
    else if (key == "s3_prefix")
        g_sync.s3_prefix = value;
    else if (key == "s3_access_key")
        g_sync.s3_access_key = value;
    else if (key == "s3_secret_key")
        g_sync.s3_secret_key = value;
    return true;
}
