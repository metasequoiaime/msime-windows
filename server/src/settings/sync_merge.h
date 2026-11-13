#pragma once

// 同步的语义层：把两边的用户数据按域合并，而不是整文件覆盖。
//
// 三处数据各有各的合并规则，硬塞进一个通用 diff 只会三边都不对：
//
// - 用户词库（msime_user.db）是操作日志，天然适合按 (dictionary, key, value) 取
//   updated_at 较新的一条；删除墓碑必须保留，否则远端一同步就把删掉的词复活。
//   weight_edited 标记也要跟着走，它是"用户亲手定的权重"，换词库时不能被降级成自动调频。
// - 统计（stats.db）要按设备分账，那需要给表加 device 维度并做 schema 迁移，
//   单独一个 PR 做，连同迁移测试一起评审。
// - 配置（config.toml）按键择优。凭据永远不参与同步，DPAPI 密文跨机解不开，
//   传上去只是多一份泄露面。

#include <cstdint>
#include <filesystem>
#include <string>

namespace user_data_sync
{
// 一次词库合并的统计，给设置页显示"合并了多少条"。
struct DictionaryMergeReport
{
    int taken_from_remote = 0; // 远端较新，采用远端
    int kept_local = 0;        // 本地较新或相同，保留本地
    int added = 0;             // 只存在于远端的条目
    int conflicts = 0;         // updated_at 相同但内容不同，按本地胜出并计数
};

// 把 remote 里的用户词库日志合并进 local。两边都是 msime_user.db 的路径。
// 合并是单向的（remote -> local），调用方负责在合并后回放日志到 msime.db。
// 两个文件都必须存在且结构可识别；否则返回 false 并给出原因。
bool MergeUserDictionary(const std::filesystem::path &local_db, const std::filesystem::path &remote_db,
                         DictionaryMergeReport &report, std::string &message);

// 配置合并：以 local 为底，把 remote 里"与 template 不同"的键覆盖过来——也就是只采纳
// 远端用户真正改过的值，远端还是出厂默认的键不动本地。凭据键一律保留本地值。
// 返回合并后的 TOML 文本。任一输入无法解析时原样返回 local_text。
std::string MergeConfig(const std::string &local_text, const std::string &remote_text,
                        const std::string &template_text);

// 从一段 TOML 文本里剔除凭据键的值，供上传前清洗用。其余行保持原样（包括注释与空行）。
std::string StripCredentials(const std::string &text);

// 设备标识：存在数据目录的 sync_state.json 里，跨升级保留。没有时生成一个。
// 格式是 UUID v4 风格的小写十六进制，不带花括号。生成后立即落盘。
std::string LoadOrCreateDeviceId(const std::filesystem::path &data_dir, std::string &message);

// 设备标识是否是我们认的格式（32 位小写十六进制）。暴露出来给测试和读取端校验。
bool IsValidDeviceId(const std::string &device_id);
} // namespace user_data_sync
