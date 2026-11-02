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

bool Import(const std::filesystem::path &source, const Hooks &hooks, std::string &message);
bool Import(const Context &context, const std::filesystem::path &source, const Hooks &hooks, std::string &message);

// Opens the archive, validates the manifest, the path whitelist, duplicate paths,
// sizes and the CRC of every entry. Applies nothing and stops nothing.
bool Validate(const std::filesystem::path &archive, std::string &message);

// Archive path rules shared with the tests: exactly the fixed layout, no
// traversal, no absolute paths, no case collision.
bool IsAllowedDataPath(const std::string &path);
} // namespace SettingsUserDataBackup
