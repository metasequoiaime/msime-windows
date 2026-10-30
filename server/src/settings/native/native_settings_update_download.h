#pragma once

// 更新安装包的下载与校验。关于页点「检查更新」发现新版本后，先取清单里的
// installerSha256，再按下面的通道顺序把安装包拉到临时目录，校验通过才交给调用方
// 去问用户装不装。
//
// 通道顺序固定为 GitHub 官方在前、国内镜像在后。镜像只是加速与兜底，不保证新版本
// 一发就缓存到（它按需回源），所以 404 单独处理：直接换下一条，不占重试次数。
//
// 安装包下载只在正式版上开放。nightly 与 dev 构建不下载：nightly 的清单不由官网提供
// （msime-web 只认 MAJOR.MINOR.PATCH 的 tag，固定 tag「nightly」解析不出可比版本），
// dev 构建的版本号每天变，比较结果没有意义。见 IsReleaseVersion。
//
// 每条通道最多试 3 次。每次从头开始，不做断点续传：续传要处理服务器不支持 Range
// 的情况，风险大于收益，而一条已经证明慢的通道续下去也不会变快。
//
// 「慢」的判定是首段测速：连接建立后观察 10 秒（或收满 1 MB，先到为准），低于
// 50 KB/s 就认为这条通道不顶用，关掉换下一条，不计入失败次数。测速期间收到的字节
// 就是下载的开头，不重开会话。
//
// 另有两条兜底：全程 20 秒收不到任何字节当作这次尝试失败；任何状态码不是 200 的响应
// 都算失败，404 除外（见上）。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace native_settings::update_download
{
// 一次尝试的结局。策略层只关心这几种，具体的错误文字在 message 里。
enum class Outcome
{
    Ok,
    // 服务器说没有这个包。镜像通常是还没缓存到新版本。
    NotFound,
    // 网络、超时、状态码异常。
    Failed,
    // 收到了完整文件但摘要不对。
    Checksum,
};

struct Attempt
{
    Outcome outcome = Outcome::Failed;
    std::string message;     // 给人看的中文说明，Ok 时为空
    std::uint64_t bytes = 0; // 这次尝试收到的字节数
    std::uint64_t seconds = 0;
    std::string url;
    // 边下边算出来的 SHA-256 十六进制串，只有 Ok 时非空。
    std::string digest;
};

struct Result
{
    Outcome outcome = Outcome::Failed;
    std::string message;
    std::filesystem::path path; // Ok 时是校验通过的安装包
    std::vector<Attempt> attempts;
};

struct Request
{
    // 本机版本号。只有正式版会下载安装包，nightly 与 dev 构建在这里被挡掉，
    // 见 IsReleaseVersion。
    std::string current_version;
    // 发布页地址，用来推资产的下载地址；安装包名与摘要来自清单。
    std::string release_url;
    std::string installer_name;
    std::string installer_sha256;
    // 落盘目录（会在这里建 installer_name 与临时文件）。
    std::filesystem::path directory;
    // 一份下载最多花多久。超时就按失败收尾，不无限挂在后台。
    std::uint64_t deadline_seconds = 900;
    // 测试用：把每个通道的地址换掉。空表示用真实地址。
    std::vector<std::string> override_channel_urls;
    // 下载进度：(已收字节, 服务端声明的总长，0 表示没声明)。在下载线程上同步调用，
    // 实现必须尽快返回；重试换通道时从 0 重新计。可以为空。
    std::function<void(std::uint64_t received, std::uint64_t total)> on_progress;
};

// 按通道顺序把安装包下载并校验到 request.directory。
//
// 这个函数会阻塞，调用方必须在后台线程上跑。返回的 Result 里带着每次尝试的
// 记录，方便调用方在日志里回看到底是哪条通道、因为什么失败。
//
// 线程安全：只依赖配置里的代理设置，不在内部保存任何跨调用的状态。
Result Download(const Request &request);

// 通道地址：GitHub 官方 → 国内镜像。release_url 不是本仓库的 releases/tag 形式
// 时返回空。
std::vector<std::string> ChannelUrls(const std::string &release_url, const std::string &installer_name);

// 清单里的 installerSha256 是否合法：64 个小写十六进制字符。
bool IsSha256Hex(const std::string &text);

// 这个版本号能不能参与安装包下载。只有正式版可以：MAJOR.MINOR.PATCH，带 v/V 前缀
// 也行，不允许任何预发布标签。nightly 与 dev 构建一律返回 false。
//
// 判定只看版本号本身，不看配置，因为通道是编译进去的事实，不是用户选项。
bool IsReleaseVersion(const std::string &version);

// 安装包文件名合法性：MetasequoiaIME_Setup_v<版本>.exe，大小写不敏感。
bool IsInstallerName(const std::string &name);

// 下载目录：系统临时目录下按进程号命名的子目录。装机级的数据目录是中等完整性、
// 对所有用户可写的，从那里拉起的 exe 在「校验完成」到「用户点 UAC」之间可能被同机
// 其他进程替换，所以安装包落在临时目录，再套一层进程唯一的子目录。
std::filesystem::path DefaultDirectory();

// 清掉目录里除 keep 之外的旧安装包与临时文件。
void PruneOldInstallers(const std::filesystem::path &directory, const std::string &keep_name);

// 重新核对文件的 SHA-256。下载目录与启动安装程序之间隔着用户点 UAC 的时间，
// 这段时间里文件可能被同机的其他进程换掉，所以启动前再算一次。
bool FileMatchesDigest(const std::filesystem::path &path, const std::string &expected_hex);

// 本次进程独占的安装包文件名：MetasequoiaIME_Setup_v0.11.1.exe.<pid>-<计数>.part。
// 每个下载任务用自己的临时名，两个实例各写各的文件，互不覆盖；谁先完成谁把它改名成
// 正式名字，另一个即使稍后写完，校验与改名也只是多走一遍，不会把好东西冲掉。
std::string PartFileName(const std::string &installer_name);
} // namespace native_settings::update_download
