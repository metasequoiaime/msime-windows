#include "settings/sync_service.h"

#include "settings/sync_merge.h"
#include "settings/user_data_backup.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace user_data_sync
{
namespace
{
constexpr char kDevicesPrefix[] = "devices";

std::string Narrow(const std::wstring &wide)
{
    if (wide.empty())
        return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
    return text;
}

std::wstring Widen(const std::string &text)
{
    if (text.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::string HostName()
{
    wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!GetComputerNameW(buffer, &size))
        return {};
    return Narrow(std::wstring(buffer, size));
}

// 极简 JSON 取值，只认本模块自己写出去的形状（单层对象、字符串与数字值）。
// 用 boost::json 也能做，但这里的输入完全由自己生成，不需要完整的解析器。
std::string JsonString(const std::string &text, const std::string &key)
{
    const std::string needle = "\"" + key + "\"";
    std::size_t position = text.find(needle);
    if (position == std::string::npos)
        return {};
    position = text.find(':', position + needle.size());
    if (position == std::string::npos)
        return {};
    position = text.find('"', position);
    if (position == std::string::npos)
        return {};
    const std::size_t end = text.find('"', position + 1);
    if (end == std::string::npos)
        return {};
    return text.substr(position + 1, end - position - 1);
}

std::int64_t JsonInt(const std::string &text, const std::string &key)
{
    const std::string needle = "\"" + key + "\"";
    std::size_t position = text.find(needle);
    if (position == std::string::npos)
        return 0;
    position = text.find(':', position + needle.size());
    if (position == std::string::npos)
        return 0;
    ++position;
    while (position < text.size() && (text[position] == ' ' || text[position] == '\t'))
        ++position;
    std::int64_t value = 0;
    bool any = false;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9')
    {
        value = value * 10 + (text[position] - '0');
        ++position;
        any = true;
    }
    return any ? value : 0;
}

std::string JsonEscape(const std::string &value)
{
    std::string out;
    out.reserve(value.size() + 2);
    for (const char ch : value)
    {
        switch (ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(ch);
        }
    }
    return out;
}

std::string MakeRevision()
{
    // 时间戳加进程号：同一秒内两次上传也不会撞名，且字典序即时间序，方便按名字排。
    const auto now = std::chrono::system_clock::now();
    const std::int64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    return std::to_string(seconds) + "-" + std::to_string(GetCurrentProcessId());
}

// revision 只允许数字与连字符（MakeRevision 的形状）。远端 JSON 被篡改成
// "../../x" 时，UriEncode 会保留斜杠，GET 的 key 就会在远端服务端做路径穿越。
bool SafeRevision(const std::string &revision)
{
    if (revision.empty())
        return false;
    return std::all_of(revision.begin(), revision.end(),
                       [](unsigned char ch) { return (ch >= '0' && ch <= '9') || ch == '-'; });
}

std::filesystem::path TemporaryArchive(const std::filesystem::path &data_dir, const std::string &revision)
{
    return data_dir / (L".user-data-sync-" + Widen(revision) + L".zip");
}
} // namespace

std::string DevicesPrefix()
{
    return kDevicesPrefix;
}

std::string DeviceInfoKey(const std::string &device_id)
{
    return std::string(kDevicesPrefix) + "/" + device_id + ".json";
}

std::string SnapshotKey(const std::string &device_id, const std::string &revision)
{
    return std::string(kDevicesPrefix) + "/" + device_id + "/" + revision + ".zip";
}

std::string DeviceIdFromInfoKey(const std::string &key)
{
    const std::string prefix = std::string(kDevicesPrefix) + "/";
    if (key.rfind(prefix, 0) != 0)
        return {};
    const std::string rest = key.substr(prefix.size());
    if (rest.find('/') != std::string::npos)
        return {};
    if (rest.size() <= 5 || rest.compare(rest.size() - 5, 5, ".json") != 0)
        return {};
    const std::string id = rest.substr(0, rest.size() - 5);
    return IsValidDeviceId(id) ? id : std::string();
}

std::string DeviceIdFromSnapshotKey(const std::string &key)
{
    const std::string prefix = std::string(kDevicesPrefix) + "/";
    if (key.rfind(prefix, 0) != 0)
        return {};
    const std::string rest = key.substr(prefix.size());
    const std::size_t slash = rest.find('/');
    if (slash == std::string::npos)
        return {};
    const std::string id = rest.substr(0, slash);
    if (!IsValidDeviceId(id))
        return {};
    const std::string name = rest.substr(slash + 1);
    if (name.find('/') != std::string::npos || name.size() <= 4 || name.compare(name.size() - 4, 4, ".zip") != 0)
        return {};
    return id;
}

SyncService::SyncService(std::unique_ptr<RemoteStore> store, std::filesystem::path data_dir, SyncOptions options)
    : store_(std::move(store)), data_dir_(std::move(data_dir)), options_(options)
{
    std::string ignored;
    device_id_ = LoadOrCreateDeviceId(data_dir_, ignored);
}

bool SyncService::WriteDeviceInfo(const DeviceInfo &info, std::string &message)
{
    std::ostringstream json;
    json << "{\n";
    json << "  \"device_id\": \"" << JsonEscape(info.device_id) << "\",\n";
    json << "  \"hostname\": \"" << JsonEscape(info.hostname) << "\",\n";
    json << "  \"revision\": \"" << JsonEscape(info.revision) << "\",\n";
    json << "  \"uploaded_unix\": " << info.uploaded_unix << ",\n";
    json << "  \"size\": " << info.size << "\n";
    json << "}\n";
    const std::filesystem::path temporary = data_dir_ / (L".sync-device-info-" + Widen(info.device_id) + L".json");
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            message = "无法写入设备信息";
            return false;
        }
        file << json.str();
        if (!file)
        {
            message = "无法写入设备信息";
            return false;
        }
    }
    const bool ok = store_->Put(DeviceInfoKey(info.device_id), temporary, {}, message);
    std::error_code error;
    std::filesystem::remove(temporary, error);
    return ok;
}

bool SyncService::ReadDeviceInfo(const std::string &device_id, DeviceInfo &info, std::string &message)
{
    const std::filesystem::path temporary = data_dir_ / (L".sync-device-info-read-" + Widen(device_id) + L".json");
    // 失败时也要把这次的临时文件清掉，否则数据目录里会堆一地的半截 JSON。
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{temporary};
    if (!store_->Get(DeviceInfoKey(device_id), temporary, {}, message))
        return false;
    std::ifstream file(temporary, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (text.empty())
    {
        message = "设备信息为空";
        return false;
    }
    info.device_id = device_id;
    info.hostname = JsonString(text, "hostname");
    info.revision = JsonString(text, "revision");
    info.uploaded_unix = JsonInt(text, "uploaded_unix");
    info.size = static_cast<std::uint64_t>(JsonInt(text, "size"));
    return true;
}

void SyncService::PruneSnapshots(const std::string &device_id)
{
    std::vector<RemoteObject> objects;
    std::string ignored;
    if (!store_->List(std::string(kDevicesPrefix) + "/" + device_id, objects, ignored))
        return;
    std::vector<RemoteObject> snapshots;
    for (RemoteObject &object : objects)
    {
        if (DeviceIdFromSnapshotKey(object.key) == device_id)
            snapshots.push_back(std::move(object));
    }
    // 名字里带时间戳，字典序即时间序；倒序后前 keep_snapshots 个是要留下的。
    std::sort(snapshots.begin(), snapshots.end(),
              [](const RemoteObject &left, const RemoteObject &right) { return left.key > right.key; });
    const std::size_t keep = static_cast<std::size_t>((std::max)(options_.keep_snapshots, 1));
    for (std::size_t index = keep; index < snapshots.size(); ++index)
        (void)store_->Delete(snapshots[index].key, ignored);
}

UploadResult SyncService::Upload(const ArchiveBuilder &build_archive, const ProgressCallback &progress)
{
    UploadResult result;
    if (device_id_.empty())
    {
        result.message = "没有可用的设备标识";
        return result;
    }
    if (!build_archive)
    {
        result.message = "缺少归档生成器";
        return result;
    }
    const std::string revision = MakeRevision();
    const std::filesystem::path archive = TemporaryArchive(data_dir_, revision);
    std::error_code error;
    std::filesystem::remove(archive, error);
    if (!build_archive(archive, result.message))
    {
        std::filesystem::remove(archive, error);
        return result;
    }
    // WebDAV 必须先建集合，否则对不存在集合的 PUT 返回 409；S3 是空实现。
    if (!store_->EnsureDirectory(SnapshotKey(device_id_, revision), result.message))
    {
        std::filesystem::remove(archive, error);
        return result;
    }
    if (!store_->Put(SnapshotKey(device_id_, revision), archive, progress, result.message))
    {
        std::filesystem::remove(archive, error);
        return result;
    }
    // 大小与清理各用一个 error_code：清理失败不该把设备列表里的大小记成 0。
    std::error_code size_error;
    const std::uint64_t size = std::filesystem::file_size(archive, size_error);
    std::error_code remove_error;
    std::filesystem::remove(archive, remove_error);

    DeviceInfo info;
    info.device_id = device_id_;
    info.hostname = HostName();
    info.revision = revision;
    info.uploaded_unix = static_cast<std::int64_t>(std::time(nullptr));
    info.size = size_error ? 0 : size;
    // 设备信息写失败不影响快照本身可用，但要让用户知道列表可能不准。
    std::string info_message;
    if (!WriteDeviceInfo(info, info_message))
    {
        result.ok = true;
        result.revision = revision;
        result.message = "快照已上传，但设备信息更新失败：" + info_message;
        return result;
    }
    PruneSnapshots(device_id_);
    result.ok = true;
    result.revision = revision;
    result.message = "已上传备份快照";
    return result;
}

bool SyncService::ListDevices(std::vector<DeviceInfo> &devices, std::string &message)
{
    devices.clear();
    std::vector<RemoteObject> objects;
    if (!store_->List(kDevicesPrefix, objects, message))
        return false;
    std::set<std::string> seen;
    for (const RemoteObject &object : objects)
    {
        const std::string id = DeviceIdFromInfoKey(object.key);
        if (id.empty() || !seen.insert(id).second)
            continue;
        DeviceInfo info;
        std::string info_message;
        // 单个设备的元信息读不出来不该让整个列表失败，跳过它。
        if (!ReadDeviceInfo(id, info, info_message))
            continue;
        devices.push_back(std::move(info));
    }
    // 最近上传的排在前面。
    std::sort(devices.begin(), devices.end(),
              [](const DeviceInfo &left, const DeviceInfo &right) { return left.uploaded_unix > right.uploaded_unix; });
    return true;
}

DownloadResult SyncService::Download(const std::string &device_id, const ProgressCallback &progress)
{
    DownloadResult result;
    std::string list_message;
    std::vector<DeviceInfo> devices;
    if (!ListDevices(devices, list_message))
    {
        result.message = list_message;
        return result;
    }
    result.devices = devices;

    const DeviceInfo *target = nullptr;
    for (const DeviceInfo &info : devices)
    {
        if (!device_id.empty())
        {
            if (info.device_id == device_id)
            {
                target = &info;
                break;
            }
            continue;
        }
        // 未指定时取最近上传的那台，但不要自己：从自己下载没有意义，而且容易误覆盖。
        if (info.device_id != device_id_)
        {
            target = &info;
            break;
        }
    }
    if (!target)
    {
        result.message = device_id.empty() ? "云端没有其他设备的备份" : "云端没有这台设备的备份";
        return result;
    }
    if (target->revision.empty() || !SafeRevision(target->revision))
    {
        result.message = "这台设备没有可下载的快照";
        return result;
    }
    const std::filesystem::path archive =
        data_dir_ / (L".user-data-sync-download-" + Widen(target->device_id) + L".zip");
    std::error_code error;
    std::filesystem::remove(archive, error);
    if (!store_->Get(SnapshotKey(target->device_id, target->revision), archive, progress, result.message))
    {
        std::filesystem::remove(archive, error);
        return result;
    }
    // 下载完立刻验一遍：损坏的归档不能进入后面的"停 Server 再覆盖"流程。
    std::string validate_message;
    if (!SettingsUserDataBackup::Validate(archive, validate_message))
    {
        std::filesystem::remove(archive, error);
        result.message = "云端备份已损坏：" + validate_message;
        return result;
    }
    result.ok = true;
    result.archive = archive;
    result.message = "已下载备份";
    return result;
}

bool SyncService::Probe(std::string &message)
{
    if (!store_)
    {
        message = "未配置同步目标";
        return false;
    }
    return store_->Probe(message);
}
} // namespace user_data_sync
