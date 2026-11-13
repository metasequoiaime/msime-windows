#include "settings/remote_store.h"
#include "settings/sync_merge.h"
#include "settings/sync_service.h"
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// 编排层的离线验证：设备命名空间、快照 key 规则、保留与下载校验。
// 真实 WebDAV 与 S3 的端到端验证需要网络，放在手工回归里。

namespace
{
namespace fs = std::filesystem;

fs::path TempDir(const char *name)
{
    const fs::path root = test::TempRoot() / "user-data-sync" / name;
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root, error);
    return root;
}

// 内存里的假 store，用来测编排层而不碰网络。内容按字节存在内存里，
// 这样 ListDevices 能读到真实的设备元信息 JSON。
class FakeStore final : public user_data_sync::RemoteStore
{
  public:
    bool Put(const std::string &key, const fs::path &file, const user_data_sync::ProgressCallback &,
             std::string &message) override
    {
        std::ifstream input(file, std::ios::binary);
        if (!input)
        {
            message = "missing file";
            return false;
        }
        objects_[key] = std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        return true;
    }
    bool Get(const std::string &key, const fs::path &file, const user_data_sync::ProgressCallback &,
             std::string &message) override
    {
        const auto found = objects_.find(key);
        if (found == objects_.end())
        {
            message = "云端没有这个文件";
            return false;
        }
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(found->second.data(), static_cast<std::streamsize>(found->second.size()));
        return true;
    }
    bool Stat(const std::string &key, user_data_sync::RemoteObject &object, std::string &) override
    {
        const auto found = objects_.find(key);
        if (found == objects_.end())
            return false;
        object.key = key;
        object.size = found->second.size();
        return true;
    }
    bool List(const std::string &prefix, std::vector<user_data_sync::RemoteObject> &objects, std::string &) override
    {
        objects.clear();
        for (const auto &[key, content] : objects_)
        {
            if (key.rfind(prefix, 0) == 0)
                objects.push_back({key, content.size(), {}, 0});
        }
        return true;
    }
    bool Delete(const std::string &key, std::string &) override
    {
        objects_.erase(key);
        return true;
    }
    bool Probe(std::string &) override
    {
        return true;
    }
    bool EnsureDirectory(const std::string &, std::string &) override
    {
        return true;
    }

    void Seed(const std::string &key, const std::string &content)
    {
        objects_[key] = content;
    }
    bool Has(const std::string &key) const
    {
        return objects_.count(key) != 0;
    }

  private:
    std::map<std::string, std::string> objects_;
};
} // namespace

TEST_CASE(device_and_snapshot_keys_round_trip)
{
    const std::string device = "0123456789abcdef0123456789abcdef";
    REQUIRE_EQ(user_data_sync::DeviceInfoKey(device), std::string("devices/") + device + ".json");
    REQUIRE_EQ(user_data_sync::DeviceIdFromInfoKey(user_data_sync::DeviceInfoKey(device)), device);

    const std::string snapshot = user_data_sync::SnapshotKey(device, "1700000000-1234");
    REQUIRE_EQ(snapshot, std::string("devices/") + device + "/1700000000-1234.zip");
    REQUIRE_EQ(user_data_sync::DeviceIdFromSnapshotKey(snapshot), device);

    // 不是我们生成的形状一律拒绝，避免远端返回的内容被拼进本地路径。
    REQUIRE_EQ(user_data_sync::DeviceIdFromInfoKey("devices/../evil.json"), std::string());
    REQUIRE_EQ(user_data_sync::DeviceIdFromSnapshotKey("devices/x/../../y.zip"), std::string());
    REQUIRE_EQ(user_data_sync::DeviceIdFromSnapshotKey("devices/" + device + "/a/b.zip"), std::string());
}

TEST_CASE(fake_store_upload_and_list_devices)
{
    const fs::path root = TempDir("sync-service");
    auto store = std::make_unique<FakeStore>();
    user_data_sync::SyncOptions options;
    user_data_sync::SyncService service(std::move(store), root, options);

    const user_data_sync::UploadResult uploaded = service.Upload(
        [](const fs::path &destination, std::string &message) {
            std::ofstream file(destination, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                message = "write failed";
                return false;
            }
            file << "payload";
            return true;
        },
        {});
    REQUIRE(uploaded.ok);
    REQUIRE(!uploaded.revision.empty());

    std::vector<user_data_sync::DeviceInfo> devices;
    std::string message;
    REQUIRE(service.ListDevices(devices, message));
    REQUIRE_EQ(devices.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(devices.front().device_id, service.DeviceId());
    REQUIRE_EQ(devices.front().revision, uploaded.revision);
}

namespace
{
bool WritePayload(const fs::path &destination, std::string &message)
{
    std::ofstream file(destination, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        message = "write failed";
        return false;
    }
    file << "payload";
    return true;
}
} // namespace

// 刚上传的那份无条件保留：系统时钟曾经超前过时，云端旧快照的时间戳比新上传的还大，
// 按时间淘汰会删掉设备信息正指向的对象，别的设备就再也恢复不了这台。
TEST_CASE(prune_never_deletes_the_snapshot_just_uploaded)
{
    const fs::path root = TempDir("sync-service-prune-skew");
    std::string message;
    const std::string device = user_data_sync::LoadOrCreateDeviceId(root, message);
    REQUIRE(user_data_sync::IsValidDeviceId(device));

    auto store = std::make_unique<FakeStore>();
    FakeStore *raw = store.get();
    const std::string future = user_data_sync::SnapshotKey(device, "9999999999-1");
    raw->Seed(future, "old but future-dated");
    user_data_sync::SyncOptions options;
    options.keep_snapshots = 1;
    user_data_sync::SyncService service(std::move(store), root, options);

    const user_data_sync::UploadResult uploaded = service.Upload(WritePayload, {});
    REQUIRE(uploaded.ok);
    REQUIRE(raw->Has(user_data_sync::SnapshotKey(device, uploaded.revision)));
    REQUIRE(!raw->Has(future));
}

// 先后按数值比：同一秒内进程号 12 比 9 新，按字符串比会反过来。
TEST_CASE(prune_orders_revisions_numerically)
{
    const fs::path root = TempDir("sync-service-prune-order");
    std::string message;
    const std::string device = user_data_sync::LoadOrCreateDeviceId(root, message);
    REQUIRE(user_data_sync::IsValidDeviceId(device));

    auto store = std::make_unique<FakeStore>();
    FakeStore *raw = store.get();
    const std::string older = user_data_sync::SnapshotKey(device, "1700000000-9");
    const std::string newer = user_data_sync::SnapshotKey(device, "1700000000-12");
    raw->Seed(older, "a");
    raw->Seed(newer, "b");
    user_data_sync::SyncOptions options;
    options.keep_snapshots = 2;
    user_data_sync::SyncService service(std::move(store), root, options);

    const user_data_sync::UploadResult uploaded = service.Upload(WritePayload, {});
    REQUIRE(uploaded.ok);
    REQUIRE(raw->Has(user_data_sync::SnapshotKey(device, uploaded.revision)));
    REQUIRE(raw->Has(newer));
    REQUIRE(!raw->Has(older));
}
