#include "settings/user_data_backup.h"

#include "settings/zip_archive.h"
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>
#include <sqlite3.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;

struct Workspace
{
    explicit Workspace(const char *name)
    {
        root = test::TempRoot() / "user-data-backup" / name;
        std::error_code error;
        fs::remove_all(root, error);
        fs::create_directories(root, error);
    }
    ~Workspace()
    {
        std::error_code error;
        fs::remove_all(root, error);
    }
    fs::path root;
};

void WriteFile(const fs::path &path, const std::string &content)
{
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string ReadFile(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void RequireOk(const bool ok, const std::string &message)
{
    if (!ok)
        throw std::runtime_error(message);
}

SettingsUserDataBackup::Context MakeContext(const fs::path &root)
{
    SettingsUserDataBackup::Context context;
    context.data_dir = root / "data";
    context.config_dir = context.data_dir;
    std::error_code error;
    fs::create_directories(context.data_dir, error);
    return context;
}

struct HookState
{
    int stop_calls = 0;
    int start_calls = 0;
    int export_calls = 0;
    std::vector<std::string> imported_dictionaries;
    std::string last_dictionary_content;
    std::string export_content = "ni'hao\t你好\t10\n";
    std::string export_failure_message = "词库导出失败";
    bool stop_ok = true;
    bool start_ok = true;
    bool export_ok = true;
    bool dictionary_import_ok = true;
};

SettingsUserDataBackup::Hooks MakeHooks(const std::shared_ptr<HookState> &state)
{
    SettingsUserDataBackup::Hooks hooks;
    hooks.stop_server = [state](std::string &message) {
        ++state->stop_calls;
        if (!state->stop_ok)
            message = "stop failed";
        return state->stop_ok;
    };
    hooks.start_server = [state](std::string &message) {
        ++state->start_calls;
        if (!state->start_ok)
            message = "输入法未自动启动，请手动启动水杉输入法。";
        return state->start_ok;
    };
    hooks.export_dictionary = [state](const std::string &dictionary, std::string &content, std::string &message) {
        ++state->export_calls;
        // SettingsDictionary reports an untouched dictionary as a failure whose message
        // starts with 当前没有可导出; that is the only case Export may treat as empty.
        if (dictionary != "quanpin")
        {
            message = "当前没有可导出的用户新增词条";
            return false;
        }
        if (!state->export_ok)
        {
            message = state->export_failure_message;
            return false;
        }
        content = state->export_content;
        return true;
    };
    hooks.import_dictionary = [state](const std::string &dictionary, const std::string &content, std::string &message) {
        state->imported_dictionaries.push_back(dictionary);
        state->last_dictionary_content = content;
        if (!state->dictionary_import_ok)
            message = "dictionary import failed";
        return state->dictionary_import_ok;
    };
    return hooks;
}

void PrepareSource(const SettingsUserDataBackup::Context &context)
{
    WriteFile(context.data_dir / "config.toml", "[general]\nlanguage = \"zh-cn\"\n");
    WriteFile(context.data_dir / "skins" / "custom" / "theme" / "skin.toml", "name = \"custom\"\n");
    WriteFile(context.data_dir / "skins" / "default" / "theme" / "skin.toml", "system skin\n");
    // A file directly under skins/ is not a skin directory. Export must not add it:
    // the archive's own whitelist only accepts skins/<name>/<file...>.
    WriteFile(context.data_dir / "skins" / "README.md", "stray notes\n");
    WriteFile(context.data_dir / "models" / "neural-desktop" / "sentence-model-desktop.safetensors", "model-bytes");
}

std::string SerializeManifest(const std::vector<user_data_zip::Entry> &entries)
{
    boost::json::array items;
    for (const user_data_zip::Entry &entry : entries)
    {
        boost::json::object item;
        item["path"] = entry.path;
        item["size"] = entry.size;
        item["crc32"] = entry.crc;
        items.push_back(std::move(item));
    }
    boost::json::object manifest;
    manifest["format"] = "metasequoia-user-data";
    manifest["version"] = 1;
    manifest["items"] = std::move(items);
    return boost::json::serialize(manifest);
}

void ReplaceAllBytes(const fs::path &path, const std::string &needle, const std::string &replacement)
{
    REQUIRE_EQ(needle.size(), replacement.size());
    std::string bytes = ReadFile(path);
    std::size_t position = bytes.find(needle);
    REQUIRE(position != std::string::npos);
    while (position != std::string::npos)
    {
        bytes.replace(position, needle.size(), replacement);
        position = bytes.find(needle, position + replacement.size());
    }
    WriteFile(path, bytes);
}

void FlipFirstDataByte(const fs::path &archive, const std::string &entry_path)
{
    user_data_zip::Reader reader(archive);
    std::string message;
    REQUIRE(reader.Open(message));
    const user_data_zip::Entry *entry = reader.Find(entry_path);
    REQUIRE(entry != nullptr);
    const std::uint64_t offset = entry->local_offset + 30ull + entry->path.size();
    std::fstream file(archive, std::ios::binary | std::ios::in | std::ios::out);
    REQUIRE(static_cast<bool>(file));
    char byte = 0;
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(&byte, 1);
    REQUIRE(file.gcount() == 1);
    byte = static_cast<char>(byte ^ 0x5A);
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(&byte, 1);
    REQUIRE(static_cast<bool>(file));
}

void CreateStatisticsDatabase(const fs::path &path, const int value)
{
    sqlite3 *database = nullptr;
    const std::string utf8 = path.u8string();
    REQUIRE(sqlite3_open_v2(utf8.c_str(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(database, "CREATE TABLE stats(value INTEGER);", nullptr, nullptr, nullptr) == SQLITE_OK);
    const std::string insert = "INSERT INTO stats VALUES(" + std::to_string(value) + ");";
    REQUIRE(sqlite3_exec(database, insert.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_close(database) == SQLITE_OK);
}

int ReadStatisticsValue(const fs::path &path)
{
    sqlite3 *database = nullptr;
    const std::string utf8 = path.u8string();
    REQUIRE(sqlite3_open_v2(utf8.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(database, "SELECT value FROM stats;", -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    const int value = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return value;
}

int ReadStatisticsMaximum(const fs::path &path)
{
    sqlite3 *database = nullptr;
    const std::string utf8 = path.u8string();
    REQUIRE(sqlite3_open_v2(utf8.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(database, "SELECT MAX(value) FROM stats;", -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    const int value = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return value;
}

// Leaves <copy>/stats.db plus a non-empty stats.db-wal behind, the state an
// ExitProcess-ed Server leaves: the copy is consistent, the WAL holds the newest rows.
void CopyStatisticsDatabaseWithWal(const fs::path &path, const int checkpointed, const int in_wal, const fs::path &copy)
{
    sqlite3 *database = nullptr;
    const std::string utf8 = path.u8string();
    std::error_code directory_error;
    fs::create_directories(path.parent_path(), directory_error);
    REQUIRE(!directory_error);
    REQUIRE(sqlite3_open_v2(utf8.c_str(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(database, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(database, "CREATE TABLE stats(value INTEGER);", nullptr, nullptr, nullptr) == SQLITE_OK);
    const std::string checkpointed_insert = "INSERT INTO stats VALUES(" + std::to_string(checkpointed) + ");";
    REQUIRE(sqlite3_exec(database, checkpointed_insert.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(database, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(database, "PRAGMA wal_autocheckpoint=0;", nullptr, nullptr, nullptr) == SQLITE_OK);
    const std::string wal_insert = "INSERT INTO stats VALUES(" + std::to_string(in_wal) + ");";
    REQUIRE(sqlite3_exec(database, wal_insert.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    fs::path wal = path;
    wal += L"-wal";
    REQUIRE(fs::file_size(wal) > 0);
    std::error_code error;
    fs::create_directories(copy, error);
    REQUIRE(fs::copy_file(path, copy / path.filename(), fs::copy_options::overwrite_existing, error));
    REQUIRE(
        fs::copy_file(wal, copy / (path.filename().wstring() + L"-wal"), fs::copy_options::overwrite_existing, error));
    REQUIRE(sqlite3_close(database) == SQLITE_OK);
}

// Builds an archive whose manifest is written by the caller so that mismatches can be
// expressed without touching a valid export afterwards.
user_data_zip::Entry AddEntry(user_data_zip::Writer &writer, const std::string &path, const std::string &content,
                              std::string &message)
{
    user_data_zip::Entry entry;
    REQUIRE(writer.AddBytes(path, content, message, &entry));
    return entry;
}
} // namespace

TEST_CASE(user_data_backup_export_never_stops_server_and_reopens)
{
    Workspace workspace("export");
    const SettingsUserDataBackup::Context context = MakeContext(workspace.root);
    PrepareSource(context);
    auto state = std::make_shared<HookState>();
    const auto archive = workspace.root / "backup.zip";
    std::string message;
    REQUIRE(SettingsUserDataBackup::Export(context, archive, false, MakeHooks(state), message));
    REQUIRE_EQ(state->stop_calls, 0);
    REQUIRE_EQ(state->start_calls, 0);
    REQUIRE(fs::is_regular_file(archive));
    REQUIRE(SettingsUserDataBackup::Validate(archive, message));

    user_data_zip::Reader reader(archive);
    REQUIRE(reader.Open(message));
    REQUIRE(reader.Find("manifest.json") != nullptr);
    REQUIRE(reader.Find("config.toml") != nullptr);
    REQUIRE(reader.Find("skins/custom/theme/skin.toml") != nullptr);
    REQUIRE(reader.Find("skins/default/theme/skin.toml") == nullptr);
    REQUIRE(reader.Find("skins/README.md") == nullptr);
    REQUIRE(reader.Find("dictionary/quanpin.txt") != nullptr);
    REQUIRE(reader.Find("models/neural-desktop/sentence-model-desktop.safetensors") == nullptr);
    REQUIRE(!message.empty());
}

TEST_CASE(user_data_backup_export_respects_include_models)
{
    Workspace workspace("models");
    const SettingsUserDataBackup::Context context = MakeContext(workspace.root);
    PrepareSource(context);
    auto state = std::make_shared<HookState>();
    std::string message;

    const auto without = workspace.root / "without.zip";
    REQUIRE(SettingsUserDataBackup::Export(context, without, false, MakeHooks(state), message));
    user_data_zip::Reader without_reader(without);
    REQUIRE(without_reader.Open(message));
    for (const user_data_zip::Entry &entry : without_reader.Entries())
        REQUIRE(entry.path.rfind("models/", 0) != 0);

    const auto with = workspace.root / "with.zip";
    RequireOk(SettingsUserDataBackup::Export(context, with, true, MakeHooks(state), message), message);
    user_data_zip::Reader with_reader(with);
    REQUIRE(with_reader.Open(message));
    bool found_model = false;
    for (const user_data_zip::Entry &entry : with_reader.Entries())
        found_model = found_model || entry.path == "models/neural-desktop/sentence-model-desktop.safetensors";
    REQUIRE(found_model);
}

TEST_CASE(user_data_backup_import_applies_files_backs_up_and_restarts)
{
    Workspace workspace("import");
    const SettingsUserDataBackup::Context source_context = MakeContext(workspace.root / "source");
    PrepareSource(source_context);
    const auto archive = workspace.root / "backup.zip";
    std::string message;
    auto source_state = std::make_shared<HookState>();
    RequireOk(SettingsUserDataBackup::Export(source_context, archive, true, MakeHooks(source_state), message), message);

    const SettingsUserDataBackup::Context destination = MakeContext(workspace.root / "destination");
    WriteFile(destination.data_dir / "config.toml", "# old config\n");
    WriteFile(destination.data_dir / "skins" / "custom" / "theme" / "skin.toml", "old skin\n");
    WriteFile(destination.data_dir / "models" / "neural-desktop" / "sentence-model-desktop.safetensors", "old model\n");

    auto state = std::make_shared<HookState>();
    REQUIRE(SettingsUserDataBackup::Import(destination, archive, MakeHooks(state), message));
    REQUIRE_EQ(state->stop_calls, 1);
    REQUIRE_EQ(state->start_calls, 1);
    REQUIRE_EQ(state->imported_dictionaries.size(), std::size_t{1});
    REQUIRE_EQ(state->imported_dictionaries[0], std::string("quanpin"));
    REQUIRE_EQ(state->last_dictionary_content, source_state->export_content);

    REQUIRE_EQ(ReadFile(destination.data_dir / "config.toml"), ReadFile(source_context.data_dir / "config.toml"));
    REQUIRE_EQ(ReadFile(destination.data_dir / "skins" / "custom" / "theme" / "skin.toml"),
               ReadFile(source_context.data_dir / "skins" / "custom" / "theme" / "skin.toml"));
    REQUIRE_EQ(ReadFile(destination.data_dir / "models" / "neural-desktop" / "sentence-model-desktop.safetensors"),
               std::string("model-bytes"));

    const fs::path backup = destination.data_dir / ".backup-before-import";
    REQUIRE(fs::is_directory(backup));
    REQUIRE_EQ(ReadFile(backup / "config.toml"), std::string("# old config\n"));
    REQUIRE_EQ(ReadFile(backup / "skins" / "custom" / "theme" / "skin.toml"), std::string("old skin\n"));
}

TEST_CASE(user_data_backup_import_without_models_keeps_local_models)
{
    Workspace workspace("no-models");
    const SettingsUserDataBackup::Context source_context = MakeContext(workspace.root / "source");
    PrepareSource(source_context);
    const auto archive = workspace.root / "backup.zip";
    std::string message;
    auto source_state = std::make_shared<HookState>();
    REQUIRE(SettingsUserDataBackup::Export(source_context, archive, false, MakeHooks(source_state), message));

    const SettingsUserDataBackup::Context destination = MakeContext(workspace.root / "destination");
    const fs::path local_model =
        destination.data_dir / "models" / "neural-desktop" / "sentence-model-desktop.safetensors";
    WriteFile(local_model, "keep me\n");

    auto state = std::make_shared<HookState>();
    REQUIRE(SettingsUserDataBackup::Import(destination, archive, MakeHooks(state), message));
    REQUIRE_EQ(state->stop_calls, 1);
    REQUIRE_EQ(state->start_calls, 1);
    REQUIRE_EQ(ReadFile(local_model), std::string("keep me\n"));
}

TEST_CASE(user_data_backup_rejects_traversal_before_stop)
{
    Workspace workspace("traversal");
    const SettingsUserDataBackup::Context source_context = MakeContext(workspace.root / "source");
    PrepareSource(source_context);
    const auto archive = workspace.root / "backup.zip";
    std::string message;
    auto source_state = std::make_shared<HookState>();
    REQUIRE(SettingsUserDataBackup::Export(source_context, archive, false, MakeHooks(source_state), message));

    ReplaceAllBytes(archive, "skins/custom/theme/skin.toml", "skins/../evil/skin.toml.abcd");
    REQUIRE(!SettingsUserDataBackup::Validate(archive, message));

    const SettingsUserDataBackup::Context destination = MakeContext(workspace.root / "destination");
    WriteFile(destination.data_dir / "config.toml", "# keep\n");
    auto state = std::make_shared<HookState>();
    REQUIRE(!SettingsUserDataBackup::Import(destination, archive, MakeHooks(state), message));
    REQUIRE_EQ(state->stop_calls, 0);
    REQUIRE_EQ(ReadFile(destination.data_dir / "config.toml"), std::string("# keep\n"));
    REQUIRE(!fs::exists(destination.data_dir / ".backup-before-import"));
}

TEST_CASE(user_data_backup_rejects_case_collision_before_stop)
{
    Workspace workspace("collision");
    const auto archive = workspace.root / "backup.zip";
    user_data_zip::Writer writer(archive);
    std::string message;
    user_data_zip::Entry first;
    user_data_zip::Entry second;
    REQUIRE(writer.AddBytes("skins/A/theme.txt", "one", message, &first));
    REQUIRE(writer.AddBytes("skins/a/theme.txt", "two", message, &second));
    REQUIRE(writer.AddBytes("manifest.json", SerializeManifest({first, second}), message));
    REQUIRE(writer.Finish(message));

    const SettingsUserDataBackup::Context destination = MakeContext(workspace.root / "destination");
    WriteFile(destination.data_dir / "config.toml", "# keep\n");
    auto state = std::make_shared<HookState>();
    REQUIRE(!SettingsUserDataBackup::Import(destination, archive, MakeHooks(state), message));
    REQUIRE_EQ(state->stop_calls, 0);
    REQUIRE_EQ(ReadFile(destination.data_dir / "config.toml"), std::string("# keep\n"));
}

TEST_CASE(user_data_backup_rejects_crc_mismatch_before_stop)
{
    Workspace workspace("crc");
    const SettingsUserDataBackup::Context source_context = MakeContext(workspace.root / "source");
    PrepareSource(source_context);
    const auto archive = workspace.root / "backup.zip";
    std::string message;
    auto source_state = std::make_shared<HookState>();
    REQUIRE(SettingsUserDataBackup::Export(source_context, archive, false, MakeHooks(source_state), message));
    FlipFirstDataByte(archive, "config.toml");
    REQUIRE(!SettingsUserDataBackup::Validate(archive, message));

    const SettingsUserDataBackup::Context destination = MakeContext(workspace.root / "destination");
    WriteFile(destination.data_dir / "config.toml", "# keep\n");
    auto state = std::make_shared<HookState>();
    REQUIRE(!SettingsUserDataBackup::Import(destination, archive, MakeHooks(state), message));
    REQUIRE_EQ(state->stop_calls, 0);
    REQUIRE_EQ(state->start_calls, 0);
    REQUIRE_EQ(ReadFile(destination.data_dir / "config.toml"), std::string("# keep\n"));
}

TEST_CASE(user_data_backup_dictionary_failure_keeps_backup_and_restarts)
{
    Workspace workspace("dictionary-failure");
    const SettingsUserDataBackup::Context source_context = MakeContext(workspace.root / "source");
    PrepareSource(source_context);
    const auto archive = workspace.root / "backup.zip";
    std::string message;
    auto source_state = std::make_shared<HookState>();
    REQUIRE(SettingsUserDataBackup::Export(source_context, archive, false, MakeHooks(source_state), message));

    const SettingsUserDataBackup::Context destination = MakeContext(workspace.root / "destination");
    WriteFile(destination.data_dir / "config.toml", "# old config\n");
    auto state = std::make_shared<HookState>();
    state->dictionary_import_ok = false;
    REQUIRE(!SettingsUserDataBackup::Import(destination, archive, MakeHooks(state), message));
    REQUIRE_EQ(state->stop_calls, 1);
    REQUIRE_EQ(state->start_calls, 1);
    REQUIRE_EQ(ReadFile(destination.data_dir / "config.toml"), ReadFile(source_context.data_dir / "config.toml"));
    REQUIRE_EQ(ReadFile(destination.data_dir / ".backup-before-import" / "config.toml"), std::string("# old config\n"));
    REQUIRE(message.find("导入前副本") != std::string::npos);
}

TEST_CASE(user_data_backup_stop_failure_writes_nothing)
{
    Workspace workspace("stop-failure");
    const SettingsUserDataBackup::Context source_context = MakeContext(workspace.root / "source");
    PrepareSource(source_context);
    const auto archive = workspace.root / "backup.zip";
    std::string message;
    auto source_state = std::make_shared<HookState>();
    REQUIRE(SettingsUserDataBackup::Export(source_context, archive, false, MakeHooks(source_state), message));

    const SettingsUserDataBackup::Context destination = MakeContext(workspace.root / "destination");
    WriteFile(destination.data_dir / "config.toml", "# keep\n");
    auto state = std::make_shared<HookState>();
    state->stop_ok = false;
    REQUIRE(!SettingsUserDataBackup::Import(destination, archive, MakeHooks(state), message));
    REQUIRE_EQ(state->stop_calls, 1);
    REQUIRE_EQ(state->start_calls, 1);
    REQUIRE_EQ(ReadFile(destination.data_dir / "config.toml"), std::string("# keep\n"));
    REQUIRE(!fs::exists(destination.data_dir / ".backup-before-import"));
}

TEST_CASE(user_data_backup_start_failure_is_reported)
{
    Workspace workspace("start-failure");
    const SettingsUserDataBackup::Context source_context = MakeContext(workspace.root / "source");
    PrepareSource(source_context);
    const auto archive = workspace.root / "backup.zip";
    std::string message;
    auto source_state = std::make_shared<HookState>();
    REQUIRE(SettingsUserDataBackup::Export(source_context, archive, false, MakeHooks(source_state), message));

    const SettingsUserDataBackup::Context destination = MakeContext(workspace.root / "destination");
    auto state = std::make_shared<HookState>();
    state->start_ok = false;
    REQUIRE(!SettingsUserDataBackup::Import(destination, archive, MakeHooks(state), message));
    REQUIRE_EQ(state->stop_calls, 1);
    REQUIRE_EQ(state->start_calls, 1);
    REQUIRE(message.find("手动启动") != std::string::npos);
}

TEST_CASE(user_data_backup_export_fails_when_dictionary_export_fails)
{
    Workspace workspace("dictionary-export-failure");
    const SettingsUserDataBackup::Context context = MakeContext(workspace.root);
    PrepareSource(context);
    auto state = std::make_shared<HookState>();
    state->export_ok = false;
    state->export_failure_message = "词库数据库被占用";
    const auto archive = workspace.root / "backup.zip";
    std::string message;
    REQUIRE(!SettingsUserDataBackup::Export(context, archive, false, MakeHooks(state), message));
    REQUIRE(message.find("quanpin") != std::string::npos);
    REQUIRE(message.find("词库数据库被占用") != std::string::npos);
    REQUIRE(!fs::exists(archive));
    REQUIRE_EQ(state->stop_calls, 0);
    REQUIRE_EQ(state->start_calls, 0);
}

TEST_CASE(user_data_backup_import_restores_statistics_snapshot)
{
    Workspace workspace("statistics");
    const SettingsUserDataBackup::Context source_context = MakeContext(workspace.root / "source");
    PrepareSource(source_context);
    CreateStatisticsDatabase(source_context.data_dir / "stats.db", 11);
    const auto archive = workspace.root / "backup.zip";
    std::string message;
    auto source_state = std::make_shared<HookState>();
    RequireOk(SettingsUserDataBackup::Export(source_context, archive, false, MakeHooks(source_state), message),
              message);

    user_data_zip::Reader reader(archive);
    REQUIRE(reader.Open(message));
    REQUIRE(reader.Find("stats.db") != nullptr);

    const SettingsUserDataBackup::Context destination = MakeContext(workspace.root / "destination");
    CreateStatisticsDatabase(destination.data_dir / "stats.db", 22);
    auto state = std::make_shared<HookState>();
    RequireOk(SettingsUserDataBackup::Import(destination, archive, MakeHooks(state), message), message);
    REQUIRE_EQ(ReadStatisticsValue(destination.data_dir / "stats.db"), 11);
    REQUIRE_EQ(ReadStatisticsValue(destination.data_dir / ".backup-before-import" / "stats.db"), 22);
}

TEST_CASE(user_data_backup_import_checkpoints_local_statistics_before_replace)
{
    Workspace workspace("statistics-wal");
    const SettingsUserDataBackup::Context source_context = MakeContext(workspace.root / "source");
    PrepareSource(source_context);
    CreateStatisticsDatabase(source_context.data_dir / "stats.db", 11);
    const auto archive = workspace.root / "backup.zip";
    std::string message;
    auto source_state = std::make_shared<HookState>();
    RequireOk(SettingsUserDataBackup::Export(source_context, archive, false, MakeHooks(source_state), message),
              message);

    const SettingsUserDataBackup::Context destination = MakeContext(workspace.root / "destination");
    const fs::path wal_source = workspace.root / "wal-source";
    const fs::path wal_copy = workspace.root / "wal-copy";
    CopyStatisticsDatabaseWithWal(wal_source / "stats.db", 22, 23, wal_copy);
    std::error_code error;
    REQUIRE(fs::copy_file(wal_copy / "stats.db", destination.data_dir / "stats.db",
                          fs::copy_options::overwrite_existing, error));
    REQUIRE(fs::copy_file(wal_copy / "stats.db-wal", destination.data_dir / "stats.db-wal",
                          fs::copy_options::overwrite_existing, error));
    REQUIRE(fs::file_size(destination.data_dir / "stats.db-wal") > 0);

    auto state = std::make_shared<HookState>();
    RequireOk(SettingsUserDataBackup::Import(destination, archive, MakeHooks(state), message), message);
    // The imported snapshot wins, and no old WAL is left to be replayed over it.
    REQUIRE_EQ(ReadStatisticsValue(destination.data_dir / "stats.db"), 11);
    // The pre-import copy includes the rows that only existed in the WAL.
    REQUIRE_EQ(ReadStatisticsMaximum(destination.data_dir / ".backup-before-import" / "stats.db"), 23);
}

TEST_CASE(user_data_backup_rejects_invalid_config_before_stop)
{
    Workspace workspace("invalid-config");
    const auto archive = workspace.root / "bad-config.zip";
    std::string message;
    user_data_zip::Writer writer(archive);
    const user_data_zip::Entry config = AddEntry(writer, "config.toml", "not = [valid toml\n", message);
    const user_data_zip::Entry skin = AddEntry(writer, "skins/custom/theme/skin.toml", "name = \"x\"\n", message);
    REQUIRE(writer.AddBytes("manifest.json", SerializeManifest({config, skin}), message));
    REQUIRE(writer.Finish(message));

    const SettingsUserDataBackup::Context destination = MakeContext(workspace.root / "destination");
    WriteFile(destination.data_dir / "config.toml", "# keep\n");
    auto state = std::make_shared<HookState>();
    REQUIRE(!SettingsUserDataBackup::Import(destination, archive, MakeHooks(state), message));
    REQUIRE_EQ(state->stop_calls, 0);
    REQUIRE_EQ(state->start_calls, 0);
    REQUIRE_EQ(ReadFile(destination.data_dir / "config.toml"), std::string("# keep\n"));
    REQUIRE(!fs::exists(destination.data_dir / ".backup-before-import"));
}

TEST_CASE(user_data_backup_rejects_invalid_statistics_before_stop)
{
    Workspace workspace("invalid-stats");
    const auto archive = workspace.root / "bad-stats.zip";
    std::string message;
    user_data_zip::Writer writer(archive);
    const user_data_zip::Entry stats = AddEntry(writer, "stats.db", std::string(64, 'x'), message);
    const user_data_zip::Entry skin = AddEntry(writer, "skins/custom/theme/skin.toml", "name = \"x\"\n", message);
    REQUIRE(writer.AddBytes("manifest.json", SerializeManifest({stats, skin}), message));
    REQUIRE(writer.Finish(message));

    const SettingsUserDataBackup::Context destination = MakeContext(workspace.root / "destination");
    WriteFile(destination.data_dir / "config.toml", "# keep\n");
    auto state = std::make_shared<HookState>();
    REQUIRE(!SettingsUserDataBackup::Import(destination, archive, MakeHooks(state), message));
    REQUIRE_EQ(state->stop_calls, 0);
    REQUIRE_EQ(state->start_calls, 0);
    REQUIRE_EQ(ReadFile(destination.data_dir / "config.toml"), std::string("# keep\n"));
    REQUIRE(!fs::exists(destination.data_dir / ".backup-before-import"));
}

TEST_CASE(user_data_backup_rejects_manifest_mismatch_before_stop)
{
    Workspace workspace("manifest-mismatch");
    const auto archive = workspace.root / "mismatch.zip";
    std::string message;
    user_data_zip::Writer writer(archive);
    const user_data_zip::Entry skin = AddEntry(writer, "skins/custom/theme/skin.toml", "name = \"x\"\n", message);
    user_data_zip::Entry wrong = skin;
    wrong.size += 5;
    REQUIRE(writer.AddBytes("manifest.json", SerializeManifest({wrong}), message));
    REQUIRE(writer.Finish(message));
    REQUIRE(!SettingsUserDataBackup::Validate(archive, message));

    const SettingsUserDataBackup::Context destination = MakeContext(workspace.root / "destination");
    auto state = std::make_shared<HookState>();
    REQUIRE(!SettingsUserDataBackup::Import(destination, archive, MakeHooks(state), message));
    REQUIRE_EQ(state->stop_calls, 0);
    REQUIRE(!fs::exists(destination.data_dir / ".backup-before-import"));
}

TEST_CASE(user_data_backup_rejects_case_variant_manifest_before_stop)
{
    Workspace workspace("manifest-duplicate");
    const auto archive = workspace.root / "duplicate-manifest.zip";
    std::string message;
    user_data_zip::Writer writer(archive);
    const user_data_zip::Entry skin = AddEntry(writer, "skins/custom/theme/skin.toml", "name = \"x\"\n", message);
    REQUIRE(writer.AddBytes("Manifest.json", "{}", message));
    REQUIRE(writer.AddBytes("manifest.json", SerializeManifest({skin}), message));
    REQUIRE(writer.Finish(message));
    REQUIRE(!SettingsUserDataBackup::Validate(archive, message));

    const SettingsUserDataBackup::Context destination = MakeContext(workspace.root / "destination");
    auto state = std::make_shared<HookState>();
    REQUIRE(!SettingsUserDataBackup::Import(destination, archive, MakeHooks(state), message));
    REQUIRE_EQ(state->stop_calls, 0);
}

TEST_CASE(user_data_backup_rejects_file_directory_conflict_before_stop)
{
    Workspace workspace("file-directory-conflict");
    const auto archive = workspace.root / "conflict.zip";
    std::string message;
    user_data_zip::Writer writer(archive);
    const user_data_zip::Entry file = AddEntry(writer, "skins/custom/theme/skin.toml", "name = \"x\"\n", message);
    const user_data_zip::Entry nested =
        AddEntry(writer, "skins/custom/theme/skin.toml/nested.txt", "nested\n", message);
    REQUIRE(writer.AddBytes("manifest.json", SerializeManifest({file, nested}), message));
    REQUIRE(writer.Finish(message));
    REQUIRE(!SettingsUserDataBackup::Validate(archive, message));

    const SettingsUserDataBackup::Context destination = MakeContext(workspace.root / "destination");
    auto state = std::make_shared<HookState>();
    REQUIRE(!SettingsUserDataBackup::Import(destination, archive, MakeHooks(state), message));
    REQUIRE_EQ(state->stop_calls, 0);
}

TEST_CASE(user_data_backup_rejects_windows_device_and_ads_paths)
{
    using SettingsUserDataBackup::IsAllowedDataPath;
    REQUIRE(!user_data_zip::IsSafeArchivePath("skins/CON/x.txt"));
    REQUIRE(!user_data_zip::IsSafeArchivePath("skins/con.txt/x.txt"));
    REQUIRE(!user_data_zip::IsSafeArchivePath("models/NUL/model.gram"));
    REQUIRE(!user_data_zip::IsSafeArchivePath("skins/COM1/x.txt"));
    REQUIRE(!user_data_zip::IsSafeArchivePath("skins/LPT9/x.txt"));
    REQUIRE(!user_data_zip::IsSafeArchivePath("skins/a.txt:stream"));
    REQUIRE(!user_data_zip::IsSafeArchivePath("C:/x.txt"));
    REQUIRE(!user_data_zip::IsSafeArchivePath("/x.txt"));
    REQUIRE(!user_data_zip::IsSafeArchivePath("skins//x.txt"));
    REQUIRE(!user_data_zip::IsSafeArchivePath("skins/./x.txt"));
    REQUIRE(!user_data_zip::IsSafeArchivePath("skins/../x.txt"));
    REQUIRE(!user_data_zip::IsSafeArchivePath("skins\\x.txt"));
    REQUIRE(!user_data_zip::IsSafeArchivePath("skins/trail./x.txt"));
    REQUIRE(!user_data_zip::IsSafeArchivePath("skins/trail /x.txt"));
    REQUIRE(user_data_zip::IsSafeArchivePath("skins/colour/background.png"));
    REQUIRE(user_data_zip::IsSafeArchivePath("models/wanxiang/model.gram"));

    REQUIRE(!IsAllowedDataPath("skins/default/theme/skin.toml"));
    REQUIRE(!IsAllowedDataPath("skins/only-name"));
    REQUIRE(!IsAllowedDataPath("dictionary/quanpin.txt.bak"));
    REQUIRE(IsAllowedDataPath("dictionary/quick.txt"));
    REQUIRE(IsAllowedDataPath("models/wanxiang/model.gram"));
}

TEST_CASE(user_data_backup_import_fails_cleanly_when_previous_backup_is_locked)
{
    Workspace workspace("locked-backup");
    const SettingsUserDataBackup::Context source_context = MakeContext(workspace.root / "source");
    PrepareSource(source_context);
    const auto archive = workspace.root / "backup.zip";
    std::string message;
    auto source_state = std::make_shared<HookState>();
    RequireOk(SettingsUserDataBackup::Export(source_context, archive, false, MakeHooks(source_state), message),
              message);

    const SettingsUserDataBackup::Context destination = MakeContext(workspace.root / "destination");
    WriteFile(destination.data_dir / "config.toml", "# keep\n");
    const fs::path backup_root = destination.data_dir / ".backup-before-import";
    const fs::path locked = backup_root / "locked.bin";
    WriteFile(locked, "locked\n");
    HANDLE handle =
        CreateFileW(locked.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);

    auto state = std::make_shared<HookState>();
    const bool imported = SettingsUserDataBackup::Import(destination, archive, MakeHooks(state), message);
    CloseHandle(handle);
    REQUIRE(!imported);
    REQUIRE_EQ(state->stop_calls, 1);
    REQUIRE_EQ(state->start_calls, 1);
    REQUIRE(message.find("无法清理旧的导入前副本") != std::string::npos);
    REQUIRE_EQ(ReadFile(destination.data_dir / "config.toml"), std::string("# keep\n"));
}

TEST_CASE(user_data_backup_import_reports_partial_failure_for_blocked_target)
{
    Workspace workspace("blocked-target");
    const SettingsUserDataBackup::Context source_context = MakeContext(workspace.root / "source");
    PrepareSource(source_context);
    const auto archive = workspace.root / "backup.zip";
    std::string message;
    auto source_state = std::make_shared<HookState>();
    RequireOk(SettingsUserDataBackup::Export(source_context, archive, true, MakeHooks(source_state), message), message);

    const SettingsUserDataBackup::Context destination = MakeContext(workspace.root / "destination");
    WriteFile(destination.data_dir / "config.toml", "# keep\n");
    // The archive wants a file here; a local directory with the same name blocks the apply.
    WriteFile(destination.data_dir / "models" / "neural-desktop" / "sentence-model-desktop.safetensors" / "keep.txt",
              "keep\n");

    auto state = std::make_shared<HookState>();
    REQUIRE(!SettingsUserDataBackup::Import(destination, archive, MakeHooks(state), message));
    REQUIRE_EQ(state->stop_calls, 1);
    REQUIRE_EQ(state->start_calls, 1);
    REQUIRE(message.find("目标路径不是普通文件") != std::string::npos);
    REQUIRE_EQ(ReadFile(destination.data_dir / "config.toml"), std::string("# keep\n"));
    REQUIRE(fs::is_directory(destination.data_dir / ".backup-before-import"));
}
