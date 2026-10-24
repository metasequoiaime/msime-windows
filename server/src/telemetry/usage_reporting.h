#pragma once

// 匿名使用统计的后台发送，跑在 Server 进程里。
// 启动时排入今天的 active 并发送一次，之后每 30 分钟检查一次，跨过 UTC 零点的进程照样记上新的一天。
// 频率与 msime 仓里新版 Windows 客户端一致。
//
// 开关是 config.toml 里的 [general] usage_reporting，只有显式 true 才发送。
// 缺键或出厂模板里的 "ask" 表示用户还没选：不发送，也不排队，安装程序会在安装或升级时询问。
// 这一点与 msime 仓的其他客户端（缺键按开启）不同，为的是满足 SignPath 对安装时提供关闭选项的要求。
// 设置页改完不用重启。关闭后不再发出任何请求，一分钟内删掉本机队列。
// install_id 保留，重新打开时不会把同一个安装算成两次。
//
// 发送在一条从不 join 的后台线程上进行，WinHTTP 每一步都有 10 秒超时，走 [network] 配置的代理。
// 网络不通或服务端出错只会让事件留在队列里等下一次，不会拖慢输入，也不会拖住 Server 退出。

#include <filesystem>

namespace usage_reporting
{
// 统计文件所在目录：%LOCALAPPDATA%\MetasequoiaImeTelemetry，按 Windows 用户各一份。
// 放在数据目录之外：数据目录全机共用、所有用户可写，覆盖安装还会清掉其中不在保留清单里的文件。
// 取不到 LocalAppData 时返回空。
std::filesystem::path DefaultDirectory();

// 启动后台线程。进程内只生效一次，--pipe-probe 这类不跑界面的启动不要调用。
void Start();
} // namespace usage_reporting
