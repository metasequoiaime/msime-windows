// 字母词（msime.db 的 latin_words：T恤、B超、卡拉OK）：单独出候选、进词格整句、不调频不造词（#1340）。
#include "../../core/input_session.h"
#include "../../core/data_path.h"
#include "../../contracts/assets/assets.h"
#include "../../quanpin/latin_words.h"
#include "../../shuangpin/shuangpin_profile.h"
#include "../../user_dictionary/user_dictionary_journal.h"
#include "test_directory_cleanup.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
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

// 候选非空且第一个就是 word。只比 find_index(...) == 0 不够：候选为空时 find_index 也返回 0。
bool first_is(const metasequoia::InputSession &session, const std::string &word)
{
    return !session.candidates().empty() && session.candidates()[0].word == word;
}

// 第一个候选是 leader，紧跟着的第二个是字母词 latin。
bool latin_follows_leader(const metasequoia::InputSession &session, const std::string &leader, const std::string &latin)
{
    const auto &candidates = session.candidates();
    return candidates.size() > 1 && candidates[0].word == leader && candidates[1].word == latin &&
           candidates[1].latin_word;
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

void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::vector<std::string> words_of(const metasequoia::InputSession &session)
{
    std::vector<std::string> words;
    for (const auto &item : session.candidates())
        words.push_back(item.word);
    return words;
}

// 去掉字母词那几行之后剩下的候选，保持原来的先后。
std::vector<std::string> ordinary_words_of(const metasequoia::InputSession &session)
{
    std::vector<std::string> words;
    for (const auto &item : session.candidates())
    {
        if (!item.latin_word)
            words.push_back(item.word);
    }
    return words;
}

// 失败信息里附上整张候选列表，红了不用另外加打印就能看出排成了什么样。
std::string listing(const metasequoia::InputSession &session)
{
    std::string text;
    for (const auto &word : words_of(session))
        text += (text.empty() ? "" : " | ") + word;
    return " Candidates: [" + text + "]";
}

std::size_t count_word(const metasequoia::InputSession &session, const std::string &word)
{
    const auto words = words_of(session);
    return static_cast<std::size_t>(std::count(words.begin(), words.end(), word));
}

void prepare_dictionary(const std::filesystem::path &directory, bool with_latin_words)
{
    std::filesystem::create_directories(directory);
    execute(directory / "msime.db", "BEGIN;"
                                    "CREATE TABLE tbl_1_m(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "INSERT INTO tbl_1_m VALUES('mai','m','买',100);"
                                    "CREATE TABLE tbl_1_z(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "INSERT INTO tbl_1_z VALUES('zuo','z','做',100);"
                                    "INSERT INTO tbl_1_z VALUES('zhe','z','这',500);"
                                    "INSERT INTO tbl_1_z VALUES('zhi','z','知',500);"
                                    "CREATE TABLE tbl_1_e(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "INSERT INTO tbl_1_e VALUES('e','e','鹅',100);"
                                    "CREATE TABLE tbl_1_n(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "INSERT INTO tbl_1_n VALUES('na','n','那',500);"
                                    "CREATE TABLE tbl_2_n(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "INSERT INTO tbl_2_n VALUES('na''bian','nb','那边',300);"
                                    "INSERT INTO tbl_2_n VALUES('na''bian','nb','哪边',200);"
                                    "CREATE TABLE tbl_1_j(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "INSERT INTO tbl_1_j VALUES('ji','j','机',500);"
                                    "CREATE TABLE tbl_2_j(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "INSERT INTO tbl_2_j VALUES('ji''ben','jb','基本',300);"
                                    "CREATE TABLE tbl_2_t(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "INSERT INTO tbl_2_t VALUES('te''xu','tx','特许',100);"
                                    "INSERT INTO tbl_2_t VALUES('tong''xue','tx','同学',200);"
                                    // 真实词库每张分表都在：调频按 t'xu'shan 去改 tbl_3_t 时不会因缺表而中途失败。
                                    "CREATE TABLE tbl_3_t(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "CREATE TABLE tbl_2_y(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "INSERT INTO tbl_2_y VALUES('yi''jian','yj','一件',100);"
                                    "CREATE TABLE tbl_2_z(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "INSERT INTO tbl_2_z VALUES('zuo''bi','zb','作弊',100);"
                                    "INSERT INTO tbl_2_z VALUES('zhe''li','zl','这里',300);"
                                    "INSERT INTO tbl_2_z VALUES('zhe''lei','zl','这类',200);"
                                    "INSERT INTO tbl_2_z VALUES('zhi''neng','zn','智能',300);"
                                    "INSERT INTO tbl_2_z VALUES('zhi''neng','zn','职能',200);"
                                    // dna 切成 d'na 的读法，覆盖整串：字母词只有补全出来的那几条。
                                    "CREATE TABLE tbl_2_d(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                                    "INSERT INTO tbl_2_d VALUES('da''na','dn','打那',100);"
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
            // 与 B超 同码、权重更低：整串命中 bchao、补全 bch 都有两条。
            "INSERT INTO latin_words VALUES('b''chao','B潮',1,0);"
            "INSERT INTO latin_words VALUES('u''pan','U盘',5,0);"
            // 补全词权重高过同前缀的整串命中词：upan 时整串命中的 U盘 仍在它前面。
            "INSERT INTO latin_words VALUES('u''pan''suo','U盘锁',20,0);"
            // 两段字母：第一段 e 打完就补，最后一段 e 要打到 erene。
            "INSERT INTO latin_words VALUES('e''ren''e''ben','e人e本',3,0);"
            // dna 补全：最重的 DNAS芯片 字母段 dnas 没打全，不占至多两条的名额；剩下三条只出最重的两条。
            "INSERT INTO latin_words VALUES('d''n''a''s''xin''pian','DNAS芯片',9,0);"
            "INSERT INTO latin_words VALUES('d''n''a''jian''ce','DNA检测',5,0);"
            "INSERT INTO latin_words VALUES('d''n''a''jian''ding','DNA鉴定',4,0);"
            "INSERT INTO latin_words VALUES('d''n''a''xu''lie','DNA序列',3,0);"
            "INSERT INTO latin_words VALUES('zhe''e','浙E',4,1);"
            "INSERT INTO latin_words VALUES('zhe''l','浙L',4,1);"
            "INSERT INTO latin_words VALUES('ji''b','吉B',4,1);"
            "INSERT INTO latin_words VALUES('n''a''b''l''a''suan''zi','Nabla算子',3,0);"
            "INSERT INTO latin_words VALUES('zhi''neng''i''c''ka','智能IC卡',3,0);"
            "COMMIT;");
}

// 蓝天辅码表（会话默认的辅码方案）里几个字的码。
void prepare_helpcodes(const std::filesystem::path &directory)
{
    std::filesystem::create_directories(directory / "helpcodes");
    std::ofstream(directory / "helpcodes" / "helpcode.txt", std::ios::binary)
        << "特=ua\n同=ub\n知=nb\n智=nk\n职=nz\n这=lw\n";
}

// Google 整句解码器的模型取源码树里那一份。
void prepare_google_model(const std::filesystem::path &directory)
{
    const auto model = metasequoia::path_from_utf8(METASEQUOIA_TEST_PINYIN_MODEL);
    if (!std::filesystem::exists(model))
        throw std::runtime_error("The Google decoder model is missing: " + metasequoia::path_to_utf8(model));
    std::filesystem::copy_file(model, directory / metasequoia::assets::pinyin_model,
                               std::filesystem::copy_options::overwrite_existing);
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
        require(latin_follows_leader(session, "特许", "T恤") && candidates[1].canonical_pinyin == "t'xu" &&
                    candidates[1].source == CandidateSource::Database,
                "txu must offer T恤 right after 特许, which covers the whole input, as a latin-word dictionary row.");
        require(find_index(session, "T恤衫") == 2 && candidates[2].latin_word,
                "Three letters must also complete to the longer latin word right after it.");
        require(find_index(session, "特许") < candidates.size(),
                "The ordinary abbreviated reading must stay among the candidates.");
    }
    {
        // tx 是常用简拼；只有两个字母时不补全，单个字母什么都不配。
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "tx");
        require(!has_latin_word(session) && first_is(session, "同学"),
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
        require(first_is(session, "U盘"), "A latin word starting with u must be offered on its own.");
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
        require(latin_follows_leader(session, "特许", "T恤"),
                "Xiaohe txu must offer T恤 right after 特许, which covers the whole input.");
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

// 打完 input 后，候选开头正好是 covering（覆盖整串输入的候选，按夹具权重手排），紧接着是字母词 latin；
// prefix_only 里每一个只覆盖输入的前一部分，都排在 latin 之后。
void expect_latin_after_full_input(const metasequoia::InputSession &session, const std::string &input,
                                   const std::vector<std::string> &covering, const std::string &latin,
                                   const std::vector<std::string> &prefix_only)
{
    const auto words = words_of(session);
    const std::string context = input + " / " + latin + ":" + listing(session);
    require(words.size() > covering.size() &&
                std::vector<std::string>(words.begin(), words.begin() + covering.size()) == covering,
            "The candidates covering the whole input must lead in their own order for " + context);
    const auto latin_index = covering.size();
    require(words.size() > latin_index && words[latin_index] == latin && session.candidates()[latin_index].latin_word,
            "The latin word must come right after the candidates covering the whole input for " + context);
    require(count_word(session, latin) == 1, "The latin word must be offered exactly once for " + context);
    for (const auto &word : prefix_only)
    {
        const auto index = find_index(session, word);
        require(index < words.size() && index > latin_index,
                "A candidate covering only a prefix of the input must stay after the latin word: " + word + " for " +
                    context);
    }
}

// 字母词排在开头那段覆盖整串输入的候选之后（MSIME-Windows #1370）：以声母结尾的输入没有键与输入
// 一字不差的词库行，字母词也不能压过覆盖了整串的普通词。
void test_latin_after_full_input(const metasequoia::RuntimePaths &paths)
{
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "zhel");
        expect_latin_after_full_input(session, "zhel", {"这里", "这类"}, "浙L", {"这"});
    }
    {
        // 打全字母段 nabla 之后的补全：没有普通词覆盖整串，补出的字母词排第一。
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "nablas");
        expect_latin_after_full_input(session, "nablas", {}, "Nabla算子", {});
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "jib");
        expect_latin_after_full_input(session, "jib", {"基本"}, "吉B", {"机"});
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "txu");
        expect_latin_after_full_input(session, "txu", {"特许"}, "T恤", {});
        const auto completion = find_index(session, "T恤衫");
        require(completion < session.candidates().size() && completion == find_index(session, "T恤") + 1 &&
                    session.candidates()[completion].latin_word,
                "The completed latin word must follow the exact one directly." + listing(session));
    }
    {
        // 词格对 t'xu 解出的整句就是 T恤：去掉那条整句，字母词只出一次，位置照旧在 特许 之后。
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        with_lattice(session);
        type(session, "txu");
        expect_latin_after_full_input(session, "txu", {"特许"}, "T恤", {});
        require(find_index(session, "T恤衫") < session.candidates().size() &&
                    find_index(session, "T恤衫") == find_index(session, "T恤") + 1,
                "The completed latin word must follow the exact one directly with the lattice on." + listing(session));
    }
    {
        // 词格整句覆盖整串输入，车牌排在它之后。
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        with_lattice(session);
        type(session, "zhee");
        expect_latin_after_full_input(session, "zhee", {"这鹅"}, "浙E", {"这"});
        require(session.candidates()[0].source == CandidateSource::Generated,
                "The leading candidate for zhee must be the lattice sentence." + listing(session));
    }
    {
        // 同组的几条字母词按权重排：整串命中 B超（6）在 B潮（1）前，补全同样如此。
        for (const char *input : {"bchao", "bch"})
        {
            metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
            type(session, input);
            const auto &candidates = session.candidates();
            require(candidates.size() > 1 && candidates[0].word == "B超" && candidates[0].latin_word &&
                        candidates[1].word == "B潮" && candidates[1].latin_word,
                    std::string("Latin words sharing a code must keep their weight order for ") + input + ":" +
                        listing(session));
        }
    }
    {
        // 没有普通词覆盖整串时字母词排第一；那边 只覆盖 na'b，排在后面。
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "nabla");
        expect_latin_after_full_input(session, "nabla", {}, "Nabla算子", {"那边", "那"});
    }

    // vin、vingic、vel 关掉双拼辅码再打：会话默认开着，奇数个键时末键当单码辅码（夹具没有辅码表也一样），
    // 那条路见 test_shuangpin_helpcodes。
    const auto &xiaohe = GetXiaoheShuangpinProfile();
    {
        // 小鹤 vi 是 zhi，vin 读作 zhi + n：还没打到 智能IC卡 的字母段 ic，不补全。
        metasequoia::InputSession session(SchemeType::Shuangpin, xiaohe, paths);
        session.set_shuangpin_helpcode_enabled(false);
        type(session, "vin");
        require(!has_latin_word(session) && first_is(session, "智能"),
                "xiaohe vin stops before the letters ic: no latin word." + listing(session));
    }
    {
        // 小鹤 智能 ving，打全字母段 ic 后补出 智能IC卡；智能 只覆盖前四个键。
        metasequoia::InputSession session(SchemeType::Shuangpin, xiaohe, paths);
        session.set_shuangpin_helpcode_enabled(false);
        type(session, "vingic");
        expect_latin_after_full_input(session, "xiaohe vingic", {}, "智能IC卡", {"智能"});
    }
    {
        // 小鹤 ve 是 zhe。
        metasequoia::InputSession session(SchemeType::Shuangpin, xiaohe, paths);
        session.set_shuangpin_helpcode_enabled(false);
        type(session, "vel");
        expect_latin_after_full_input(session, "xiaohe vel", {"这里", "这类"}, "浙L", {"这"});
    }
    {
        metasequoia::InputSession session(SchemeType::Shuangpin, xiaohe, paths);
        type(session, "nabla");
        expect_latin_after_full_input(session, "xiaohe nabla", {}, "Nabla算子", {"那"});
    }
}

// 补全门槛：输入打全词里第一段连续字母之后才补出整条词。Nabla算子 的字母段是 nabla，卡拉OK 的是 ok
// （在词尾，打全它就是整串命中，没有补全）。
void test_completion_gate(const metasequoia::RuntimePaths &paths)
{
    const auto quanpin = [&](const std::string &input) {
        auto session = std::make_unique<metasequoia::InputSession>(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(*session, input);
        return session;
    };
    {
        const auto session = quanpin("nab");
        require(first_is(*session, "那边") && find_index(*session, "Nabla算子") == session->candidates().size(),
                "nab covers only part of the letters nabla: Nabla算子 must not be completed." + listing(*session));
    }
    {
        const auto session = quanpin("nabl");
        require(find_index(*session, "Nabla算子") == session->candidates().size(),
                "nabl stops one letter short of nabla: Nabla算子 must not be completed." + listing(*session));
    }
    for (const char *input : {"nabla", "nablas", "nablasuan"})
    {
        const auto session = quanpin(input);
        const auto index = find_index(*session, "Nabla算子");
        require(index < session->candidates().size() && session->candidates()[index].latin_word &&
                    count_word(*session, "Nabla算子") == 1,
                std::string("Typing all of nabla must complete Nabla算子 once for ") + input + listing(*session));
    }
    for (const char *input : {"kal", "kala", "kalao"})
    {
        const auto session = quanpin(input);
        require(find_index(*session, "卡拉OK") == session->candidates().size(),
                std::string("卡拉OK's letters come last: no completion before kalaok for ") + input +
                    listing(*session));
    }
    {
        // 字母段在开头且只有一个字母：txu 盖住了 t，T恤衫 照样补出。
        const auto session = quanpin("txu");
        require(find_index(*session, "T恤衫") < session->candidates().size(),
                "txu covers the letter t: T恤衫 must still be completed." + listing(*session));
    }
    {
        // 补全词 U盘锁 权重 20 高过整串命中的 U盘 5，仍排在 U盘 后面。
        const auto session = quanpin("upan");
        const auto exact = find_index(*session, "U盘");
        const auto completion = find_index(*session, "U盘锁");
        require(exact < session->candidates().size() && completion == exact + 1 &&
                    session->candidates()[completion].latin_word,
                "The exact latin word must stay ahead of a heavier completion." + listing(*session));
    }

    const auto &xiaohe = GetXiaoheShuangpinProfile();
    const auto shuangpin = [&](const std::string &input) {
        auto session = std::make_unique<metasequoia::InputSession>(SchemeType::Shuangpin, xiaohe, paths);
        session->set_shuangpin_helpcode_enabled(false);
        type(*session, input);
        return session;
    };
    for (const char *input : {"nab", "nabl"})
    {
        const auto session = shuangpin(input);
        require(find_index(*session, "Nabla算子") == session->candidates().size(),
                std::string("Xiaohe ") + input + " must not complete Nabla算子." + listing(*session));
    }
    for (const char *input : {"nabla", "nablasr"})
    {
        const auto session = shuangpin(input);
        require(find_index(*session, "Nabla算子") < session->candidates().size(),
                std::string("Xiaohe ") + input + " covers nabla and must complete Nabla算子." + listing(*session));
    }
    // 智能IC卡 的字母段 ic 前面有两个汉字，各占两个键：ving、vingi 都还没打到 ic。
    for (const char *input : {"ving", "vingi"})
    {
        const auto session = shuangpin(input);
        require(find_index(*session, "智能IC卡") == session->candidates().size(),
                std::string("Xiaohe ") + input + " stops before the letters ic: 智能IC卡 must not be completed." +
                    listing(*session));
    }
}

// 门槛按词里第一段字母算，补全至多两条，被门槛挡掉的词不占名额。
void test_completion_gate_shapes(const metasequoia::RuntimePaths &paths)
{
    {
        // e人e本：打完第一段 e 就够门槛，eren 还没打到第二段 e，照样补出。
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "eren");
        const auto index = find_index(session, "e人e本");
        require(index < session.candidates().size() && session.candidates()[index].latin_word,
                "eren covers the first letter run e: e人e本 must be completed." + listing(session));
    }
    {
        // dna 盖住了 DNA检测、DNA鉴定、DNA序列 的字母段，没盖住最重的 DNAS芯片 的 dnas。
        // 打那 覆盖整串，补出来的字母词排在它后面，按权重取前两条。
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "dna");
        expect_latin_after_full_input(session, "dna", {"打那"}, "DNA检测", {});
        const auto second = find_index(session, "DNA鉴定");
        require(second < session.candidates().size() && second == find_index(session, "DNA检测") + 1,
                "The second completion must follow the first." + listing(session));
        require(find_index(session, "DNAS芯片") == session.candidates().size(),
                "DNAS芯片's letters dnas are not all typed: it must not be completed." + listing(session));
        require(find_index(session, "DNA序列") == session.candidates().size(),
                "At most two latin words are completed." + listing(session));
    }
}

// 小鹤 vin、vel 开着辅码（会话默认开）且有辅码表：末键 n、l 当单码辅码，辅码筛出的 知、这 排到最前，
// 它们的 pinyin 只到 vi、ve，可选中后连辅码一起吃掉整串，同样算覆盖整串。
void test_shuangpin_helpcodes(const metasequoia::RuntimePaths &paths)
{
    const auto &xiaohe = GetXiaoheShuangpinProfile();
    {
        // vin 没打到 智能IC卡 的字母段，没有字母词；辅码筛出的 知 照样吃掉整串。
        metasequoia::InputSession session(SchemeType::Shuangpin, xiaohe, paths);
        type(session, "vin");
        const auto words = words_of(session);
        require(!has_latin_word(session) && words.size() >= 4 &&
                    std::vector<std::string>(words.begin(), words.begin() + 4) ==
                        std::vector<std::string>{"知", "智能", "职能", "知"},
                "xiaohe vin with helpcodes must keep the helpcode order 知 智能 职能 知 and offer no latin word." +
                    listing(session));
        require(session.select_candidate(find_index(session, "知")).commit == "知" && !session.has_composition(),
                "The helpcode-filtered candidate must consume the whole input when selected.");
    }
    {
        metasequoia::InputSession session(SchemeType::Shuangpin, xiaohe, paths);
        type(session, "vel");
        expect_latin_after_full_input(session, "xiaohe vel with helpcodes", {"这", "这里", "这类", "这"}, "浙L", {});
    }
}

// 双拼开着 Google 整句：小鹤 vingic 解出的整句排在最前，它也覆盖整串，字母词排在它之后；智能 只覆盖 ving。
void test_shuangpin_google_sentence(const metasequoia::RuntimePaths &paths)
{
    metasequoia::InputSession session(SchemeType::Shuangpin, GetXiaoheShuangpinProfile(), paths);
    session.set_shuangpin_helpcode_enabled(false);
    SentenceAssociationOptions association;
    association.google = true;
    session.set_sentence_association(association);
    type(session, "vingic");
    const auto &candidates = session.candidates();
    const auto sentence =
        std::find_if(candidates.begin(), candidates.end(), [](const WordItem &item) { return !item.latin_word; });
    require(
        sentence != candidates.end() && sentence->source == CandidateSource::Fallback && sentence->sentence_association,
        "The Google decoder must lead the ordinary candidates of xiaohe vingic with a sentence." + listing(session));
    expect_latin_after_full_input(session, "xiaohe vingic with Google sentences", {sentence->word}, "智能IC卡",
                                  {"智能"});
}

// 普通候选彼此的先后与没有字母词的词库逐项相同：字母词只是插进去，不挪动别的候选。
// 打不出字母词的输入，整张候选列表都相同。
void test_ordinary_order_unchanged(const metasequoia::RuntimePaths &paths, const metasequoia::RuntimePaths &old_paths)
{
    const auto compare = [](metasequoia::InputSession &current, metasequoia::InputSession &old,
                            const std::string &input, bool lattice, bool expect_latin) {
        if (lattice)
        {
            with_lattice(current);
            with_lattice(old);
        }
        type(current, input);
        type(old, input);
        const std::string context =
            input + (lattice ? " (lattice)" : "") + ":" + listing(current) + " vs" + listing(old);
        require(!words_of(old).empty(), "The comparison input must have candidates: " + context);
        require(has_latin_word(current) == expect_latin, "Unexpected latin-word presence for " + context);
        require(ordinary_words_of(current) == words_of(old),
                "Ordinary candidates must keep the order of a dictionary without latin words for " + context);
    };
    for (const auto &[input, lattice, expect_latin] : std::vector<std::tuple<std::string, bool, bool>>{
             {"zhel", false, true},
             {"nab", false, false},
             {"jib", false, true},
             {"txu", false, true},
             {"txu", true, true},
             {"nabla", false, true},
             {"tongxue", false, false},
             {"tx", false, false},
             {"zhe", false, false},
             {"maiyijian", true, false},
             {"zuobi", true, false},
         })
    {
        metasequoia::InputSession current(SchemeType::Quanpin, 0u, true, true, true, paths);
        metasequoia::InputSession old(SchemeType::Quanpin, 0u, true, true, true, old_paths);
        compare(current, old, input, lattice, expect_latin);
    }
    const auto &xiaohe = GetXiaoheShuangpinProfile();
    for (const auto &[input, expect_latin] : std::vector<std::pair<std::string, bool>>{
             {"vin", false},
             {"vingic", true},
             {"vel", true},
             {"txu", true},
             {"vi", false},
             {"nabm", false},
             {"veli", false},
         })
    {
        metasequoia::InputSession current(SchemeType::Shuangpin, xiaohe, paths);
        metasequoia::InputSession old(SchemeType::Shuangpin, xiaohe, old_paths);
        compare(current, old, input, false, expect_latin);
    }
}

// 设置页导入、新增字母词时读音的各种写法（#1340 后续：用户字母词）。
void test_parse_entries()
{
    const auto expect = [](const char *word, const char *reading, const char *key) {
        std::string error;
        const auto entry = quanpin::parse_latin_entry(word, reading, error);
        if (!entry || entry->key != key || entry->value != word)
            throw std::runtime_error(std::string("Latin entry ") + word + " / " + reading + " should read as " + key +
                                     ": " + (entry ? entry->key : error));
    };
    expect("T恤", "t'xu", "t'xu");
    expect("T恤", "T xu", "t'xu");
    expect("T恤", "txu", "t'xu");
    expect("T恤", "T xu4", "t'xu");
    expect("卡拉OK", "kǎ'lā'OK", "ka'la'o'k");
    expect("卡拉OK", "ka la o k", "ka'la'o'k");
    expect("卡拉OK", "kalaok", "ka'la'o'k");
    expect("PPT模板", "PPT mo ban", "p'p't'mo'ban");
    expect("A方案", "a fang'an", "a'fang'an");
    expect("X光", "x guāng", "x'guang");
    expect("绿V", "lǜ V", "lv'v");

    const auto reject = [](const char *word, const char *reading, const char *why) {
        std::string error;
        if (quanpin::parse_latin_entry(word, reading, error) || error.empty())
            throw std::runtime_error(std::string("Latin entry ") + word + " / " + reading +
                                     " must be rejected: " + why);
    };
    reject("T恤", "te'xu", "the letter T does not match te");
    reject("T恤", "t'x'u", "a separator splits the syllable xu");
    reject("T恤", "", "no reading");
    reject("T恤3", "txu", "digits are not part of latin words");
    reject("TX", "tx", "no Han character");
    reject("恤", "xu", "no letter");
    reject("T 恤", "txu", "spaces are not part of latin words");
    reject("A方案", "afangan", "fang'an and fan'gan both fit");

    std::string error;
    const auto plate = quanpin::parse_latin_entry("鄂A", "e A", error);
    require(plate && plate->key == "e'a" && plate->exact_only, "A license plate must be marked exact-only.");
    const auto annotated = quanpin::make_latin_entry("T恤衫", {"xu", "shan"}, error);
    require(annotated && annotated->key == "t'xu'shan" && !annotated->exact_only,
            "Annotated syllables must fill the Han characters in order.");
    require(!quanpin::make_latin_entry("T恤衫", {"xu"}, error), "A short annotation must be rejected.");
    require(quanpin::latin_word_han_runs("A股B股衫") == std::vector<std::string>{"股", "股衫"},
            "Han runs must be split at letters.");
}

std::string utf8_path(const std::filesystem::path &path)
{
    return metasequoia::path_to_utf8(path);
}

std::vector<user_dictionary::UserLatinWord> words_from(const std::vector<std::pair<const char *, const char *>> &rows)
{
    std::vector<user_dictionary::UserLatinWord> words;
    for (const auto &[word, reading] : rows)
    {
        std::string error;
        const auto entry = quanpin::parse_latin_entry(word, reading, error);
        if (!entry)
            throw std::runtime_error("Fixture latin word did not parse: " + error);
        words.push_back({entry->key, entry->value, 100, entry->exact_only});
    }
    return words;
}

void retype(metasequoia::InputSession &session, const std::string &text)
{
    session.handle_command(metasequoia::Command::Cancel);
    type(session, text);
}

// 用户加的字母词：已开着的会话里下一次按键就能打出来（全拼、双拼），出厂词不能改删，改名、删除生效。
void test_user_words(const std::filesystem::path &directory)
{
    const auto paths = paths_for(directory);
    const auto main = utf8_path(directory / "msime.db");
    const auto journal = utf8_path(paths.user(metasequoia::assets::user_journal));
    const auto &xiaohe = GetXiaoheShuangpinProfile();
    metasequoia::InputSession quanpin_session(SchemeType::Quanpin, 0u, true, true, true, paths);
    metasequoia::InputSession shuangpin_session(SchemeType::Shuangpin, xiaohe, paths);
    with_lattice(quanpin_session);
    type(quanpin_session, "xguang");
    type(shuangpin_session, "xgl");
    require(find_index(quanpin_session, "X光") == quanpin_session.candidates().size() &&
                find_index(shuangpin_session, "X光") == shuangpin_session.candidates().size(),
            "X光 must not exist before it is added.");
    // 先让词格把 a'gu 这个跨度（还没有 A股）记进跨度缓存，加词后换一串没打过的输入再用到它。
    retype(quanpin_session, "maiagu");

    const auto added = user_dictionary::add_user_latin_words(
        main, journal, words_from({{"X光", "X guang"}, {"T恤", "t'xu"}, {"A股", "A gu"}}));
    require(added == std::vector<user_dictionary::LatinWordWrite>{user_dictionary::LatinWordWrite::Written,
                                                                  user_dictionary::LatinWordWrite::Exists,
                                                                  user_dictionary::LatinWordWrite::Written},
            "A new latin word must be written and a shipped one reported as existing.");
    retype(quanpin_session, "zuoagu");
    require(find_index(quanpin_session, "做A股") < quanpin_session.candidates().size(),
            "A sentence through a span cached before the word was added must see the new word.");

    retype(quanpin_session, "xguang");
    retype(shuangpin_session, "xgl");
    require(first_is(quanpin_session, "X光") && quanpin_session.candidates()[0].latin_word,
            "A latin word added while the quanpin session is open must show on the next key.");
    require(first_is(shuangpin_session, "X光"),
            "A latin word added while the shuangpin session is open must show on the next key.");
    retype(quanpin_session, "txu");
    require(latin_follows_leader(quanpin_session, "特许", "T恤"), "Shipped latin words must stay after a reload.");
    retype(quanpin_session, "maiyijianxguang");
    require(!quanpin_session.candidates().empty() && quanpin_session.candidates()[0].word == "买一件X光",
            "A user latin word must also join sentences.");

    using user_dictionary::LatinWordWrite;
    require(user_dictionary::delete_user_latin_word(main, journal, "t'xu", "T恤") == LatinWordWrite::Missing,
            "Shipped latin words must not be deletable.");
    require(user_dictionary::update_user_latin_word(main, journal, "x'guang", "X光", {"t'xu", "T恤", 1, false}) ==
                LatinWordWrite::Exists,
            "Renaming onto an existing latin word must be refused.");
    const auto renamed = words_from({{"X线", "X xian"}}).front();
    require(user_dictionary::update_user_latin_word(main, journal, "x'guang", "X光", renamed) ==
                LatinWordWrite::Written,
            "A user latin word must be renamable.");
    const auto stored = user_dictionary::user_latin_words(journal);
    require(stored.size() == 2 && stored[0].value == "A股" && stored[1].value == "X线" && stored[1].key == "x'xian",
            "The journal must follow the rename.");
    retype(quanpin_session, "xguang");
    require(find_index(quanpin_session, "X光") == quanpin_session.candidates().size(),
            "The old name must be gone after a rename.");
    retype(quanpin_session, "xxian");
    require(first_is(quanpin_session, "X线"), "The new name must show after a rename.");

    require(user_dictionary::add_user_latin_words(main, journal, words_from({{"鄂A", "e A"}})).front() ==
                LatinWordWrite::Written,
            "A user license plate must be accepted.");
    retype(quanpin_session, "ea");
    require(find_index(quanpin_session, "鄂A") < quanpin_session.candidates().size(),
            "A user license plate must show on an exact match.");
    require(user_dictionary::delete_user_latin_word(main, journal, "e'a", "鄂A") == LatinWordWrite::Written,
            "A user latin word must be deletable.");
    retype(quanpin_session, "ea");
    require(find_index(quanpin_session, "鄂A") == quanpin_session.candidates().size(),
            "A deleted user latin word must disappear on the next key.");
}

// 换词库（安装、升级）后回放：用户字母词写回新词库，词库是还没有 latin_words 的旧版本时补建表，
// 早期没有 exact_only 列的表补上列。
void test_replay(const std::filesystem::path &root, const std::filesystem::path &journal_directory)
{
    const auto journal = utf8_path(paths_for(journal_directory).user(metasequoia::assets::user_journal));
    for (const char *name : {"replay-without-table", "replay-with-table", "replay-old-columns"})
    {
        const auto directory = root / name;
        prepare_dictionary(directory, std::string(name) == "replay-with-table");
        if (std::string(name) == "replay-old-columns")
            execute(directory / "msime.db", "CREATE TABLE latin_words(key TEXT, value TEXT, weight INTEGER);"
                                            "INSERT INTO latin_words VALUES('t''xu','T恤',10);");
        const auto result =
            user_dictionary::replay(journal, utf8_path(directory / "msime.db"), utf8_path(directory / "english.db"));
        if (!result.error.empty() || result.failed != 0)
            throw std::runtime_error(std::string("Replaying user latin words failed on ") + name + ": " + result.error);
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths_for(directory));
        type(session, "xxian");
        if (!first_is(session, "X线"))
            throw std::runtime_error(std::string("A replayed user latin word must show on ") + name);
        if (std::string(name) != "replay-without-table")
        {
            session.handle_command(metasequoia::Command::Cancel);
            type(session, "txu");
            if (!latin_follows_leader(session, "特许", "T恤"))
                throw std::runtime_error(std::string("Replay must keep the shipped latin words on ") + name);
        }
    }
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
        test_latin_after_full_input(paths);
        test_completion_gate(paths);
        test_completion_gate_shapes(paths);
        test_ordinary_order_unchanged(paths, paths_for(old));
        const auto helpcoded = root / "helpcoded";
        prepare_dictionary(helpcoded, true);
        prepare_helpcodes(helpcoded);
        test_shuangpin_helpcodes(paths_for(helpcoded));
        const auto google = root / "google";
        prepare_dictionary(google, true);
        prepare_google_model(google);
        test_shuangpin_google_sentence(paths_for(google));
        test_parse_entries();
        const auto user = root / "user";
        prepare_dictionary(user, true);
        test_user_words(user);
        test_replay(root, user);
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
