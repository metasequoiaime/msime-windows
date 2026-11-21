#pragma once

// 神经网络整句高精度档（效果优先 / desktop）模型的按需下载与状态查询。安装包只带速度档，
// 高精度档约 26 MB，用户在设置页下载后才可用；落到 <DataDir>/models/neural-desktop/，
// models/ 整目录在安装器里是「升级保留、卸载删除」，跨升级不必重下。
//
// 与八股文模型不同，这里钉死字节数与 SHA256：模型来自本组织的 chinese-ime-lm 仓库，
// rerank-v2-safetensors 这个 tag 下的 rerank-8x448.safetensors 与 neural-model/lock.json 记录的
// 一致，不会原地重传。换模型版本要连同 lock.json 一起改、一起评审，并换一个落位文件名。
//
// 换文件名是为了老用户：之前下载的 model-v1 效果档（kLegacyModelFileName）留在同一目录。它在新
// 代码下照样能用（format 版本 1，精确 GELU、softmax），所以新版下载之前继续用它，状态报
// update_available 让设置页提示更新；新版校验落位之后，下载器删掉它。同名覆盖做不到这一点：状态
// 「文件存在 = ready」看不出磁盘上是哪一版，老用户永远不会被提示，也拿不到新模型。
//
// 状态以磁盘为准（文件存在 = ready，落位发生在校验之后），下载态是本进程内存里的 runtime
// 状态。下载与导入共用单槽，同一时刻至多一个在进行。实现在 model_download，与词格语言模型共用。

#include "engine/core/data_path.h"

#include <Windows.h>

#include <filesystem>
#include <string>

namespace neural_model
{

inline constexpr char kModelDirectoryName[] = "neural-desktop";
inline constexpr char kModelFileName[] = "sentence-model-desktop-v2.safetensors";
// model-v1 的效果档。旧版设置页下载到这里，更旧的版本随包装在数据目录根下、由安装器挪到这里。
inline constexpr char kLegacyModelFileName[] = "sentence-model-desktop.safetensors";

// 内联在头文件里：Server 的会话层只需要据此解析路径，不必链接下载实现。
inline std::filesystem::path ModelFile()
{
    return metasequoia::data_directory() / "models" / kModelDirectoryName / kModelFileName;
}

inline std::filesystem::path LegacyModelFile()
{
    return metasequoia::data_directory() / "models" / kModelDirectoryName / kLegacyModelFileName;
}

// Server 实际加载的文件：新版在就用新版，否则用旧版，都不在返回空路径。
inline std::filesystem::path ResolveUsableModelFile()
{
    std::error_code error;
    for (const std::filesystem::path &file : {ModelFile(), LegacyModelFile()})
    {
        if (std::filesystem::is_regular_file(file, error) && !error)
            return file;
    }
    return {};
}

// 模型的 HTTPS 直链。设置页把它交给浏览器（应用内下载慢时的备用通道），NOTICE.md 也写它。
std::string SourceUrl();

struct ModelStatus
{
    std::string state; // 新版的状态："absent" | "downloading" | "importing" | "ready" | "error"
    int progress = 0;  // 下载中 0-100；其余状态无意义
    std::string error; // state == "error" 时的人类可读原因
    // 新版还没落位、旧版（model-v1）在：引擎照样有模型可用，设置页提示更新。
    bool update_available = false;
};

ModelStatus GetModelStatus();

// 文件必须与 lock.json 记录的字节数和 SHA256 完全一致。下载与导入落位前都走这里。
bool VerifyModelFile(const std::filesystem::path &file, std::string &error);

// 幂等：已就绪或已在下载时返回 true 不重复启动；导入进行中返回 false。
bool StartDownload();

// 把用户用浏览器下好的文件导入：后台复制、校验、原子落位，校验不过进 error 态不留残件。
// 源文件不是普通文件或单槽被下载占用时返回 false。
bool ImportModel(const std::filesystem::path &source);

// 弹出系统「打开文件」对话框，取消时返回空路径。须在 UI 线程调用。
std::filesystem::path PromptForModelFile(HWND owner);

// 删除模型目录（连同旧版）。下载或导入中拒绝。模型被 Server 读进内存后不占文件句柄，可以直接删；
// 开关不替用户改，页面在模型缺席时把它置灰。
bool DeleteModel();

} // namespace neural_model
