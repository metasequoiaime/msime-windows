// 字母词（msime.db 的 latin_words：T恤、B超、卡拉OK）：单独出候选、进词格整句、不调频不造词（#1340）。
#include "../../core/input_session.h"
#include "../../core/data_path.h"
#include "../../contracts/assets/assets.h"
#include "../../shuangpin/shuangpin_profile.h"
#include "test_directory_cleanup.h"

#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void execute(const std::filesystem::path &path, const char *sql)
{
    sqlite3 *database = nullptr;
    if (sqlite3_open(metasequoia::path_to_utf8(path).c_str(), &database) != SQLITE_OK)
    {
        sqlite3_close(database);
        throw std::runtime_error("Failed to open the latin-word test dictionary.");
    }
    char *error = nullptr;
    const int rc = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    const std::string message = error == nullptr ? "SQLite operation failed." : error;
    sqlite3_free(error);
    sqlite3_close(database);
    if (rc != SQLITE_OK)
        throw std::runtime_error(message);
}

// 词库里所有拼音分表中值为 value 的行数。
int pinyin_rows_with_value(const std::filesystem::path &path, const std::string &value)
{
    sqlite3 *database = nullptr;
    if (sqlite3_open(metasequoia::path_to_utf8(path).c_str(), &database) != SQLITE_OK)
    {
        sqlite3_close(database);
        throw std::runtime_error("Failed to open the latin-word test dictionary.");
    }
    std::vector<std::string> tables;
    sqlite3_stmt *statement = nullptr;
    sqlite3_prepare_v2(database,
                       "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl\\_%' ESCAPE '\\'", -1,
                       &statement, nullptr);
    while (sqlite3_step(statement) == SQLITE_ROW)
        tables.emplace_back(reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)));
    sqlite3_finalize(statement);
    int rows = 0;
    for (const auto &table : tables)
    {
        const std::string sql = "SELECT count(*) FROM \"" + table + "\" WHERE value = ?1";
        sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr);
        sqlite3_bind_text(statement, 1, value.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(statement) == SQLITE_ROW)
            rows += sqlite3_column_int(statement, 0);
        sqlite3_finalize(statement);
    }
    sqlite3_close(database);
    return rows;
}

// 用户日志（连同 WAL）里出现过这段文字：调频的回放记录和选择计数都记在这里。
bool journal_mentions(const metasequoia::RuntimePaths &paths, const std::string &text)
{
    const auto journal = paths.user(metasequoia::assets::user_journal);
    auto wal = journal;
    wal += "-wal";
    for (const auto &path : {journal, wal})
    {
        std::ifstream stream(path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        if (bytes.find(text) != std::string::npos)
            return true;
    }
    return false;
}

void type(metasequoia::InputSession &session, const std::string &text)
{
    for (const char character : text)
        require(session.handle_character(character).handled, "A pinyin character was not handled.");
}

std::size_t find_index(const metasequoia::InputSession &session, const std::string &word)
{
    const auto &candidates = session.candidates();
    for (std::size_t i = 0; i < candidates.size(); ++i)
    {
        if (candidates[i].word == word)
            return i;
    }
    return candidates.size();
}

bool has_latin_word(const metasequoia::InputSession &session)
{
    for (const auto &item : session.candidates())
    {
        if (item.latin_word)
            return true;
    }
    return false;
}

void prepare_dictionary(const std::filesystem::path &directory, bool with_latin_words)
{
    std::filesystem::create_directories(directory);
    execute(directory / "msime.db", "BEGIN;"
                                    "CREATE TABLE tbl_1_m(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "INSERT INTO tbl_1_m VALUES('mai','m','买',100);"
                                    "CREATE TABLE tbl_1_z(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "INSERT INTO tbl_1_z VALUES('zuo','z','做',100);"
                                    "CREATE TABLE tbl_2_t(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "INSERT INTO tbl_2_t VALUES('te''xu','tx','特许',100);"
                                    "INSERT INTO tbl_2_t VALUES('tong''xue','tx','同学',200);"
                                    // 真实词库每张分表都在：调频按 t'xu'shan 去改 tbl_3_t 时不会因缺表而中途失败。
                                    "CREATE TABLE tbl_3_t(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "CREATE TABLE tbl_2_y(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "INSERT INTO tbl_2_y VALUES('yi''jian','yj','一件',100);"
                                    "CREATE TABLE tbl_2_z(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "INSERT INTO tbl_2_z VALUES('zuo''bi','zb','作弊',100);"
                                    "COMMIT;");
    if (!with_latin_words)
        return;
    execute(directory / "msime.db",
            "BEGIN;"
            "CREATE TABLE latin_words(key TEXT, value TEXT, weight INTEGER, exact_only INTEGER);"
            "INSERT INTO latin_words VALUES('t''xu','T恤',10,0);"
            "INSERT INTO latin_words VALUES('t''xu''shan','T恤衫',9,0);"
            "INSERT INTO latin_words VALUES('ka''la''o''k','卡拉OK',8,0);"
            "INSERT INTO latin_words VALUES('a''a''zhi','AA制',7,0);"
            "INSERT INTO latin_words VALUES('b''chao','B超',6,0);"
            "INSERT INTO latin_words VALUES('u''pan','U盘',5,0);"
            "INSERT INTO latin_words VALUES('zhe''e','浙E',4,1);"
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

void with_lattice(metasequoia::InputSession &session)
{
    SentenceAssociationOptions association;
    association.word_lattice = true;
    session.set_sentence_association(association);
}

void test_quanpin_candidates(const metasequoia::RuntimePaths &paths)
{
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "txu");
        const auto &candidates = session.candidates();
        require(!candidates.empty() && candidates[0].word == "T恤" && candidates[0].latin_word &&
                    candidates[0].canonical_pinyin == "t'xu" && candidates[0].source == CandidateSource::Database,
                "txu must offer T恤 first as a latin-word dictionary row.");
        require(find_index(session, "T恤衫") == 1 && candidates[1].latin_word,
                "Three letters must also complete to the longer latin word right after it.");
        require(find_index(session, "特许") < candidates.size(),
                "The ordinary abbreviated reading must stay among the candidates.");
    }
    {
        // tx 是常用简拼；只有两个字母时不补全，单个字母什么都不配。
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "tx");
        require(!has_latin_word(session) && find_index(session, "同学") == 0,
                "Two letters that are only a prefix of a latin word must not bring it in.");
    }
    {
        // 切分是 ka'lao'k，字母词按字母串认，不看切分。
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "kalaok");
        const auto index = find_index(session, "卡拉OK");
        require(index < session.candidates().size() && session.candidates()[index].latin_word,
                "A latin word must match on letters even when the cut differs from its key.");
        require(session.select_candidate(index).commit == "卡拉OK" && !session.has_composition(),
                "Selecting a latin word must consume the whole input.");
    }
    {
        // u 开头切不成拼音，照样按字母串认；U 模式要按 Shift+U 才进。
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "upan");
        require(find_index(session, "U盘") == 0, "A latin word starting with u must be offered on its own.");
    }
}

void test_quanpin_sentence(const metasequoia::RuntimePaths &paths)
{
    metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
    with_lattice(session);
    type(session, "maiyijiantxu");
    require(!session.candidates().empty() && session.candidates()[0].word == "买一件T恤" &&
                session.candidates()[0].source == CandidateSource::Generated,
            "The lattice must compose a sentence through the t segment with the latin word.");
}

void test_shuangpin(const metasequoia::RuntimePaths &paths)
{
    const auto &xiaohe = GetXiaoheShuangpinProfile();
    {
        metasequoia::InputSession session(SchemeType::Shuangpin, xiaohe, paths);
        type(session, "txu");
        require(!session.candidates().empty() && session.candidates()[0].word == "T恤" &&
                    session.candidates()[0].latin_word,
                "Xiaohe txu must offer T恤 first.");
    }
    {
        metasequoia::InputSession session(SchemeType::Shuangpin, xiaohe, paths);
        with_lattice(session);
        // 小鹤 买 md、一件 yijm：贪心切分本身就留下 t'xu 两段。
        type(session, "mdyijmtxu");
        require(!session.candidates().empty() && session.candidates()[0].word == "买一件T恤",
                "A shuangpin cut that leaves the latin letter on its own must compose the sentence.");
    }
    {
        metasequoia::InputSession session(SchemeType::Shuangpin, xiaohe, paths);
        with_lattice(session);
        // 做 zo、B超 bic：贪心切分读成 zo'bi'c（作弊 + c），要在边界上换一条按 B超 切的切分。
        type(session, "zobic");
        require(find_index(session, "做B超") < session.candidates().size(),
                "A latin word the greedy shuangpin cut splits apart must still reach the sentence.");
    }
}

// 车牌（exact_only）：整串正好是它时单独出候选，不补全、不进整句。zhe'e、mai'zhe'e 全是完整音节，
// 夹具里 zhe、e 都没有词，不标 exact_only 的话词格只能经过浙E，必然组出「买浙E」。
void test_exact_only(const metasequoia::RuntimePaths &paths)
{
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "zhee");
        const auto index = find_index(session, "浙E");
        require(index < session.candidates().size() && session.candidates()[index].latin_word,
                "A license plate must be offered when the input is exactly its code.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "zhe");
        require(find_index(session, "浙E") == session.candidates().size(), "A license plate must not be completed.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        with_lattice(session);
        type(session, "maizhee");
        require(find_index(session, "买浙E") == session.candidates().size(),
                "A license plate must not enter a quanpin sentence.");
    }
    const auto &xiaohe = GetXiaoheShuangpinProfile();
    {
        // 小鹤 浙 ve，字母 E 一个键。
        metasequoia::InputSession session(SchemeType::Shuangpin, xiaohe, paths);
        type(session, "vee");
        require(find_index(session, "浙E") < session.candidates().size(),
                "A shuangpin license plate must be offered on an exact match.");
    }
    {
        metasequoia::InputSession session(SchemeType::Shuangpin, xiaohe, paths);
        with_lattice(session);
        type(session, "mdvee");
        require(find_index(session, "买浙E") == session.candidates().size(),
                "A license plate must not enter a shuangpin sentence.");
    }
}

void test_no_learning(const std::filesystem::path &directory, const metasequoia::RuntimePaths &paths)
{
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        require(session.set_frequency_adjustment({metasequoia::FrequencyAdjustmentMode::Linear, 1, 1}),
                "The frequency adjustment fixture was not accepted.");
        type(session, "txu");
        const auto index = find_index(session, "T恤衫");
        require(!session.pin_candidate(index).handled, "A latin word must not be pinnable.");
        require(session.select_candidate(index).commit == "T恤衫", "The completion must be selectable.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        with_lattice(session);
        type(session, "maiyijiantxu");
        require(session.select_candidate(0).commit == "买一件T恤", "The latin sentence must be selectable.");
    }
    for (const char *word : {"T恤", "T恤衫", "买一件T恤"})
    {
        require(pinyin_rows_with_value(directory / "msime.db", word) == 0,
                "Selecting latin words must not write them into the pinyin tables.");
        require(!journal_mentions(paths, word), "Selecting latin words must not record them in the user journal.");
    }
}

void test_old_dictionary(const metasequoia::RuntimePaths &paths)
{
    metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
    with_lattice(session);
    type(session, "maiyijiantxu");
    require(!has_latin_word(session) && find_index(session, "买一件T恤") == session.candidates().size(),
            "A dictionary without latin_words must behave as before.");
}
} // namespace

int main()
{
    try
    {
        const auto root = std::filesystem::temp_directory_path() / "metasequoia-latin-words-test";
        std::filesystem::remove_all(root);
        metasequoia::test::ScopedDataDirectoryCleanup cleanup(root);
        const auto current = root / "current";
        const auto old = root / "old";
        prepare_dictionary(current, true);
        prepare_dictionary(old, false);
        const auto paths = paths_for(current);
        test_quanpin_candidates(paths);
        test_quanpin_sentence(paths);
        test_shuangpin(paths);
        test_exact_only(paths);
        test_no_learning(current, paths);
        test_old_dictionary(paths_for(old));
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
