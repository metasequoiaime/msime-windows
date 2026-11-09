#pragma once

#include <filesystem>
#include <functional>
#include <string>

// Export and import of the user's configuration, statistics, skins, models and
// user-dictionary contents as one ZIP archive.
//
// Export never stops the input method: every source is a point-in-time snapshot
// (config file, SQLite online backup, dictionary export requests). Import
// validates the whole archive before it may stop anything, keeps a copy of every
// file it overwrites in <DataDir>/.backup-before-import, and never touches the
// user-dictionary databases directly: dictionary entries travel through the
// existing SettingsDictionary import requests.
namespace SettingsUserDataBackup
{
struct Context
{
    std::filesystem::path data_dir;
    std::filesystem::path config_dir;
};

// Paths resolved through CommonUtils, i.e. the live user profile.
Context DefaultContext();

struct Hooks
{
    // Stops the input method and waits for the Server and Watchdog processes to
    // exit. message carries the reason on failure.
    std::function<bool(std::string &message)> stop_server;
    // Starts the Watchdog again. Called whenever stop_server was attempted,
    // including after a failed stop, and never after a pre-stop validation
    // failure.
    std::function<bool(std::string &message)> start_server;
    // Runs SettingsDictionary's export request for "quanpin" / "wubi" /
    // "english" / "quick". content is the original export text.
    std::function<bool(const std::string &dictionary, std::string &content, std::string &message)> export_dictionary;
    // Runs SettingsDictionary's import request for one dictionary file.
    std::function<bool(const std::string &dictionary, const std::string &content, std::string &message)>
        import_dictionary;
};

// Hooks wired to the production Aux pipe, Watchdog process and
// SettingsDictionary::HandleRequest.
Hooks ProductionHooks();

bool Export(const std::filesystem::path &destination, bool include_models, const Hooks &hooks, std::string &message);
bool Export(const Context &context, const std::filesystem::path &destination, bool include_models, const Hooks &hooks,
            std::string &message);

// 同步上传用的导出：与 Export 相同，只是 config.toml 在写进归档之前先过一遍
// transform_config。远端永远不该出现 DPAPI 密文，凭据在传输层之前就该被剥掉。
using ConfigTransform = std::function<std::string(const std::string &)>;
bool ExportSanitized(const Context &context, const std::filesystem::path &destination, bool include_models,
                     const Hooks &hooks, const ConfigTransform &transform_config, std::string &message);

bool Import(const std::filesystem::path &source, const Hooks &hooks, std::string &message);
bool Import(const Context &context, const std::filesystem::path &source, const Hooks &hooks, std::string &message);

// Opens the archive, validates the manifest, the path whitelist, duplicate paths,
// sizes and the CRC of every entry. Applies nothing and stops nothing.
bool Validate(const std::filesystem::path &archive, std::string &message);

// Archive path rules shared with the tests: exactly the fixed layout, no
// traversal, no absolute paths, no case collision.
bool IsAllowedDataPath(const std::string &path);

// 归档导入的合并模式。常规 Import 是「整包覆盖」，用于用户自己选的备份文件；
// 同步恢复走这里：配置按键合并、凭据保留本地值、用户词库按日志合并。
//
// 为什么不能直接覆盖：同步归档里的 config.toml 在上传前被剥离过凭据（远端不该出现
// DPAPI 密文），整包覆盖会把本机的 token、语音密钥、同步密码全部清空。词库也一样，
// 覆盖会让删除墓碑失效、把别的设备上删掉的词复活。
struct MergeImportReport
{
    bool config_merged = false;
    bool dictionary_merged = false;
    int dictionary_taken_from_remote = 0;
    int dictionary_kept_local = 0;
};

bool ImportMerging(const std::filesystem::path &source, const Hooks &hooks, MergeImportReport &report,
                   std::string &message);
bool ImportMerging(const Context &context, const std::filesystem::path &source, const Hooks &hooks,
                   MergeImportReport &report, std::string &message);

// 归档里的 config.toml 与本地 config.toml 按键合并，凭据键保留本地值。暴露给测试。
std::string MergeArchivedConfig(const std::string &local_text, const std::string &archived_text,
                                const std::string &template_text);
} // namespace SettingsUserDataBackup
