#pragma once

// 同步编排：把传输层（RemoteStore）和语义层（sync_merge）串起来。
//
// 布局按设备分目录，每台设备只写自己那一个命名空间，多设备并发不会互相覆盖：
//
//   <根>/devices/<device-id>.json      设备元信息（主机名、最近快照、时间）
//   <根>/devices/<device-id>/<rev>.zip 快照，保留最近 N 份
//
// 读取端先 List 拿全设备清单，再决定拉哪几份。上传与下载都是整包快照，合并发生在本地，
// 远端没有"最后写入者胜"的竞争窗口。

#include "settings/remote_store.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace user_data_sync
{
struct SyncOptions
{
    // 每台设备保留的快照份数。超过就删最旧的。
    int keep_snapshots = 5;
    // 是否把 models/ 打进归档。模型动辄几百 MB，默认不带。
    bool include_models = false;
};

struct DeviceInfo
{
    std::string device_id;
    std::string hostname;
    std::string revision;
    std::int64_t uploaded_unix = 0;
    std::uint64_t size = 0;
};

struct UploadResult
{
    bool ok = false;
    std::string message;
    std::string revision;
};

struct DownloadResult
{
    bool ok = false;
    std::string message;
    // 下载到本地临时目录的归档路径。调用方负责用它做后续校验与导入。
    std::filesystem::path archive;
    // 本次列举到的全部设备，含本机。
    std::vector<DeviceInfo> devices;
};

class SyncService
{
  public:
    SyncService(std::unique_ptr<RemoteStore> store, std::filesystem::path data_dir, SyncOptions options);

    // 上传本机快照。build_archive 由调用方给：它生成一个临时 ZIP（复用现有导出逻辑，
    // 并挂上清洗凭据的钩子），返回值决定归档路径。本函数负责上传、更新设备元信息和
    // 清理旧快照。
    using ArchiveBuilder = std::function<bool(const std::filesystem::path &destination, std::string &message)>;
    UploadResult Upload(const ArchiveBuilder &build_archive, const ProgressCallback &progress);

    // 列举远端设备。
    bool ListDevices(std::vector<DeviceInfo> &devices, std::string &message);

    // 下载指定设备的最新快照到本地临时文件。device_id 为空时取最近上传的那台（不含本机）。
    DownloadResult Download(const std::string &device_id, const ProgressCallback &progress);

    // 测试连接。
    bool Probe(std::string &message);

    const std::string &DeviceId() const
    {
        return device_id_;
    }

  private:
    bool WriteDeviceInfo(const DeviceInfo &info, std::string &message);
    bool ReadDeviceInfo(const std::string &device_id, DeviceInfo &info, std::string &message);
    // 这台设备目录下实际存在的快照 revision，新的在前（按数值比，不按字符串）。
    std::vector<std::string> ListSnapshotRevisions(const std::string &device_id, std::string &message);
    // 保留最近 keep_snapshots 份；current_revision 是刚上传、设备信息正指向的那份，无条件保留。
    void PruneSnapshots(const std::string &device_id, const std::string &current_revision);

    std::unique_ptr<RemoteStore> store_;
    std::filesystem::path data_dir_;
    SyncOptions options_;
    std::string device_id_;
};

// 远端根目录下各路径的约定。暴露出来给测试与界面用，避免两处各写一遍字符串。
std::string DevicesPrefix();
std::string DeviceInfoKey(const std::string &device_id);
std::string SnapshotKey(const std::string &device_id, const std::string &revision);
// 从对象 key 里取出设备标识；不是设备元信息文件时返回空。
std::string DeviceIdFromInfoKey(const std::string &key);
// 从快照 key 里取出设备标识；不是快照时返回空。
std::string DeviceIdFromSnapshotKey(const std::string &key);
} // namespace user_data_sync
