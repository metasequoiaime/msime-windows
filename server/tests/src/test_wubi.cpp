#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"
#include "engine/contracts/assets/assets.h"
#include "engine/core/runtime_paths.h"
#include "engine/providers/wubi_candidate_provider.h"
#include "engine/schemes/wubi_scheme.h"
#include <filesystem>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <windows.h>

namespace
{
void InputLetters(WubiScheme &scheme, const std::string &keys)
{
    for (const char ch : keys)
    {
        const char upper = ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - ('a' - 'A')) : ch;
        scheme.handle_key(static_cast<UINT>(upper), 0, static_cast<WCHAR>(ch));
    }
}

// 与 test_user_dictionary_journal.cpp 的同名助手同型：打开临时库跑语句、取标量。
class TestDatabase
{
  public:
    explicit TestDatabase(const std::filesystem::path &path)
    {
        REQUIRE_EQ(sqlite3_open(test::Utf8(path).c_str(), &db_), SQLITE_OK);
    }
    ~TestDatabase()
    {
        if (db_ != nullptr)
            sqlite3_close(db_);
    }
    void exec(const std::string &sql)
    {
        REQUIRE_EQ(sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    }
    std::int64_t scalar_int64(const std::string &sql)
    {
        sqlite3_stmt *stmt = nullptr;
        REQUIRE_EQ(sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK);
        REQUIRE_EQ(sqlite3_step(stmt), SQLITE_ROW);
        const std::int64_t value = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return value;
    }
    std::int64_t scalar_int(const std::string &sql)
    {
        return scalar_int64(sql);
    }

  private:
    sqlite3 *db_ = nullptr;
};

// journal 路径经 RuntimePaths 注入临时目录，避免测试写真实用户目录；目录须先存在，
// 与生产环境一致（RuntimePaths 管理的世代目录总是已建好）。
metasequoia::RuntimePaths PathsIn(const std::filesystem::path &directory)
{
    metasequoia::RuntimePaths paths;
    paths.resources = directory / "resources";
    paths.user_data = directory / "user";
    paths.cache = directory / "cache";
    paths.dictionaries = directory / "resources";
    std::filesystem::create_directories(paths.user_data);
    return paths;
}

std::filesystem::path CreateWubiDatabase()
{
    const auto path = test::TempRoot() / "msime-wubi-provider-test.db";
    std::filesystem::remove(path);

    sqlite3 *db = nullptr;
    if (sqlite3_open(test::Utf8(path).c_str(), &db) != SQLITE_OK)
    {
        throw std::runtime_error("Failed to create temporary Wubi database.");
    }

    const char *sql = "CREATE TABLE wubi86 (\"key\" TEXT, \"value\" TEXT, \"weight\" INTEGER);"
                      "INSERT INTO wubi86 VALUES ('a', '工', 20);"
                      "INSERT INTO wubi86 VALUES ('a', '戈', 10);"
                      "INSERT INTO wubi86 VALUES ('aa', '式', 20);"
                      // 上界邻居：'y' 前缀查询必须命中 'yh'，且排除上界之外的 'z' 行。
                      "INSERT INTO wubi86 VALUES ('y', '也', 5);"
                      "INSERT INTO wubi86 VALUES ('yh', '又', 6);"
                      "INSERT INTO wubi86 VALUES ('z', '越过上界', 999);";
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    if (result != SQLITE_OK)
    {
        std::filesystem::remove(path);
        throw std::runtime_error("Failed to initialize temporary Wubi database.");
    }
    return path;
}

QueryRequest WubiRequest(const std::string &code)
{
    QueryRequest request;
    request.scheme = SchemeType::Wubi;
    request.raw_input = code;
    request.normalized_input = code;
    request.valid = true;
    return request;
}
} // namespace

TEST_CASE(WubiSchemeBuildsDirectCodeRequest)
{
    WubiScheme scheme;
    InputLetters(scheme, "aa");

    const QueryRequest request = scheme.build_request();
    REQUIRE(request.valid);
    REQUIRE_EQ(request.scheme, SchemeType::Wubi);
    REQUIRE_EQ(request.raw_input, std::string("aa"));
    REQUIRE_EQ(request.normalized_input, std::string("aa"));
    REQUIRE_EQ(request.segmentation, std::string("aa"));
}

TEST_CASE(WubiSchemeAcceptsAtMostFourCodesAndDoesNotUseZ)
{
    WubiScheme scheme;
    InputLetters(scheme, "abcdez");
    REQUIRE_EQ(scheme.build_request().raw_input, std::string("abcd"));

    scheme.handle_key(VK_BACK, 0, 0);
    REQUIRE_EQ(scheme.build_request().raw_input, std::string("abc"));
}

TEST_CASE(WubiProviderReturnsPrefixMatchesWithExactCodeFirst)
{
    const auto db_path = CreateWubiDatabase();
    {
        WubiCandidateProvider provider(test::Utf8(db_path));

        // 1 码：精确等长行在前（按权重），更长前缀行按权重跟随——逐键提示的基础。
        const auto one_code = provider.query(WubiRequest("a"));
        REQUIRE_EQ(one_code.size(), static_cast<size_t>(3));
        REQUIRE_EQ(one_code[0].pinyin, std::string("a"));
        REQUIRE_EQ(one_code[0].word, std::string("工"));
        REQUIRE_EQ(one_code[0].weight, 20);
        REQUIRE_EQ(one_code[1].word, std::string("戈"));
        REQUIRE_EQ(one_code[2].pinyin, std::string("aa"));
        REQUIRE_EQ(one_code[2].word, std::string("式"));

        // 每一码都是前缀查询：2 码精确命中只剩等长行。
        const auto two_code = provider.query(WubiRequest("aa"));
        REQUIRE_EQ(two_code.size(), static_cast<size_t>(1));
        REQUIRE_EQ(two_code[0].word, std::string("式"));

        // 精确优先压过权重：'y'(5) 排在前缀行 'yh'(6) 之前；上界构造排除 'z' 行。
        const auto upper_edge = provider.query(WubiRequest("y"));
        REQUIRE_EQ(upper_edge.size(), static_cast<size_t>(2));
        REQUIRE_EQ(upper_edge[0].word, std::string("也"));
        REQUIRE_EQ(upper_edge[1].word, std::string("又"));
    }
    std::filesystem::remove(db_path);
}

TEST_CASE(WubiProviderLimitsPrefixScanToBoundedRows)
{
    const auto directory = test::TempRoot() / ("msime-wubi-provider-limit-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto db_path = directory / "limit.db";
    {
        TestDatabase db(db_path);
        std::string sql = "CREATE TABLE wubi86(\"key\" TEXT,\"value\" TEXT,\"weight\" INTEGER);";
        // 55 个精确行 + 5 个前缀行：LIMIT 截断后可见的只能是权重最高的 50 个精确行，
        // 前缀行整体排在精确组之后，不应被返回。
        for (int weight = 1; weight <= 55; ++weight)
        {
            sql += "INSERT INTO wubi86 VALUES('a','字" + std::to_string(weight) + "'," + std::to_string(weight) + ");";
        }
        for (int weight = 1; weight <= 5; ++weight)
        {
            sql += "INSERT INTO wubi86 VALUES('ab','前" + std::to_string(weight) + "'," + std::to_string(weight) + ");";
        }
        db.exec(sql);
    }
    {
        WubiCandidateProvider provider(test::Utf8(db_path));
        const auto candidates = provider.query(WubiRequest("a"));
        REQUIRE_EQ(candidates.size(), static_cast<size_t>(50));
        REQUIRE_EQ(candidates[0].weight, 55);
        REQUIRE_EQ(candidates[49].weight, 6);
    }
    std::filesystem::remove_all(directory);
}

TEST_CASE(WubiProviderLearnsSelectionIntoWeightAndJournal)
{
    const auto directory = test::TempRoot() / ("msime-wubi-provider-learn-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto db_path = directory / "main.db";
    const auto paths = PathsIn(directory);
    {
        TestDatabase db(db_path);
        db.exec("CREATE TABLE wubi86(\"key\" TEXT,\"value\" TEXT,\"weight\" INTEGER);"
                "INSERT INTO wubi86 VALUES('aa','式',20);"
                "INSERT INTO wubi86 VALUES('aa','工',10);");
    }
    {
        WubiCandidateProvider provider(test::Utf8(db_path), paths);
        // 选中次位候选：权重升到同码组最高（MAX+1），与全拼 build_sql_for_updating_word 语义一致。
        REQUIRE_EQ(provider.update_weight_by_pinyin_and_word(SchemeType::Wubi, "aa", "工"), 0);
        const auto candidates = provider.query(WubiRequest("aa"));
        REQUIRE_EQ(candidates.size(), static_cast<size_t>(2));
        REQUIRE_EQ(candidates[0].word, std::string("工"));
        REQUIRE_EQ(candidates[0].weight, 21);

        // 行不存在不造行。
        REQUIRE_EQ(provider.update_weight_by_pinyin_and_word(SchemeType::Wubi, "ab", "不存在"), -1);
    }
    {
        TestDatabase journal(paths.user(metasequoia::assets::user_journal));
        REQUIRE_EQ(journal.scalar_int("SELECT COUNT(*) FROM user_dictionary_operations WHERE dictionary='wubi' "
                                      "AND key='aa' AND value='工' AND operation='upsert' AND weight=21"),
                   1);
        REQUIRE_EQ(journal.scalar_int("SELECT COUNT(*) FROM user_dictionary_operations WHERE value='不存在'"), 0);
    }
    std::filesystem::remove_all(directory);
}

TEST_CASE(WubiProviderDeletesWubiCandidateThroughJournal)
{
    const auto directory = test::TempRoot() / ("msime-wubi-provider-delete-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto db_path = directory / "main.db";
    const auto paths = PathsIn(directory);
    {
        TestDatabase db(db_path);
        db.exec("CREATE TABLE wubi86(\"key\" TEXT,\"value\" TEXT,\"weight\" INTEGER);"
                "INSERT INTO wubi86 VALUES('aa','式',20);"
                "INSERT INTO wubi86 VALUES('aa','工',10);");
    }
    {
        WubiCandidateProvider provider(test::Utf8(db_path), paths);
        REQUIRE_EQ(provider.delete_by_pinyin_and_word(SchemeType::Wubi, "aa", "式"), 0);
        // 再删已不存在的行：changes()==0，失败且不写墓碑。
        REQUIRE_EQ(provider.delete_by_pinyin_and_word(SchemeType::Wubi, "aa", "式"), -1);
        const auto remaining = provider.query(WubiRequest("aa"));
        REQUIRE_EQ(remaining.size(), static_cast<size_t>(1));
        REQUIRE_EQ(remaining[0].word, std::string("工"));
    }
    {
        TestDatabase journal(paths.user(metasequoia::assets::user_journal));
        REQUIRE_EQ(journal.scalar_int("SELECT COUNT(*) FROM user_dictionary_operations WHERE dictionary='wubi' "
                                      "AND key='aa' AND value='式' AND operation='delete'"),
                   1);
    }
    std::filesystem::remove_all(directory);
}
