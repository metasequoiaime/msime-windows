#pragma once

// 神经网络模型整句（效果优先 / desktop）的按需下载与状态查询。
// 固定从 https://github.com/metasequoiaime/chinese-ime-lm/releases/tag/model-v1 下载，
// 模型落到 <DataDir>/models/neural-desktop/<文件名>。

#include <map>
#include <string>

namespace neural_model
{

// 模型 ID（固定值）
inline constexpr char kDesktopModelId[] = "neural-desktop";

struct ModelStatus
{
    std::string state; // "absent" | "downloading" | "ready" | "error"
    int progress = 0;  // 下载中 0-100；其余状态无意义
    std::string error; // state == "error" 时的人类可读原因
};

// 磁盘现状 + 本进程下载态。轻量，可随配置快照频繁调用。
ModelStatus GetModelStatus();

// 幂等启动指定模型的下载：该模型已在下载或已就绪时返回 true 不重复启动；
// 同一时刻至多一个下载在飞，其他请求忙碌时返回 false。
bool StartDownload();

// 删除已下载的神经网络效果优先模型。返回成功与否。
bool DeleteModel();

} // namespace neural_model
