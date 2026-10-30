#pragma once

// 按需下载的单文件模型：从钉死的 HTTPS 直链下载（或导入用户用浏览器下好的文件），按钉死的字节数与
// SHA256 校验后原子落位到 <DataDir>/models/<目录>/<文件名>，再写一份 NOTICE.md。models/ 整目录在
// 安装器里是「升级保留、卸载删除」，跨升级不必重下。神经整句高精度档模型（neural_model）与词格语言
// 模型 sc.lm（lattice_model）各持一个实例。
//
// 状态以磁盘为准（文件存在 = ready，落位发生在校验之后），下载态是本进程内存里的 runtime 状态。
// 下载与导入共用单槽，同一时刻至多一个在进行。

#include "engine/core/data_path.h"

#include <Windows.h>

#include <filesystem>
#include <mutex>
#include <string>

namespace model_download
{

// <DataDir>/models/<directory>/<file>。内联：Server 的会话层只需要据此解析路径，不必链接下载实现。
inline std::filesystem::path ModelFile(const char *directory, const char *file)
{
    return metasequoia::data_directory() / "models" / directory / file;
}

// 删除时文件被 Server 映射着（kenlm 只读映射 sc.lm）就删不掉，改在目录里留这个标记，
// 由 Server 在下次启动、文件尚未映射时删除；标记在时 Server 不再使用该文件。
inline constexpr char kPendingDeleteMarker[] = ".pending-delete";

// Server 侧解析：有删除标记时先试着删模型文件（Server 重启后文件未映射，此时能删成），删成了才连
// 标记一起清掉目录，然后按缺席处理；否则文件存在才返回它。返回空路径 = 模型缺席。
// 先删文件再清目录：文件还映射着时 remove_all 可能先删掉标记再失败，下一次解析就又用上了文件。
inline std::filesystem::path ResolveInstalledFile(const std::filesystem::path &file)
{
    std::error_code error;
    if (std::filesystem::exists(file.parent_path() / kPendingDeleteMarker, error))
    {
        if (std::filesystem::remove(file, error) || !std::filesystem::exists(file, error))
            std::filesystem::remove_all(file.parent_path(), error);
        return {};
    }
    if (!std::filesystem::is_regular_file(file, error) || error)
        return {};
    return file;
}

struct ModelSpec
{
    const char *directory;  // models/ 下的目录名
    const char *file;       // 落位的文件名
    const wchar_t *host;    // 下载主机
    const wchar_t *path;    // 下载路径（release 资产，经 302 跳到 CDN）
    long long size;         // 钉死的字节数
    const char *sha256;     // 钉死的 SHA256（小写十六进制）
    const char *source;     // 校验失败时报给用户的来源名，如「chinese-ime-lm model-v1」
    const char *title;      // NOTICE.md 的标题
    const char *license;    // NOTICE.md 的许可行
    const char *notice;     // NOTICE.md 末尾附加的说明，可为空串
    const wchar_t *filter;  // 导入对话框的文件类型名
    const wchar_t *pattern; // 导入对话框的通配符
    const wchar_t *prompt;  // 导入对话框的标题
    // 文件被 Server 映射期间删不掉时，是否改为留删除标记、待重启后删除（见 kPendingDeleteMarker）。
    bool deferred_delete;
};

struct ModelStatus
{
    std::string state;           // "absent" | "downloading" | "importing" | "ready" | "error"
    int progress = 0;            // 下载中 0-100；其余状态无意义
    std::string error;           // state == "error" 时的人类可读原因
    bool delete_pending = false; // 已删除但文件仍被 Server 占用，重启输入法后才真正删掉
};

class ModelDownloader
{
  public:
    explicit ModelDownloader(const ModelSpec &spec) : spec_(spec)
    {
    }

    ModelDownloader(const ModelDownloader &) = delete;
    ModelDownloader &operator=(const ModelDownloader &) = delete;

    std::filesystem::path ModelFile() const
    {
        return model_download::ModelFile(spec_.directory, spec_.file);
    }

    // 模型的 HTTPS 直链。设置页把它交给浏览器（应用内下载慢时的备用通道），NOTICE.md 也写它。
    std::string SourceUrl() const;

    ModelStatus GetStatus();

    // 文件必须与钉死的字节数和 SHA256 完全一致。下载与导入落位前都走这里。
    bool Verify(const std::filesystem::path &file, std::string &error) const;

    // 幂等：已就绪或已在下载时返回 true 不重复启动；导入进行中返回 false。
    bool StartDownload();

    // 把用户用浏览器下好的文件导入：后台复制、校验、原子落位，校验不过进 error 态不留残件。
    // 源文件不是普通文件或单槽被下载占用时返回 false。
    bool Import(const std::filesystem::path &source);

    // 弹出系统「打开文件」对话框，取消时返回空路径。须在 UI 线程调用。
    std::filesystem::path PromptForFile(HWND owner) const;

    // 删除模型目录。下载或导入中拒绝。文件被占用且 spec.deferred_delete 时留删除标记并返回 true。
    bool Delete();

  private:
    enum class State
    {
        Idle,
        Downloading,
        Importing,
        Error,
    };

    std::filesystem::path Directory() const;
    std::filesystem::path PartFile() const;
    void SetState(State state, int progress, std::string error);
    void Fail(std::string error);
    void InstallPart(bool imported);
    void DownloadThread();
    void ImportThread(std::filesystem::path source);
    bool ClaimSlot(State state, bool &satisfied);

    const ModelSpec &spec_;
    std::mutex mutex_;
    State state_ = State::Idle;
    int progress_ = 0;
    std::string error_;
};

} // namespace model_download
