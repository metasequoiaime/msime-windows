#include "settings/dictionary_manager.h"
#include "settings/foreign_dictionary.h"
#include "engine/quanpin/quanpin_query.h"
#include "engine/user_dictionary/user_dictionary_journal.h"
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include <windows.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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

std::string Le32(uint32_t value)
{
    return Le16(static_cast<uint16_t>(value & 0xFFFF)) + Le16(static_cast<uint16_t>(value >> 16));
}

std::string Utf16(const std::wstring &text)
{
    std::string out;
    for (const wchar_t ch : text)
        out += Le16(static_cast<uint16_t>(ch));
    return out;
}

void Put(std::string &buffer, size_t offset, const std::string &bytes)
{
    if (buffer.size() < offset + bytes.size())
        buffer.resize(offset + bytes.size(), '\0');
    buffer.replace(offset, bytes.size(), bytes);
}
} // namespace

TEST_CASE(ForeignDictionaryFormatsMatchTheContract)
{
    // 下拉框选项与协议的 format 枚举一一对应，顺序就是界面里的顺序。
    std::vector<std::string> ids;
    for (const auto &format : ForeignDictionary::Formats())
        ids.push_back(format.id);
    REQUIRE_EQ(ids, (std::vector<std::string>{"rime", "scel", "sgpy", "sgpybin", "qqpy", "qcel", "bdpy", "bdict",
                                              "libimetxt", "libimebin", "plist"}));
    REQUIRE(ForeignDictionary::FindFormat("scel") != nullptr);
    REQUIRE(ForeignDictionary::FindFormat("zip") == nullptr);

    std::vector<Entry> entries;
    std::string error;
    REQUIRE(!ForeignDictionary::Parse("zip", "abc", entries, error));
    REQUIRE(!error.empty());
}

TEST_CASE(ForeignDictionaryReadsTextFormats)
{
    // Rime：跳过 YAML 头和注释；只有权重没有读音的行留给自动注音。
    REQUIRE_EQ(Pairs(ParseOk("rime", "---\nname: test\ncolumns:\n  - text\n...\n# 注释\n"
                                     "你好\tni hao\t100\r\n世界\t200\n略\tlve\n")),
               (std::vector<std::string>{"你好|ni'hao", "世界|", "略|lve"}));

    // 搜狗文本：GBK、行首单引号。
    REQUIRE_EQ(Pairs(ParseOk("sgpy", "'ni'hao \xC4\xE3\xBA\xC3\r\nnot a word line\r\n")),
               (std::vector<std::string>{"你好|ni'hao"}));

    // QQ 文本：UTF-16LE 带 BOM，逗号后面是别的字段。
    REQUIRE_EQ(Pairs(ParseOk("qqpy", "\xFF\xFE" + Utf16(L"ni'hao 你好 1, 2\r\nhao 好 3\r\n"))),
               (std::vector<std::string>{"你好|ni'hao", "好|hao"}));

    // 百度文本：UTF-16LE 没有 BOM；两列的英文词条不进拼音词库。
    REQUIRE_EQ(Pairs(ParseOk("bdpy", Utf16(L"你好\tni'hao\t3\r\nhello\t5\r\n"))),
               (std::vector<std::string>{"你好|ni'hao"}));

    // LibIME 文本：引号包裹的词面按 fcitx 规则还原；只认两列或三列。
    REQUIRE_EQ(Pairs(ParseOk("libimetxt", "你好 ni'hao 0\n\"你\\\"好\" ni'hao\nbad a b c\n")),
               (std::vector<std::string>{"你好|ni'hao", "你\"好|ni'hao"}));

    // Mac plist：按 phrase / shortcut 取值，实体要还原。
    const std::string plist = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><array>\n"
                              "<dict><key>phrase</key><string>你好</string><key>shortcut</key><string>ni hao</string>"
                              "</dict>\n<dict><key>shortcut</key><string>nh</string><key>phrase</key>"
                              "<string>&#x4F60;&amp;好</string></dict>\n</array></plist>\n";
    REQUIRE_EQ(Pairs(ParseOk("plist", plist)), (std::vector<std::string>{"你好|ni'hao", "你&好|nh"}));
}

TEST_CASE(ForeignDictionaryReadsCellDictionaries)
{
    // 搜狗细胞词库：0x120 处是同音组数，0x1540 起是拼音表，随后是词条区。
    std::string scel(0x1540, '\0');
    Put(scel, 0x120, Le32(2));
    scel += Le32(2);
    scel += Le16(0) + Le16(4) + Utf16(L"ni");
    scel += Le16(1) + Le16(6) + Utf16(L"hao");
    scel += Le16(2) + Le16(4) + Le16(0) + Le16(1); // 两个同音词，拼音 ni'hao
    scel += Le16(4) + Utf16(L"你好") + Le16(10) + std::string(10, '\0');
    scel += Le16(4) + Utf16(L"拟好") + Le16(10) + std::string(10, '\0');
    scel += Le16(1) + Le16(2) + Le16(2); // 超出拼音表的序号是字母 a
    scel += Le16(2) + Utf16(L"A") + Le16(10) + std::string(10, '\0');
    REQUIRE_EQ(Pairs(ParseOk("scel", scel)), (std::vector<std::string>{"你好|ni'hao", "拟好|ni'hao", "A|a"}));

    // 截断的词条区保留已经读出的部分。
    REQUIRE_EQ(Pairs(ParseOk("scel", scel.substr(0, scel.size() - 8))),
               (std::vector<std::string>{"你好|ni'hao", "拟好|ni'hao"}));

    // 百度分类词库：旧版头部（0x60 为 0）用 0x44 的长度推算词条区结尾。
    std::string word = Le32(2) + std::string("\x0B\x1D\x05\x0D", 4) + Utf16(L"你好");
    std::string bdict(0x350, '\0');
    Put(bdict, 0x44, Le32(static_cast<uint32_t>(word.size())));
    bdict += word;
    REQUIRE_EQ(Pairs(ParseOk("bdict", bdict)), (std::vector<std::string>{"你好|ni'hao"}));

    // 搜狗备份词库新版（"SGPU"）：索引区给出每个词的偏移。
    std::string bin(0x120, '\0');
    Put(bin, 0, "SGPU");
    Put(bin, 0x38, Le32(0x100) + Le32(4) + Le32(1) + Le32(0x120));
    Put(bin, 0x100, Le32(0));
    bin += Le16(5) + std::string(2 + 5, '\0') + Le16(4) + Le16(0) + Le16(1) + std::string(2, '\0') + Le16(4) +
           Utf16(L"啊哎");
    REQUIRE_EQ(Pairs(ParseOk("sgpybin", bin)), (std::vector<std::string>{"啊哎|a'ai"}));
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
    // 把文本文件当成二进制格式选进来：报格式不对，不崩。
    for (const char *format : {"scel", "qcel", "bdict", "sgpybin", "libimebin"})
    {
        std::vector<Entry> entries;
        std::string error;
        REQUIRE(!ForeignDictionary::Parse(format, "你好\tni hao\t1\n", entries, error));
        REQUIRE(!error.empty());
    }
    // 随机字节当作搜狗备份词库：索引越界要报错，不能越界读。
    std::string garbage(0x400, '\x7F');
    std::vector<Entry> entries;
    std::string error;
    REQUIRE(!ForeignDictionary::Parse("sgpybin", garbage, entries, error));
}

// 用真实词库核对条数。默认不跑：设 MSIME_FOREIGN_DICTIONARY_SAMPLES 指向样本目录才跑，
// 目录里放 imewlconverter 测试资源里的 唐诗300首【官方推荐】.scel、星际战甲.qcel、travel.bdict、
// 搜狗备份.bin，以及由前者经 imewlconverter 转出的 tangshi.<格式>.<扩展名>。
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
        {"scel", L"唐诗300首【官方推荐】.scel", 3563},
        {"qcel", L"星际战甲.qcel", 4675},
        {"bdict", L"travel.bdict", 300},
        {"sgpybin", L"搜狗备份.bin", 30},
        {"rime", L"tangshi.rime.txt", 3563},
        {"sgpy", L"tangshi.sgpy.txt", 3563},
        {"qqpy", L"tangshi.qqpy.txt", 3563},
        {"bdpy", L"tangshi.bdpy.txt", 3563},
        {"libimetxt", L"tangshi.libimetxt.txt", 3563},
        {"libimebin", L"tangshi.libimebin.dict", 3563},
        {"plist", L"tangshi.plist.plist", 3563},
        {"scel", L"tangshi.scel.scel", 3563},
    };
    for (const auto &sample : samples)
    {
        const auto entries = ParseOk(sample.format, read(sample.file));
        // bdict 的样本条数是下限（与 imewlconverter 测试一致），其余要精确相等。
        if (std::string(sample.format) == "bdict")
            REQUIRE(entries.size() >= sample.count);
        else
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
} // namespace

TEST_CASE(ForeignDictionaryImportWritesThroughThePinyinImportPath)
{
    ScopedForeignDataDir data;
    {
        sqlite3 *raw = nullptr;
        REQUIRE_EQ(sqlite3_open(test::Utf8(data.path() / L"msime.db").c_str(), &raw), SQLITE_OK);
        const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
        for (const char *key : {"ni'hao", "shi'jie", "lve'wei"})
        {
            const std::string sql = "CREATE TABLE IF NOT EXISTS \"" +
                                    quanpin::build_table_name(quanpin::split_segments(key)) +
                                    "\"(key TEXT NOT NULL,jp TEXT NOT NULL,value TEXT NOT NULL,"
                                    "weight INTEGER NOT NULL,UNIQUE(key,value))";
            REQUIRE_EQ(sqlite3_exec(db.get(), sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
        }
    }

    // 文件名带中文：路径经 UTF-8 传过来，宿主要按宽字符打开。
    const std::filesystem::path file = data.path() / L"其他词库.txt";
    {
        std::ofstream stream(file, std::ios::binary);
        stream << "你好\tni hao\t1\n世界\tshi jie\t1\n你好\tni hao\t1\n略微\tlue wei\t1\n１２\tzz\t1\n";
    }
    const auto request = [&file](const char *dictionary, const char *format) {
        return SettingsDictionary::HandleRequest(
            {{"dictionary", dictionary}, {"action", "importForeign"}, {"format", format}, {"path", test::Utf8(file)}});
    };

    const auto imported = request("quanpin", "rime");
    REQUIRE(imported.at("ok").as_bool());
    const std::string message(imported.at("message").as_string());
    REQUIRE(message.rfind("成功导入 3 条，跳过 1 条（已存在），失败 1 条。第 5 条：１２：", 0) == 0);

    // lue 按查询侧的规范写法 lve 入库，查得到。
    for (const char *code : {"nihao", "shijie", "lvewei"})
    {
        const auto query =
            SettingsDictionary::HandleRequest({{"dictionary", "quanpin"}, {"action", "query"}, {"code", code}});
        REQUIRE(query.at("ok").as_bool());
        REQUIRE_EQ(query.at("rows").as_array().size(), size_t{1});
    }

    // 只进拼音词库；未知格式、文件不存在都给出明确原因。
    REQUIRE(!request("wubi", "rime").at("ok").as_bool());
    REQUIRE_EQ(std::string(request("quanpin", "zip").at("message").as_string()), std::string("不支持的词库格式"));
    const auto missing = SettingsDictionary::HandleRequest({{"dictionary", "quanpin"},
                                                            {"action", "importForeign"},
                                                            {"format", "scel"},
                                                            {"path", test::Utf8(data.path() / L"不存在.scel")}});
    REQUIRE_EQ(std::string(missing.at("message").as_string()), std::string("读取文件失败"));
}
