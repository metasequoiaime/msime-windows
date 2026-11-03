// 自定义标点的配置读写、升级合并，以及发给 TSF 的载荷。期望值按施工规格与美式键盘手写。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include "config/ime_config.h"
#include "engine/contracts/custom_punctuation.h"
#include "engine/contracts/ipc_protocol_limits.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace
{
class ScopedEnv
{
  public:
    ScopedEnv(const wchar_t *name, const std::wstring &value) : name_(name)
    {
        wchar_t buffer[32768];
        const DWORD length = GetEnvironmentVariableW(name, buffer, 32768);
        had_previous_ = length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        previous_.assign(buffer, length);
        SetEnvironmentVariableW(name, value.c_str());
    }
    ~ScopedEnv()
    {
        SetEnvironmentVariableW(name_.c_str(), had_previous_ ? previous_.c_str() : nullptr);
    }
    ScopedEnv(const ScopedEnv &) = delete;
    ScopedEnv &operator=(const ScopedEnv &) = delete;

  private:
    std::wstring name_;
    std::wstring previous_;
    bool had_previous_ = false;
};

// 配置目录钉在临时目录里，不碰本机数据目录。
class ScopedConfigDirectory
{
  public:
    explicit ScopedConfigDirectory(const std::filesystem::path &root)
        : local_app_data_(L"LOCALAPPDATA", root.wstring()),
          config_dir_(L"METASEQUOIA_IME_CONFIG_DIR", (root / L"metasequoiaime").wstring()),
          data_dir_(L"METASEQUOIA_IME_DATA_DIR", (root / L"metasequoiaime").wstring())
    {
    }

  private:
    ScopedEnv local_app_data_;
    ScopedEnv config_dir_;
    ScopedEnv data_dir_;
};

std::string ReadText(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void WriteText(const std::filesystem::path &path, const std::string &text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(output));
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::vector<std::string> StoredMap(const std::string &text)
{
    const auto table = toml::parse(text);
    std::vector<std::string> entries;
    const auto *array = table["input"]["custom_punctuation_map"].as_array();
    REQUIRE(array != nullptr);
    for (const auto &node : *array)
    {
        const auto value = node.value<std::string>();
        REQUIRE(value.has_value());
        entries.push_back(*value);
    }
    return entries;
}

struct ConfigFixture
{
    explicit ConfigFixture(const wchar_t *name)
        : root(test::TempRoot() /
               (std::wstring(L"msime-自定义标点-") + name + L"-" + std::to_wstring(GetCurrentProcessId()))),
          data_dir(root / L"metasequoiaime"), config_path(data_dir / L"config.toml")
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(data_dir, ec);
        REQUIRE(!ec);
    }
    ~ConfigFixture()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::filesystem::path root;
    std::filesystem::path data_dir;
    std::filesystem::path config_path;
};

// 含 = " # \ 与 Shift 的一组映射，以及它们在 TSF 一侧该对应的触发字符与 UTF-16 符号。
const std::vector<std::string> kSpecialEntries = {
    "Shift+BracketLeft=「", "Shift+BracketRight=」", "Equal==",  "Quote=\"",
    "Shift+Semicolon=#",    "Backslash=\\",          "Comma=……", "Shift+Equal=#=\"\\",
};

std::string LoadInstalledTemplate()
{
    std::ifstream input(MSIME_DEFAULT_CONFIG_PATH, std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    return std::string((std::istreambuf_iterator<char>(input)), {});
}
} // namespace

TEST_CASE(custom_punctuation_defaults_off_and_empty)
{
    ConfigFixture fixture(L"defaults");
    ScopedConfigDirectory location(fixture.root);
    WriteText(fixture.config_path, "[input]\npaired_punctuation = true\n");
    InitImeConfig();
    REQUIRE(!GetConfiguredCustomPunctuationEnabled());
    REQUIRE(GetConfiguredCustomPunctuationMap().empty());
    REQUIRE(FormatCustomPunctuationWorkerPayload().empty());
}

TEST_CASE(custom_punctuation_map_round_trips_special_symbols)
{
    ConfigFixture fixture(L"round-trip");
    ScopedConfigDirectory location(fixture.root);
    WriteText(fixture.config_path, "[input]\ncustom_punctuation = false\ncustom_punctuation_map = []\n");
    InitImeConfig();

    REQUIRE(SetConfiguredCustomPunctuationMap(kSpecialEntries));
    REQUIRE(GetConfiguredCustomPunctuationMap() == kSpecialEntries);
    // 写进文件的是 TOML 字符串数组，用独立的 TOML 解析器读回来逐个相等。
    REQUIRE(StoredMap(ReadText(fixture.config_path)) == kSpecialEntries);
    InitImeConfig();
    REQUIRE(GetConfiguredCustomPunctuationMap() == kSpecialEntries);

    REQUIRE(SetConfiguredCustomPunctuationEnabled(true));
    InitImeConfig();
    REQUIRE(GetConfiguredCustomPunctuationEnabled());
    REQUIRE(SetConfiguredCustomPunctuationEnabled(false));
    InitImeConfig();
    REQUIRE(!GetConfiguredCustomPunctuationEnabled());
    REQUIRE(GetConfiguredCustomPunctuationMap() == kSpecialEntries);

    // 空表也能写回去。
    REQUIRE(SetConfiguredCustomPunctuationMap({}));
    InitImeConfig();
    REQUIRE(GetConfiguredCustomPunctuationMap().empty());
    REQUIRE(StoredMap(ReadText(fixture.config_path)).empty());
}

TEST_CASE(custom_punctuation_map_rejects_duplicates_and_invalid_rows_without_writing)
{
    ConfigFixture fixture(L"reject");
    ScopedConfigDirectory location(fixture.root);
    WriteText(fixture.config_path, "[input]\ncustom_punctuation_map = []\n");
    InitImeConfig();
    const std::vector<std::string> kept = {"BracketLeft=「"};
    REQUIRE(SetConfiguredCustomPunctuationMap(kept));
    const std::string before = ReadText(fixture.config_path);

    const std::vector<std::vector<std::string>> rejected = {
        // 同一触发键两行。
        {"BracketLeft=「", "BracketLeft=『"},
        {"Shift+Comma=‹", "Shift+Comma=«"},
        // 格式不对。
        {"BracketLeft"},
        {"Ctrl+BracketLeft=「"},
        {"KeyA=「"},
        {"BracketLeft="},
        {"Period=12345678"},
    };
    for (const auto &entries : rejected)
    {
        REQUIRE(!SetConfiguredCustomPunctuationMap(entries));
        REQUIRE(ReadText(fixture.config_path) == before);
        REQUIRE(GetConfiguredCustomPunctuationMap() == kept);
    }
    // 同一个键按不按 Shift 是两种触发键，可以并存。
    REQUIRE((SetConfiguredCustomPunctuationMap({"BracketLeft=「", "Shift+BracketLeft=『"})));
}

TEST_CASE(custom_punctuation_reader_keeps_valid_rows_of_a_hand_edited_config)
{
    ConfigFixture fixture(L"hand-edited");
    ScopedConfigDirectory location(fixture.root);
    // 手改的配置：不合法的元素与非字符串丢掉，同一触发键以先出现的一行为准。
    WriteText(fixture.config_path, "[input]\n"
                                   "custom_punctuation = true\n"
                                   "custom_punctuation_map = [\n"
                                   "  \"Shift+BracketLeft=「\", # 注释\n"
                                   "  \"bogus\",\n"
                                   "  42,\n"
                                   "  \"Shift+BracketLeft=『\",\n"
                                   "  \"Equal==\",\n"
                                   "]\n");
    InitImeConfig();
    REQUIRE(GetConfiguredCustomPunctuationEnabled());
    const std::vector<std::string> expected = {"Shift+BracketLeft=「", "Equal=="};
    REQUIRE(GetConfiguredCustomPunctuationMap() == expected);

    // 不是数组时按空表处理。
    WriteText(fixture.config_path, "[input]\ncustom_punctuation = true\ncustom_punctuation_map = \"Equal==\"\n");
    InitImeConfig();
    REQUIRE(GetConfiguredCustomPunctuationMap().empty());
    REQUIRE(FormatCustomPunctuationWorkerPayload().empty());
}

TEST_CASE(custom_punctuation_payload_from_a_hand_edited_config_is_always_accepted_by_the_dll)
{
    ConfigFixture fixture(L"hand-edited-payload");
    ScopedConfigDirectory location(fixture.root);
    // 手改的配置里有重复的触发键与不合法的元素：Server 规范化之后组出的载荷，DLL 整条收下。
    WriteText(fixture.config_path, "[input]\n"
                                   "custom_punctuation = true\n"
                                   "custom_punctuation_map = [\"Shift+BracketLeft=「\", \"Shift+BracketLeft=『\", "
                                   "\"Period=12345678\", \"Equal==\", \"Equal=＝\", \"KeyA=x\"]\n");
    InitImeConfig();
    FanyImeCustomPunctuation::Table parsed;
    REQUIRE(FanyImeCustomPunctuation::ParsePayload(FormatCustomPunctuationWorkerPayload(), parsed));
    REQUIRE(parsed.Rows().size() == 2);
    REQUIRE(parsed.Rows()[0].trigger == L'{');
    REQUIRE(parsed.Rows()[0].symbol == L"「");
    REQUIRE(parsed.Rows()[1].trigger == L'=');
    REQUIRE(parsed.Rows()[1].symbol == L"=");
}

TEST_CASE(custom_punctuation_payload_matches_what_the_dll_parses)
{
    ConfigFixture fixture(L"payload");
    ScopedConfigDirectory location(fixture.root);
    WriteText(fixture.config_path, "[input]\ncustom_punctuation = false\ncustom_punctuation_map = []\n");
    InitImeConfig();
    REQUIRE(SetConfiguredCustomPunctuationMap(kSpecialEntries));

    // 总开关关着：不管表里有什么，TSF 拿到空表。
    REQUIRE(FormatCustomPunctuationWorkerPayload().empty());

    REQUIRE(SetConfiguredCustomPunctuationEnabled(true));
    const std::wstring payload = FormatCustomPunctuationWorkerPayload();
    REQUIRE(payload.size() <= FanyImePipeLimits::CandidateTextMaxLength);
    FanyImeCustomPunctuation::Table parsed;
    REQUIRE(FanyImeCustomPunctuation::ParsePayload(payload, parsed));
    // 美式键盘：Shift+[ 是 {，Shift+] 是 }，' 是 '，Shift+; 是 :，Shift+= 是 +。
    const std::vector<std::pair<wchar_t, std::wstring>> expected = {
        {L'{', L"「"}, {L'}', L"」"},  {L'=', L"="},  {L'\'', L"\""},
        {L':', L"#"},  {L'\\', L"\\"}, {L',', L"……"}, {L'+', L"#=\"\\"},
    };
    REQUIRE(parsed.Rows().size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        REQUIRE(parsed.Rows()[i].trigger == expected[i].first);
        REQUIRE(parsed.Rows()[i].symbol == expected[i].second);
    }

    REQUIRE(SetConfiguredCustomPunctuationMap({}));
    REQUIRE(FormatCustomPunctuationWorkerPayload().empty());
}

TEST_CASE(custom_punctuation_payload_with_all_twenty_two_rows_fits_one_frame)
{
    ConfigFixture fixture(L"full");
    ScopedConfigDirectory location(fixture.root);
    WriteText(fixture.config_path, "[input]\ncustom_punctuation = true\ncustom_punctuation_map = []\n");
    InitImeConfig();
    const char *names[] = {"Backquote", "Minus", "Equal", "BracketLeft", "BracketRight", "Backslash",
                           "Semicolon", "Quote", "Comma", "Period",      "Slash"};
    std::vector<std::string> entries;
    for (const char *name : names)
    {
        // 7 个 UTF-16 单元：上限。
        entries.push_back(std::string(name) + "=﹁﹂﹃﹄︻︼︗");
        entries.push_back(std::string("Shift+") + name + "=🙆‍♂️12");
    }
    REQUIRE(entries.size() == 22);
    REQUIRE(SetConfiguredCustomPunctuationMap(entries));
    const std::wstring payload = FormatCustomPunctuationWorkerPayload();
    REQUIRE(payload.size() == 22 * 9);
    REQUIRE(payload.size() <= FanyImePipeLimits::CandidateTextMaxLength);
    FanyImeCustomPunctuation::Table parsed;
    REQUIRE(FanyImeCustomPunctuation::ParsePayload(payload, parsed));
    REQUIRE(parsed.Rows().size() == 22);
    REQUIRE(parsed.Find(L'?') != nullptr && *parsed.Find(L'?') == L"🙆‍♂️12");
    REQUIRE(parsed.Find(L'`') != nullptr && *parsed.Find(L'`') == L"﹁﹂﹃﹄︻︼︗");
}

TEST_CASE(shipped_templates_add_custom_punctuation_off_and_empty)
{
    std::ifstream asset(MSIME_ASSET_CONFIG_PATH, std::ios::binary);
    REQUIRE(static_cast<bool>(asset));
    const std::string working((std::istreambuf_iterator<char>(asset)), {});
    for (const std::string &text : {LoadInstalledTemplate(), working})
    {
        const auto table = toml::parse(text);
        const auto enabled = table["input"]["custom_punctuation"].value<bool>();
        REQUIRE(enabled.has_value() && !*enabled);
        const auto *map = table["input"]["custom_punctuation_map"].as_array();
        REQUIRE(map != nullptr && map->empty());
    }
}

TEST_CASE(custom_punctuation_survives_template_upgrade)
{
    const std::string installed = LoadInstalledTemplate();

    // 老配置没有这两个键：升级后补上出厂值。基线为空，用户的每个值都算改过，paired_punctuation = false 留下。
    const std::string legacy = "[input]\npaired_punctuation = false\n";
    const auto upgraded = toml::parse(MergeConfigIntoTemplate(installed, legacy, ""));
    REQUIRE(!upgraded["input"]["custom_punctuation"].value_or(true));
    REQUIRE(upgraded["input"]["custom_punctuation_map"].as_array() != nullptr);
    REQUIRE(upgraded["input"]["custom_punctuation_map"].as_array()->empty());
    REQUIRE(!upgraded["input"]["paired_punctuation"].value_or(true));

    // 已经有这两个键：含 = " # \ ] 的元素、多行写法与行尾注释都原样留下。
    std::string configured = "[input]\ncustom_punctuation = true\ncustom_punctuation_map = [\n";
    std::vector<std::string> entries = kSpecialEntries;
    entries.push_back("Shift+Slash=]");
    for (const auto &entry : entries)
    {
        std::string escaped;
        for (const char ch : entry)
        {
            if (ch == '"' || ch == '\\')
                escaped.push_back('\\');
            escaped.push_back(ch);
        }
        configured += "  \"" + escaped + "\", # 行尾注释 ] =\n";
    }
    configured += "]\n";
    for (const std::string &baseline : {std::string(), installed})
    {
        const std::string merged = MergeConfigIntoTemplate(installed, configured, baseline);
        const auto table = toml::parse(merged);
        REQUIRE(table["input"]["custom_punctuation"].value_or(false));
        REQUIRE(StoredMap(merged) == entries);
        // 再升级一次结果不变。
        REQUIRE(StoredMap(MergeConfigIntoTemplate(installed, merged, installed)) == entries);
    }
}
