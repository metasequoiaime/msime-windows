#include "settings/dictionary_manager.h"
#include "settings/dictionary_validation.h"
#include "engine/user_dictionary/user_dictionary_journal.h"
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include <windows.h>
#include <sqlite3.h>
#include <stdlib.h>

#include <filesystem>
#include <memory>
#include <string>

TEST_CASE(DictionaryFullPinyinValidationRejectsAbbreviatedSyllables)
{
    quanpin::Segments segments;
    std::string normalized;

    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin("nihao", segments, normalized));
    REQUIRE_EQ(normalized, std::string("ni'hao"));
    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin("ni hao", segments, normalized));
    REQUIRE_EQ(normalized, std::string("ni'hao"));
    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin("xi'an", segments, normalized));
    REQUIRE_EQ(normalized, std::string("xi'an"));
    // ü 别名按词库的写法入库，否则查询归一后永远查不到这一行。
    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin("xvan'lue", segments, normalized));
    REQUIRE_EQ(normalized, std::string("xuan'lve"));
    REQUIRE(!SettingsDictionary::Validation::NormalizeFullPinyin("nh", segments, normalized));
    REQUIRE(!SettingsDictionary::Validation::NormalizeFullPinyin("ni'h", segments, normalized));
    REQUIRE(!SettingsDictionary::Validation::NormalizeFullPinyin("ni'", segments, normalized));
}

TEST_CASE(DictionaryFullPinyinPicksSegmentationMatchingSyllableCount)
{
    quanpin::Segments segments;
    std::string normalized;

    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin("xian", segments, normalized));
    REQUIRE_EQ(normalized, std::string("xian"));
    REQUIRE_EQ(segments.size(), static_cast<size_t>(1));

    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin("xian", segments, normalized, 2));
    REQUIRE_EQ(normalized, std::string("xi'an"));
    REQUIRE_EQ(segments.size(), static_cast<size_t>(2));

    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin("a'a'a'a'a'a'a'a", segments, normalized, 8));
    REQUIRE_EQ(normalized, std::string("a'a'a'a'a'a'a'a"));
    REQUIRE_EQ(segments.size(), static_cast<size_t>(8));

    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin("abalatiyayunhai", segments, normalized, 7));
    REQUIRE_EQ(normalized, std::string("a'ba'la'ti'ya'yun'hai"));
    REQUIRE_EQ(segments.size(), static_cast<size_t>(7));
}

TEST_CASE(QuickPhraseValidationMatchesNamedPipeWcharCapacity)
{
    REQUIRE(SettingsDictionary::Validation::QuickPhraseFitsNamedPipe(std::string(199, 'a')));
    REQUIRE(!SettingsDictionary::Validation::QuickPhraseFitsNamedPipe(std::string(200, 'a')));

    const std::string emoji = "\xF0\x9F\x98\x80";
    REQUIRE(SettingsDictionary::Validation::QuickPhraseFitsNamedPipe(std::string(197, 'a') + emoji));
    REQUIRE(!SettingsDictionary::Validation::QuickPhraseFitsNamedPipe(std::string(198, 'a') + emoji));
}

TEST_CASE(CodedDictionaryImportRequiresTabsAndPreservesSpacesInWords)
{
    std::string word;
    std::string code;
    std::string message;
    int weight = -1;

    REQUIRE(SettingsDictionary::Validation::ParseCodedImportLine("包含 空格的词\tbao'han'kong'ge'de'ci\t123", word,
                                                                 code, weight, message));
    REQUIRE_EQ(word, std::string("包含 空格的词"));
    REQUIRE_EQ(code, std::string("bao'han'kong'ge'de'ci"));
    REQUIRE_EQ(weight, 123);

    REQUIRE(!SettingsDictionary::Validation::ParseCodedImportLine("普通词 putongci 10", word, code, weight, message));
    REQUIRE(!SettingsDictionary::Validation::ParseCodedImportLine("普通词\tputongci\t10\textra", word, code, weight,
                                                                  message));
}

TEST_CASE(CodedDictionaryImportAcceptsOptionalWeightAndRimeUserdb)
{
    std::string word;
    std::string code;
    std::string message;
    int weight = -1;

    REQUIRE(SettingsDictionary::Validation::ParseCodedImportLine("普通词\tputongci", word, code, weight, message));
    REQUIRE_EQ(word, std::string("普通词"));
    REQUIRE_EQ(code, std::string("putongci"));
    REQUIRE_EQ(weight, SettingsDictionary::Validation::kDefaultPinyinImportWeight);

    REQUIRE(SettingsDictionary::Validation::ParseCodedImportLine("你好\tni hao\tc=3 d=0.12 t=12345", word, code, weight,
                                                                 message));
    REQUIRE_EQ(word, std::string("你好"));
    REQUIRE_EQ(code, std::string("ni hao"));
    REQUIRE_EQ(weight, SettingsDictionary::Validation::kDefaultPinyinImportWeight);

    REQUIRE(SettingsDictionary::Validation::ParseCodedImportLine("西安\txi an\tc=1", word, code, weight, message));
    REQUIRE_EQ(word, std::string("西安"));
    REQUIRE_EQ(code, std::string("xi an"));
    REQUIRE_EQ(weight, SettingsDictionary::Validation::kDefaultPinyinImportWeight);

    REQUIRE(
        !SettingsDictionary::Validation::ParseCodedImportLine("普通词\tputongci\tabc", word, code, weight, message));
}

TEST_CASE(ImportLineSkipWalksYamlFrontMatterAndComments)
{
    bool in_yaml_header = false;
    REQUIRE(SettingsDictionary::Validation::ShouldSkipImportLine("", in_yaml_header));
    REQUIRE(!in_yaml_header);
    REQUIRE(SettingsDictionary::Validation::ShouldSkipImportLine("   ", in_yaml_header));
    REQUIRE(SettingsDictionary::Validation::ShouldSkipImportLine("# Rime user dictionary", in_yaml_header));
    REQUIRE(SettingsDictionary::Validation::ShouldSkipImportLine("#@/db_name\tluna_pinyin", in_yaml_header));
    REQUIRE(!SettingsDictionary::Validation::ShouldSkipImportLine("你好\tni hao\t1", in_yaml_header));

    REQUIRE(SettingsDictionary::Validation::ShouldSkipImportLine("---", in_yaml_header));
    REQUIRE(in_yaml_header);
    REQUIRE(SettingsDictionary::Validation::ShouldSkipImportLine("name: luna_pinyin", in_yaml_header));
    REQUIRE(SettingsDictionary::Validation::ShouldSkipImportLine("sort: by_weight", in_yaml_header));
    REQUIRE(SettingsDictionary::Validation::ShouldSkipImportLine("  ---", in_yaml_header));
    REQUIRE(in_yaml_header);
    REQUIRE(SettingsDictionary::Validation::ShouldSkipImportLine("...", in_yaml_header));
    REQUIRE(!in_yaml_header);
    REQUIRE(!SettingsDictionary::Validation::ShouldSkipImportLine("你好\tni hao", in_yaml_header));
    REQUIRE(SettingsDictionary::Validation::ShouldSkipImportLine("  # comment after body", in_yaml_header));
}

TEST_CASE(CodedQuanpinImportAcceptsExportedApostropheTsv)
{
    std::string word;
    std::string code;
    std::string message;
    int weight = -1;
    quanpin::Segments segments;
    std::string normalized;

    REQUIRE(SettingsDictionary::Validation::ParseCodedImportLine("啊啊啊啊啊啊啊啊\ta'a'a'a'a'a'a'a\t41", word, code,
                                                                 weight, message));
    REQUIRE_EQ(word, std::string("啊啊啊啊啊啊啊啊"));
    REQUIRE_EQ(code, std::string("a'a'a'a'a'a'a'a"));
    REQUIRE_EQ(weight, 41);
    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin(code, segments, normalized, 8));
    REQUIRE_EQ(segments.size(), static_cast<size_t>(8));

    REQUIRE(SettingsDictionary::Validation::ParseCodedImportLine("阿巴拉提亚云海\ta'ba'la'ti'ya'yun'hai\t13", word,
                                                                 code, weight, message));
    REQUIRE_EQ(word, std::string("阿巴拉提亚云海"));
    REQUIRE_EQ(code, std::string("a'ba'la'ti'ya'yun'hai"));
    REQUIRE_EQ(weight, 13);
    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin(code, segments, normalized, 7));
    REQUIRE_EQ(normalized, std::string("a'ba'la'ti'ya'yun'hai"));
    REQUIRE_EQ(segments.size(), static_cast<size_t>(7));
}

namespace
{
// 单字码取自出厂 wubi86 词库（wubi86 WHERE length(value)=1，取 4 级全码，无 4 级时取最长码）。
// 环境/计算机/诸葛亮/中华人民共和国 在词库里有词条，下面写的期望值就是它们真实的 key；
// 张三/五笔输入法 词库里没有，期望值由规则推出。一并用它们当回归锚点。
SettingsDictionary::Validation::WubiCharCodes RealWubiCharCodes()
{
    using WubiCharCodes = SettingsDictionary::Validation::WubiCharCodes;
    return WubiCharCodes{
        {"环", "ggiy"}, {"境", "fujq"}, // 环境 -> ggfu
        {"五", "gghg"}, {"笔", "ttfn"}, {"输", "lwgj"}, {"入", "tyi"},
        {"法", "ifcy"}, {"计", "yfh"},  {"算", "thaj"}, {"机", "smn"}, // 计算机 -> ytsm
        {"中", "khk"},  {"华", "wxfj"}, {"人", "wwww"}, {"民", "nav"},
        {"共", "awu"},  {"和", "tkg"},  {"国", "lgyi"}, // 中华人民共和国 -> kwwl
        {"张", "xtay"}, {"三", "dggg"},                 // 张三 -> xtdg
        {"诸", "yftj"}, {"葛", "ajqn"}, {"亮", "ypmb"}, // 诸葛亮 -> yayp
        {"节", "abj"},  {"式", "aa"},
    };
}

std::string Compose(const std::string &word)
{
    return SettingsDictionary::Validation::ComposeWubiPhraseCode(word, RealWubiCharCodes());
}

// 指向一个空目录，让每条分支都在写库之前就返回。
//
// 必须同时写两处环境变量，否则会静默污染用户真实词库：
//   - server 的 get_ime_data_path() 走 GetEnvironmentVariableW，读 Win32 环境块；
//   - 引擎的 data_file_path()（default_user_db_path 用它）走 _wdupenv_s，读 CRT 副本。
// 只调 SetEnvironmentVariableW 的话前者生效、后者看不到，引擎侧仍指向真实数据目录，
// record_user_insert 就把测试数据写进了 %LOCALAPPDATA% 的真实词库。
// ime_paths 每次调用都重读、不缓存，所以改动不会漏给别的用例。
class ScopedDataDir
{
  public:
    explicit ScopedDataDir(const std::filesystem::path &path)
    {
        wchar_t buffer[32768];
        const DWORD length = GetEnvironmentVariableW(kName, buffer, 32768);
        had_previous_ = length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        previous_.assign(buffer, length);

        const std::wstring value = path.wstring();
        SetEnvironmentVariableW(kName, value.c_str());
        _wputenv_s(kName, value.c_str());
    }
    ~ScopedDataDir()
    {
        SetEnvironmentVariableW(kName, had_previous_ ? previous_.c_str() : nullptr);
        _wputenv_s(kName, had_previous_ ? previous_.c_str() : L"");
    }

    ScopedDataDir(const ScopedDataDir &) = delete;
    ScopedDataDir &operator=(const ScopedDataDir &) = delete;

  private:
    static constexpr const wchar_t *kName = L"METASEQUOIA_IME_DATA_DIR";
    std::wstring previous_;
    bool had_previous_ = false;
};

// 临时数据目录，析构时整棵删掉。PID 后缀不是洁癖：目录开场会 remove_all，固定名在
// 并行 ctest 或多个工作树同时跑时会互相删掉——同样的理由见 test_caret_prefix_input_session.cpp:27。
class ScopedTempDir
{
  public:
    explicit ScopedTempDir(const wchar_t *name)
        : path_(std::filesystem::temp_directory_path() /
                (std::wstring(name) + L"-" + std::to_wstring(GetCurrentProcessId())))
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
    }
    ~ScopedTempDir()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    ScopedTempDir(const ScopedTempDir &) = delete;
    ScopedTempDir &operator=(const ScopedTempDir &) = delete;

    const std::filesystem::path &path() const
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

// 端到端导入用的数据目录：一个只含 wubi86 表的 msime.db，种子单字码与上面
// RealWubiCharCodes 一致。
class ScopedWubiDataDir
{
  public:
    ScopedWubiDataDir() : dir_(L"msime-hans-wubi-import-test")
    {
        sqlite3 *db = nullptr;
        REQUIRE_EQ(sqlite3_open(test::Utf8(dir_.path() / L"msime.db").c_str(), &db), SQLITE_OK);
        REQUIRE_EQ(sqlite3_exec(db,
                                "CREATE TABLE wubi86(key TEXT NOT NULL, value TEXT NOT NULL,"
                                "weight INTEGER NOT NULL DEFAULT 0, UNIQUE(key,value))",
                                nullptr, nullptr, nullptr),
                   SQLITE_OK);
        for (const auto &entry : RealWubiCharCodes())
        {
            sqlite3_stmt *stmt = nullptr;
            REQUIRE_EQ(
                sqlite3_prepare_v2(db, "INSERT INTO wubi86(key,value,weight) VALUES(?1,?2,10)", -1, &stmt, nullptr),
                SQLITE_OK);
            REQUIRE_EQ(sqlite3_bind_text(stmt, 1, entry.second.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
            REQUIRE_EQ(sqlite3_bind_text(stmt, 2, entry.first.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
            REQUIRE_EQ(sqlite3_step(stmt), SQLITE_DONE);
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }
    ScopedWubiDataDir(const ScopedWubiDataDir &) = delete;
    ScopedWubiDataDir &operator=(const ScopedWubiDataDir &) = delete;

    const std::filesystem::path &path() const
    {
        return dir_.path();
    }

  private:
    ScopedTempDir dir_;
};
} // namespace

TEST_CASE(WubiPhraseCodeTakesTwoLettersFromEachOfTwoCharWords)
{
    // 2 字词：首字前 2 位 + 次字前 2 位
    REQUIRE_EQ(Compose("环境"), std::string("ggfu"));
    REQUIRE_EQ(Compose("张三"), std::string("xtdg"));
}

TEST_CASE(WubiPhraseCodeTakesOneOneThenTwoForThreeCharWords)
{
    // 3 字词：首字 1 位 + 次字 1 位 + 末字前 2 位
    REQUIRE_EQ(Compose("计算机"), std::string("ytsm"));
    REQUIRE_EQ(Compose("诸葛亮"), std::string("yayp"));
}

TEST_CASE(WubiPhraseCodeTakesTheLastCharNotTheFourthForLongWords)
{
    // 4 字以上：首、次、三、末各 1 位。末字是「最后一个字」而非第 4 个字——
    // 中华人民共和国里 民/共/和 都不参与取码，用的是末尾的 国。
    REQUIRE_EQ(Compose("五笔输入法"), std::string("gtli"));
    REQUIRE_EQ(Compose("中华人民共和国"), std::string("kwwl"));
    // 实现若误取第 4 个字（民）会得到 kwwn，与 kwwl 不同，这条断言能区分。
}

TEST_CASE(WubiPhraseCodeKeepsSingleCharAtItsNaturalLength)
{
    // 单字保持自然码长且不补 z：wubi86 里「节」存的就是 abj，补成 abjz 会造出重复词条。
    REQUIRE_EQ(Compose("节"), std::string("abj"));
    REQUIRE_EQ(Compose("式"), std::string("aa"));
    REQUIRE_EQ(Compose("国"), std::string("lgyi"));
}

TEST_CASE(WubiPhraseCodeFailsWhenAnyCharacterHasNoCode)
{
    // 「观」在出厂 wubi86 里其实有码（cmqn），这里用它是因为本用例构造的码表里没放它——
    // 纯函数只认传进来的表，不查库。
    REQUIRE(!Compose("环").empty());
    REQUIRE(Compose("环观").empty());
    REQUIRE(Compose("观").empty());
    REQUIRE(Compose("").empty());
    REQUIRE(Compose("节观").empty()); // 首字有码也不放过
}

TEST_CASE(WubiPhraseCodeRejectsCharsWhoseCodeIsShorterThanTheRuleNeeds)
{
    // 取码规则要求每一位都取到指定字母数。某个字只有 1 位码时，硬拼会得到一个短一位的
    // key——它能查得到、能上屏，但永远凑不满 has_complete_code() 的 4 位，静默写进
    // wubi86 就是一条永远选不中的垃圾词条。所以这一步必须失败而不是凑合。
    SettingsDictionary::Validation::WubiCharCodes short_codes{{"甲", "g"}, {"乙", "aaaa"}};
    const auto compose = [&short_codes](const std::string &word) {
        return SettingsDictionary::Validation::ComposeWubiPhraseCode(word, short_codes);
    };

    REQUIRE(compose("甲乙").empty()); // 甲 只 1 位，取 2 位取不满
    REQUIRE(compose("甲甲").empty());
    REQUIRE_EQ(compose("乙乙"), std::string("aaaa")); // 两个都是 4 位码，取 2+2 正常
    // 4 字以上每位只取 1 位，所以 1 位码够用——这条守卫不该误伤长词。
    REQUIRE_EQ(compose("甲甲乙乙"), std::string("ggaa"));

    // 单字走 npos「全取」分支，不设长度下限：1 位码原样出码。
    REQUIRE_EQ(compose("甲"), std::string("g"));
}

TEST_CASE(WubiImportDefaultWeightSitsOnTheWubiScaleNotThePinyinScale)
{
    // 出厂词库实测：拼音 2 字词 p50=2805/max=9931703，五笔 2 字词 p50=10/max=110。
    // 同一个 10000 落在拼音 p50~p60 合理区间，搬到五笔就是全库 max 的 91 倍。
    REQUIRE_EQ(SettingsDictionary::Validation::kDefaultPinyinImportWeight, 10000);
    REQUIRE_EQ(SettingsDictionary::Validation::kDefaultWubiImportWeight, 30);

    std::string word;
    std::string code;
    std::string message;
    int weight = -1;
    // 不传 default_weight 时保持拼音缺省，拼音导入行为不变。
    REQUIRE(SettingsDictionary::Validation::ParseCodedImportLine("环境\tggfu", word, code, weight, message));
    REQUIRE_EQ(weight, 10000);
    // 五笔导入显式传自己的刻度。
    REQUIRE(SettingsDictionary::Validation::ParseCodedImportLine(
        "环境\tggfu", word, code, weight, message, SettingsDictionary::Validation::kDefaultWubiImportWeight));
    REQUIRE_EQ(weight, 30);
    // 文件里显式给了权重时不改写。
    REQUIRE(SettingsDictionary::Validation::ParseCodedImportLine(
        "环境\tggfu\t55", word, code, weight, message, SettingsDictionary::Validation::kDefaultWubiImportWeight));
    REQUIRE_EQ(weight, 55);
}

TEST_CASE(HansImportIsRoutedByTheTargetDictionary)
{
    const ScopedTempDir dir(L"msime-empty-hans-import");
    const ScopedDataDir scoped(dir.path());

    const auto response_for = [](const char *dictionary) {
        return SettingsDictionary::HandleRequest(
            {{"dictionary", dictionary}, {"action", "importHans"}, {"content", "环境"}});
    };
    const auto message_of = [](const boost::json::object &response) {
        return std::string(response.at("message").as_string());
    };

    // 英文与快捷短语词库收不了纯汉字。改动前这两种请求会一路落到拼音分支，
    // 把汉字写进拼音表；必须明确报错，不能静默成功。
    for (const char *dictionary : {"english", "quick"})
    {
        const auto response = response_for(dictionary);
        REQUIRE(!response.at("ok").as_bool());
        REQUIRE_EQ(message_of(response), std::string("该词库不支持纯汉字导入"));
    }

    // 五笔走 wubi86 单字码组合、拼音走注音引擎，两条分支的报错文案不同，
    // 正好能证明请求没有落到对方那条路径上。空数据目录下两条都开不了库。
    const auto wubi = response_for("wubi");
    REQUIRE(!wubi.at("ok").as_bool());
    REQUIRE_EQ(message_of(wubi).rfind("打开五笔词库失败", 0), static_cast<size_t>(0));

    const auto quanpin = response_for("quanpin");
    REQUIRE(!quanpin.at("ok").as_bool());
    REQUIRE(message_of(quanpin).rfind("打开五笔词库失败", 0) != 0);
    REQUIRE(message_of(quanpin) != "该词库不支持纯汉字导入");
}

TEST_CASE(HansImportIntoWubiWritesDerivedCodesAtTheWubiWeight)
{
    ScopedWubiDataDir data;
    const ScopedDataDir scoped(data.path());

    // 第 4 行不是纯汉字，第 5 行的「观园」在种子码表里没有编码。两者都应被计为
    // 失败并带行号，且不中断整批——前 3 行必须照常落库。
    const std::string content = "环境\n计算机\n诸葛亮\nabc\n观园\n";
    const auto response =
        SettingsDictionary::HandleRequest({{"dictionary", "wubi"}, {"action", "importHans"}, {"content", content}});
    REQUIRE(response.at("ok").as_bool());
    const std::string message = std::string(response.at("message").as_string());
    REQUIRE(message.find("成功导入 3 条") != std::string::npos);
    REQUIRE(message.find("失败 2 条") != std::string::npos);
    REQUIRE(message.find("第 4 行") != std::string::npos);
    REQUIRE(message.find("第 5 行") != std::string::npos);

    // 落库的码由规则推出，权重取五笔刻度的 30，而不是拼音的 10000。
    sqlite3 *db = nullptr;
    REQUIRE_EQ(sqlite3_open(test::Utf8(data.path() / L"msime.db").c_str(), &db), SQLITE_OK);
    struct Expect
    {
        const char *code;
        const char *word;
    };
    const Expect expected[] = {{"ggfu", "环境"}, {"ytsm", "计算机"}, {"yayp", "诸葛亮"}};
    for (const auto &row : expected)
    {
        sqlite3_stmt *stmt = nullptr;
        REQUIRE_EQ(sqlite3_prepare_v2(db, "SELECT weight FROM wubi86 WHERE key=?1 AND value=?2", -1, &stmt, nullptr),
                   SQLITE_OK);
        REQUIRE_EQ(sqlite3_bind_text(stmt, 1, row.code, -1, SQLITE_TRANSIENT), SQLITE_OK);
        REQUIRE_EQ(sqlite3_bind_text(stmt, 2, row.word, -1, SQLITE_TRANSIENT), SQLITE_OK);
        REQUIRE_EQ(sqlite3_step(stmt), SQLITE_ROW);
        REQUIRE_EQ(sqlite3_column_int(stmt, 0), 30);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);

    // record_user_insert 必须落到用户词库，「导出五笔用户词库」才收得回这些词条。
    sqlite3 *user_db = nullptr;
    REQUIRE_EQ(sqlite3_open(test::Utf8(user_dictionary::default_user_db_path()).c_str(), &user_db), SQLITE_OK);
    sqlite3_stmt *count = nullptr;
    REQUIRE_EQ(sqlite3_prepare_v2(user_db,
                                  "SELECT COUNT(*) FROM user_dictionary_operations "
                                  "WHERE dictionary='wubi' AND operation='upsert' AND user_inserted=1",
                                  -1, &count, nullptr),
               SQLITE_OK);
    REQUIRE_EQ(sqlite3_step(count), SQLITE_ROW);
    REQUIRE_EQ(sqlite3_column_int(count, 0), 3);
    sqlite3_finalize(count);
    sqlite3_close(user_db);

    // 再导一次应全部判为已存在，不产生重复行。注意 ok 仍为 false：这批里 2 行失败，
    // 而 SummarizeImport 的 ok 定义是 inserted>0 || (failed==0 && skipped>0)，与改动前
    // 拼音路径完全一致——有失败就报失败，不因为「有跳过」而算成功。
    const auto again =
        SettingsDictionary::HandleRequest({{"dictionary", "wubi"}, {"action", "importHans"}, {"content", content}});
    REQUIRE_EQ(again.at("ok").as_bool(), false);
    const std::string again_message = std::string(again.at("message").as_string());
    REQUIRE(again_message.find("跳过 3 条（已存在）") != std::string::npos);
}

TEST_CASE(HansImportIntoWubiIgnoresUserCharCodesAndAcceptsExtensionHan)
{
    ScopedWubiDataDir data;
    const ScopedDataDir scoped(data.path());

    const auto create = [](const char *code, const char *word) {
        const auto response = SettingsDictionary::HandleRequest(
            {{"dictionary", "wubi"}, {"action", "create"}, {"code", code}, {"word", word}, {"weight", 10}});
        REQUIRE(response.at("ok").as_bool());
    };
    // 用户给「张」自造的 aaaa 字典序在出厂 xtay 前面。它不能参与取码，否则张三会变成 aadg。
    create("aaaa", "张");
    // 「䶮」(U+4DAE, 扩展 A) 出厂码表里没有，用户补的码是它唯一的来源，应当兜底用上。
    create("dxyb", "\xE4\xB6\xAE");

    const std::string content = "张三\n\xE4\xB6\xAE张\n";
    const auto response =
        SettingsDictionary::HandleRequest({{"dictionary", "wubi"}, {"action", "importHans"}, {"content", content}});
    REQUIRE(response.at("ok").as_bool());
    REQUIRE(std::string(response.at("message").as_string()).find("成功导入 2 条") != std::string::npos);

    sqlite3 *db = nullptr;
    REQUIRE_EQ(sqlite3_open(test::Utf8(data.path() / L"msime.db").c_str(), &db), SQLITE_OK);
    const auto code_of = [db](const char *word) {
        sqlite3_stmt *stmt = nullptr;
        REQUIRE_EQ(sqlite3_prepare_v2(db, "SELECT key FROM wubi86 WHERE value=?1", -1, &stmt, nullptr), SQLITE_OK);
        REQUIRE_EQ(sqlite3_bind_text(stmt, 1, word, -1, SQLITE_TRANSIENT), SQLITE_OK);
        REQUIRE_EQ(sqlite3_step(stmt), SQLITE_ROW);
        std::string code(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)));
        sqlite3_finalize(stmt);
        return code;
    };
    REQUIRE_EQ(code_of("张三"), std::string("xtdg"));
    REQUIRE_EQ(code_of("\xE4\xB6\xAE张"), std::string("dxxt"));
    sqlite3_close(db);
}

namespace
{
class ScopedQuickPhraseDataDir
{
  public:
    ScopedQuickPhraseDataDir() : dir_(L"msime-quick-phrase-test"), scoped_(dir_.path()), db_(nullptr, sqlite3_close)
    {
        sqlite3 *db = nullptr;
        const int result = sqlite3_open(test::Utf8(dir_.path() / L"msime.db").c_str(), &db);
        db_.reset(db);
        REQUIRE_EQ(result, SQLITE_OK);
        exec("CREATE TABLE quick_parases(key TEXT NOT NULL, value TEXT NOT NULL,"
             "weight INTEGER NOT NULL DEFAULT 0, UNIQUE(key,value));"
             "INSERT INTO quick_parases VALUES('abc','测试短语',10),('abc','另一短语',20)");
    }
    ~ScopedQuickPhraseDataDir()
    {
        user_dictionary::close_default_user_database();
    }

    void exec(const char *sql)
    {
        REQUIRE_EQ(sqlite3_exec(db_.get(), sql, nullptr, nullptr, nullptr), SQLITE_OK);
    }

    boost::json::value rows() const
    {
        const auto response = SettingsDictionary::HandleRequest({{"dictionary", "quick"}, {"action", "query"}});
        REQUIRE(response.at("ok").as_bool());
        return response.at("rows");
    }

  private:
    ScopedTempDir dir_;
    ScopedDataDir scoped_;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db_;
};
} // namespace

TEST_CASE(QuickPhraseDuplicateCreateReportsExistingPhrase)
{
    ScopedQuickPhraseDataDir data;
    const auto before = data.rows();
    const auto response = SettingsDictionary::HandleRequest(
        {{"dictionary", "quick"}, {"action", "create"}, {"code", "ABC"}, {"word", "测试短语"}, {"weight", 99}});

    REQUIRE(!response.at("ok").as_bool());
    REQUIRE_EQ(std::string(response.at("message").as_string()), std::string("相同编码和内容的快捷短语已存在"));
    REQUIRE_EQ(data.rows(), before);
    REQUIRE(!std::filesystem::exists(std::filesystem::u8path(user_dictionary::default_user_db_path())));
}

TEST_CASE(QuickPhraseDuplicateUpdatePreservesBothPhrases)
{
    ScopedQuickPhraseDataDir data;
    const auto before = data.rows();
    const auto response = SettingsDictionary::HandleRequest({{"dictionary", "quick"},
                                                             {"action", "update"},
                                                             {"code", "abc"},
                                                             {"word", "测试短语"},
                                                             {"weight", 99},
                                                             {"oldCode", "abc"},
                                                             {"oldWord", "另一短语"}});

    REQUIRE(!response.at("ok").as_bool());
    REQUIRE_EQ(std::string(response.at("message").as_string()), std::string("相同编码和内容的快捷短语已存在"));
    REQUIRE_EQ(data.rows(), before);
    REQUIRE(!std::filesystem::exists(std::filesystem::u8path(user_dictionary::default_user_db_path())));
}

TEST_CASE(QuickPhraseAllowsSharedCodesAndWeightUpdates)
{
    ScopedQuickPhraseDataDir data;
    for (const auto &request : {
             boost::json::object{{"dictionary", "quick"}, {"action", "create"}, {"code", "abc"}, {"word", "新短语"}},
             boost::json::object{{"dictionary", "quick"}, {"action", "create"}, {"code", "def"}, {"word", "测试短语"}},
             boost::json::object{{"dictionary", "quick"},
                                 {"action", "update"},
                                 {"code", "abc"},
                                 {"word", "测试短语"},
                                 {"weight", 99},
                                 {"oldCode", "abc"},
                                 {"oldWord", "测试短语"}},
         })
    {
        REQUIRE(SettingsDictionary::HandleRequest(request).at("ok").as_bool());
    }
    const auto rows = data.rows().as_array();
    REQUIRE_EQ(rows.size(), static_cast<size_t>(4));
    REQUIRE_EQ(std::string(rows.front().as_object().at("word").as_string()), std::string("测试短语"));
    REQUIRE_EQ(rows.front().as_object().at("weight").as_int64(), 99);
}

TEST_CASE(QuickPhraseOtherConstraintsKeepTheirErrorDetails)
{
    ScopedQuickPhraseDataDir data;
    data.exec("CREATE TRIGGER reject_quick_phrase BEFORE INSERT ON quick_parases "
              "BEGIN SELECT RAISE(ABORT, 'test rejection'); END");
    const auto before = data.rows();
    const auto response = SettingsDictionary::HandleRequest(
        {{"dictionary", "quick"}, {"action", "create"}, {"code", "abc"}, {"word", "新短语"}});

    REQUIRE(!response.at("ok").as_bool());
    REQUIRE_EQ(std::string(response.at("message").as_string()), std::string("新增失败：test rejection"));
    REQUIRE_EQ(data.rows(), before);
}
