#pragma once

// 版本发布通道。三个通道签名方式、发布节奏和承诺都不同（见 installer/README.md），
// 所以更新检查必须知道自己在跟哪一条走：release 只跟稳定的正式版比，nightly 只跟
// nightly 通道，dev 构建不参与自动更新。
//
// 版本字符串本身就带着通道，不需要额外的配置项：正式版是 MAJOR.MINOR.PATCH，
// nightly 与 dev 由 scripts/dev-build-version.py 生成，形如
// MAJOR.MINOR.PATCH-nightly.YYYYMMDD.<短提交>。
//
// 当前只在正式版上真正下载安装包，nightly 与 dev 的判断保留在这里，是为了让
// SupportsAutoUpdate 的语义完整；见 native_settings_update_download.h 的
// IsReleaseVersion。
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace update_channel
{
enum class Kind
{
    Release,
    Nightly,
    Dev,
};

struct Version
{
    Kind kind = Kind::Release;
    bool valid = false;
    std::string text;        // 规范化后的版本号，三段数字加可选预发布标签
    std::vector<long> parts; // 主版本三段，缺失的段在比较时按 0 处理
    std::string prerelease;  // "nightly.20261022.abcdef123456" 或 "dev.…"；正式版为空
    std::string label;       // 预发布标签的第一个标识符：nightly / dev；正式版为空
};

// 解析形如 "0.7.1"、"0.7.1-nightly.20261022.abcdef123456"、"v0.7.1-dev.…" 的字符串。
// 空串、缺主版本段、非 10 进制数字一律判为无效。
bool Parse(const std::string &text, Version &version);

// 同一通道内的版本比较，返回 -1 / 0 / 1。
//
// 预发布版本的排序规则是 semver 的一个简化：主版本相同时，有预发布标签的版本排在
// 正式版之前；两个都有标签时按标识符逐段比较，数字段按数值、其它段按字典序，
// 段数少的一方算小。跨通道不定义大小关系，调用方必须先比对通道。
int Compare(const Version &left, const Version &right);

// 这次更新检查要不要参与。自动下载只在发布通道上生效：正式版跟上正式版，
// nightly 跟上 nightly，dev 构建永远不参与。
bool SupportsAutoUpdate(const Version &current);

// 判定结果对应的通道名，供界面判断要不要渲染自动下载的勾选框：
// "release" | "nightly" | ""（dev 或无法识别，不参与自动更新）。
const char *ChannelNameOf(const std::string &version);

// 从 Release 页面地址里取出 tag，用来和候选版本号交叉核对。不是 releases/tag/
// 形式的地址返回空串。
std::string TagFromReleaseUrl(const std::string &url);

// 后台下载把安装包放在数据目录的 updates/<版本>/ 下。版本号直接当目录名，所以这里
// 先挡掉路径穿越；版本号不可用或数据目录未知时返回空。
std::filesystem::path InstallerDirectory(const std::string &version);

// 后台下载的进度。设置页靠它显示「正在下载 42%（约 12 分钟）」，Server 每收到一块
// 数据就更新一次。数值只用于展示，不参与任何安全判断。
struct DownloadProgress
{
    enum class State
    {
        Idle,        // 没在下载，也没有已完成的包
        Downloading, // 正在下载
        Ready,       // 已经下好并校验通过，等待用户安装
        Failed,      // 最近一次失败，reason 里是原因
    };

    State state = State::Idle;
    std::string version;          // 目标版本号
    std::uint64_t received = 0;   // 已收字节
    std::uint64_t total = 0;      // 服务端声明的总长；0 表示没声明
    std::string reason;           // Failed 时的中文原因
    std::uint64_t updated_at = 0; // 最后一次写入的 Unix 秒，用来判断状态是不是旧的
};

// 进度文件路径：数据目录下的 updates/progress.txt。数据目录不可用时返回空。
std::filesystem::path ProgressPath();

// 读进度。文件不存在或内容损坏时返回 Idle，绝不因此报错。
DownloadProgress ReadProgress();

// 写进度。走临时文件加改名，读的一侧不会看到半截内容。
void WriteProgress(const DownloadProgress &progress);
} // namespace update_channel