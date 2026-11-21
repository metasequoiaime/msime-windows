#pragma once

// 本地语音识别模型的按需下载、导入与删除，跑在设置进程里。目录与磁盘布局见
// voice-input/local_asr_catalog.h。
//
// 下载一个模型时，缺什么补什么：sherpa-onnx 运行库（各模型共用）、silero_vad.onnx（整句模型共用）、
// 模型压缩包本身。每一份都按钉死的字节数与 SHA256 校验，压缩包用系统自带的 tar.exe 解出需要的成员，
// 先落到同卷的 .staging 目录、最后写入完成标记再原子改名到位，所以「目录里有标记 = 曾通过校验」。
// 应用内下载太慢时，用户可以用浏览器下好任一组件再导入：按 SHA256 认出它是哪个组件，走同一套安装。
//
// 状态以磁盘为准，下载态是本进程内存里的 runtime 状态。同一时刻至多一个任务在进行。

#include <Windows.h>

#include <filesystem>
#include <map>
#include <string>

namespace local_asr_download
{

struct ModelStatus
{
    std::string state;            // "absent" | "downloading" | "extracting" | "importing" | "ready" | "error"
    int progress = 0;             // 下载中 0-100
    std::string error;            // state == "error" 时的人类可读原因
    std::string detail;           // absent 时说明还缺哪些组件（导入了一部分时）
    bool model_installed = false; // 模型文件本身在盘上（可删除），不论运行库与 VAD 是否齐全
};

// 按目录条目逐 id 给出。轻量，绝不联网。
std::map<std::string, ModelStatus> GetStatuses();

// 幂等：已就绪或该 id 已在下载时返回 true；其他任务进行中返回 false。
bool StartDownload(const std::string &model_id);

// 导入用户自行下载的文件：运行库压缩包、silero_vad.onnx 或任一模型压缩包，按 SHA256 识别。
// 后台校验并安装，结果记在 model_id 的状态里。源文件不是普通文件或有任务进行中时返回 false。
bool Import(const std::string &model_id, const std::filesystem::path &source);

// 弹出系统「打开文件」对话框，取消时返回空路径。须在 UI 线程调用。
std::filesystem::path PromptForFile(HWND owner);

// 删除模型目录。运行库与 VAD 是共用的，不随模型删除（运行库还可能被 Server 载入着）。
// 该 id 任务进行中拒绝。
bool Delete(const std::string &model_id);

} // namespace local_asr_download
