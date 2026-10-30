#pragma once

// octagram 语法模型（八股文，.gram）的按需下载与状态查询。内置目录（Catalog）随签名安装包
// 走：收录社区模型包需要改代码发版，不引入动态目录源。下载按目录条目的源列表直链进行
// （万象等条目可带镜像：首次下载镜像优先、失败回退主源；确认有更新后的更新下载反过来
// 主源优先，让「有新版本」这个来自主源探测的结论与拿到的字节对齐），落到
// <DataDir>/models/<id>/<id>.gram——确定性布局让引擎侧解析与安装器「models/ 整目录升级
// 保留、卸载删除」对新 id 自动成立。
//
// 完整性靠格式，不靠摘要。上游会原地重传（tag 不变、字节变）且没有不可变的资产 URL，把摘要
// 钉死就意味着每次重训都要改代码、发一个签名安装包，否则所有用户的下载一起失败——这个成本
// 消不掉。而重训只换权重，「Rime::Grammar/」魔数与双数组布局不变，所以这里只要求字节能被
// 引擎读取端打开（魔数 + 边界检查，engine/ngram/octagram/octagram_gram.cpp 的 GramDb::open）；
// 读取端不按文件自带的长度做堆分配，未知权重的字节是安全输入。传输完整性交给 HTTPS 加
// Content-Length 比对，以及逐源下载的停滞判据：某个源拿到响应头后 60 秒没有新字节就换下一个源
// （实现与阈值见 collocation_model.cpp 的 DownloadThread）。
//
// 状态以磁盘为准（模型文件存在 = ready，落位发生在格式校验之后），下载态是各进程内存里的
// runtime 状态：独立设置进程与 Server 各自能看到自己的下载进度。同一时刻至多一个下载在飞
// （单网络槽），忙碌时对新 id 的下载请求直接拒绝，页面在下载态禁用其余下载按钮。
//
// 更新检测只对目录里 track_updates 的条目生效（本期仅万象）：安装成功时把远端身份记进
// models/<id>/.remote，设置进程打开时做一次 HEAD 探测比较。身份连同「来自哪个下载源」
// 一起记：同一个文件在两个源上的 etag/last-modified 完全不同，跨源比校验器必然误报。
// 主源探不到时（连不上 GitHub 的国内用户）用镜像的 GET + Range: bytes=0-0 探测兜底。
// 镜像只给长度（HEAD 只回 Content-Length，校验器要 GET 才有），纯长度口径只能回答「字节数
// 变了没有」——长度相同判 Current 只用于校准记录来源，不构成「确认最新」：上游原地重传且
// 字节数不变时看不出来。所以镜像兜底只在主源探不到时生效，判据始终弱于 GitHub 的 ETag。
// 校准来源时必须留住旧校验器：弱口径结论若把强口径记录覆盖成纯长度，下一轮就成了同源纯
// 长度比对，弱结论被固化，主源恢复后也再判不出只换校验器的重传。因此补记录只换 source，
// ETag 只在新旧两侧同源、且本次探测确实给了 ETag 时才写。
// 已就绪不重下只在「没有已确认更新」时成立；确认有更新后允许 StartDownload 覆盖 .gram。
// 目标被输入法进程映射、无法原子替换时，校验过的新文件落成 <id>.gram.new（身份落 .remote.new），
// 由下一次 Server 启动或设置进程启动的 ApplyPendingUpdates() 完成落位。

#include <Windows.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace collocation
{

// 推荐包（万象）的 id：目录首条目，只是展示与排序上的推荐位，不承载默认或回退语义——
// 激活值默认留空 = 未选择任何模型（整句加成关闭）。
inline constexpr char kRecommendedModelId[] = "wanxiang-lts-zh-hans";

// 内置目录的一个条目。host/path 是 HTTPS 直链（release 资产与 raw 文件都经 302 跳 CDN，
// 下载器显式放开重定向）；size_hint/license/license_note 进设置页展示与 NOTICE.md 署名；
// badge 是候选窗的整句来源标签（不含六角括号），随包公布，用户据此分辨各家模型。
struct CatalogEntry
{
    const char *id;
    const char *display_name;
    const char *badge;
    const wchar_t *host;
    const wchar_t *path;
    const char *size_hint;
    const char *license;
    const char *license_note; // 可为空串：无补充说明
    // 是否参与「打开设置时探测远端更新」。只有上游会原地重传、且地址稳定的条目才置 true；
    // 八股文固定 tag、白霜 raw master 保持默认 false，后续启用只改这一行。
    bool track_updates = false;
    // 可选镜像源：首次下载时优先尝试，失败（连不上/非 200/读中断/长度不符）回退 host/path
    // 主源；更新下载与更新探测顺序相反，主源优先、镜像兜底。镜像给的校验器与主源不同，
    // 由 .remote 的 source 字段区分，跨源只比长度。为空表示该条目没有镜像。
    const wchar_t *mirror_host = nullptr;
    const wchar_t *mirror_path = nullptr;
};

// 编译期内置目录，首条目是推荐包。
const std::vector<CatalogEntry> &Catalog();

// 条目的 HTTPS 直链。设置页把它给用户在浏览器里下载（应用内下载慢时的备用通道），
// 同一个地址也写进 NOTICE.md 作来源署名。
std::string SourceUrl(const CatalogEntry &entry);

// 一个下载源。CatalogEntry 的 host/path 是主源；镜像字段可选。
struct DownloadSource
{
    const wchar_t *host;
    const wchar_t *path;
};

// 条目的有序下载源列表。prefer_primary 为 false（首次下载）时镜像（如有）在前、主源兜底；
// 为 true（覆盖已有模型的更新下载）时主源在前、镜像兜底——「有更新」的结论来自主源探测，
// 先拿主源能把结论与字节对齐。单独成函数是为了让顺序可被纯测试钉住。
std::vector<DownloadSource> DownloadSources(const CatalogEntry &entry, bool prefer_primary = false);

// 候选窗给该模型包的整句候选挂的来源标签（不含六角括号）。目录里没有的 id——手填进
// config.toml 的自备模型——落回八股：那是 octagram 这套格式的通称，不冒充任何一家。
const char *BadgeForModel(const std::string &model_id);

struct ModelStatus
{
    std::string state;             // "absent" | "downloading" | "importing" | "ready" | "error"
    int progress = 0;              // 下载中 0-100；其余状态无意义
    std::string error;             // state == "error" 时的人类可读原因
    bool update_available = false; // 远端身份与本地记录不同，可以更新
    bool update_pending = false;   // 新版本已下载并通过校验，等输入法进程重启落位
};

// models/<id>/.remote 的内容：一行一个 key=value（etag / last-modified / length / source）。
// ETag 与 Last-Modified 可能为空，length 始终记录。source 是这组身份来自哪个下载源的 host
// （"github.com" / "cnb.cool"），空串表示旧版本写的记录，按条目主源理解。写入先落 .tmp 再
// 原子 rename。
struct RemoteIdentity
{
    std::string etag;
    std::string last_modified;
    long long length = 0; // 0 = 未知
    std::string source;   // 身份来自哪个源的 host；空串 = 旧记录，视为主源
};

enum class UpdateVerdict
{
    Current, // 与本地记录一致
    Update,  // 远端重传过
    Unknown, // 远端没有可用信号，无法判断
};

// 纯判定：installed 为空结构表示旧安装（没有 .remote），此时按 local_bytes 与远端长度比较。
// installed_source / remote_source 是两个身份各自来自的源 host（UTF-8）：不同源时只比长度，
// 因为同一个文件在两个源上的 etag/last-modified 完全不同，比校验器必然误报更新。
// installed_source 为空（旧记录）按空串参与比较——只有两边都非空才可能判为换源。
// 未发过 ETag/Last-Modified 的源（镜像）探测拿回来的身份只剩长度，「长度相同」因此不能
// 当成「确认最新」——它只够支撑 Current，不足以否认上游改过内容。跨源本来就只比长度，
// 同源缺少校验器时同样退到长度：两种情况都只回 Current，不产生 Update；结论能否当成
// 「确认最新」，取决于这次探测到底比了什么，调用方据此决定要不要补记录。
// 不联网、不读盘，供 CheckForUpdates 与测试共用。
UpdateVerdict CompareRemoteIdentity(const RemoteIdentity &installed, long long local_bytes,
                                    const RemoteIdentity &remote, const std::string &installed_source,
                                    const std::string &remote_source);

// 补记录判据：调用方已保证 CompareRemoteIdentity 判了 Current，这里只回答「这个结论是否站不住、
// 该把本次探测的身份写回 .remote」。站不住指三者之一——本次探测的源与记录来源不同（跨源只比过
// 长度，校准过来下一轮才是同源比校验器）、记录里没有 ETag、本次探测没给 ETag。同源且两边都有
// ETag 时不写：ETag 已相同，不该每次打开设置都重写 .remote。远端没有可用身份时也不写——那本就
// 判不出 Current。不联网、不读盘，供 CheckForUpdates 与测试共用。
bool ShouldBackfillIdentity(const RemoteIdentity &installed, const std::string &installed_source,
                            const RemoteIdentity &remote, const std::string &remote_source);

// 补记录的写入形态：在 installed 基础上只采用本次探测确实拿到的东西。
// - source 总取探测源的 host（旧记录没有 source 时，这等于把主源来源固定下来）；
// - ETag / Last-Modified 只在新旧同源、且本次探测给了非空值时才写；
// - length 取本次探测的（探测失败时它本就是 0，不可能走到这里）。
// 不同源时**绝不**采用探测源的 ETag：镜像的校验器与 GitHub 不是一回事，写进去等于把记录
// 从强口径换成另一个源的弱口径。此时保留旧的 ETag/Last-Modified —— 它们属于旧来源，配合新
// source 读出来会被跨源分支忽略（只比长度），等主源恢复后再补写；这样弱结论不会固化成永久。
// 纯函数，供 CheckForUpdates 与测试共用。
RemoteIdentity MergeIdentityForBackfill(const RemoteIdentity &installed, const std::string &installed_source,
                                        const RemoteIdentity &remote, const std::string &remote_source);

// 下载停滞判据：last_progress_tick 是上一次拿到字节的时刻（GetTickCount64 毫秒），距 now_tick
// 严格超过 60 秒即认为这个源已经不再给字节（恰好等于阈值不判停滞）。它的用途只有一个：在
// WinHttpQueryDataAvailable 因接收超时失败之后，区分「连接活着但不给字节」与一次普通读中断，
// 不参与换不换源的决策。刷新进度是调用方的事——每读到字节就把 last_progress_tick 设为当时
// 的 tick。阈值与 WinHttpSetTimeouts 的接收超时用同一个常量，慢网速下每次接收的间隔都远小于它，
// 不会被当成坏源。
bool IsStalled(unsigned long long last_progress_tick, unsigned long long now_tick);

// .remote 的读写。model_directory 是该模型的目录；文件不存在或解析失败都返回空结构，
// 写入失败返回 false，不抛异常（下载线程不能因旁路文件失败而崩）。
RemoteIdentity ReadRemoteIdentity(const std::filesystem::path &model_directory);
bool WriteRemoteIdentity(const std::filesystem::path &model_directory, const RemoteIdentity &identity);

// 磁盘现状 + 本进程下载态，按目录条目逐 id 给出。轻量，可随配置快照频繁调用；绝不联网。
std::map<std::string, ModelStatus> GetModelStatuses();

// 对 track_updates 且已就绪的条目做一次探测并把结论记进本进程内存。先 HEAD 主源；主源探不到
// （连不上 / 非 200 / 无可用身份）或探到了却判不了（例如跨源只剩一侧长度）时，条目有镜像就用
// 镜像的 Range 请求兜底，比对照镜像自己的 source 做。一个进程只探一轮，重复调用直接返回；
// 两条路都得不出结论时静默放弃，不改变已有结论。判「当前」且记录里的身份不足以支持这个结论
// （记录来自别的源、缺 ETag、或本次探测没给 ETag）时把本次身份写回 .remote。
void CheckForUpdates();

// 把校验通过、等待落位的 <id>.gram.new 换到 <id>.gram（及 .remote.new → .remote）。
// Server 启动与设置进程初始 worker 各调一次，谁先拿到文件谁完成；旧文件仍被映射时保留待更新。
void ApplyPendingUpdates();

// 幂等启动指定模型的下载：该 id 已在下载或已就绪时返回 true 不重复启动；同一时刻至多一个
// 下载在飞，其他 id 忙碌时返回 false。目录里没有的 id、无法创建目录等同步失败也返回
// false，原因记入该 id 的状态。完成/失败经状态查询可见。
bool StartDownload(const std::string &model_id);

// 把用户在浏览器里自行下载的 .gram 导入为指定条目：后台复制到模型目录后，与应用内下载走
// 同一套格式校验与原子落位，校验不过同样进 error 态、不留残件。与下载共用单槽（任一下载或
// 导入在进行时拒绝其他 id）；源文件不存在或不是普通文件、目录里没有该 id 时返回 false。
bool ImportModel(const std::string &model_id, const std::filesystem::path &source);

// 弹出系统「打开文件」对话框让用户选 .gram 文件，取消时返回空路径。须在 UI 线程调用。
std::filesystem::path PromptForModelFile(HWND owner);

// 删除已下载的模型目录（.gram 与 NOTICE.md 一并）。守卫：该 id 下载或导入中拒绝。删除当前
// 激活的包时激活值一并清空（未选择），整句加成随之关闭，重新下载后需重新激活。若 .gram
// 还被引擎的内存映射占用，物理删除推迟到映射释放（状态查询按未下载呈现，重启 Server
// 必然清掉）。目录里没有的 id 返回 false。
bool DeleteModel(const std::string &model_id);

} // namespace collocation
