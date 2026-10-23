#include "collocation_model.h"

#include "config/ime_config.h"
#include "engine/core/data_path.h"
#include "engine/ngram/octagram/octagram_gram.h"
#include "utils/network_proxy.h"

#include <Windows.h>
#include <ShObjIdl.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace collocation
{

// 编译期内置目录，首条目是推荐包。收录新模型 = 改这里发版，条目随签名安装包走；
// 不引入动态目录源，避免免签分发渠道。zh-moqi 的构建链未声明许可，license_note 如实
// 说明并标实验。
const std::vector<CatalogEntry> &Catalog()
{
    static const std::vector<CatalogEntry> kCatalog = {
        {kRecommendedModelId, "万象 LTS（推荐）", "万象", L"github.com",
         L"/amzxyz/RIME-LMDG/releases/download/LTS/wanxiang-lts-zh-hans.gram", "约 390 MB", "CC-BY-4.0",
         "© amzxyz / RIME-LMDG 项目", true},
        {"zh-hans-t-essay-bgw", "八股文·词级", "八股", L"github.com",
         L"/lotem/rime-octagram-data/releases/download/20260712/zh-hans-t-essay-bgw.gram", "约 197 MB", "LGPL", ""},
        {"zh-hans-t-essay-bgw-compact", "八股文·词级紧凑", "八股", L"github.com",
         L"/lotem/rime-octagram-data/releases/download/20260712/zh-hans-t-essay-bgw-compact.gram", "约 39 MB", "LGPL",
         ""},
        {"zh-moqi", "白霜（实验）", "墨奇", L"raw.githubusercontent.com", L"/gaboolic/rime-frost/master/zh-moqi.gram",
         "约 7 MB", "GPL-3.0", "随 GPL-3.0 仓库（gaboolic/rime-frost）分发，构建链未声明许可，实验性收录"},
    };
    return kCatalog;
}

namespace
{

const CatalogEntry *FindEntry(const std::string &model_id)
{
    for (const auto &entry : Catalog())
    {
        if (model_id == entry.id)
            return &entry;
    }
    return nullptr;
}

std::filesystem::path ModelDirectory(const std::string &model_id)
{
    return metasequoia::data_directory() / "models" / model_id;
}

std::filesystem::path ModelFile(const std::string &model_id)
{
    return ModelDirectory(model_id) / (model_id + ".gram");
}

std::filesystem::path PartFile(const std::string &model_id)
{
    // 同一卷上的临时名，下载完成后原子 rename 到位；放进模型自己的目录，多个模型的
    // 下载互不碰撞。
    return ModelDirectory(model_id) / (model_id + ".gram.part");
}

// 已校验、等待输入法进程重启后落位的新版本；只在校验通过后才创建。
std::filesystem::path PendingModelFile(const std::string &model_id)
{
    return ModelDirectory(model_id) / (model_id + ".gram.new");
}

// <id>.gram 的远端身份（etag / last-modified / length），一行一个 key=value。
std::filesystem::path RemoteFile(const std::string &model_id)
{
    return ModelDirectory(model_id) / ".remote";
}

// <id>.gram.new 的远端身份；落位时一并换到 .remote。
std::filesystem::path PendingRemoteFile(const std::string &model_id)
{
    return ModelDirectory(model_id) / ".remote.new";
}

std::string TrimSpaces(std::string value)
{
    const size_t begin = value.find_first_not_of(" \t");
    if (begin == std::string::npos)
        return {};
    const size_t end = value.find_last_not_of(" \t");
    return value.substr(begin, end - begin + 1);
}

// 先写 <file>.tmp 再原子 rename：半截文件不会被下一次探测当成有效身份。
bool WriteIdentityFile(const std::filesystem::path &file, const RemoteIdentity &identity)
{
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error)
        return false;
    const std::filesystem::path temporary = file.wstring() + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;
        auto write_value = [&output](const char *key, std::string value) {
            // 值里不允许换行：一行一个 key=value，坏了就让下一次读取把它当空值。
            std::replace(value.begin(), value.end(), '\r', ' ');
            std::replace(value.begin(), value.end(), '\n', ' ');
            output << key << '=' << value << '\n';
        };
        write_value("etag", identity.etag);
        write_value("last-modified", identity.last_modified);
        output << "length=" << identity.length << '\n';
        if (!output)
            return false;
    }
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

bool HasIdentity(const RemoteIdentity &identity)
{
    return !identity.etag.empty() || !identity.last_modified.empty() || identity.length > 0;
}

// 逻辑删除的清扫：.gram 被引擎的内存映射占着时物理删除会失败，DeleteModel 落下的
// .trash 标记把目录转成「待清扫」。这里在状态查询与重新下载前重试，映射一旦释放
// （切换模型或重启 Server）目录就被清干净。
void PurgeIfTrashed(const std::string &model_id)
{
    std::error_code error;
    const std::filesystem::path trash = ModelDirectory(model_id) / ".trash";
    if (!std::filesystem::exists(trash, error) || error)
    {
        return;
    }
    if (std::filesystem::exists(ModelFile(model_id), error) && !error)
    {
        std::filesystem::remove(ModelFile(model_id), error);
        if (error)
        {
            return;
        }
    }
    std::filesystem::remove_all(ModelDirectory(model_id), error);
}

// 目录条目的 host/path 都是 ASCII（域名与仓库路径），NOTICE.md 是 UTF-8 文本，写之前把
// 宽字符转回来；走 CP_UTF8 而不是逐字节截断，将来条目里出现非 ASCII 也不会产出坏字节。
std::string Utf8FromWide(const wchar_t *wide)
{
    if (wide == nullptr)
        return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string narrow(bytes > 0 ? static_cast<size_t>(bytes) - 1 : 0, '\0');
    if (bytes > 1)
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow.data(), bytes, nullptr, nullptr);
    return narrow;
}

// 格式锁：唯一真正拦下坏模型的检查。让引擎读取端自己开一次，Magic 与双数组边界
// 都在 open() 里，越界在 set_array 之前就被拒，且不按文件自带的长度做堆分配。
// 刻意用局部实例而不是 shared_gram_db——后者按路径永久缓存，会一直攥着 .part 的
// 映射，而映射是以 FILE_SHARE_READ 打开的（没有 FILE_SHARE_DELETE），改名将失败。
bool ValidateFormat(const std::filesystem::path &file, std::string &error)
{
    gram::GramDb db;
    if (db.open(file))
    {
        return true;
    }
    error = db.error();
    return false;
}

// 读最终响应的字符串头。WinHTTP 对自定义头要求先问长度再取缓冲区；缺失时返回空串。
std::wstring QueryStringHeader(HINTERNET request, const wchar_t *name)
{
    DWORD bytes = 0;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, WINHTTP_NO_OUTPUT_BUFFER, &bytes,
                             WINHTTP_NO_HEADER_INDEX) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    {
        return {};
    }
    if (bytes == 0)
        return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, value.data(), &bytes, WINHTTP_NO_HEADER_INDEX))
        return {};
    value.resize(bytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0')
        value.pop_back();
    return value;
}

// 只取响应头的一次 HEAD 探测。失败（离线、代理不可用、404/403、超时）一律返回 false，
// 调用方静默保留上一结论；不做 Range 兜底，GitHub 支持 HEAD。
bool ProbeRemoteIdentity(const CatalogEntry &entry, RemoteIdentity &identity)
{
    HINTERNET session = NetworkProxy::OpenWinHttpSession(L"MetasequoiaImeServer/1.0");
    if (session == nullptr)
        return false;
    // 探测必须短：它跑在设置进程的后台线程里，但一次挂住会拖慢「打开设置即可见更新」。
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
    HINTERNET connection = WinHttpConnect(session, entry.host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = nullptr;
    bool ok = false;
    if (connection != nullptr)
    {
        request = WinHttpOpenRequest(connection, L"HEAD", entry.path, nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (request != nullptr)
        {
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
            ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                 WinHttpReceiveResponse(request, nullptr);
        }
    }
    if (ok)
    {
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) &&
             status == HTTP_STATUS_OK;
    }
    if (ok)
    {
        identity.etag = Utf8FromWide(QueryStringHeader(request, L"ETag").c_str());
        identity.last_modified = Utf8FromWide(QueryStringHeader(request, L"Last-Modified").c_str());
        DWORD length = 0;
        DWORD length_size = sizeof(length);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &length, &length_size, WINHTTP_NO_HEADER_INDEX))
            identity.length = length;
        ok = HasIdentity(identity);
    }
    if (request != nullptr)
        WinHttpCloseHandle(request);
    if (connection != nullptr)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

enum class DownloadState
{
    Idle,
    Downloading,
    Importing,
    Error,
};

// 下载与导入共用单槽：两者都在写同一个模型目录，且都要等格式校验落位。
bool IsBusy(DownloadState state)
{
    return state == DownloadState::Downloading || state == DownloadState::Importing;
}

struct RuntimeState
{
    DownloadState state = DownloadState::Idle;
    int progress = 0;
    std::string error;
};

std::mutex g_mutex;
// 按 id 的下载态，map 大小就是目录条目数。成功后该 id 留下一个 Idle 条目，无碍：
// 查询侧以磁盘为准，runtime 态只在文件缺席时补充 downloading/error。
std::map<std::string, RuntimeState> g_states;
// 本进程探测出的「该 id 有新版本」。只在 g_mutex 下访问；进程生命周期内不做后台轮询。
std::map<std::string, bool> g_updates;
// CheckForUpdates 一个进程只跑一轮，重复调用（比如窗口重复激活）直接返回。
std::atomic<bool> g_update_check_started{false};

void ClearUpdateFlag(const std::string &model_id)
{
    std::lock_guard lock(g_mutex);
    g_updates.erase(model_id);
}

bool HasUpdateFlag(const std::string &model_id)
{
    std::lock_guard lock(g_mutex);
    const auto it = g_updates.find(model_id);
    return it != g_updates.end() && it->second;
}

void SetState(const std::string &model_id, DownloadState state, int progress, std::string error)
{
    std::lock_guard lock(g_mutex);
    RuntimeState &runtime = g_states[model_id];
    runtime.state = state;
    runtime.progress = progress;
    runtime.error = std::move(error);
}

void InstallPart(const CatalogEntry &entry, bool imported, const RemoteIdentity &identity, bool replacing);

// 后台线程：下载 -> 校验 -> 原子落位。全程只碰 PartFile，失败时不留半个模型
// 在正式位置上。条目按值收进线程：detached 线程的生命周期长于调用栈，host/path/id
// 必须自持。release 资产与 raw 文件都经 302 跳转到 CDN，显式放开自动重定向。
// replacing 表示这次下载是针对已就绪旧模型的更新（只有确认有更新时才为 true）。
void DownloadThread(CatalogEntry entry, bool replacing)
{
    const std::string model_id = entry.id;
    SetState(model_id, DownloadState::Downloading, 0, {});

    std::error_code fs_error;
    const std::filesystem::path directory = ModelDirectory(model_id);
    std::filesystem::create_directories(directory, fs_error);
    if (fs_error)
    {
        SetState(model_id, DownloadState::Error, 0, "无法创建模型目录");
        return;
    }
    // 上次失败可能留下半截 .part；重新下载前先清掉，避免续写错位的字节。
    std::error_code cleanup_error;
    std::filesystem::remove(PartFile(model_id), cleanup_error);

    HINTERNET session = NetworkProxy::OpenWinHttpSession(L"MetasequoiaImeServer/1.0");
    if (session == nullptr)
    {
        SetState(model_id, DownloadState::Error, 0, "无法初始化网络");
        return;
    }
    // 400MB 不是快操作：连接阶段给足超时，收发阶段不设上限靠进度与失败兜底。
    WinHttpSetTimeouts(session, 30000, 30000, 30000, 0);
    HINTERNET connection = WinHttpConnect(session, entry.host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = nullptr;
    if (connection != nullptr)
    {
        request = WinHttpOpenRequest(connection, L"GET", entry.path, nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (request != nullptr)
        {
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        }
    }
    bool ok = connection != nullptr && request != nullptr &&
              WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (ok)
    {
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
        ok = status == HTTP_STATUS_OK;
    }

    // 总长取响应自己声明的 Content-Length，不再钉死字节数常量：上游重训会让字节数
    // 变，而这个值顺带充当截断检测的基准（收完比对一次），比钉常量更贴近实际。
    long long expected = 0;
    RemoteIdentity identity;
    if (ok)
    {
        DWORD length = 0;
        DWORD length_size = sizeof(length);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &length, &length_size, WINHTTP_NO_HEADER_INDEX))
        {
            expected = length;
            identity.length = length;
        }
        // 安装成功时把最终响应的校验器记进 .remote，下一次探测只比这几百字节的头。
        identity.etag = Utf8FromWide(QueryStringHeader(request, L"ETag").c_str());
        identity.last_modified = Utf8FromWide(QueryStringHeader(request, L"Last-Modified").c_str());
    }

    std::ofstream out;
    if (ok)
    {
        out.open(PartFile(model_id), std::ios::binary | std::ios::trunc);
        ok = static_cast<bool>(out);
    }

    long long received = 0;
    while (ok)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
        {
            ok = false;
            break;
        }
        if (available == 0)
            break;
        std::vector<char> buffer(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read) || read == 0)
        {
            ok = false;
            break;
        }
        out.write(buffer.data(), static_cast<std::streamsize>(read));
        if (!out)
        {
            ok = false;
            break;
        }
        received += read;
        if (expected > 0)
        {
            const int progress = static_cast<int>((std::min)(100LL, received * 100LL / expected));
            SetState(model_id, DownloadState::Downloading, progress, {});
        }
    }
    if (request != nullptr)
        WinHttpCloseHandle(request);
    if (connection != nullptr)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    out.close();

    if (!ok)
    {
        SetState(model_id, DownloadState::Error, 0,
                 status != 0 && status != HTTP_STATUS_OK ? "下载响应异常" : "下载中断");
        std::error_code cleanup;
        std::filesystem::remove(PartFile(model_id), cleanup);
        return;
    }

    // 传输完整性：收完的字节数必须等于响应声明的 Content-Length。声明缺失时跳过，
    // 交给下面的格式校验兜底。
    if (expected > 0 && received != expected)
    {
        SetState(model_id, DownloadState::Error, 0, "下载字节数与响应声明不符");
        std::error_code cleanup;
        std::filesystem::remove(PartFile(model_id), cleanup);
        return;
    }

    InstallPart(entry, false, identity, replacing);
}

// 后台线程：把用户在浏览器里下好的文件复制成 PartFile，之后与下载走同一条校验落位路径。
// 先复制再校验，而不是直接校验原文件：校验通过后的 rename 必须在同一卷上原子完成，
// 原文件可能在别的盘，也不该被我们挪走。
void ImportThread(CatalogEntry entry, std::filesystem::path source)
{
    const std::string model_id = entry.id;
    std::error_code fs_error;
    std::filesystem::create_directories(ModelDirectory(model_id), fs_error);
    if (fs_error)
    {
        SetState(model_id, DownloadState::Error, 0, "无法创建模型目录");
        return;
    }
    std::filesystem::copy_file(source, PartFile(model_id), std::filesystem::copy_options::overwrite_existing, fs_error);
    if (fs_error)
    {
        SetState(model_id, DownloadState::Error, 0, "无法读取所选文件");
        std::error_code cleanup;
        std::filesystem::remove(PartFile(model_id), cleanup);
        return;
    }
    InstallPart(entry, true, {}, false);
}

// 署名义务随模型走：NOTICE 与模型同目录，注明来源与许可；各条目许可不同，
// zh-moqi 的许可状态在 license_note 里如实说明。字节数按当前 .gram 重写。
void WriteNotice(const CatalogEntry &entry, bool imported)
{
    std::error_code size_error;
    const long long model_bytes = static_cast<long long>(std::filesystem::file_size(ModelFile(entry.id), size_error));
    std::ofstream notice(ModelDirectory(entry.id) / "NOTICE.md", std::ios::binary | std::ios::trunc);
    if (!notice)
        return;
    const std::string note = entry.license_note == nullptr ? std::string() : std::string(entry.license_note);
    notice << "# " << entry.display_name << "（" << entry.id << "）\n\n"
           << "- 来源：" << SourceUrl(entry) << (imported ? "（用户自行下载后本地导入）" : "") << "\n"
           << "- 许可：" << entry.license << (note.empty() ? "" : "（" + note + "）") << "\n"
           << "- 文件：" << entry.id << ".gram（" << model_bytes << " 字节）\n";
}

// PartFile 已完整写好之后的公共尾段：格式校验 -> 原子落位 -> NOTICE。下载与本地导入都走这里，
// 所以「文件存在 = 曾通过校验」对两条来源同样成立。replacing 只在「确认有更新、覆盖已就绪
// 旧模型」时为 true；其余情况（首次下载、逻辑删除后重下）沿用原有的 rename + error 语义。
void InstallPart(const CatalogEntry &entry, bool imported, const RemoteIdentity &identity, bool replacing)
{
    const std::string model_id = entry.id;

    // 格式锁：唯一真正拦下坏模型的检查，也是「已下载」的定义——能被引擎读取端
    // 打开（魔数与双数组边界合法）就放行落位，打开不了的一律按损坏拒绝。
    std::string format_error;
    if (!ValidateFormat(PartFile(model_id), format_error))
    {
        SetState(model_id, DownloadState::Error, 0, "模型格式校验失败（上游可能改了格式）：" + format_error);
        std::error_code cleanup;
        std::filesystem::remove(PartFile(model_id), cleanup);
        return;
    }

    // ValidateFormat 的局部 GramDb 已析构、映射已解除，否则下面的 rename 会被
    // 以 FILE_SHARE_READ 打开的旧句柄拒绝。
    const std::filesystem::path part = PartFile(model_id);
    const std::filesystem::path target = ModelFile(model_id);
    std::error_code exists_error;
    const bool target_exists = std::filesystem::exists(target, exists_error) && !exists_error;
    bool placed = false;
    bool target_busy = false;
    if (replacing && target_exists)
    {
        // 更新路径必须覆盖旧模型。目标还被输入法进程以 FILE_SHARE_READ 映射着时，
        // MoveFileExW 会以共享冲突失败——这不是错误，转「待重启」。
        if (MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            placed = true;
        }
        else
        {
            const DWORD move_error = GetLastError();
            target_busy = move_error == ERROR_SHARING_VIOLATION || move_error == ERROR_ACCESS_DENIED ||
                          move_error == ERROR_LOCK_VIOLATION;
        }
    }
    else
    {
        std::error_code rename_error;
        std::filesystem::rename(part, target, rename_error);
        placed = !rename_error;
    }

    if (!placed && target_busy && replacing && target_exists)
    {
        // 新版本已通过校验，先留在 .gram.new（连同身份）。旧模型继续可用，下一次
        // Server 启动或设置进程启动时由 ApplyPendingUpdates 落位。只有「覆盖已就绪的
        // 旧模型」才走这条路：逻辑删除后的重下仍按原来的失败处理，不能把已删除的包复活。
        const std::filesystem::path pending = PendingModelFile(model_id);
        std::error_code pending_error;
        std::filesystem::remove(pending, pending_error);
        pending_error.clear();
        std::filesystem::rename(part, pending, pending_error);
        if (pending_error)
        {
            SetState(model_id, DownloadState::Error, 0, "模型落位失败");
            std::error_code cleanup;
            std::filesystem::remove(part, cleanup);
            return;
        }
        if (HasIdentity(identity))
            WriteIdentityFile(PendingRemoteFile(model_id), identity);
        else
            std::filesystem::remove(PendingRemoteFile(model_id), pending_error);
        ClearUpdateFlag(model_id);
        SetState(model_id, DownloadState::Idle, 100, {});
        return; // NOTICE 等真正落位后再按新文件字节数重写
    }

    if (!placed)
    {
        // 其他失败（目标存在但不是映射冲突、同一目录 rename 被拒）沿用原有 error 路径：
        // .part 不留，重试会整个重下，旧模型不受影响。
        SetState(model_id, DownloadState::Error, 0, "模型落位失败");
        std::error_code cleanup;
        std::filesystem::remove(part, cleanup);
        return;
    }

    // 身份随文件走：下载记最终响应头；本地导入没有响应头，删掉旧记录，让下一次
    // 探测按「旧安装」口径用长度补记录或提示更新。
    if (imported || !HasIdentity(identity))
        std::filesystem::remove(RemoteFile(model_id), exists_error);
    else
        WriteRemoteIdentity(ModelDirectory(model_id), identity);
    ClearUpdateFlag(model_id);

    WriteNotice(entry, imported);
    SetState(model_id, DownloadState::Idle, 100, {});
}

// 下载与导入的公共前置：目录内 id、已就绪不重做、单槽占位。占位成功返回 entry 并把该 id
// 置为 state；请求已被满足（已就绪，或同 id 同类任务已在进行）时 satisfied 置 true 并返回
// nullptr；其余拒绝返回 nullptr。replace 为 true 时允许覆盖已就绪的 .gram（仅在有已确认
// 更新时由 StartDownload 传入）；已有 .gram.new 等待落位时一律不重复下载。replacing 是本次
// 占位实际是否在覆盖旧模型：逻辑删除（.trash）后的重下不算，必须走原来的失败路径。
const CatalogEntry *ClaimSlot(const std::string &model_id, DownloadState state, bool replace, bool &replacing,
                              bool &satisfied)
{
    replacing = false;
    satisfied = false;
    const CatalogEntry *entry = FindEntry(model_id);
    if (entry == nullptr)
        return nullptr;
    PurgeIfTrashed(model_id);
    std::error_code fs_error;
    if (std::filesystem::exists(PendingModelFile(model_id), fs_error) && !fs_error)
    {
        satisfied = true;
        return nullptr;
    }
    // 已存在且通过格式校验的包不重做：落位只在格式校验之后发生，文件存在即曾通过校验，
    // 重做只会白白覆盖一份好包。逻辑已删除（.trash 在）的包不在此列：它们必须允许重新
    // 获取，哪怕旧 .gram 的物理删除还在等映射释放（小包可能在旧映射释放前就落位，
    // 落位失败进 Error 态，等映射释放后重试即可成功）。确认有更新时例外：replace 跳过
    // 这条判断，但下面的单槽守卫仍然生效。
    const bool trashed = std::filesystem::exists(ModelDirectory(model_id) / ".trash", fs_error) && !fs_error;
    const bool target_exists = std::filesystem::exists(ModelFile(model_id), fs_error) && !fs_error;
    if (!replace && !trashed && target_exists)
    {
        satisfied = true;
        return nullptr;
    }
    replacing = !trashed && replace && target_exists;
    std::lock_guard lock(g_mutex);
    // 单槽：同一时刻至多一个下载或导入，不做并发队列。同 id 同类请求幂等；其他 id
    // 忙碌时拒绝，页面在忙碌态禁用其余下载/导入按钮。
    for (const auto &[id, runtime] : g_states)
    {
        if (IsBusy(runtime.state))
        {
            satisfied = id == model_id && runtime.state == state;
            return nullptr;
        }
    }
    RuntimeState &runtime = g_states[model_id];
    runtime.state = state;
    runtime.progress = 0;
    runtime.error.clear();
    return entry;
}

} // namespace

std::string SourceUrl(const CatalogEntry &entry)
{
    return "https://" + Utf8FromWide(entry.host) + Utf8FromWide(entry.path);
}

RemoteIdentity ReadRemoteIdentity(const std::filesystem::path &model_directory)
{
    RemoteIdentity identity;
    std::ifstream input(model_directory / ".remote", std::ios::binary);
    if (!input)
        return identity;
    std::string line;
    while (std::getline(input, line))
    {
        // 容忍 CRLF：值不跨行，行尾的 \r 不属于值。
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const size_t separator = line.find('=');
        if (separator == std::string::npos)
            continue;
        const std::string key = TrimSpaces(line.substr(0, separator));
        const std::string value = TrimSpaces(line.substr(separator + 1));
        if (key == "etag")
        {
            identity.etag = value;
        }
        else if (key == "last-modified")
        {
            identity.last_modified = value;
        }
        else if (key == "length")
        {
            char *end = nullptr;
            const long long parsed = std::strtoll(value.c_str(), &end, 10);
            if (end != value.c_str() && parsed > 0)
                identity.length = parsed;
        }
        // 未知键忽略：以后加字段时旧版本仍能读。
    }
    return identity;
}

bool WriteRemoteIdentity(const std::filesystem::path &model_directory, const RemoteIdentity &identity)
{
    return WriteIdentityFile(model_directory / ".remote", identity);
}

UpdateVerdict CompareRemoteIdentity(const RemoteIdentity &installed, long long local_bytes,
                                    const RemoteIdentity &remote)
{
    // 远端一个信号都没有：本次探测失败或头被中间层剥掉，不改结论、不误报更新。
    if (!HasIdentity(remote))
        return UpdateVerdict::Unknown;
    // 旧安装没有 .remote：只能比长度。相等即认为是最新，顺便把 etag/last-modified 补记下来；
    // 不相等才提示更新。字节数恰好相同的重传会漏掉一次，这是不下载 390 MB 的代价。
    if (!HasIdentity(installed))
    {
        if (remote.length <= 0 || local_bytes <= 0)
            return UpdateVerdict::Unknown;
        return remote.length == local_bytes ? UpdateVerdict::Current : UpdateVerdict::Update;
    }
    if (!installed.etag.empty() && !remote.etag.empty() && installed.etag != remote.etag)
        return UpdateVerdict::Update;
    if (!installed.last_modified.empty() && !remote.last_modified.empty() &&
        installed.last_modified != remote.last_modified)
        return UpdateVerdict::Update;
    if (installed.length > 0 && remote.length > 0 && installed.length != remote.length)
        return UpdateVerdict::Update;
    return UpdateVerdict::Current;
}

std::map<std::string, ModelStatus> GetModelStatuses()
{
    std::map<std::string, ModelStatus> statuses;
    for (const auto &entry : Catalog())
    {
        PurgeIfTrashed(entry.id);
        std::error_code fs_error;
        bool update_available = false;
        // runtime 下载与错误态优先于磁盘：刚失败的落位比「旧的已验证文件还在」更值得
        // 看见。逻辑已删除（.trash 标记在）一律按未下载呈现，哪怕 .gram 还没物理消失。
        {
            std::lock_guard lock(g_mutex);
            const auto update = g_updates.find(entry.id);
            update_available = update != g_updates.end() && update->second;
            const auto it = g_states.find(entry.id);
            if (it != g_states.end() && it->second.state == DownloadState::Downloading)
            {
                ModelStatus downloading{"downloading", it->second.progress, {}};
                downloading.update_available = update_available;
                statuses[entry.id] = downloading;
                continue;
            }
            if (it != g_states.end() && it->second.state == DownloadState::Importing)
            {
                statuses[entry.id] = {"importing", 0, {}};
                continue;
            }
            if (it != g_states.end() && it->second.state == DownloadState::Error)
            {
                // 失败态保留 update_available，界面据此仍显示「更新」以便重试。
                statuses[entry.id] = {"error", 0, it->second.error};
                statuses[entry.id].update_available = update_available;
                continue;
            }
        }
        if (std::filesystem::exists(ModelDirectory(entry.id) / ".trash", fs_error) && !fs_error)
        {
            statuses[entry.id] = {"absent", 0, {}};
            continue;
        }
        // 落位发生在格式校验之后，所以「文件存在」即代表曾经通过校验。这里不再比对
        // 字节数：钉死它就等于钉死上游的每次重训。
        if (std::filesystem::exists(ModelFile(entry.id), fs_error) && !fs_error)
        {
            ModelStatus ready{"ready", 100, {}};
            if (std::filesystem::exists(PendingModelFile(entry.id), fs_error) && !fs_error)
            {
                // 新版本已校验、等重启落位；此时不再提示「可更新」，按钮是「待重启」。
                ready.update_pending = true;
            }
            else
            {
                ready.update_available = update_available;
            }
            statuses[entry.id] = ready;
            continue;
        }
        statuses[entry.id] = {"absent", 0, {}};
    }
    return statuses;
}

void CheckForUpdates()
{
    // 一个设置进程只探一轮：窗口反复显示/隐藏不重复联网；失败也不循环重试。
    if (g_update_check_started.exchange(true))
        return;
    for (const auto &entry : Catalog())
    {
        const std::string model_id = entry.id;
        if (!entry.track_updates)
            continue;
        std::error_code fs_error;
        if (!std::filesystem::exists(ModelFile(model_id), fs_error) || fs_error)
            continue;
        // 已逻辑删除的包不再探测：删除意愿优先，避免并发下载把已删除的包重新带回来。
        if (std::filesystem::exists(ModelDirectory(model_id) / ".trash", fs_error) && !fs_error)
            continue;
        // 已经有待落位的新版本：不再探测，避免把「待重启」覆盖成「可更新」。
        if (std::filesystem::exists(PendingModelFile(model_id), fs_error) && !fs_error)
            continue;
        {
            std::lock_guard lock(g_mutex);
            const auto runtime = g_states.find(model_id);
            if (runtime != g_states.end() && IsBusy(runtime->second.state))
                continue;
        }

        const RemoteIdentity installed = ReadRemoteIdentity(ModelDirectory(model_id));
        RemoteIdentity remote;
        if (!ProbeRemoteIdentity(entry, remote))
            continue; // 离线 / 代理不可用 / 4xx：静默保留上一结论

        std::error_code size_error;
        const auto file_bytes = std::filesystem::file_size(ModelFile(model_id), size_error);
        const long long local_bytes = size_error ? 0 : static_cast<long long>(file_bytes);
        const UpdateVerdict verdict = CompareRemoteIdentity(installed, local_bytes, remote);
        if (verdict == UpdateVerdict::Unknown)
            continue;
        // 探测要几百毫秒到几秒，期间用户可能已经删掉这个模型：删除意愿优先。这里再看一眼，
        // 既不写结论，也不能让下面的补记录把刚删掉的目录重新建出来（WriteIdentityFile
        // 会 create_directories）。
        if (!std::filesystem::exists(ModelFile(model_id), fs_error) || fs_error)
            continue;
        if (std::filesystem::exists(ModelDirectory(model_id) / ".trash", fs_error) && !fs_error)
            continue;

        const bool write_legacy_record = !HasIdentity(installed) && verdict == UpdateVerdict::Current;
        // 网络 I/O 已结束，改内存结论很快；补记录放到锁外，避免磁盘操作拖住状态查询。
        {
            std::lock_guard lock(g_mutex);
            g_updates[model_id] = verdict == UpdateVerdict::Update;
        }
        if (write_legacy_record)
            WriteRemoteIdentity(ModelDirectory(model_id), remote);
    }
}

void ApplyPendingUpdates()
{
    for (const auto &entry : Catalog())
    {
        if (!entry.track_updates)
            continue;
        const std::string model_id = entry.id;
        std::error_code fs_error;
        const std::filesystem::path pending = PendingModelFile(model_id);
        if (!std::filesystem::exists(pending, fs_error) || fs_error)
            continue;
        // 用户已经删除了这个模型：删除意愿优先，待更新文件随目录一起清掉，不复活。
        if (std::filesystem::exists(ModelDirectory(model_id) / ".trash", fs_error) && !fs_error)
            continue;

        // .gram.new 只在格式校验通过后才创建，这里不再重复校验（design 的「不重复校验」）。
        // 直接原子替换：旧模型还被映射时 MoveFileExW 会失败并保持原文件不动，不会出现
        // 「旧的已删、新的没换上」的窗口。
        const std::filesystem::path target = ModelFile(model_id);
        if (!MoveFileExW(pending.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
            continue; // 旧模型仍被映射或其他落位失败：保留 .new 与 .remote.new，下次启动再试

        const std::filesystem::path pending_remote = PendingRemoteFile(model_id);
        std::error_code remote_error;
        if (std::filesystem::exists(pending_remote, remote_error) && !remote_error)
        {
            if (!MoveFileExW(pending_remote.c_str(), RemoteFile(model_id).c_str(), MOVEFILE_REPLACE_EXISTING))
                std::filesystem::remove(RemoteFile(model_id), remote_error);
        }
        else
        {
            // 没有新身份（例如旧版本留下的 .gram.new）：删掉旧记录，下一次探测按旧安装口径补。
            std::filesystem::remove(RemoteFile(model_id), remote_error);
        }
        {
            std::lock_guard lock(g_mutex);
            g_updates.erase(model_id);
        }
        WriteNotice(entry, false);
    }
}

// 手填的 id（自备模型、目录收录前就装好的包）不在目录里。八股是 octagram 这套格式的通称，
// 不属于任何一家，用它兜底比挂一个猜出来的品牌名诚实。
constexpr const char *kGenericBadge = "八股";

const char *BadgeForModel(const std::string &model_id)
{
    const CatalogEntry *entry = FindEntry(model_id);
    return entry == nullptr ? kGenericBadge : entry->badge;
}

bool StartDownload(const std::string &model_id)
{
    bool satisfied = false;
    bool replacing = false;
    // 只有探测确认有更新时才允许覆盖已就绪的 .gram；否则保持原来的幂等语义。
    const CatalogEntry *entry =
        ClaimSlot(model_id, DownloadState::Downloading, HasUpdateFlag(model_id), replacing, satisfied);
    if (entry == nullptr)
        return satisfied;
    std::thread(DownloadThread, *entry, replacing).detach();
    return true;
}

bool ImportModel(const std::string &model_id, const std::filesystem::path &source)
{
    std::error_code fs_error;
    if (!std::filesystem::is_regular_file(source, fs_error) || fs_error)
        return false;
    bool satisfied = false;
    bool replacing = false;
    const CatalogEntry *entry = ClaimSlot(model_id, DownloadState::Importing, false, replacing, satisfied);
    if (entry == nullptr)
        return satisfied;
    std::thread(ImportThread, *entry, source).detach();
    return true;
}

std::filesystem::path PromptForModelFile(HWND owner)
{
    // 设置窗口的 UI 线程启动时已初始化过 STA，这里的 CoInitializeEx 只是配平引用计数。
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::filesystem::path selected;
    IFileOpenDialog *dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        const COMDLG_FILTERSPEC filters[] = {{L"octagram 语法模型 (*.gram)", L"*.gram"}, {L"所有文件", L"*.*"}};
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        dialog->SetTitle(L"选择已下载的 .gram 模型文件");
        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options)))
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item)))
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                selected = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
        dialog->Release();
    }
    if (SUCCEEDED(init))
        CoUninitialize();
    return selected;
}

bool DeleteModel(const std::string &model_id)
{
    if (FindEntry(model_id) == nullptr)
        return false;
    {
        std::lock_guard lock(g_mutex);
        const auto it = g_states.find(model_id);
        if (it != g_states.end() && IsBusy(it->second.state))
            return false;
    }
    PurgeIfTrashed(model_id);
    // 删的是当前激活的包时先清激活再删文件：清空写入失败就原地返回 false，什么都没
    // 发生，用户重试即可；反过来先删文件再清空，一次写入失败就会留下「指向已删除包」
    // 的僵尸选中态。未选择（留空）时没有需要清的目标。
    if (model_id == GetConfiguredAssocSentenceCollocationModel() && !SetConfiguredAssocSentenceCollocationModel(""))
    {
        return false;
    }
    std::error_code error;
    // 待更新/远端身份是旁路文件，不受 .gram 映射影响；单独删掉，不能靠 remove_all 在
    // .gram 删除失败时顺带清理（它可能提前中止）。
    std::filesystem::remove(PendingModelFile(model_id), error);
    std::filesystem::remove(PendingRemoteFile(model_id), error);
    std::filesystem::remove(RemoteFile(model_id), error);
    error.clear();
    // .gram 与 NOTICE.md 都在这个目录里，整目录删除即卸载；目录不存在时 remove_all
    // 无错返回，删除因此是幂等的。
    std::filesystem::remove_all(ModelDirectory(model_id), error);
    if (std::filesystem::exists(ModelFile(model_id), error))
    {
        // .gram 还被引擎的内存映射占着（激活后打过字就会映射，句柄不带
        // FILE_SHARE_DELETE），物理删除要等引用释放。落一个标记把删除降级为逻辑删除：
        // 状态按未下载呈现、不可选中；下面的 evict 摘掉缓存引用后，词典在下一次应用
        // 配置时松手，状态查询的重试清扫就能真正删掉文件。
        std::ofstream trash(ModelDirectory(model_id) / ".trash", std::ios::binary | std::ios::trunc);
    }
    // 内存里的更新结论随删除一起清掉：重新下载后由下一次探测重新建立。
    ClearUpdateFlag(model_id);
    // 引擎缓存里这个路径的映射引用摘掉：词典在下一次应用配置（击键）松手后，
    // 上面的 .trash 清扫就能真正物理删除残留的 .gram（不用等重启）。
    gram::shared_gram_db_evict(ModelFile(model_id));
    return true;
}

} // namespace collocation
