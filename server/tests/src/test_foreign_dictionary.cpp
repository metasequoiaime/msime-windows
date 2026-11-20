#include "settings/dictionary_manager.h"
#include "settings/dictionary_validation.h"
#include "settings/foreign_dictionary.h"
#include "engine/quanpin/quanpin_query.h"
#include "engine/user_dictionary/user_dictionary_journal.h"
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include <windows.h>
#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
using ForeignDictionary::Entry;

std::vector<Entry> ParseOk(const char *format, const std::string &bytes)
{
    std::vector<Entry> entries;
    std::string error;
    REQUIRE(ForeignDictionary::Parse(format, bytes, entries, error));
    REQUIRE(error.empty());
    return entries;
}

std::vector<std::string> Pairs(const std::vector<Entry> &entries)
{
    std::vector<std::string> pairs;
    for (const auto &entry : entries)
        pairs.push_back(entry.word + "|" + entry.pinyin);
    return pairs;
}

std::string Le16(uint16_t value)
{
    return {static_cast<char>(value & 0xFF), static_cast<char>(value >> 8)};
}

std::string Utf16(const std::wstring &text)
{
    std::string out;
    for (const wchar_t ch : text)
        out += Le16(static_cast<uint16_t>(ch));
    return out;
}
} // namespace

TEST_CASE(ForeignDictionaryFormatsMatchTheContract)
{
    // 下拉框选项与协议的 format 枚举一一对应，顺序就是界面里的顺序。
    std::vector<std::string> ids;
    for (const auto &format : ForeignDictionary::Formats())
        ids.push_back(format.id);
    REQUIRE_EQ(ids, (std::vector<std::string>{"rime", "libimetxt", "libimebin"}));
    REQUIRE(ForeignDictionary::FindFormat("zip") == nullptr);

    // 商业输入法的私有格式有意不支持：不在下拉框里，宿主也不解析。
    for (const char *format : {"scel", "sgpy", "sgpybin", "qqpy", "qcel", "bdpy", "bdict", "plist"})
    {
        REQUIRE(ForeignDictionary::FindFormat(format) == nullptr);
        std::vector<Entry> entries;
        std::string error;
        REQUIRE(!ForeignDictionary::Parse(format, "abc", entries, error));
        REQUIRE_EQ(error, std::string("不支持的词库格式"));
    }
}

TEST_CASE(ForeignDictionaryReadsTextFormats)
{
    // Rime：跳过 YAML 头和注释；只有权重没有读音的行留给自动注音。
    REQUIRE_EQ(Pairs(ParseOk("rime", "---\nname: test\ncolumns:\n  - text\n...\n# 注释\n"
                                     "你好\tni hao\t100\r\n世界\t200\n略\tlve\n")),
               (std::vector<std::string>{"你好|ni'hao", "世界|", "略|lve"}));

    // 经 Windows 工具转存过的文本：UTF-16LE（带或不带 BOM）、GBK 都能认。
    REQUIRE_EQ(Pairs(ParseOk("rime", "\xFF\xFE" + Utf16(L"你好\tni hao\r\n"))),
               (std::vector<std::string>{"你好|ni'hao"}));
    REQUIRE_EQ(Pairs(ParseOk("rime", Utf16(L"你好\tni hao\r\n"))), (std::vector<std::string>{"你好|ni'hao"}));
    REQUIRE_EQ(Pairs(ParseOk("rime", "\xC4\xE3\xBA\xC3\tni hao\r\n")), (std::vector<std::string>{"你好|ni'hao"}));

    // LibIME 文本：引号包裹的词面按 fcitx 规则还原；只认两列或三列。
    REQUIRE_EQ(Pairs(ParseOk("libimetxt", "你好 ni'hao 0\n\"你\\\"好\" ni'hao\nbad a b c\n")),
               (std::vector<std::string>{"你好|ni'hao", "你\"好|ni'hao"}));
}
TEST_CASE(ForeignDictionaryReadsLibimeBinary)
{
    // imewlconverter 生成的 fcitx5 词库（第 2 版，zstd 压缩的 DATrie），源文本：
    //   你好 ni hao / 世界 shi jie / 略微 lve wei
    static const unsigned char kDict[] = {
        0x00, 0x0f, 0xc6, 0x13, 0x00, 0x00, 0x00, 0x02, 0x28, 0xb5, 0x2f, 0xfd, 0x04, 0x58, 0xd5, 0x0c, 0x00, 0xe4,
        0x14, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x01, 0x00, 0x4e, 0x4b, 0x45, 0x21, 0xe4, 0xbd, 0xa0, 0xe5, 0xa5,
        0xbd, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x61, 0x57, 0x47, 0x21, 0xe7, 0x95, 0xa5, 0xe5, 0xbe, 0xae, 0x4e, 0x4c,
        0x50, 0x21, 0xe4, 0xb8, 0x96, 0xe7, 0x95, 0x8c, 0x00, 0x00, 0x00, 0x00, 0xff, 0x01, 0xff, 0xff, 0xff, 0xfe,
        0xff, 0xfd, 0xfc, 0xfb, 0xfa, 0xf9, 0xf8, 0xf7, 0xf6, 0xf5, 0xf4, 0xf3, 0xf2, 0xf1, 0xf0, 0xef, 0xee, 0xed,
        0xec, 0xeb, 0xea, 0xe9, 0xe8, 0xe7, 0xe6, 0xe5, 0xe4, 0xe3, 0xe2, 0xe1, 0xe0, 0xdf, 0xde, 0xdd, 0xdc, 0xdb,
        0xda, 0xd9, 0xd8, 0xd7, 0xd6, 0xd5, 0xd4, 0xd3, 0xd2, 0xd1, 0xd0, 0xcf, 0xce, 0xcd, 0xcc, 0xcb, 0xca, 0xc9,
        0xc8, 0xc7, 0xc6, 0xc5, 0xc4, 0xc3, 0xc2, 0xc1, 0xc0, 0xbf, 0xbe, 0xbd, 0xbc, 0xbb, 0xba, 0xb7, 0xff, 0xff,
        0xff, 0xfc, 0xed, 0xba, 0xff, 0xff, 0xff, 0xb6, 0xb5, 0xb4, 0xb3, 0xb2, 0xb1, 0xb0, 0xae, 0xff, 0xff, 0xff,
        0xde, 0xb0, 0xff, 0xff, 0xff, 0xad, 0xac, 0xab, 0xaa, 0xa9, 0xa8, 0xa7, 0xa6, 0xa5, 0xa4, 0xa3, 0xa2, 0xa1,
        0xa0, 0x9f, 0x9e, 0x9d, 0x9c, 0x9b, 0x9a, 0x99, 0x98, 0x97, 0x96, 0x95, 0x94, 0x93, 0x92, 0x91, 0x90, 0x8f,
        0x8e, 0x8d, 0x8c, 0x8b, 0x8a, 0x89, 0x88, 0x87, 0x86, 0x85, 0x84, 0x83, 0x82, 0x81, 0x80, 0x7f, 0x7e, 0x7d,
        0x7c, 0x7b, 0x7a, 0x79, 0x78, 0x77, 0x76, 0x75, 0x74, 0x73, 0x72, 0x71, 0x70, 0x6f, 0x6e, 0x6d, 0x6c, 0x6b,
        0x6a, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64, 0x63, 0x62, 0x61, 0x60, 0x5f, 0x5e, 0x5d, 0x5c, 0x5b, 0x5a, 0x59,
        0x58, 0x57, 0x56, 0x55, 0x54, 0x53, 0x52, 0x51, 0x50, 0x4f, 0x4e, 0x4d, 0x4c, 0x4b, 0x4a, 0x49, 0x48, 0x47,
        0x46, 0x45, 0x44, 0x43, 0x42, 0x41, 0x40, 0x3f, 0x3e, 0x3d, 0x3c, 0x3b, 0x3a, 0x39, 0x38, 0x37, 0x36, 0x35,
        0x34, 0x33, 0x32, 0x31, 0x30, 0x2f, 0x2e, 0x2d, 0x2c, 0x2b, 0x2a, 0x29, 0x28, 0x27, 0x26, 0x25, 0x24, 0x23,
        0x22, 0x21, 0x20, 0x1f, 0x1e, 0x1d, 0x1c, 0x1b, 0x1a, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
        0x10, 0x0f, 0x0e, 0x0d, 0x0c, 0x0b, 0x0a, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0xff, 0x00,
        0x47, 0x00, 0x48, 0x00, 0x51, 0x00, 0xfd, 0x01, 0x00, 0x00, 0x01, 0x81, 0x04, 0xa8, 0x11, 0xa0, 0x2b, 0x48,
        0x0a, 0xc3, 0xde, 0x01, 0xb1, 0x07, 0x49, 0x61, 0x19, 0x03, 0x12, 0x40, 0x20, 0x10, 0xf0, 0xd3, 0xfe, 0xff,
        0x7f, 0x1c, 0x4d, 0x32, 0x4d, 0x74, 0x2d, 0x3f, 0x51, 0x6a, 0x27, 0x0c, 0x91, 0x9a, 0xb5, 0x6a, 0xad, 0x5a,
        0xf3, 0xf4, 0x54, 0x3e, 0x2a, 0x5d, 0x0e, 0x9f, 0x1e, 0x9f, 0x5a, 0x5a, 0xff, 0xb0, 0x5a, 0xb5, 0x76, 0x9c,
        0x75, 0xcc, 0x26, 0x0a, 0xa6, 0xb4, 0x98, 0xaa, 0xc9, 0xb4, 0x02, 0x70, 0x7f, 0x3b, 0x66, 0xe6, 0xa6};
    const std::string dict(reinterpret_cast<const char *>(kDict), sizeof(kDict));

    auto pairs = Pairs(ParseOk("libimebin", dict));
    std::sort(pairs.begin(), pairs.end());
    REQUIRE_EQ(pairs, (std::vector<std::string>{"世界|shi'jie", "你好|ni'hao", "略微|lve'wei"}));
    // 每条的 float 值原样带出（这份样本都是 1.0）。
    for (const auto &entry : ParseOk("libimebin", dict))
        REQUIRE_EQ(entry.frequency, std::optional<double>(1.0));

    // 截断、改坏的文件要整体报错，不能崩，也不能返回半截。
    for (const size_t keep : {size_t{4}, size_t{8}, size_t{40}, dict.size() - 1})
    {
        std::vector<Entry> entries;
        std::string error;
        REQUIRE(!ForeignDictionary::Parse("libimebin", dict.substr(0, keep), entries, error));
        REQUIRE(!error.empty());
        REQUIRE(entries.empty());
    }
    std::string wrongMagic = dict;
    wrongMagic[1] = 0x00;
    std::vector<Entry> entries;
    std::string error;
    REQUIRE(!ForeignDictionary::Parse("libimebin", wrongMagic, entries, error));
}

TEST_CASE(ForeignDictionaryRejectsFilesOfAnotherFormat)
{
    // 把文本文件当成二进制词库选进来：报格式不对，不崩。
    std::vector<Entry> entries;
    std::string error;
    REQUIRE(!ForeignDictionary::Parse("libimebin", "你好\tni hao\t1\n", entries, error));
    REQUIRE(!error.empty());
    // 文件头对、内容是乱码（未压缩的第 1 版）：trie 越界要报错，不能越界读。
    std::string garbage("\x00\x0f\xc6\x13\x00\x00\x00\x01", 8);
    garbage += std::string(0x400, '\x7F');
    REQUIRE(!ForeignDictionary::Parse("libimebin", garbage, entries, error));
    REQUIRE(entries.empty());
}

// 用真实词库核对条数。默认不跑：设 MSIME_FOREIGN_DICTIONARY_SAMPLES 指向样本目录才跑，
// 目录里放经 imewlconverter 转出的 tangshi.<格式>.<扩展名>（源是它测试资源里的
// 唐诗300首【官方推荐】.scel，3563 条）。
TEST_CASE(ForeignDictionaryMatchesImewlconverterOnRealSamples)
{
    const wchar_t *root = _wgetenv(L"MSIME_FOREIGN_DICTIONARY_SAMPLES");
    if (!root || !*root)
        return;
    const std::filesystem::path dir(root);
    const auto read = [&dir](const std::wstring &name) {
        std::ifstream stream(dir / name, std::ios::binary);
        REQUIRE(static_cast<bool>(stream));
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };
    struct Sample
    {
        const char *format;
        const wchar_t *file;
        size_t count;
    };
    const Sample samples[] = {
        {"rime", L"tangshi.rime.txt", 3563},
        {"libimetxt", L"tangshi.libimetxt.txt", 3563},
        {"libimebin", L"tangshi.libimebin.dict", 3563},
    };
    for (const auto &sample : samples)
    {
        const auto entries = ParseOk(sample.format, read(sample.file));
        REQUIRE_EQ(entries.size(), sample.count);
        size_t withPinyin = 0;
        for (const auto &entry : entries)
        {
            REQUIRE(!entry.word.empty());
            withPinyin += entry.pinyin.empty() ? 0 : 1;
        }
        REQUIRE(withPinyin * 10 >= entries.size() * 9);
    }
}

namespace
{
class ScopedForeignDataDir
{
  public:
    ScopedForeignDataDir()
        : path_(test::TempRoot() / (L"msime-foreign-import-" + std::to_wstring(GetCurrentProcessId())))
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
        wchar_t buffer[32768];
        const DWORD length = GetEnvironmentVariableW(kName, buffer, 32768);
        hadPrevious_ = length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        previous_.assign(buffer, length);
        // 与 test_dictionary_validation.cpp 的 ScopedDataDir 同理：Win32 环境块和 CRT 副本都要改，
        // 否则引擎侧的用户日志会写进真实数据目录。
        SetEnvironmentVariableW(kName, path_.wstring().c_str());
        _wputenv_s(kName, path_.wstring().c_str());
    }
    ~ScopedForeignDataDir()
    {
        user_dictionary::close_default_user_database();
        SetEnvironmentVariableW(kName, hadPrevious_ ? previous_.c_str() : nullptr);
        _wputenv_s(kName, hadPrevious_ ? previous_.c_str() : L"");
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    ScopedForeignDataDir(const ScopedForeignDataDir &) = delete;
    ScopedForeignDataDir &operator=(const ScopedForeignDataDir &) = delete;

    const std::filesystem::path &path() const
    {
        return path_;
    }

  private:
    static constexpr const wchar_t *kName = L"METASEQUOIA_IME_DATA_DIR";
    std::filesystem::path path_;
    std::wstring previous_;
    bool hadPrevious_ = false;
};
// 测试数据目录里建好这些读音所在的全拼分表。
void CreatePinyinTables(const ScopedForeignDataDir &data, std::initializer_list<const char *> keys)
{
    sqlite3 *raw = nullptr;
    REQUIRE_EQ(sqlite3_open(test::Utf8(data.path() / L"msime.db").c_str(), &raw), SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    for (const char *key : keys)
    {
        const std::string sql = "CREATE TABLE IF NOT EXISTS \"" +
                                quanpin::build_table_name(quanpin::split_segments(key)) +
                                "\"(key TEXT NOT NULL,jp TEXT NOT NULL,value TEXT NOT NULL,"
                                "weight INTEGER NOT NULL,UNIQUE(key,value))";
        REQUIRE_EQ(sqlite3_exec(db.get(), sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    }
}

// 按编码查拼音词库，取某个词的权重；查不到返回 -1。
int64_t PinyinWeight(const char *code, const std::string &word)
{
    const auto query =
        SettingsDictionary::HandleRequest({{"dictionary", "quanpin"}, {"action", "query"}, {"code", code}});
    REQUIRE(query.at("ok").as_bool());
    for (const auto &row : query.at("rows").as_array())
    {
        const auto &object = row.as_object();
        if (object.at("word").as_string() == word)
            return object.at("weight").as_int64();
    }
    return -1;
}
} // namespace

TEST_CASE(ImportFrequenciesMapByRankIntoTheImportRange)
{
    using SettingsDictionary::Validation::MapImportFrequencies;
    const std::optional<double> none;
    // 不同的词频值排名次：最小 10、最大 1000、中间均分；相同词频同一权重；没给词频记 10。
    REQUIRE_EQ(MapImportFrequencies({5.0, none, 1.0, 5.0, 100.0}), (std::vector<int>{505, 10, 10, 505, 1000}));
    // 只看名次不看数值：libime 的负对数概率、悬殊的次数都一样排。
    REQUIRE_EQ(MapImportFrequencies({-5.5, -1.0, -3.0}), (std::vector<int>{10, 1000, 505}));
    REQUIRE_EQ(MapImportFrequencies({1.0, 2.0, 3.0, 4.0, 1e9}), (std::vector<int>{10, 258, 505, 753, 1000}));
    // 只有一种词频值，分不出高低，等于没有词频。
    REQUIRE_EQ(MapImportFrequencies({7.0, 7.0, none}), (std::vector<int>{10, 10, 10}));
    REQUIRE_EQ(MapImportFrequencies({std::nan(""), 3.0, 9.0}), (std::vector<int>{10, 10, 1000}));
    REQUIRE(MapImportFrequencies({}).empty());
}

TEST_CASE(ForeignDictionaryReportsTheFrequencyEachFormatCarries)
{
    const auto frequencies = [](const char *format, const std::string &bytes) {
        std::vector<std::optional<double>> out;
        for (const auto &entry : ParseOk(format, bytes))
            out.push_back(entry.frequency);
        return out;
    };
    const std::optional<double> none;
    REQUIRE_EQ(frequencies("rime", "你好\tni hao\t100\n世界\t200\n略\tlve\n单\tdan\t50%\n"),
               (std::vector<std::optional<double>>{100.0, 200.0, none, 50.0}));
    REQUIRE_EQ(frequencies("libimetxt", "你好 ni'hao -4.25\n好 hao\n"),
               (std::vector<std::optional<double>>{-4.25, none}));
}

TEST_CASE(ForeignDictionaryImportWritesThroughThePinyinImportPath)
{
    ScopedForeignDataDir data;
    CreatePinyinTables(data, {"ni'hao", "shi'jie", "lve'wei"});

    // 文件名带中文：路径经 UTF-8 传过来，宿主要按宽字符打开。
    const std::filesystem::path file = data.path() / L"其他词库.txt";
    {
        std::ofstream stream(file, std::ios::binary);
        stream << "你好\tni hao\t100\n世界\tshi jie\t50\n你好\tni hao\t100\n略微\tlue wei\n１２\tzz\t1\n";
    }
    const auto request = [&file](const char *dictionary, const char *format) {
        return SettingsDictionary::HandleRequest(
            {{"dictionary", dictionary}, {"action", "importForeign"}, {"format", format}, {"path", test::Utf8(file)}});
    };

    const auto imported = request("quanpin", "rime");
    REQUIRE(imported.at("ok").as_bool());
    const std::string message(imported.at("message").as_string());
    REQUIRE(message.rfind("成功导入 3 条，跳过 1 条（已存在），失败 1 条。第 5 条：１２：", 0) == 0);

    // 词频 {1, 50, 100} 排名次换算；没写词频的记 10。lue 按规范写法 lve 入库，查得到。
    REQUIRE_EQ(PinyinWeight("nihao", "你好"), int64_t{1000});
    REQUIRE_EQ(PinyinWeight("shijie", "世界"), int64_t{505});
    REQUIRE_EQ(PinyinWeight("lvewei", "略微"), int64_t{10});

    // 只进拼音词库；未知格式、文件不存在都给出明确原因。
    REQUIRE(!request("wubi", "rime").at("ok").as_bool());
    REQUIRE_EQ(std::string(request("quanpin", "zip").at("message").as_string()), std::string("不支持的词库格式"));
    REQUIRE_EQ(std::string(request("quanpin", "scel").at("message").as_string()), std::string("不支持的词库格式"));
    const auto missing = SettingsDictionary::HandleRequest({{"dictionary", "quanpin"},
                                                            {"action", "importForeign"},
                                                            {"format", "libimebin"},
                                                            {"path", test::Utf8(data.path() / L"不存在.dict")}});
    REQUIRE_EQ(std::string(missing.at("message").as_string()), std::string("读取文件失败"));
}

TEST_CASE(ForeignDictionaryImportCreatesManageableCellDictionary)
{
    ScopedForeignDataDir data;
    CreatePinyinTables(data, {"ni'hao", "shi'jie"});
    const std::filesystem::path file = data.path() / L"文学词库.txt";
    {
        std::ofstream stream(file, std::ios::binary);
        stream << "你好\tni hao\t100\n世界\tshi jie\t50\n";
    }
    const auto imported = SettingsDictionary::HandleRequest({{"dictionary", "quanpin"},
                                                             {"action", "importForeign"},
                                                             {"format", "rime"},
                                                             {"path", test::Utf8(file)},
                                                             {"name", "文学"}});
    REQUIRE(imported.at("ok").as_bool());
    const std::string source_id(imported.at("sourceId").as_string());
    REQUIRE(!source_id.empty());
    REQUIRE_EQ(std::string(imported.at("sourceName").as_string()), std::string("文学"));
    REQUIRE_EQ(PinyinWeight("nihao", "你好"), int64_t{1000});

    auto sources = SettingsDictionary::HandleRequest({{"dictionary", "cells"}, {"action", "listSources"}});
    REQUIRE(sources.at("ok").as_bool());
    REQUIRE_EQ(sources.at("sources").as_array().size(), size_t{1});
    REQUIRE_EQ(std::string(sources.at("sources").as_array()[0].as_object().at("id").as_string()), source_id);
    REQUIRE_EQ(sources.at("sources").as_array()[0].as_object().at("entryCount").as_int64(), int64_t{2});

    const auto personal = SettingsDictionary::HandleRequest(
        {{"dictionary", "quanpin"}, {"action", "query"}, {"scope", "user"}, {"offset", 0}, {"limit", 100}});
    REQUIRE(personal.at("rows").as_array().empty());
    const auto entries = SettingsDictionary::HandleRequest(
        {{"dictionary", "cells"}, {"action", "query"}, {"sourceId", source_id}, {"offset", 0}, {"limit", 1}});
    REQUIRE_EQ(entries.at("rows").as_array().size(), size_t{1});
    REQUIRE(entries.at("hasMore").as_bool());

    const auto disabled = SettingsDictionary::HandleRequest(
        {{"dictionary", "cells"}, {"action", "setSourceEnabled"}, {"sourceId", source_id}, {"enabled", false}});
    REQUIRE(disabled.at("ok").as_bool());
    REQUIRE_EQ(PinyinWeight("nihao", "你好"), int64_t{-1});
    REQUIRE_EQ(PinyinWeight("shijie", "世界"), int64_t{-1});
    const auto personal_insert = SettingsDictionary::HandleRequest(
        {{"dictionary", "quanpin"}, {"action", "create"}, {"code", "ni hao"}, {"word", "你好"}, {"weight", 77}});
    REQUIRE(personal_insert.at("ok").as_bool());
    const auto enabled = SettingsDictionary::HandleRequest(
        {{"dictionary", "cells"}, {"action", "setSourceEnabled"}, {"sourceId", source_id}, {"enabled", true}});
    REQUIRE(enabled.at("ok").as_bool());
    // 停用期间手工加的同键词已转为个人词条，重新启用来源不会接管，删除来源也不会误删。
    REQUIRE_EQ(PinyinWeight("nihao", "你好"), int64_t{77});
    REQUIRE_EQ(PinyinWeight("shijie", "世界"), int64_t{505});

    const auto removed = SettingsDictionary::HandleRequest(
        {{"dictionary", "cells"}, {"action", "deleteSource"}, {"sourceId", source_id}});
    REQUIRE(removed.at("ok").as_bool());
    REQUIRE_EQ(PinyinWeight("nihao", "你好"), int64_t{77});
    REQUIRE_EQ(PinyinWeight("shijie", "世界"), int64_t{-1});
    sources = SettingsDictionary::HandleRequest({{"dictionary", "cells"}, {"action", "listSources"}});
    REQUIRE(sources.at("sources").as_array().empty());
}

TEST_CASE(PinyinBatchImportMapsWeightsUnlessRestoringABackup)
{
    ScopedForeignDataDir data;
    CreatePinyinTables(data, {"ni'hao", "shi'jie", "lve'wei", "shi'ji"});

    // 设置页的批量导入：权重列按文件内名次换算，没写权重的行记 10。
    const auto imported =
        SettingsDictionary::HandleRequest({{"dictionary", "quanpin"},
                                           {"action", "import"},
                                           {"content", "你好\tni hao\t300\n世界\tshi jie\t20\n略微\tlve wei\n"}});
    REQUIRE(imported.at("ok").as_bool());
    REQUIRE_EQ(PinyinWeight("nihao", "你好"), int64_t{1000});
    REQUIRE_EQ(PinyinWeight("shijie", "世界"), int64_t{10});
    REQUIRE_EQ(PinyinWeight("lvewei", "略微"), int64_t{10});

    // 备份恢复（keepWeights）写回的是自己导出的真实权重，原样保留。设置窗口发不出这个字段：
    // 协议的 dictionaryRequest 不收它。
    const auto restored = SettingsDictionary::HandleRequest(
        {{"dictionary", "quanpin"}, {"action", "import"}, {"keepWeights", true}, {"content", "世纪\tshi ji\t4321\n"}});
    REQUIRE(restored.at("ok").as_bool());
    REQUIRE_EQ(PinyinWeight("shiji", "世纪"), int64_t{4321});
}
