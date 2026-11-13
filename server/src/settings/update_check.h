#pragma once

// 后台检查更新与自动下载安装包。跑在 Server 进程里：Server 常驻，用户关不关设置窗口
// 都不影响它，下载也就不会因为关窗口中断。
//
// 一次探测：
//   1. 本机版本必须是正式版（update_channel::SupportsAutoUpdate），否则连清单都不请求；
//   2. 已经装上的版本不再需要安装包：清掉 updates/ 下不比本机新的版本目录和「等待安装」状态；
//   3. 读配置 appearance.auto_download_update，关着就到此为止；
//   4. GET https://msime.app/update.json，版本必须同通道且比当前新；
//   5. 已有校验通过的包就跳过；否则交给下载器，落进数据目录的 updates/<版本>/；
//   6. 进度写成「等待安装」（updates/progress.txt）。
//
// Server 不弹窗、也不拉起安装程序：设置页「关于」读进度文件，显示「立即安装」按钮，
// 安装由用户在那里触发；「检查更新」发现同一版本已经下好时也直接复用这份安装包。
// 轮询失败静默收敛到下一次轮询：更新检查出问题是用户处理不了的。

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

// 起一次探测。可重入：已有一轮在跑时直接返回。
void CheckOnce();

// 后台轮询线程。进程启动后创建一次，之后跟着进程活着。
void StartPolling();
} // namespace update_check
