// 长词前缀补全（#1343）：首选是四个字以上的词时，以它开头的更长的词插到第二位，选中吃掉整串输入，
// 只按自己的键调频。
#include "../../core/input_session.h"
#include "../../core/data_path.h"
#include "../../quanpin/prefix_completion.h"
#include "../../shuangpin/shuangpin_profile.h"
#include "test_directory_cleanup.h"

#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

sqlite3 *open(const std::filesystem::path &path)
{
    sqlite3 *database = nullptr;
    if (sqlite3_open(metasequoia::path_to_utf8(path).c_str(), &database) != SQLITE_OK)
    {
        sqlite3_close(database);
        throw std::runtime_error("Failed to open the prefix-completion test dictionary.");
    }
    return database;
}

void execute(const std::filesystem::path &path, const char *sql)
{
    sqlite3 *database = open(path);
    char *error = nullptr;
    const int rc = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    const std::string message = error == nullptr ? "SQLite operation failed." : error;
    sqlite3_free(error);
    sqlite3_close(database);
    if (rc != SQLITE_OK)
        throw std::runtime_error(message);
}

std::int64_t weight_of(const std::filesystem::path &path, const char *table, const char *key, const char *value)
{
    sqlite3 *database = open(path);
    sqlite3_stmt *statement = nullptr;
    const std::string sql = std::string("SELECT weight FROM ") + table + " WHERE key = ?1 AND value = ?2";
    sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, value, -1, SQLITE_TRANSIENT);
    const std::int64_t weight = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return weight;
}

void type(metasequoia::InputSession &session, const std::string &text)
{
    for (const char character : text)
        require(session.handle_character(character).handled, "A pinyin character was not handled.");
}

std::size_t completion_index(const metasequoia::InputSession &session)
{
    const auto &candidates = session.candidates();
    for (std::size_t i = 0; i < candidates.size(); ++i)
    {
        if (candidates[i].prefix_completion)
            return i;
    }
    return candidates.size();
}

void prepare_dictionary(const std::filesystem::path &directory)
{
    std::filesystem::create_directories(directory);
    // 补全只认权重高过出厂平线（10）的行，或语言模型认得的行；测试里没有模型，靠权重区分。
    execute(directory / "msime.db",
            "BEGIN;"
            "CREATE TABLE tbl_2_z(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
            "INSERT INTO tbl_2_z VALUES('zhong''hua','zh','中华',500);"
            "CREATE TABLE tbl_3_z(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
            "INSERT INTO tbl_3_z VALUES('zhong''hua''ren','zhr','中华人',100);"
            "CREATE TABLE tbl_4_z(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
            "INSERT INTO tbl_4_z VALUES('zhong''hua''ren''min','zhrm','中华人民',3796);"
            "CREATE TABLE tbl_5_z(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
            // 同读音不同字：值不以首选开头，不能补。
            "INSERT INTO tbl_5_z VALUES('zhong''hua''ren''min''zu','zhrmz','中华仁民族',900);"
            "CREATE TABLE tbl_6_z(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
            "INSERT INTO tbl_6_z VALUES('zhong''hua''ren''min''yin''hang','zhrmyh','中华人民银行',5);"
            "CREATE TABLE tbl_7_z(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
            "INSERT INTO tbl_7_z VALUES('zhong''hua''ren''min''gong''he''guo','zhrmghg','中华人民共和国',50);"
            "CREATE TABLE tbl_others_z(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
            "CREATE TABLE tbl_4_b(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
            "INSERT INTO tbl_4_b VALUES('bei''jing''da''xue','bjdx','北京大学',6311);"
            "CREATE TABLE tbl_5_b(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
            // 出厂平线上的切词残片：没有模型时不显示。
            "INSERT INTO tbl_5_b VALUES('bei''jing''da''xue''xing','bjdxx','北京大学星',10);"
            "CREATE TABLE tbl_6_b(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
            "INSERT INTO tbl_6_b VALUES('bei''jing''da''xue''xiao''shi','bjdxxs','北京大学校史',10);"
            // 前缀是词库词、以它开头的只有一行：只多一个字、权重在平线上也补。
            "CREATE TABLE tbl_4_h(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
            "INSERT INTO tbl_4_h VALUES('hai''nei''cun''zhi','hncz','海内存知',576);"
            "CREATE TABLE tbl_5_h(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
            "INSERT INTO tbl_5_h VALUES('hai''nei''cun''zhi''ji','hnczj','海内存知己',10);"
            // 诗句：前四个字不是词库里的词，首选只能是整句猜的，字还可能猜错。
            "CREATE TABLE tbl_5_c(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
            "INSERT INTO tbl_5_c VALUES('chuang''qian''ming''yue''guang','cqmyg','床前明月光',10);"
            "COMMIT;");
}

metasequoia::RuntimePaths paths_for(const std::filesystem::path &directory)
{
    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;
    return paths;
}

metasequoia::PrefixCompletionOptions enabled(std::size_t minimum_chars = 4)
{
    metasequoia::PrefixCompletionOptions options;
    options.enabled = true;
    options.minimum_chars = minimum_chars;
    return options;
}

void test_query_rows(const std::filesystem::path &directory)
{
    sqlite3 *database = open(directory / "msime.db");
    std::unordered_map<std::string, sqlite3_stmt *> statements;
    auto rows = quanpin::query_prefix_completion_rows(database, statements, "zhong'hua'ren'min", "中华人民");
    quanpin::score_prefix_completions(rows, 4, true, quanpin::PrefixCompletionScorers{});
    for (auto &entry : statements)
        sqlite3_finalize(entry.second);
    sqlite3_close(database);
    require(rows.size() == 2, "Only longer rows whose value starts with the head word must be returned.");
    require(rows[0].value == "中华人民共和国" && rows[0].confident,
            "A row above the shipped flat weight must rank first and be shown.");
    require(rows[1].value == "中华人民银行" && !rows[1].confident,
            "A row at the shipped flat weight must not be shown without a language model.");
}

// 首选是整句猜的（chuangqianmingyue 猜成「窗前明月」）：只按读音补，平线权重也补。
void test_guessed_head(const std::filesystem::path &directory)
{
    sqlite3 *database = open(directory / "msime.db");
    std::unordered_map<std::string, sqlite3_stmt *> statements;
    quanpin::PrefixCompletionCache cache;
    const auto guessed = [] {
        std::vector<WordItem> result;
        WordItem head("chuang'qian'ming'yue", "窗前明月", 1, CandidateSource::Fallback, "chuang'qian'ming'yue");
        head.sentence_association = true;
        result.push_back(head);
        result.emplace_back("chuang'qian", "窗前", 500, CandidateSource::Database, "chuang'qian");
        return result;
    };
    auto result = guessed();
    quanpin::insert_prefix_completion(result, "chuang'qian'ming'yue", enabled(), database, statements, cache,
                                      quanpin::PrefixCompletionScorers{});
    const bool completed = result.size() == 3 && result[1].word == "床前明月光" && result[1].prefix_completion &&
                           result[1].pinyin == "chuang'qian'ming'yue";
    // 同一读音、首选是词库词时仍按字补：「窗前明月」开头的词没有，不补。
    auto word_head = guessed();
    word_head[0].source = CandidateSource::Database;
    quanpin::insert_prefix_completion(word_head, "chuang'qian'ming'yue", enabled(), database, statements, cache,
                                      quanpin::PrefixCompletionScorers{});
    for (auto &entry : statements)
        sqlite3_finalize(entry.second);
    sqlite3_close(database);
    require(completed, "A guessed head must be completed by its reading even when its characters are wrong.");
    require(word_head.size() == 2, "A dictionary-word head must still be completed by its characters.");
}

void test_quanpin(const metasequoia::RuntimePaths &paths, const std::filesystem::path &directory)
{
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "zhonghuarenmin");
        require(completion_index(session) == session.candidates().size(), "Prefix completion must default to off.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        session.set_prefix_completion_options(enabled());
        type(session, "zhonghuarenmin");
        const auto &candidates = session.candidates();
        require(candidates.size() >= 2 && candidates[0].word == "中华人民",
                "The exact four-character word must stay first.");
        require(candidates[1].word == "中华人民共和国" && candidates[1].prefix_completion,
                "The best completion must take the second slot.");
        require(candidates[1].canonical_pinyin == "zhong'hua'ren'min'gong'he'guo" &&
                    candidates[1].pinyin == candidates[0].pinyin,
                "A completion must keep its own reading and consume what the head consumes.");
        require(session.select_candidate(1).commit == "中华人民共和国" && !session.has_composition(),
                "Selecting the completion must commit it and consume the whole input.");
    }
    require(weight_of(directory / "msime.db", "tbl_7_z", "zhong'hua'ren'min'gong'he'guo", "中华人民共和国") > 50,
            "Selecting a completion must raise its weight under its own key.");
    require(weight_of(directory / "msime.db", "tbl_4_z", "zhong'hua'ren'min", "中华人民") == 3796,
            "Selecting a completion must not touch the head's row.");
    {
        // 三个字不补：zhonghuaren 的首选只有三个字。
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        session.set_prefix_completion_options(enabled());
        type(session, "zhonghuaren");
        require(completion_index(session) == session.candidates().size(),
                "A head shorter than the minimum must not be completed.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        session.set_prefix_completion_options(enabled(5));
        type(session, "zhonghuarenmin");
        require(completion_index(session) == session.candidates().size(), "The minimum length must follow the option.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        session.set_prefix_completion_options(enabled());
        type(session, "beijingdaxue");
        require(session.candidates().front().word == "北京大学" &&
                    completion_index(session) == session.candidates().size(),
                "Rows at the shipped flat weight must not be offered without a language model.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        session.set_prefix_completion_options(enabled());
        type(session, "haineicunzhi");
        const auto &candidates = session.candidates();
        require(candidates.size() >= 2 && candidates[0].word == "海内存知" && candidates[1].word == "海内存知己" &&
                    candidates[1].prefix_completion,
                "The only longer word starting with the head must be offered even one character longer.");
    }
    {
        // 置顶会拿别的键当阶梯，补全行不接受。
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        session.set_prefix_completion_options(enabled());
        type(session, "zhonghuarenmin");
        require(completion_index(session) == 1 && !session.pin_candidate(1).handled,
                "A completion must not be pinned.");
    }
}

void test_shuangpin(const metasequoia::RuntimePaths &paths)
{
    const auto &xiaohe = GetXiaoheShuangpinProfile();
    metasequoia::InputSession session(SchemeType::Shuangpin, xiaohe, paths);
    session.set_prefix_completion_options(enabled());
    // 小鹤：zhong=vs hua=hx ren=rf min=mb
    type(session, "vshxrfmb");
    const auto &candidates = session.candidates();
    require(!candidates.empty() && candidates[0].word == "中华人民", "Shuangpin must offer the exact word first.");
    const auto index = completion_index(session);
    require(index == 1 && candidates[1].word == "中华人民共和国",
            "Shuangpin must complete the head word in the second slot.");
    require(session.select_candidate(1).commit == "中华人民共和国" && !session.has_composition(),
            "Selecting the shuangpin completion must consume the whole input.");
}
} // namespace

int main()
{
    try
    {
        const auto root = std::filesystem::temp_directory_path() / "metasequoia-prefix-completion-test";
        std::filesystem::remove_all(root);
        metasequoia::test::ScopedDataDirectoryCleanup cleanup(root);
        const auto directory = root / "data";
        prepare_dictionary(directory);
        const auto paths = paths_for(directory);
        test_query_rows(directory);
        test_guessed_head(directory);
        test_quanpin(paths, directory);
        test_shuangpin(paths);
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "test_prefix_completion failed: %s\n", error.what());
        return 1;
    }
    std::puts("test_prefix_completion passed");
    return 0;
}
