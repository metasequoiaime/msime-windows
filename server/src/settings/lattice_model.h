#pragma once

// 词格整句打分用的语言模型 sc.lm（libime lm_sc 转换的 kenlm 量化 trie，约 35 MB）的按需下载。
// 它不随安装包分发：缺它时词格整句退回启发式打分；用户在设置页下载后落到
// <DataDir>/models/language-model/sc.lm，Server 下一次应用配置时就换用它，不必重启。
//
// 来源是本组织 chinese-ime-lm 仓库 language-model-libime-20260629 这个 tag 下的资产，字节数与 SHA256
// 与 language-model/lock.json 的 output 一致。换模型版本要连同 lock.json 一起改、一起评审。
//
// 与神经模型不同，kenlm 只读映射这个文件，Server 用着时删不掉：删除改为留标记，Server 立即停用、
// 下次启动时删除（见 model_download::kPendingDeleteMarker）。

#include "engine/contracts/assets/assets.h"
#include "model_download.h"

#include <Windows.h>

#include <filesystem>
#include <string>

namespace lattice_model
{

inline constexpr char kModelDirectoryName[] = "language-model";
inline constexpr const char *kModelFileName = metasequoia::assets::language_model;

// 内联在头文件里：Server 的会话层只需要据此解析路径，不必链接下载实现。
inline std::filesystem::path ModelFile()
{
    return model_download::ModelFile(kModelDirectoryName, kModelFileName);
}

std::string SourceUrl();

model_download::ModelStatus GetModelStatus();

// 文件必须与 lock.json 记录的字节数和 SHA256 完全一致。下载与导入落位前都走这里。
bool VerifyModelFile(const std::filesystem::path &file, std::string &error);

bool StartDownload();
bool ImportModel(const std::filesystem::path &source);
std::filesystem::path PromptForModelFile(HWND owner);

// 删除模型。Server 正映射着文件时留删除标记并返回 true，状态带 delete_pending。
bool DeleteModel();

} // namespace lattice_model
