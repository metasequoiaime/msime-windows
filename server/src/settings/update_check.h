#pragma once

// 后台检查更新与自动下载安装包。跑在 Server 进程里：Server 常驻，用户关不关设置窗口
// 都不影响它，下载也就不会因为关窗口中断。
//
// 一次探测：
//   1. 本机版本必须是正式版（update_channel::SupportsAutoUpdate），否则连清单都不请求；
//   2. 读配置 appearance.auto_download_update，关着就什么都不做；
//   3. GET https://msime.app/update.json，版本必须同通道且比当前新；
//   4. 已有校验通过的包就跳过；否则交给下载器，落进数据目录的 updates/<版本>/；
//   5. 通过 UpdateInstallerReady 广播给所有已连接的 TSF 客户端，由它询问用户。
//
// 失败一律静默收敛到下一次轮询：更新检查出问题是用户处理不了的，弹窗只会打扰。

#include <cstdint>
#include <filesystem>
#include <string>

namespace update_check
{
// 探测间隔。文件几十 MB，一天几次足够；启动后先等一段时间，避开开机时的网络与
// 其它启动任务。
inline constexpr unsigned kPollIntervalMinutes = 6 * 60;
inline constexpr unsigned kStartupDelayMinutes = 2;

// 本进程链接时刻的版本号。设置页与服务端对同一台机器必须给出同一结论。
std::string CurrentVersion();

// 安装包所在版本目录：数据目录下的 updates/<版本>。版本号不可用时返回空。
std::filesystem::path InstallerDirectory(const std::string &version);

// 最近一次探测认为「比本机新」的版本号，供设置页判断要不要给提示。
// 没有可安装的包时返回空串。
std::string PendingVersion();

// 用户在弹窗里同意安装时调用。路径只从磁盘上校验过的那一份推出来，
// requested_installer 仅用来核对是不是同一个文件。
bool LaunchPendingInstaller(const std::wstring &requested_installer);

// 起一次探测。可重入：已有一轮在跑时直接返回。
void CheckOnce();

// 后台轮询线程。进程启动后创建一次，之后跟着进程活着。
void StartPolling();
} // namespace update_check
