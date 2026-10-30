#pragma once

// 神经网络整句高精度档（效果优先 / desktop）模型的按需下载与状态查询。安装包只带速度档，
// 高精度档约 25 MB，用户在设置页下载后才可用；落到 <DataDir>/models/neural-desktop/，
// models/ 整目录在安装器里是「升级保留、卸载删除」，跨升级不必重下。
//
// 与八股文模型不同，这里钉死字节数与 SHA256：模型来自本组织的 chinese-ime-lm 仓库，
// model-v1 这个 tag 下的资产与 neural-model/lock.json 记录的一致，不会原地重传。换模型版本
// 要连同 lock.json 一起改、一起评审。
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
inline constexpr char kModelFileName[] = "sentence-model-desktop.safetensors";

// 内联在头文件里：Server 的会话层只需要据此解析路径，不必链接下载实现。
inline std::filesystem::path ModelFile()
{
    return metasequoia::data_directory() / "models" / kModelDirectoryName / kModelFileName;
}

// 模型的 HTTPS 直链。设置页把它交给浏览器（应用内下载慢时的备用通道），NOTICE.md 也写它。
std::string SourceUrl();

struct ModelStatus
{
    std::string state; // "absent" | "downloading" | "importing" | "ready" | "error"
    int progress = 0;  // 下载中 0-100；其余状态无意义
    std::string error; // state == "error" 时的人类可读原因
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

// 删除模型目录。下载或导入中拒绝。模型被 Server 读进内存后不占文件句柄，可以直接删；
// 开关不替用户改，页面在模型缺席时把它置灰。
bool DeleteModel();

} // namespace neural_model
