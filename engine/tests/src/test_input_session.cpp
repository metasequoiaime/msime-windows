#include "../../core/input_session.h"
#include "../../common/helpcode_utils.h"
#include "../../core/data_path.h"
#include "../../user_dictionary/user_dictionary_journal.h"
#include "test_directory_cleanup.h"
#include "../../contracts/dictionary/format.h"
#include "../../quanpin/quanpin_query.h"
#include "../../quanpin/quanpin_utils.h"
#include "../../quanpin/quanpin_dictionary.h"
#include "../../schemes/quanpin_scheme.h"
#include "../../shuangpin/shuangpin_profile.h"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{
class Database
{
  public:
    explicit Database(const std::filesystem::path &path)
    {
        if (sqlite3_open(metasequoia::path_to_utf8(path).c_str(), &database_) != SQLITE_OK)
        {
            throw std::runtime_error("Failed to create the input-session test dictionary.");
        }
    }

    ~Database()
    {
        sqlite3_close(database_);
    }

    void execute(const char *sql)
    {
        char *error = nullptr;
        if (sqlite3_exec(database_, sql, nullptr, nullptr, &error) != SQLITE_OK)
        {
            const std::string message = error == nullptr ? "SQLite operation failed." : error;
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

    std::int64_t query_integer(const char *sql)
    {
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
            sqlite3_step(statement) != SQLITE_ROW)
        {
            sqlite3_finalize(statement);
            throw std::runtime_error("Failed to query the input-session test dictionary.");
        }
        const std::int64_t value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return value;
    }

  private:
    sqlite3 *database_ = nullptr;
};

void type(metasequoia::InputSession &session, const std::string &text)
{
    for (const char character : text)
    {
        if (!session.handle_character(character).handled)
        {
            throw std::runtime_error("A pinyin character was not handled.");
        }
    }
}

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void write_file(const std::filesystem::path &path, const std::string &contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path);
    stream << contents;
    if (!stream)
    {
        throw std::runtime_error("Failed to prepare an input-session helpcode fixture.");
    }
}

void set_data_directory(const std::filesystem::path &directory)
{
#ifdef _WIN32
    if (_wputenv_s(L"METASEQUOIA_IME_DATA_DIR", directory.c_str()) != 0)
#else
    if (setenv("METASEQUOIA_IME_DATA_DIR", metasequoia::path_to_utf8(directory).c_str(), 1) != 0)
#endif
    {
        throw std::runtime_error("Failed to set the data directory override.");
    }
}

void prepare_frequency_fixture(const std::filesystem::path &directory)
{
    std::filesystem::create_directories(directory);
    Database database(directory / "msime.db");
    database.execute("BEGIN;"
                     "CREATE TABLE tbl_1_n(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                     "INSERT INTO tbl_1_n VALUES('ni', 'n', '甲', 100);"
                     "INSERT INTO tbl_1_n VALUES('ni', 'n', '乙', 90);"
                     "INSERT INTO tbl_1_n VALUES('ni', 'n', '丙', 80);"
                     "INSERT INTO tbl_1_n VALUES('ni', 'n', '丁', 70);"
                     "INSERT INTO tbl_1_n VALUES('ni', 'n', '戊', 60);"
                     "INSERT INTO tbl_1_n VALUES('ni', 'n', '己', 50);"
                     "COMMIT;");
}

void prepare_shuangpin_frequency_fixture(const std::filesystem::path &directory)
{
    std::filesystem::create_directories(directory);
    Database database(directory / "msime.db");
    database.execute("BEGIN;"
                     "CREATE TABLE tbl_2_n(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                     "INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', '你好', 100);"
                     "INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', '拟好', 50);"
                     "COMMIT;");
}

void prepare_wubi_frequency_fixture(const std::filesystem::path &directory)
{
    std::filesystem::create_directories(directory);
    Database database(directory / "msime.db");
    database.execute("BEGIN;"
                     "CREATE TABLE wubi86(key TEXT, value TEXT, weight INTEGER);"
                     "INSERT INTO wubi86 VALUES('aaaa', '工', 100);"
                     "INSERT INTO wubi86 VALUES('aaaa', '或', 50);"
                     "COMMIT;");
}

std::size_t candidate_index(const metasequoia::InputSession &session, const std::string &word)
{
    const auto found = std::find_if(session.candidates().begin(), session.candidates().end(),
                                    [&](const WordItem &item) { return item.word == word; });
    if (found == session.candidates().end())
    {
        std::string message = "The expected edge-selection candidate was not produced: " + word + "; actual:";
        for (const auto &candidate : session.candidates())
        {
            message += " [" + candidate.word + "]";
        }
        throw std::runtime_error(message);
    }
    return static_cast<std::size_t>(std::distance(session.candidates().begin(), found));
}

bool same_candidate_words(const metasequoia::InputSession &left, const metasequoia::InputSession &right)
{
    if (left.candidates().size() != right.candidates().size())
    {
        return false;
    }
    return std::equal(left.candidates().begin(), left.candidates().end(), right.candidates().begin(),
                      [](const auto &left_item, const auto &right_item) { return left_item.word == right_item.word; });
}

// 同 candidate_index，但未命中返回 candidates().size() 而不是拖出：供「不得出现」断言用。
std::size_t find_candidate_index(const metasequoia::InputSession &session, const std::string &word)
{
    const auto found = std::find_if(session.candidates().begin(), session.candidates().end(),
                                    [&](const WordItem &item) { return item.word == word; });
    return static_cast<std::size_t>(std::distance(session.candidates().begin(), found));
}

// ü 系拼写别名归一与轻标记的会话级回归（AC1–AC6）。自建隔离词库，不依赖主 fixture：
// 词库正键全部用标准拼写，另放真实音节 nu/lu 供隔离断言。打标与开关无关，掩码取
// both 仅代表真实前端配置。
void run_umlaut_alias_session_tests(const std::filesystem::path &data_directory)
{
    const std::filesystem::path directory = data_directory / "umlaut-alias";
    std::filesystem::create_directories(directory);
    {
        Database database(directory / "msime.db");
        database.execute("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_n VALUES('nve', 'n', '虐', 100);"
                         "INSERT INTO tbl_1_n VALUES('nv', 'n', '女', 90);"
                         "INSERT INTO tbl_1_n VALUES('nu', 'n', '怒', 80);");
        database.execute("CREATE TABLE tbl_1_l(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_l VALUES('lve', 'l', '略', 100);"
                         "INSERT INTO tbl_1_l VALUES('lv', 'l', '绿', 90);"
                         "INSERT INTO tbl_1_l VALUES('lu', 'l', '路', 80);");
        database.execute("CREATE TABLE tbl_1_j(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_j VALUES('jue', 'j', '决', 100);"
                         "INSERT INTO tbl_1_j VALUES('juan', 'j', '捐', 100);"
                         "INSERT INTO tbl_1_j VALUES('jun', 'j', '军', 100);");
        database.execute("CREATE TABLE tbl_1_q(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_q VALUES('quan', 'q', '全', 100);"
                         "INSERT INTO tbl_1_q VALUES('qun', 'q', '群', 100);");
        database.execute("CREATE TABLE tbl_1_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_x VALUES('xu', 'x', '需', 100);"
                         "INSERT INTO tbl_1_x VALUES('xuan', 'x', '选', 100);"
                         "INSERT INTO tbl_1_x VALUES('xun', 'x', '巡', 100);");
        database.execute("CREATE TABLE tbl_1_y(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_y VALUES('yuan', 'y', '原', 100);"
                         "INSERT INTO tbl_1_y VALUES('yun', 'y', '云', 100);");
        database.execute("CREATE TABLE tbl_2_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_x VALUES('xu''an', 'xa', '虚案', 100);");
        database.execute("CREATE TABLE tbl_1_e(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_e VALUES('e', 'e', '鹅', 50);");
        database.execute("CREATE TABLE tbl_2_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_n VALUES('nu''e', 'ne', '怒鹅', 30);");
    }

    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;

    // jqxy + van/vn are complete syllables, regardless of the autocorrection setting
    const struct
    {
        const char *alias;
        const char *canonical;
        const char *word;
    } aliases[] = {{"xvan", "xuan", "选"}, {"xvn", "xun", "巡"}, {"jvan", "juan", "捐"}, {"jvn", "jun", "军"},
                   {"qvan", "quan", "全"}, {"qvn", "qun", "群"}, {"yvan", "yuan", "原"}, {"yvn", "yun", "云"}};
    for (const auto &test : aliases)
    {
        for (const auto mask : {0u, both})
        {
            metasequoia::InputSession session(SchemeType::Quanpin, mask, true, true, false, paths);
            type(session, test.alias);
            require(session.get_pinyin_segmentation_with_cases() == test.alias && session.preedit() == test.alias,
                    "An umlaut alias must stay a single syllable with the original preedit.");
            const auto index = candidate_index(session, test.word);
            const auto &item = session.candidates()[index];
            require(item.pinyin == test.canonical && item.corrected_from == test.alias &&
                        item.source == CandidateSource::Database,
                    "An umlaut alias must resolve to the canonical dictionary entry.");
            require(session.select_candidate(index).commit == test.word && !session.has_composition(),
                    "Selecting an umlaut alias must consume the whole syllable.");
        }
        require(quanpin::to_google_spelling(test.alias) == test.canonical,
                "Decoder spelling must canonicalize complete umlaut aliases.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, false, paths);
        type(session, "xv'an");
        require(session.preedit() == "xv'an" && find_candidate_index(session, "选") == session.candidates().size(),
                "An explicit separator must not be merged into an umlaut syllable.");
        require(session.select_candidate(candidate_index(session, "虚案")).commit == "虚案",
                "Explicitly separated umlaut syllables must keep their dictionary reading.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, false, paths);
        SentenceAssociationOptions association;
        association.word_lattice = true;
        session.set_sentence_association(association);
        type(session, "xvanqvn");
        const auto index = candidate_index(session, "选群");
        require(session.candidates()[index].source == CandidateSource::Generated,
                "The word lattice must compose canonical entries from umlaut aliases.");
        require(session.select_candidate(index).commit == "选群" && !session.has_composition(),
                "A composed umlaut phrase must consume both syllables.");
        require(quanpin::to_google_spelling("xvan'qvn") == "xuan'qun",
                "Decoder spelling must preserve syllable separators.");
    }

    // AC1 + preedit 非回归：nue 命中 nve 行并带标记，preedit 仍画原样字母。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "nue");
        require(session.get_pinyin_segmentation_with_cases() == "nue",
                "The preedit for 'nue' must keep the typed letters, not the alias rewrite.");
        require(session.preedit() == "nue", "The engine preedit for 'nue' must stay unrewritten.");
        const auto found = find_candidate_index(session, "虐");
        require(found < session.candidates().size() && session.candidates()[found].corrected_from == "nue",
                "The 'nue' candidate for 虐 must carry corrected_from='nue'.");
        require(found < session.candidates().size() && session.candidates()[found].pinyin == "nve",
                "The 'nue' candidate must carry the canonical pinyin 'nve'.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "nve");
        const auto found = find_candidate_index(session, "虐");
        require(found < session.candidates().size() && session.candidates()[found].corrected_from.empty(),
                "The standard spelling 'nve' must stay unmarked.");
    }

    // AC2：lue/lve 同理。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "lue");
        const auto found = find_candidate_index(session, "略");
        require(found < session.candidates().size() && session.candidates()[found].corrected_from == "lue",
                "The 'lue' candidate for 略 must carry corrected_from='lue'.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "lve");
        const auto found = find_candidate_index(session, "略");
        require(found < session.candidates().size() && session.candidates()[found].corrected_from.empty(),
                "The standard spelling 'lve' must stay unmarked.");
    }

    // R2 例外：纠错全关时 ü 归一（非手误）仍然生效——nue -> nve、lve 直接命中。
    // 只断言可达（nue 另有 nu'e 读法与它竞争，谁领衔由词频裁决，与开关无关）。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "nue");
        require(find_candidate_index(session, "虐") < session.candidates().size(),
                "The ü alias rewrite must stay active for 'nue' with autocorrection off.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "lve");
        require(find_candidate_index(session, "略") < session.candidates().size(),
                "The standard 'lve' spelling must stay reachable with autocorrection off.");
    }

    // AC3：jqxy 系既有归一从无标变带标（行为变更），标准拼法无标。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "jve");
        require(session.get_pinyin_segmentation_with_cases() == "jve",
                "The preedit for 'jve' must keep the typed letters.");
        const auto found = find_candidate_index(session, "决");
        require(found < session.candidates().size() && session.candidates()[found].corrected_from == "jve",
                "The 'jve' candidate for 决 must carry corrected_from='jve'.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "jue");
        const auto found = find_candidate_index(session, "决");
        require(found < session.candidates().size() && session.candidates()[found].corrected_from.empty(),
                "The standard spelling 'jue' must stay unmarked.");
    }

    // AC4：nu/nv/lu/lv 真实音节互不串，标准拼法均无标记。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "nu");
        require(find_candidate_index(session, "怒") < session.candidates().size() &&
                    find_candidate_index(session, "女") == session.candidates().size() &&
                    find_candidate_index(session, "虐") == session.candidates().size(),
                "'nu' must only offer 怒, never 女/虐.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "nv");
        const auto found = find_candidate_index(session, "女");
        require(found < session.candidates().size() &&
                    find_candidate_index(session, "怒") == session.candidates().size() &&
                    find_candidate_index(session, "虐") == session.candidates().size() &&
                    session.candidates()[found].corrected_from.empty(),
                "'nv' must only offer unmarked 女, never 怒/虐.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "lu");
        require(find_candidate_index(session, "路") < session.candidates().size() &&
                    find_candidate_index(session, "绿") == session.candidates().size() &&
                    find_candidate_index(session, "略") == session.candidates().size(),
                "'lu' must only offer 路, never 绿/略.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "lv");
        const auto found = find_candidate_index(session, "绿");
        require(found < session.candidates().size() &&
                    find_candidate_index(session, "路") == session.candidates().size() &&
                    find_candidate_index(session, "略") == session.candidates().size() &&
                    session.candidates()[found].corrected_from.empty(),
                "'lv' must only offer unmarked 绿, never 路/略.");
    }

    // AC5：手动分隔符 nu'e 切分为 怒+鹅，不触发别名（虐不可见），无标记。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "nu'e");
        require(find_candidate_index(session, "怒鹅") < session.candidates().size() &&
                    find_candidate_index(session, "怒") < session.candidates().size() &&
                    find_candidate_index(session, "虐") == session.candidates().size(),
                "'nu'e' must split as 怒+鹅 and never trigger the nue alias.");
        require(std::none_of(session.candidates().begin(), session.candidates().end(),
                             [](const WordItem &item) { return !item.corrected_from.empty(); }),
                "'nu'e' must produce no marked candidates.");
    }

    // AC6：别名命中的候选上屏后，调频数据落在标准拼法键 nve 上。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "nue");
        (void)session.select_candidate(find_candidate_index(session, "虐"));
        require(!session.has_composition(), "Selecting the only candidate must finish the composition.");
        Database database(directory / "msime.db");
        require(database.query_integer("SELECT weight FROM tbl_1_n WHERE key='nve' AND value='虐'") == 101,
                "Selecting 虐 from 'nue' must update the canonical 'nve' row.");
        require(database.query_integer("SELECT COUNT(*) FROM tbl_1_n WHERE key='nue'") == 0,
                "No user data may accumulate under the alias key 'nue'.");
    }

    std::filesystem::remove_all(directory);
}

void run_autocorrect_mixed_jianpin_tests(const std::filesystem::path &data_directory)
{
    const auto directory = data_directory / "autocorrect-mixed-jianpin";
    std::filesystem::create_directories(directory);
    {
        Database database(directory / "msime.db");
        database.execute("CREATE TABLE tbl_2_y(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_y VALUES('ying''wen', 'yw', '英文', 100);"
                         "INSERT INTO tbl_2_y VALUES('yu''wen', 'yw', '语文', 90);"
                         "CREATE TABLE tbl_1_w(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_w VALUES('wen', 'w', '问', 1000);"
                         "CREATE TABLE tbl_2_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_x VALUES('xue''sheng', 'xs', '学生', 100);"
                         "CREATE TABLE tbl_1_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_s VALUES('sheng', 's', '生', 1000);"
                         "INSERT INTO tbl_1_s VALUES('shang', 's', '上', 1000);"
                         "CREATE TABLE tbl_2_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_s VALUES('shi''shang', 'ss', '世上', 100);"
                         "CREATE TABLE tbl_3_y(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_3_y VALUES('ying''wen''hua', 'ywh', '英文化', 100);"
                         "CREATE TABLE tbl_2_w(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_w VALUES('wei''wen', 'ww', '慰问', 100);"
                         "INSERT INTO tbl_2_w VALUES('wen''hua', 'wh', '文化', 1000);");
    }
    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;
    struct Case
    {
        const char *input;
        const char *word;
        const char *segmentation;
    };
    const std::array cases{Case{"ywen", "英文", "y'wen"}, Case{"xsheng", "学生", "x'sheng"},
                           Case{"ywenhua", "英文化", "y'wen'hua"}, Case{"sshang", "世上", "s'shang"},
                           Case{"wwen", "慰问", "w'wen"}};
    for (const unsigned mask : {0u, quanpin::kAutocorrectTransposition, quanpin::kAutocorrectNeighbor,
                                quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor,
                                quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor |
                                    quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion})
    {
        for (const auto &test : cases)
        {
            metasequoia::InputSession session(SchemeType::Quanpin, mask, true, true, false, paths);
            type(session, test.input);
            require(candidate_index(session, test.word) == 0,
                    "Autocorrection must not displace a mixed-jianpin candidate.");
            require(session.candidates().front().corrected_from.empty(),
                    "A correctly typed mixed-jianpin candidate must not be marked as corrected.");
            require(session.get_pinyin_segmentation_with_cases() == test.segmentation,
                    "The mixed-jianpin preedit must keep the typed syllable boundaries.");
            if (std::string(test.input) == "ywen")
            {
                require(find_candidate_index(session, "语文") < session.candidates().size(),
                        "Autocorrection must retain mixed-jianpin alternatives.");
            }
            require(session.select_candidate(0).commit == test.word && session.preedit().empty(),
                    "Selecting a mixed-jianpin word must consume the original input.");
        }
    }
    for (const char *typo : {"sahng", "shabg", "shng", "bqng", "hau", "gau", "uanli"})
    {
        require(!quanpin::looks_like_jianpin_spelling(typo),
                "Malformed spellings must still be eligible for autocorrection.");
    }
}

void run_literal_mixed_pinyin_tests(const std::filesystem::path &data_directory)
{
    const auto directory = data_directory / "literal-mixed-pinyin";
    std::filesystem::create_directories(directory);
    {
        Database database(directory / "msime.db");
        database.execute("CREATE TABLE tbl_3_w(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_3_w VALUES('wei''shen''me','wsm','为什么',100);"
                         "CREATE TABLE tbl_2_w(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_w VALUES('wei''min','wm','为民',100000);"
                         "CREATE TABLE tbl_1_w(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_w VALUES('wei','w','为',100000);"
                         "CREATE TABLE tbl_3_k(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_3_k VALUES('kan''dian''ying','kdy','看电影',100);"
                         "INSERT INTO tbl_3_k VALUES('kan''dian''ying','kdy','看电映',90);"
                         "CREATE TABLE tbl_4_k(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_4_k VALUES('kan''dian''ying''qu','kdyq','看电影去',200);"
                         "CREATE TABLE tbl_2_k(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_k VALUES('kan''yang','ky','看样',100000);"
                         "INSERT INTO tbl_2_k VALUES('kan''yang','ky','看电影',90000);"
                         "INSERT INTO tbl_2_k VALUES('kan''dao','kd','看到',1000);"
                         "CREATE TABLE tbl_1_k(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_k VALUES('kan','k','看',100000);"
                         "CREATE TABLE tbl_4_g(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_4_g VALUES('gai''zen''me''ban','gzmb','该怎么办',100);"
                         "CREATE TABLE tbl_3_g(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_3_g VALUES('gai''zen''me','gzm','该怎么',1000);"
                         "CREATE TABLE tbl_1_g(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_g VALUES('gai','g','该',100000);"
                         "CREATE TABLE tbl_1_b(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_b VALUES('ban','b','办',100);"
                         "INSERT INTO tbl_1_b VALUES('bang','b','帮',100);"
                         "CREATE TABLE tbl_3_z(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_3_z VALUES('zhe''yang''zi','zyz','这样子',100);"
                         "CREATE TABLE tbl_1_q(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_q VALUES('qu','q','去',1000);"
                         "CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_n VALUES('ni','n','你',1000);"
                         "CREATE TABLE tbl_1_z(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_z VALUES('zhong','z','中',100);"
                         "INSERT INTO tbl_1_z VALUES('zhe','z','这',100000);");
    }
    metasequoia::RuntimePaths paths;
    paths.resources = paths.user_data = paths.cache = paths.dictionaries = directory;
    struct Case
    {
        const char *key;
        const char *word;
    };
    const std::array phrases{Case{"wei'shen'me", "为什么"}, Case{"kan'dian'ying", "看电影"},
                             Case{"gai'zen'me'ban", "该怎么办"}, Case{"zhe'yang'zi", "这样子"}};
    struct Boundary
    {
        const char *input;
        const char *code;
        const char *reading;
        unsigned expected;
    };
    const std::array boundaries{
        Boundary{"kandy", "kan'd'y", "kan'dian'ying", 1},
        Boundary{"zheyzi", "zhe'y'zi", "zhe'yang'zi", 1},
        Boundary{"ad", "a'd", "a'de", 1},
        Boundary{"gaizenmbq", "gai'zen'm'b", "gai'zen'me'ban", 2},
        Boundary{"gaizenmbzh", "gai'zen'm'b", "gai'zen'me'ban", 2},
        Boundary{"gaizenmbch", "gai'zen'm'b", "gai'zen'me'ban", 2},
        Boundary{"gaizenmbsh", "gai'zen'm'b", "gai'zen'me'ban", 2},
        Boundary{"kandianying", "kan'dian'ying", "kan'dian'ying", 0},
        Boundary{"kdy", "k'd'y", "kan'dian'ying", 0},
        Boundary{"kan'd'y", "kan'd'y", "kan'dian'ying", 0},
        Boundary{"kanmy", "kan'd'y", "kan'dian'ying", 0},
        Boundary{"kandy", "kan'd'y", "kan'ying", 0},
        Boundary{"bqng", "b'q'n'g", "ba'qu'ni'gu", 0},
        Boundary{"gaizenmban", "gai'zen'm", "gai'zen'me", 0},
        Boundary{"gaizenmbq", "gai'zen'm", "gai'zen'me", 0},
        Boundary{"gaizenmbzho", "gai'zen'm'b", "gai'zen'me'ban", 0},
        Boundary{"kandy", "kan'd'y", "", 0},
    };
    for (const auto &test : boundaries)
        require(quanpin::literal_mixed_pinyin_match(test.input, test.code, test.reading) == test.expected,
                (std::string("Unexpected literal mixed-pinyin boundary: ") + test.input + "/" + test.code).c_str());
    const auto check = [&](metasequoia::InputSession &session, const std::string &input, const Case &test,
                           const std::string &code, const std::string &tail = "") {
        const auto &head = session.candidates().at(0);
        if (head.word != test.word || head.canonical_pinyin != test.key || head.pinyin != code ||
            !head.corrected_from.empty())
        {
            throw std::runtime_error("Original mixed-pinyin candidate lost its rank or metadata: " + input +
                                     "; actual=" + head.word + "," + head.pinyin + "," + head.canonical_pinyin);
        }
        const auto expected_display = code + (tail.empty() ? "" : "'" + tail);
        const auto display = session.get_pinyin_segmentation_with_cases();
        if (input.find('\'') != std::string::npos)
        {
            auto letters = display;
            letters.erase(std::remove(letters.begin(), letters.end(), '\''), letters.end());
            auto expected_letters = code;
            expected_letters.erase(std::remove(expected_letters.begin(), expected_letters.end(), '\''),
                                   expected_letters.end());
            require(session.raw_segmentation() == code && letters == expected_letters,
                    "Manual delimiters must preserve raw boundaries and all display letters.");
        }
        else if (display != expected_display)
            throw std::runtime_error("Unexpected mixed-pinyin display: " + input + "; expected=" + expected_display +
                                     "; actual=" + display);
        session.set_candidate_learning_enabled(false);
        require(session.select_candidate(0).commit == test.word && session.preedit() == tail,
                "Mixed-pinyin selection must consume exactly its original letters.");
    };
    for (const unsigned mask : {0u, 1u, 2u, 3u})
    {
        // 8 + 8 + 16 + 8 spellings, including all-full and all-initial controls, under all four switches.
        for (const auto &phrase : phrases)
        {
            const auto syllables = quanpin::split_segments(phrase.key);
            for (size_t bits = 0; bits < (size_t{1} << syllables.size()); ++bits)
            {
                auto matched = syllables;
                std::string input;
                for (size_t i = 0; i < matched.size(); ++i)
                {
                    if (bits & (size_t{1} << i))
                        matched[i].resize(1);
                    input += matched[i];
                }
                const auto code = quanpin::join_segments(matched);
                for (const bool delimited : {false, true})
                {
                    metasequoia::InputSession session(SchemeType::Quanpin, mask, true, true, false, paths);
                    for (int repetition = 0; repetition < 2; ++repetition)
                    {
                        type(session, delimited ? code : input);
                        check(session, (delimited ? code : input) + "/mask=" + std::to_string(mask), phrase, code);
                    }
                }
            }
        }
        for (const char *tail : {"q", "n", "zh", "ch", "sh"})
        {
            metasequoia::InputSession session(SchemeType::Quanpin, mask, true, true, false, paths);
            SentenceAssociationOptions association;
            association.word_lattice = true;
            session.set_sentence_association(association);
            type(session, std::string("gaizenmb") + tail);
            check(session, std::string("gaizenmb") + tail, phrases[2], "gai'zen'm'b", tail);
        }
        for (const auto &prefix : std::array{Case{"dy", "看"}, Case{"y", "看到"}, Case{"", "看电影"}})
        {
            metasequoia::InputSession session(SchemeType::Quanpin, mask, true, true, false, paths);
            type(session, "kand");
            type(session, "y");
            (void)session.handle_command(metasequoia::Command::Backspace);
            type(session, "y");
            session.set_candidate_learning_enabled(false);
            require(session.select_candidate(candidate_index(session, prefix.word)).commit == prefix.word &&
                        session.preedit() == prefix.key,
                    "Selecting a literal prefix must not consume corrected spans.");
        }
        {
            metasequoia::InputSession session(SchemeType::Quanpin, mask, true, true, false, paths);
            type(session, "kandyq");
            check(session, "kandyq", Case{"kan'dian'ying'qu", "看电影去"}, "kan'd'y'q");
        }
        {
            metasequoia::InputSession session(SchemeType::Quanpin, mask, true, true, false, paths);
            type(session, "gaizenmb");
            session.set_candidate_learning_enabled(false);
            require(session.select_candidate(candidate_index(session, "该怎么")).commit == "该怎么" &&
                        session.preedit() == "b" && session.select_candidate(0).commit == "办",
                    "Partial mixed-pinyin selection must retain the last initial.");
        }
    }
    struct DoublePinyin
    {
        const ShuangpinProfile *profile;
        std::array<const char *, 4> inputs;
    };
    const std::array double_pinyin{
        DoublePinyin{&GetXiaoheShuangpinProfile(), {"wwufme", "kjdmyk", "gdzfmebj", "veyhzi"}},
        DoublePinyin{&GetZiranmaShuangpinProfile(), {"wzufme", "kjdmyy", "glzfmebj", "veyhzi"}},
        DoublePinyin{&GetShoudaoShuangpinProfile(), {"wmekme", "kjdnyg", "glzkmebj", "veyyzi"}},
        DoublePinyin{&GetMicrosoftShuangpinProfile(), {"wzufme", "kjdmy;", "glzfmebj", "veyhzi"}},
        DoublePinyin{&GetSogouShuangpinProfile(), {"wzufme", "kjdmy;", "glzfmebj", "veyhzi"}},
        DoublePinyin{&GetZiguangShuangpinProfile(), {"wkiwme", "krdfy;", "gpzwmebr", "ueyszi"}},
        DoublePinyin{&GetZhinengAbcShuangpinProfile(), {"wqvfme", "kjdwyy", "glzfmebj", "aeyhzi"}},
        DoublePinyin{&GetGuobiaoShuangpinProfile(), {"wburme", "kfddyj", "gkzrmebf", "veygzi"}},
        DoublePinyin{&GetPinyinJiajiaShuangpinProfile(), {"wwirme", "kfdjyq", "gszrmebf", "veygzi"}},
    };
    const auto type_double_pinyin = [](metasequoia::InputSession &session, const std::string &input) {
        for (const char ch : input)
            session.handle_engine_key(ch == ';' ? 0xBAu : static_cast<unsigned>(ch - 'a' + 'A'), 0, ch);
    };
    for (const auto &layout : double_pinyin)
    {
        for (const unsigned mask : {0u, 1u, 2u, 3u})
        {
            for (size_t i = 0; i < phrases.size(); ++i)
            {
                metasequoia::InputSession session(SchemeType::Shuangpin, *layout.profile, paths);
                session.set_shuangpin_helpcode_enabled(false);
                session.set_candidate_learning_enabled(false);
                session.set_quanpin_autocorrect_types(mask);
                for (int repetition = 0; repetition < 2; ++repetition)
                {
                    type_double_pinyin(session, layout.inputs[i]);
                    require(!session.candidates().empty() && session.candidates().front().word == phrases[i].word &&
                                session.candidates().front().canonical_pinyin == phrases[i].key &&
                                session.candidates().front().corrected_from.empty() &&
                                session.normalized_segmentation() == phrases[i].key,
                            ("Mixed-pinyin ordering must preserve Shuangpin: " + layout.profile->name + "/" +
                             layout.inputs[i] + "/mask=" + std::to_string(mask))
                                .c_str());
                    require(session.select_candidate(0).commit == phrases[i].word && session.preedit().empty(),
                            "Shuangpin selection must consume exactly its two-key syllables.");
                }
            }
            metasequoia::InputSession partial(SchemeType::Shuangpin, *layout.profile, paths);
            partial.set_shuangpin_helpcode_enabled(false);
            partial.set_candidate_learning_enabled(false);
            partial.set_quanpin_autocorrect_types(mask);
            type_double_pinyin(partial, layout.inputs[1]);
            require(partial.select_candidate(candidate_index(partial, "看")).commit == "看" &&
                        partial.preedit() == std::string(layout.inputs[1]).substr(2),
                    "A Shuangpin prefix selection must retain the remaining two-key units.");
        }
    }
    {
        metasequoia::InputSession switching(SchemeType::Quanpin, 3u, false, true, false, paths);
        type(switching, "kandy");
        check(switching, "kandy before scheme switch", phrases[1], "kan'd'y");
        switching.switch_scheme(SchemeType::Shuangpin);
        switching.set_shuangpin_helpcode_enabled(false);
        type_double_pinyin(switching, "kjdmyk");
        require(switching.select_candidate(0).commit == "看电影" && switching.preedit().empty(),
                "Switching to Shuangpin must discard Quanpin candidate state.");
        switching.switch_scheme(SchemeType::Quanpin);
        type(switching, "zheyzi");
        check(switching, "zheyzi after scheme switch", phrases[3], "zhe'y'zi");
    }
    metasequoia::InputSession session(SchemeType::Quanpin, 3u, false, true, false, paths);
    for (const char *input : {"kandy", "kanwy", "kandy"})
    {
        type(session, input);
        metasequoia::InputSession fresh(SchemeType::Quanpin, 3u, false, true, false, paths);
        type(fresh, input);
        require(same_candidate_words(session, fresh), "A correction cache must not leak another input's literal rows.");
        session.reset_state();
    }
    type(session, "kandy");
    {
        Database database(directory / "msime.db");
        database.execute("UPDATE tbl_3_k SET weight=200 WHERE value='看电映';");
    }
    session.reset_cache();
    session.recompute_candidates();
    require(session.candidates().front().word == "看电映", "Literal candidates must retain their own frequency order.");
    session.set_quanpin_autocorrect_types(0);
    session.recompute_candidates();
    require(session.candidates().front().word == "看电映",
            "Changing correction switches must refresh candidate caches.");
    session.set_quanpin_autocorrect_types(3);
    session.set_fuzzy_pinyin_options({static_cast<std::uint32_t>(metasequoia::FuzzyPinyinRule::IAN_IANG)});
    SentenceAssociationOptions association;
    association.word_lattice = true;
    session.set_sentence_association(association);
    session.set_rescoring_context("请");
    session.recompute_candidates();
    require(session.candidates().front().word == "看电映" && session.candidates().front().corrected_from.empty(),
            "Fuzzy/context reranking must retain original mixed-pinyin priority and metadata.");
    require(session.pin_candidate(candidate_index(session, "看电影")).handled &&
                session.candidates().front().word == "看电影",
            "Explicit frequency pinning must still reorder words within the literal group.");
    session.enable_fixed_positions();
    // 用「看到」（输入字面就能命中的混合简拼候选）。此前这里用「看样」——它靠
    // 生成式远键插入 kand->kan 与简拼尾 y 组合出 kan'y 才出现；本任务移除远键
    // 插入后该读法不再存在，改用一个不依赖它的候选验固定位置优先级。
    require(session.set_candidate_position(candidate_index(session, "看到"), 1).handled &&
                session.candidates().front().word == "看到",
            "Explicit fixed positions must take precedence over the default mixed-pinyin order.");
    metasequoia::InputSession cased(SchemeType::Quanpin, 3u, false, true, false, paths);
    cased.handle_engine_key('K', 0, 'K');
    type(cased, "andy");
    require(cased.get_pinyin_segmentation_with_cases() == "Kan'd'y",
            "The literal mixed-pinyin preedit must preserve the original letter cases.");
    for (const char *typo : {"zhogn", "zhng", "bqng"})
    {
        metasequoia::InputSession corrected(SchemeType::Quanpin, 3u, false, true, false, paths);
        type(corrected, typo);
        require(candidate_index(corrected, std::string(typo) == "bqng" ? "帮" : "中") < corrected.candidates().size(),
                "True typos must still produce correction candidates.");
    }
}

// 单字母输入不做字面整词提升：一个字母任何单字都能整串吃下，提升只会把「嗯」这类
// 单字母读音的语气词顶到词频更高的常用字前面（打 n 的首位从「你」变成「嗯」）。
// 字面整词提升的用途是让「nbn」这类整词读法领衔，那需要至少两个字母才谈得上整词。
void run_single_letter_candidate_order_tests(const std::filesystem::path &data_directory)
{
    const auto directory = data_directory / "single-letter-candidate-order";
    std::filesystem::create_directories(directory);
    {
        Database database(directory / "msime.db");
        database.execute("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_n VALUES('ni','n','你',1000000);"
                         "INSERT INTO tbl_1_n VALUES('nian','n','年',900000);"
                         "INSERT INTO tbl_1_n VALUES('na','n','呐',800000);"
                         "INSERT INTO tbl_1_n VALUES('na','n','那',700000);"
                         "INSERT INTO tbl_1_n VALUES('na','n','拿',600000);"
                         "INSERT INTO tbl_1_n VALUES('ne','n','呢',500000);"
                         "INSERT INTO tbl_1_n VALUES('neng','n','能',400000);"
                         "INSERT INTO tbl_1_n VALUES('n','n','嗯',300000);"
                         "INSERT INTO tbl_1_n VALUES('ng','n','嗯',200000);");
    }

    QuanpinDictionary dictionary(metasequoia::path_to_utf8(directory / "msime.db"));

    const auto candidates = dictionary.query("n");
    require(!candidates.empty(), "A single-letter query must return candidates.");
    require(candidates.front().word == "你",
            "A single letter must keep dictionary frequency order; 嗯 (pinyin n) must not lead the page.");
    const auto index_of = [&candidates](const char *word) {
        const auto found =
            std::find_if(candidates.begin(), candidates.end(), [word](const auto &item) { return item.word == word; });
        return static_cast<std::size_t>(std::distance(candidates.begin(), found));
    };
    require(index_of("嗯") > index_of("能"), "A single-letter query must not lift 嗯 above higher-frequency words.");

    // 回归：两字母输入照常按词频排序，提升开关对它没有影响。
    const auto two_letter = dictionary.query("ne");
    require(!two_letter.empty() && two_letter.front().word == "呢",
            "A two-letter query must keep its dictionary head.");
}

// 光标驱动的前缀解码（PRD R2–R7，Stage 1）：候选与量化边界按「光标之前的完整音节
// 单元前缀」重算。自建隔离词库，不与主 fixture 互相污染；Server（Stage 2）将以
// set_caret + recompute_candidates 的同一方式消费这些入口。
// 阶段 1 上下文消解（任务 quanpin-autocorrect-context-ranking）：同档纠错读法
// 在词格路径分边际达标时由胜出切分接管领衔。zhng 的删除目标按表序 zhang 在前
// （主切），fixture 让 zheng 侧的办证权重高 20 倍——启发式路径分差 ln(20)=3.0
// （log10 1.3），两种标度下都过 1.0 接管边际。fixture 无 sc.lm，词格退回
// 启发式打分（ln(weight)+词长奖励），权重完全决定路径分，测试因此确定。
void run_autocorrect_context_ranking_tests(const std::filesystem::path &data_directory)
{
    const std::filesystem::path directory = data_directory / "autocorrect-context";
    std::filesystem::create_directories(directory);
    {
        Database database(directory / "msime.db");
        database.execute("CREATE TABLE tbl_2_b(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_b VALUES('ban''zheng', 'bz', '办证', 100000);"
                         "INSERT INTO tbl_2_b VALUES('ban''zheng', 'bz', '辩证', 10);"
                         "INSERT INTO tbl_2_b VALUES('ban''zhang', 'bz', '班长', 5000);"
                         "INSERT INTO tbl_2_b VALUES('ban''zhang', 'bz', '搬账', 4000);");
        database.execute("CREATE TABLE tbl_1_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_s VALUES('shang', 's', '上', 900);"
                         "INSERT INTO tbl_1_s VALUES('sheng', 's', '生', 800);");
    }

    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
    // "banzhng" drops the "e" of zheng (a deletion-shaped typo), so the full product
    // mask -- all four independent switches on -- is what this context-ranking case
    // needs. The two legacy bits alone no longer carry the deletion bit.
    const unsigned all_types = both | quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;
    SentenceAssociationOptions lattice_only;
    lattice_only.word_lattice = true;

    const auto candidate_words = [](const metasequoia::InputSession &session) {
        std::vector<std::string> words;
        for (const auto &item : session.candidates())
        {
            words.push_back(item.word);
        }
        return words;
    };

    // 关联关闭 = 现状静态路径：合并池按权重排序，办证系与班长系交错。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, all_types, true, true, true, paths);
        type(session, "banzhng");
        const auto words = candidate_words(session);
        require(words.size() == 4 && words[0] == "办证" && words[1] == "班长" && words[2] == "搬账" &&
                    words[3] == "辩证",
                "The static same-tier merge must interleave the readings by dictionary weight.");
    }

    // 关联开启：zheng 切分上下文胜出接管领衔——办证系整体前移，辩证从末位
    // 升到第 2 位，班长系整体后移。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, all_types, true, true, true, paths);
        session.set_sentence_association(lattice_only);
        type(session, "banzhng");
        const auto words = candidate_words(session);
        require(words.size() == 4 && words[0] == "办证" && words[1] == "辩证" && words[2] == "班长" &&
                    words[3] == "搬账",
                "The context-ranked reading must lead with its whole word group.");
    }

    // 单音节纠错无词格可搭（词格要求 >=2 完整音节）：两种状态必须逐位一致。
    {
        metasequoia::InputSession off(SchemeType::Quanpin, all_types, true, true, true, paths);
        type(off, "shng");
        metasequoia::InputSession on(SchemeType::Quanpin, all_types, true, true, true, paths);
        on.set_sentence_association(lattice_only);
        type(on, "shng");
        require(same_candidate_words(off, on),
                "A single-syllable correction must take the static path even with the word lattice on.");
        require(candidate_words(on).size() == 2, "The single-syllable fixture must surface both deletion targets.");
    }
}

// 审阅缺陷 D1（PR #592 行内评论）：上下文重排接管后，胜出切分的单字前缀曾被插到
// 同键位整词前面。query_series 的 count 从整键递减到 1，ban'zheng 的输出里除了
// 办证/辩证/整句还带着首音节的单字；这些单字排在 rest（班长、搬账）之前，把同键位的
// 其他整词整组挤出首页。原 fixture 没有 tbl_1_b，前缀这条路根本没被走到。
// 这里补单字表，并直接断言「整键在前、前缀在后」这条分层不变量。
// 同上一组 fixture：词库目录里没有 sc.lm，词格退回启发式打分，权重完全决定路径分，
// 因此 zheng 侧稳定胜出、重排稳定接管，用例不依赖任何模型文件。
void run_autocorrect_context_layering_tests(const std::filesystem::path &data_directory)
{
    const std::filesystem::path directory = data_directory / "autocorrect-context-layering";
    std::filesystem::create_directories(directory);
    {
        Database database(directory / "msime.db");
        database.execute("CREATE TABLE tbl_2_b(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_b VALUES('ban''zheng', 'bz', '办证', 100000);"
                         "INSERT INTO tbl_2_b VALUES('ban''zheng', 'bz', '辩证', 10);"
                         "INSERT INTO tbl_2_b VALUES('ban''zhang', 'bz', '班长', 5000);"
                         "INSERT INTO tbl_2_b VALUES('ban''zhang', 'bz', '搬账', 4000);");
        // 单字表：query_series 递减到 count==1 时查它，前缀行由此产生。单字权重取 1e6
        // 量级（真实词库里单字是语料计数），确保它们不会被 append 阶段的去重或调频挪走。
        database.execute("CREATE TABLE tbl_1_b(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_b VALUES('ban', 'b', '办', 1000000);"
                         "INSERT INTO tbl_1_b VALUES('ban', 'b', '半', 900000);");
    }

    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
    // "banzhng" 是漏字形状（zheng 掉了 e）：这里要的是产品全开语义，四个开关都开。
    const unsigned all_types = both | quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;
    SentenceAssociationOptions lattice_only;
    lattice_only.word_lattice = true;

    metasequoia::InputSession session(SchemeType::Quanpin, all_types, true, true, true, paths);
    session.set_sentence_association(lattice_only);
    type(session, "banzhng");

    const auto &items = session.candidates();
    const auto index_of = [&items](const std::string &word) {
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (items[i].word == word)
            {
                return i;
            }
        }
        return items.size();
    };

    // 先确认 fixture 真的同时产出了整键行与单字前缀行，否则下面的分层断言是空的。
    const size_t prefix_a = index_of("办");
    const size_t prefix_b = index_of("半");
    require(prefix_a != items.size() && prefix_b != items.size(),
            "The layering fixture must surface single-character prefix rows for the winner cut.");
    const size_t sibling = index_of("班长");
    require(sibling != items.size(), "The layering fixture must surface the same-tier whole word.");

    require(index_of("办证") < prefix_a, "The winner cut's whole word must precede its own single-character prefixes.");
    require(sibling < prefix_a && sibling < prefix_b,
            "Same-tier whole words must not be pushed behind the winner cut's single-character prefixes.");

    // 分层的完整形式：所有整键行都在任何前缀行之前。全拼键用 ' 分隔音节，
    // 单音节键不含 '。
    const auto is_prefix_row = [&items](size_t i) { return items[i].canonical_pinyin.find('\'') == std::string::npos; };
    bool seen_prefix = false;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (is_prefix_row(i))
        {
            seen_prefix = true;
        }
        else
        {
            require(!seen_prefix, "Every whole-key row must precede every single-character prefix row.");
        }
    }
}

// 用户在主切读音上的选择优先于上下文接管。加载 LM 时路径分只剩 dictionary_tiebreak
// 一点点词库权重，调频翻不过接管边际，所以前两组 fixture 的启发式打分测不出来：这里
// 放一份手写 ARPA 当 sc.lm（kenlm 按文本格式载入），让 LM 稳定偏向办证（log10 差 2.5），
// 同时把班长的词库权重设成调频后的样子——越过办证。
void run_autocorrect_context_user_choice_tests(const std::filesystem::path &data_directory)
{
    const auto make_fixture = [&data_directory](const char *name, int banzhang_weight) {
        const std::filesystem::path directory = data_directory / name;
        std::filesystem::create_directories(directory);
        {
            Database database(directory / "msime.db");
            database.execute("CREATE TABLE tbl_2_b(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                             "INSERT INTO tbl_2_b VALUES('ban''zheng', 'bz', '办证', 100000);"
                             "INSERT INTO tbl_2_b VALUES('ban''zheng', 'bz', '辩证', 10);"
                             "INSERT INTO tbl_2_b VALUES('ban''zhang', 'bz', '搬账', 4000);");
            const std::string banzhang =
                "INSERT INTO tbl_2_b VALUES('ban''zhang', 'bz', '班长', " + std::to_string(banzhang_weight) + ");";
            database.execute(banzhang.c_str());
        }
        // 二进制写：文本模式在 Windows 上会把换行写成 \r\n，kenlm 的 ARPA 解析不认。
        std::ofstream arpa(directory / metasequoia::assets::language_model, std::ios::binary);
        arpa << "\\data\\\n"
                "ngram 1=7\n"
                "ngram 2=1\n"
                "\n"
                "\\1-grams:\n"
                "-1.0\t<unk>\t0\n"
                "-99\t<s>\t0\n"
                "-1.0\t</s>\t0\n"
                "-1.0\t办证\t0\n"
                "-3.5\t班长\t0\n"
                "-3.5\t搬账\t0\n"
                "-4.0\t辩证\t0\n"
                "\n"
                "\\2-grams:\n"
                "-0.5\t<s> 办证\n"
                "\n"
                "\\end\\\n";
        require(static_cast<bool>(arpa), "Failed to write the user-choice language model fixture.");
        metasequoia::RuntimePaths paths;
        paths.resources = directory;
        paths.user_data = directory;
        paths.cache = directory;
        paths.dictionaries = directory;
        return paths;
    };
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
    // "banzhng" 是漏字形状：这里要的是产品全开语义，四个开关都开。
    const unsigned all_types = both | quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;
    SentenceAssociationOptions lattice_only;
    lattice_only.word_lattice = true;
    const auto first_word = [all_types, &lattice_only](const metasequoia::RuntimePaths &paths) {
        metasequoia::InputSession session(SchemeType::Quanpin, all_types, true, true, true, paths);
        session.set_sentence_association(lattice_only);
        type(session, "banzhng");
        require(!session.candidates().empty(), "The user-choice fixture produced no candidates.");
        return session.candidates().front().word;
    };

    {
        const auto paths = make_fixture("autocorrect-context-user-choice", 150000);
        // 没有用户记录：班长权重再高也只是出货词频，仍交给上下文裁决。这一步同时确认
        // ARPA 真的载入了——启发式打分下班长权重更高，不会是办证领衔。
        require(first_word(paths) == "办证",
                "Without a user record the language model must still let the context reading lead.");

        require(user_dictionary::record_upsert(metasequoia::path_to_utf8(paths.user(metasequoia::assets::user_journal)),
                                               user_dictionary::DictionaryKind::Pinyin, "ban'zhang", "班长", 150000),
                "Failed to write the user-choice journal record.");
        require(first_word(paths) == "班长",
                "A word the user ranked above the context winner must not be displaced by the context takeover.");
    }

    {
        // 用户动过主切的键，但主切首位仍低于胜出切分首位：用户并没有把这组读音选到前面，照常接管。
        const auto paths = make_fixture("autocorrect-context-user-touched", 5000);
        require(user_dictionary::record_upsert(metasequoia::path_to_utf8(paths.user(metasequoia::assets::user_journal)),
                                               user_dictionary::DictionaryKind::Pinyin, "ban'zhang", "搬账", 4000),
                "Failed to write the user-touched journal record.");
        require(first_word(paths) == "办证",
                "A user record that does not outrank the context winner must not block the takeover.");
    }
}

// 合法输入上的手误：ziazheliya 切成 zi'a'zhe'li'ya、jioa 切成 ji'o'a、niohao 切成
// ni'o'hao，每一段都合法，纠错入口的闸原本把它们整个挡在外面。现在按噪声信道
// 整句打分：词格路径分 - 每处纠正的代价（换位 2.0，多字/漏字 3.0），原读法代价 0。
// fixture 无 sc.lm，词格走启发式
// （单字 ln(w/1e6)，词组 ln(w)+3×音节数），下面每组的分数都按它算好写在注释里。
void run_legal_input_correction_tests(const std::filesystem::path &data_directory)
{
    const auto readings = [](const std::string &pinyin, const quanpin::Segments &segments, unsigned types) {
        std::vector<std::string> keys;
        for (const auto &cut : quanpin::legal_input_correction_cuts(pinyin, segments, types, 3))
        {
            quanpin::Segments syllables;
            for (const auto &segment : cut.segments)
            {
                syllables.push_back(segment.syllable);
            }
            keys.push_back(quanpin::join_segments(syllables));
        }
        return keys;
    };
    const auto offers = [](const std::vector<std::string> &keys, const char *key) {
        return std::find(keys.begin(), keys.end(), key) != keys.end();
    };
    const unsigned transposition = quanpin::kAutocorrectTransposition;
    require(offers(readings("ziazheliya", {"zi", "a", "zhe", "li", "ya"}, transposition), "zai'zhe'li'ya"),
            "A transposition inside a longer sentence must still offer the corrected reading.");
    require(offers(readings("jioa", {"ji", "o", "a"}, transposition), "jiao"), "jioa must read as jiao.");
    require(offers(readings("nia", {"ni", "a"}, transposition), "nai"), "nia must read as nai.");
    require(offers(readings("liazheli", {"lia", "zhe", "li"}, transposition), "lai'zhe'li"),
            "A rare legal syllable must offer its common transposition.");
    require(readings("lia", {"lia"}, transposition).empty(),
            "A single rare syllable has no context and must stay uncorrected.");
    require(
        offers(readings("niohao", {"ni", "o", "hao"}, quanpin::kAutocorrectInsertion), "ni'hao"),
        "An extra QWERTY-neighbor letter that splits off a zero-initial syllable must offer the corrected reading.");
    require(readings("ziazheli", {"zi", "a", "zhe", "li"}, quanpin::kAutocorrectNeighbor).empty(),
            "Neighbor substitutions must stay off on legal input.");
    require(
        readings("nihao", {"ni", "hao"}, transposition | quanpin::kAutocorrectInsertion | quanpin::kAutocorrectDeletion)
            .empty(),
        "An ordinary legal input must not produce correction readings.");

    const std::filesystem::path directory = data_directory / "legal-input-correction";
    std::filesystem::create_directories(directory);
    {
        Database database(directory / "msime.db");
        database.execute("CREATE TABLE tbl_1_z(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_z VALUES('zi', 'z', '字', 800000);"
                         "INSERT INTO tbl_1_z VALUES('zai', 'z', '在', 900000);"
                         "INSERT INTO tbl_1_z VALUES('zhe', 'z', '这', 900000);");
        database.execute("CREATE TABLE tbl_3_z(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_3_z VALUES('zai''zhe''li', 'zzl', '在这里', 5000);");
        database.execute("CREATE TABLE tbl_1_a(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_a VALUES('a', 'a', '啊', 600000);");
        database.execute("CREATE TABLE tbl_1_o(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_o VALUES('o', 'o', '哦', 300000);");
        database.execute("CREATE TABLE tbl_1_y(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_y VALUES('ya', 'y', '呀', 500000);"
                         "INSERT INTO tbl_1_y VALUES('yang', 'y', '养', 500000);");
        database.execute("CREATE TABLE tbl_1_l(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_l VALUES('li', 'l', '里', 900000);"
                         "INSERT INTO tbl_1_l VALUES('lia', 'l', '俩', 500000);"
                         "INSERT INTO tbl_1_l VALUES('lai', 'l', '来', 900000);");
        database.execute("CREATE TABLE tbl_3_l(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_3_l VALUES('lai''zhe''li', 'lzl', '来这里', 3000);");
        database.execute("CREATE TABLE tbl_1_j(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_j VALUES('jiao', 'j', '叫', 2000000);"
                         "INSERT INTO tbl_1_j VALUES('jiao', 'j', '教', 1500000);"
                         "INSERT INTO tbl_1_j VALUES('ji', 'j', '几', 800000);"
                         "INSERT INTO tbl_1_j VALUES('jiu', 'j', '就', 900000);");
        database.execute("CREATE TABLE tbl_3_j(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_3_j VALUES('jiu''zhe''yang', 'jzy', '就这样', 5000);");
        database.execute("CREATE TABLE tbl_1_e(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_e VALUES('e', 'e', '恶', 300000);");
        database.execute("CREATE TABLE tbl_2_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_x VALUES('xie''e', 'xe', '邪恶', 1000);");
        database.execute("CREATE TABLE tbl_1_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_x VALUES('xie', 'x', '写', 2000000);"
                         "INSERT INTO tbl_1_x VALUES('xie', 'x', '些', 1500000);");
        database.execute("CREATE TABLE tbl_2_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_s VALUES('shu''e', 'se', '数额', 1000);");
        database.execute("CREATE TABLE tbl_1_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_s VALUES('she', 's', '社', 2000000);"
                         "INSERT INTO tbl_1_s VALUES('shu', 's', '数', 900000);");
        database.execute("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_n VALUES('ni', 'n', '你', 900000);"
                         "INSERT INTO tbl_1_n VALUES('nai', 'n', '奶', 900000);");
        database.execute("CREATE TABLE tbl_3_w(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_3_w VALUES('wo''men''lia', 'wml', '我们俩', 2000);"
                         "INSERT INTO tbl_3_w VALUES('wo''men''lai', 'wml', '我们来', 2500);");
        database.execute("CREATE TABLE tbl_2_w(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_w VALUES('wo''men', 'wm', '我们', 3000);");
    }

    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
    // 本函数里出现漏字 / 多字形状的输入（banzhng、zaiozheli、shue），都要产品全开语义。
    const unsigned all_types = both | quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;
    SentenceAssociationOptions lattice_only;
    lattice_only.word_lattice = true;

    const auto first_two = [](const metasequoia::InputSession &session, const char *first, const char *second) {
        return session.candidates().size() >= 2 && session.candidates()[0].word == first &&
               session.candidates()[1].word == second;
    };

    // 句中纠错：在这里呀 = 在这里(ln5000+9) + 呀(ln0.5) - 2 = 14.8，字啊这里呀全是单字
    // = -1.6。纠错领衔，原读法的整句紧跟在第 2 位；预编辑按领衔读法分隔。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        session.set_sentence_association(lattice_only);
        type(session, "ziazheliya");
        require(first_two(session, "在这里呀", "字啊这里呀"),
                "ziazheliya must lead with 在这里呀 and keep the uncorrected sentence second.");
        require(session.candidates()[0].corrected_from == "ziazheliya",
                "The legal-input correction must carry the corrected_from mark.");
        require(session.candidates()[1].corrected_from.empty(), "The uncorrected reading must stay unmarked.");
        require(session.get_pinyin_segmentation_with_cases() == "zia'zhe'li'ya",
                "The preedit must follow the leading corrected reading.");

        // 选纠错读法里的前缀词：换位不改字母数，消耗 ziazheli 八个字母，剩下 ya。
        (void)session.select_candidate(candidate_index(session, "在这里"));
        require(session.has_composition() && session.preedit() == "ya",
                "Selecting a corrected prefix must consume exactly the letters it covers.");
    }
    // 整句开关关着照样纠：判断读音不看显示开关，只是首选退回整键/前缀词。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "ziazheliya");
        require(first_two(session, "在这里", "字"),
                "With sentence candidates off the corrected prefix word must still lead.");
    }

    // jiao 叫 = ln2 - 2 = -1.3，几哦啊 = ln0.8 + ln0.3 + ln0.6 = -1.9：纠错领衔。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "jioa");
        require(first_two(session, "叫", "几"), "jioa must lead with jiao and keep the uncorrected reading second.");
        require(find_candidate_index(session, "教") < session.candidates().size(),
                "The rest of the corrected reading must follow.");
        require(session.get_pinyin_segmentation_with_cases() == "jioa",
                "A single corrected syllable must drop the literal ji'o'a separators.");
    }
    // 多打一个邻键元音（o 是 i 的邻键）：zaiozheli 字面切成 zai'o'zhe'li 全合法，
    // 纠错读法 zai'zhe'li 命中「在这里」并带 corrected_from（双击重复键已不算多字
    // 纠错，见 test_pinyin 的 shangg 断言；这里改用邻键插入验长度改变读法的会话链路）。
    // 多字位独立后要显式打开，不再随两个 legacy 开关联动。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, all_types, true, true, true, paths);
        type(session, "zaiozheli");
        const auto found = find_candidate_index(session, "在这里");
        require(found < session.candidates().size() && session.candidates()[found].corrected_from == "zaiozheli",
                "A neighbor-inserted vowel must still reach the corrected phrase end to end.");
        const auto committed = session.select_candidate(found);
        require(committed.commit == "在这里" && !session.has_composition(),
                "Selecting the neighbor-insertion corrected phrase must consume every typed letter.");
    }

    // 字面切分是真词（数额）时不认改变字母数的读法：shue 按多一个 u 读成 she 的「社」
    // 不出现——少一个音节的句子在真实语言模型里常常分更高，会稳定挂在第 2 位。候选里
    // 不应有任何纠错标记（社 作为 shu 的前缀单字以外不该出现）。多字位独立后要显式打开，
    // 否则这一条断言的是「没开纠错」，失去意义。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, all_types, true, true, true, paths);
        type(session, "shue");
        require(session.candidates().front().word == "数额" &&
                    std::none_of(session.candidates().begin(), session.candidates().end(),
                                 [](const WordItem &item) { return !item.corrected_from.empty(); }),
                "A length-changing correction must not be offered over a literal dictionary word.");
    }
    // 被裁掉的纠错读法不能波及原读法：xiee 按多一个 e 读成 xie 被丢掉之后，邪恶 后面的
    // 前缀单字 写/些 读音也是 xie，曾因此全被标成纠错。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, all_types, true, true, true, paths);
        type(session, "xiee");
        require(!session.candidates().empty() && session.candidates().front().word == "邪恶" &&
                    find_candidate_index(session, "写") < session.candidates().size() &&
                    std::none_of(session.candidates().begin(), session.candidates().end(),
                                 [](const WordItem &item) { return !item.corrected_from.empty(); }),
                "A discarded correction reading must not mark the literal reading's prefix characters.");
    }

    // 罕见音节：来这里 = ln3000+9-2 = 15.0，俩这里 = -0.9。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "liazheli");
        require(first_two(session, "来这里", "俩"),
                "liazheli must lead with 来这里 and keep the uncorrected reading second.");
    }

    // 没有上下文时原读法占优：你啊 = ln0.9 + ln0.6 = -0.6，奶 = ln0.9 - 2 = -2.1。
    // 原读法领衔，分差 1.5 在 3.0 以内，纠错读法占第 2 位。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "nia");
        require(first_two(session, "你", "奶"), "nia must keep 你 first and offer 奶 second.");
        require(session.get_pinyin_segmentation_with_cases() == "ni'a",
                "The preedit must keep the literal reading while it leads.");
    }

    // 两边都是整词：我们俩 = ln2000+9 = 16.6，我们来 = ln2500+9-2 = 14.8。原读法领衔，
    // 纠错第 2 位。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "womenlia");
        require(first_two(session, "我们俩", "我们来"),
                "A close correction must take the second slot under the leading typed reading.");
    }
    // 纠错读法远远落后（我们来 降到 20：最好的路径 我们+来 = 11.9，差 4.7 > 3.0）：不出现。
    {
        Database database(directory / "msime.db");
        database.execute("UPDATE tbl_3_w SET weight = 20 WHERE value = '我们来';");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "womenlia");
        require(session.candidates().front().word == "我们俩" &&
                    find_candidate_index(session, "我们来") == session.candidates().size(),
                "A correction far behind the typed reading must not appear at all.");
    }
    // 纠错读法分数更高（我们来 = ln50000+9-2 = 17.8 > 16.6），但字面切分本身就是词库
    // 里的真词：首位留给它，纠错只排第 2。
    {
        Database database(directory / "msime.db");
        database.execute("UPDATE tbl_3_w SET weight = 50000 WHERE value = '我们来';");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "womenlia");
        require(first_two(session, "我们俩", "我们来"),
                "A correction must never displace a literal dictionary word from the first slot.");
    }

    // 同一条规则也管已有的辅音复制别名：yongan 被读成 yong'gan，勇敢权重再高也不能压过
    // 字面整词永安。同一串字母的另一种切分不受影响（见 fangan 的方案/反感）。
    {
        Database database(directory / "msime.db");
        database.execute("CREATE TABLE tbl_2_y(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_y VALUES('yong''an', 'ya', '永安', 100);"
                         "INSERT INTO tbl_2_y VALUES('yong''gan', 'yg', '勇敢', 100000);");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "yongan");
        require(session.candidates().front().word == "永安",
                "An alias reading that rewrites the letters must not displace the literal dictionary word.");
    }

    // 纠错关闭：合法输入照原样查。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0, true, true, true, paths);
        type(session, "ziazheli");
        require(find_candidate_index(session, "在这里") == session.candidates().size(),
                "The legal-input correction must stay off with autocorrection disabled.");
    }

    std::filesystem::remove_all(directory);
}

// 贵档读法在上下文明显更好时可以领衔：sahngna 的换位读法 sahng->shang（权重 10，
// 上+拿）比邻键插入读法 sahng->sang（权重 12，桑拿）便宜一档，按旧规则贵档永远排在
// 最后。fixture 无 sc.lm，启发式打分：桑拿(ln10000+6) - 2.4 = 15.2，上+拿全是单字
// = ln0.9 + ln0.9 - 2.0 = -1.8。（双击/远键插入已随 R1 移除，本用例改用邻键插入。）
void run_autocorrect_costlier_context_tests(const std::filesystem::path &data_directory)
{
    const std::filesystem::path directory = data_directory / "autocorrect-costlier-context";
    std::filesystem::create_directories(directory);
    {
        Database database(directory / "msime.db");
        database.execute("CREATE TABLE tbl_1_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_s VALUES('shang', 's', '上', 900000);"
                         "INSERT INTO tbl_1_s VALUES('sang', 's', '桑', 900000);");
        database.execute("CREATE TABLE tbl_2_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_s VALUES('sang''na', 'sn', '桑拿', 10000);");
        database.execute("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_n VALUES('na', 'n', '拿', 900000);");
    }

    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
    SentenceAssociationOptions lattice_only;
    lattice_only.word_lattice = true;

    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        session.set_sentence_association(lattice_only);
        type(session, "sahngna");
        require(!session.candidates().empty() && session.candidates().front().word == "桑拿",
                "A costlier neighbor-insertion correction the context clearly prefers must lead.");
        require(session.get_pinyin_segmentation_with_cases() == "sahng'na",
                "The preedit must follow the promoted costlier correction.");
    }
    // 只开万象来源行、不显示 Trigram 整句（用户实际配置）：上下文消解照样生效。fixture
    // 没有 .gram，出不了〔万象〕整句行，贵档胜者由它的最长前缀词「桑拿」领衔。
    {
        SentenceAssociationOptions collocation_only;
        collocation_only.collocation_rerank = true;
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        session.set_sentence_association(collocation_only);
        type(session, "sahngna");
        require(!session.candidates().empty() && session.candidates().front().word == "桑拿",
                "The context rerank must run whenever any sentence source is on, not only the Trigram row.");
    }

    std::filesystem::remove_all(directory);
}

// 纠错开关关掉后，查询之外的几条路径也必须按字面切分，否则它们和查询落在不同的键上：
// - 写回：选中后调频要把字面键 hao'yong'a 的行改掉，而不是别名重切出的 hao'yong'ga；
// - 辅助码：带辅助码的输入自己切底串，不能让 haoyonga 经别名层变成 hao'yong'ga（辅助码只在底串是完整拼音时生效）；
// - 固定位置：laing 的上下文 key 不能被切成 liang，和真正输入 liang 的那组混到一起。
void run_autocorrect_off_consistency_tests(const std::filesystem::path &data_directory)
{
    const std::filesystem::path directory = data_directory / "autocorrect-off-consistency";
    std::filesystem::create_directories(directory);
    write_file(directory / "helpcodes" / "helpcode.txt", "好=ef\n用=gh\n啊=ij\n阿=kl\n");
    {
        Database database(directory / "msime.db");
        database.execute("CREATE TABLE tbl_3_h(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_3_h VALUES('hao''yong''a', 'hya', '好用啊', 100);"
                         "INSERT INTO tbl_3_h VALUES('hao''yong''a', 'hya', '好用阿', 200);"
                         "INSERT INTO tbl_3_h VALUES('hao''yong''ga', 'hyg', '好用嘎', 300);");

        database.execute("CREATE TABLE tbl_1_l(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_l VALUES('lai', 'l', '来', 200);"
                         "INSERT INTO tbl_1_l VALUES('liang', 'l', '两', 200);");
    }

    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;

    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "haoyonga");
        require(session.select_candidate(candidate_index(session, "好用啊")).commit == "好用啊",
                "The literal reading of haoyonga must be selectable with autocorrection off.");
        Database database(directory / "msime.db");
        require(database.query_integer("SELECT weight FROM tbl_3_h WHERE value='好用啊'") > 100,
                "Learning a pick with autocorrection off must update the literal key it was queried under.");
    }

    {
        metasequoia::InputSession helpcode(SchemeType::Quanpin, 0u, true, true, true, paths);
        helpcode.set_quanpin_helpcode_enabled(true);
        require(helpcode.set_helpcode_schema("lantian"), "The helpcode fixture was not selected.");
        type(helpcode, "haoyongaK");
        require(find_candidate_index(helpcode, "好用啊") < helpcode.candidates().size(),
                "With autocorrection off a helpcode must still offer the literal reading hao'yong'a.");
        require(find_candidate_index(helpcode, "好用嘎") == helpcode.candidates().size(),
                "With autocorrection off a helpcode must not query the alias re-cut hao'yong'ga.");
    }

    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        session.enable_fixed_positions();
        type(session, "laing");
        const auto index = candidate_index(session, "来");
        const auto selected = session.candidates()[index];
        require(session.set_candidate_position(index, 2).handled, "Pinning a literal candidate must succeed.");
        const auto key = selected.canonical_pinyin.empty() ? selected.pinyin : selected.canonical_pinyin;
        const auto journal = metasequoia::path_to_utf8(paths.user(metasequoia::assets::user_journal));
        require(!user_dictionary::is_fixed(journal, "liang", key, selected.word),
                "With autocorrection off, laing must not pin into the liang context.");
        const auto literal = quanpin::cut_pinyin_by_mode("laing", "correction", false);
        require(!literal.empty() &&
                    user_dictionary::is_fixed(journal, quanpin::join_segments(literal.front()), key, selected.word),
                "With autocorrection off, laing must pin under its literal context.");
    }

    // 方案与词典按同一条规则读掩码：只带删除 / 插入位的掩码在词典侧按「关」处理，方案也不能
    // 跑别名层，否则预编辑显示纠正、候选却按未纠正查。
    {
        QuanpinScheme scheme;
        scheme.set_raw_input("sahng", "sahng");
        scheme.set_autocorrect_types(quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion);
        require(scheme.build_request().segmentation != "shang",
                "A mask without the user-facing bits must not run the alias layer in the scheme.");
        scheme.set_autocorrect_types(quanpin::kAutocorrectTransposition);
        require(scheme.build_request().segmentation == "shang",
                "The transposition switch must run the alias layer in the scheme.");
    }

    std::filesystem::remove_all(directory);
}

// 合法输入的等音节数字面切分按整句打分争领衔（arbitrate_literal_segmentations）。
// yibananzhao 的最少音节规则平局时取较短首音节，落在 yi'ba'nan'zhao；fixture 只给
// yi'ban'an'zhao 一侧词组（一般、按照），另一侧只有单字。词库目录没有 sc.lm，词格退回
// 启发式打分（单字 ln(w/1e6)、词组 ln(w)+3×音节数），分差完全由权重决定，用例因此确定。
void run_literal_segmentation_arbitration_tests(const std::filesystem::path &data_directory)
{
    require(quanpin::enumerate_min_segmentations("yibananzhao", 8).size() == 2,
            "Both four-syllable readings of yibananzhao must be enumerated.");
    require(quanpin::enumerate_min_segmentations("nihao", 8).size() == 1,
            "An unambiguous input must have exactly one minimal segmentation.");

    const std::filesystem::path directory = data_directory / "literal-segmentation";
    std::filesystem::create_directories(directory);
    {
        Database database(directory / "msime.db");
        database.execute("CREATE TABLE tbl_2_y(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_y VALUES('yi''ban', 'yb', '一般', 1000);");
        database.execute("CREATE TABLE tbl_2_a(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_a VALUES('an''zhao', 'az', '按照', 1000);");
        database.execute("CREATE TABLE tbl_1_y(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_y VALUES('yi', 'y', '一', 1000);");
        database.execute("CREATE TABLE tbl_1_b(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_b VALUES('ba', 'b', '把', 1000);");
        database.execute("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_n VALUES('nan', 'n', '难', 1000);");
        database.execute("CREATE TABLE tbl_1_z(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_z VALUES('zhao', 'z', '找', 1000);");
        // fanganhao：fan'gan'hao（主切）与 fang'an'hao 只差 ln(1000/600)=0.5，主切领衔，
        // 另一读法的整句在分差以内，占第 2 位。
        database.execute("CREATE TABLE tbl_2_f(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_f VALUES('fan''gan', 'fg', '反感', 1000);"
                         "INSERT INTO tbl_2_f VALUES('fang''an', 'fa', '方案', 600);");
        database.execute("CREATE TABLE tbl_1_h(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_h VALUES('hao', 'h', '好', 1000);");
    }

    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;
    SentenceAssociationOptions lattice_only;
    lattice_only.word_lattice = true;
    const auto has_word = [](const metasequoia::InputSession &target, const std::string &word) {
        const auto &items = target.candidates();
        return std::any_of(items.begin(), items.end(), [&](const WordItem &item) { return item.word == word; });
    };

    // 另一种切分的整句分高出一截：它领衔，预编辑跟着它分；原切分的整句分差太远，不占第 2 位。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0, true, true, true, paths);
        session.set_sentence_association(lattice_only);
        type(session, "yibananzhao");
        require(!session.candidates().empty() && session.candidates().front().word == "一般按照",
                "The better-scoring literal segmentation must lead.");
        require(session.get_pinyin_segmentation_with_cases() == "yi'ban'an'zhao",
                "The preedit must follow the leading segmentation.");
        require(!has_word(session, "一把难找"), "A far-behind literal sentence must not be listed.");
        (void)session.select_candidate(candidate_index(session, "一般"));
        require(session.has_composition() && session.preedit() == "anzhao" &&
                    session.candidates().front().word == "按照",
                "Selecting a prefix of the leading segmentation must leave the rest of it.");
    }
    // 整句来源全关：没有整句可比，与改造前一致，仍按最少音节切分。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0, true, true, true, paths);
        type(session, "yibananzhao");
        require(session.get_pinyin_segmentation_with_cases() == "yi'ba'nan'zhao" && !has_word(session, "一般按照"),
                "Without any sentence source the segmentation must not be re-arbitrated.");
    }
    // 主切领衔、另一读法分差以内：它的整句紧跟在第 2 位。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0, true, true, true, paths);
        session.set_sentence_association(lattice_only);
        type(session, "fanganhao");
        const auto &candidates = session.candidates();
        require(candidates.size() >= 2 && candidates[0].word == "反感好" && candidates[1].word == "方案好",
                "A close runner-up segmentation must take the second slot.");
        require(session.get_pinyin_segmentation_with_cases() == "fan'gan'hao",
                "The preedit must stay on the leading segmentation.");
    }

    std::filesystem::remove_all(directory);
}

// 多字、漏字纠错的读音和原始字母不等长，选词时要按纠错切分记录的原始区间消耗字母，
// 不能按读音长度：buuhui 选「不会」曾剩下一个 i，选「张」会把 zhngg 一起吞掉。
void run_autocorrect_selection_consumption_tests(const std::filesystem::path &data_directory)
{
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
    const unsigned all_types = both | quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;
    require(quanpin::corrected_reading_raw_length("buuhui", {"bu", "hui"}, all_types) == std::optional<size_t>(6),
            "An insertion correction must cover every typed letter.");
    // 多出的 u 归前归后两可（buu -> bu 或 uhui -> hui，代价相同），两种都对，只要剩下的
    // 部分仍能纠回「会」；不能出现的是按读音长度把 hui 吞掉一截。
    const auto prefix_length = quanpin::corrected_reading_raw_length("buuhui", {"bu"}, all_types);
    require(prefix_length == std::optional<size_t>(2) || prefix_length == std::optional<size_t>(3),
            "A corrected prefix must cover exactly the letters of one explanation.");
    require(quanpin::corrected_reading_raw_length("zhngguo", {"zhang"}, all_types) == std::optional<size_t>(4),
            "A deletion correction must cover only the letters actually typed.");
    require(!quanpin::corrected_reading_raw_length("nihao", {"ni"}, all_types).has_value(),
            "A plain prefix of a legal input must keep the reading-length consumption.");
    // 贪心切分在 jin 上卡死、要回溯才切得开的合法输入：曾被判成手误，「再」按 zaij -> zai 吞掉 j。
    require(quanpin::is_complete_pinyin_input("zaijinuta") && quanpin::looks_like_jianpin_spelling("zaijinut"),
            "A legal input that needs backtracking must still count as legal.");
    require(!quanpin::corrected_reading_raw_length("zaijinuta", {"zai"}, all_types).has_value(),
            "A plain prefix of a backtracking-only legal input must keep the reading-length consumption.");

    const std::filesystem::path directory = data_directory / "autocorrect-selection-consumption";
    std::filesystem::create_directories(directory);
    {
        Database database(directory / "msime.db");
        database.execute("CREATE TABLE tbl_2_b(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_b VALUES('bu''hui', 'bh', '不会', 1000);");
        database.execute("CREATE TABLE tbl_1_b(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_b VALUES('bu', 'b', '不', 900000);");
        database.execute("CREATE TABLE tbl_1_h(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_h VALUES('hui', 'h', '会', 900000);"
                         "INSERT INTO tbl_1_h VALUES('hao', 'h', '好', 900000);");
        database.execute("CREATE TABLE tbl_1_z(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_z VALUES('zhang', 'z', '张', 900000);"
                         "INSERT INTO tbl_1_z VALUES('zai', 'z', '再', 900000);");
        database.execute("CREATE TABLE tbl_1_j(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_j VALUES('ji', 'j', '激', 900000);");
        database.execute("CREATE TABLE tbl_1_t(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_t VALUES('ta', 't', '他', 900000);");
        database.execute("CREATE TABLE tbl_1_g(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_g VALUES('guo', 'g', '国', 900000);");
        database.execute("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_n VALUES('ni', 'n', '你', 900000);"
                         "INSERT INTO tbl_1_n VALUES('nu', 'n', '怒', 900000);"
                         "INSERT INTO tbl_1_n VALUES('niu', 'n', '牛', 900000);");
        database.execute("CREATE TABLE tbl_2_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', '你好', 1000);");
    }

    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;

    // 多打一个 u：整词上屏后不能剩下 i。多字位独立后要显式打开。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, all_types, true, true, true, paths);
        type(session, "buuhui");
        const auto committed = session.select_candidate(candidate_index(session, "不会"));
        require(committed.commit == "不会" && !session.has_composition(),
                "Selecting the insertion-corrected word must consume the whole input.");
    }
    // 只选前缀「不」：剩下的部分（hui 或 uhui）仍要能出「会」。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, all_types, true, true, true, paths);
        type(session, "buuhui");
        (void)session.select_candidate(candidate_index(session, "不"));
        require(session.has_composition() && (session.preedit() == "hui" || session.preedit() == "uhui") &&
                    find_candidate_index(session, "会") < session.candidates().size(),
                "After a corrected prefix the rest must still read as hui.");
    }
    // 漏打一个 a：选「张」只消耗 zhng，剩下 guo。漏字位独立后要显式打开。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, all_types, true, true, true, paths);
        type(session, "zhngguo");
        (void)session.select_candidate(candidate_index(session, "张"));
        require(session.has_composition() && session.preedit() == "guo",
                "A deletion-corrected prefix must not swallow the following letters.");
    }
    // 合法输入照旧按读音长度消耗。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "nihao");
        (void)session.select_candidate(candidate_index(session, "你"));
        require(session.has_composition() && session.preedit() == "hao",
                "A plain prefix of a legal input must keep its consumption.");
    }
    // 要回溯才切得开的合法输入不进纠错：预编辑按字面切分，选「再」剩 jinuta。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, all_types, true, true, true, paths);
        type(session, "zaijinuta");
        require(session.get_pinyin_segmentation_with_cases() == "zai'ji'nu'ta",
                "A legal input must be displayed by its literal segmentation.");
        (void)session.select_candidate(candidate_index(session, "再"));
        require(session.has_composition() && session.preedit() == "jinuta",
                "Selecting a literal prefix must not consume letters of the next syllable.");
    }

    std::filesystem::remove_all(directory);
}

// 阶段 2 生成式纠错空间（任务 quanpin-autocorrect-generated-space）：静态表
// 形状之外的单编辑手误由生成式索引兜底，权重落贵档（15）。本任务把生成式的
// 「任意字母插入」分片移除（远键插入不再是纠错形状），只保留远键替换。
// 隔离 fixture：
// - shatg = shang 的 n→t（t 非邻键，远键替换）
// - schang：远键插入（c 既非 s 的邻键也非 h 的邻键），本任务后不得再纠错
//   （移除断言；不用 shxang——它同时是 shuang 的远键替换，会留下替换读法）
// - chng 同键双读：chng→chang（静态漏字 11）与 chng→cang（生成 15），
//   静态最优时生成切分整体丢弃——静态优先语义的判别性断言
// - shatngzh = head shatng + 简拼尾 zh：head 组合路径同享静态优先
void run_autocorrect_generated_space_tests(const std::filesystem::path &data_directory)
{
    const std::filesystem::path directory = data_directory / "autocorrect-generated";
    std::filesystem::create_directories(directory);
    {
        Database database(directory / "msime.db");
        database.execute("CREATE TABLE tbl_1_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_s VALUES('shang', 's', '上', 900);");
        database.execute("CREATE TABLE tbl_1_c(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_c VALUES('chang', 'c', '长', 900);"
                         "INSERT INTO tbl_1_c VALUES('cang', 'c', '仓', 5000);"
                         "INSERT INTO tbl_1_c VALUES('chu', 'c', '出', 900);"
                         "INSERT INTO tbl_1_c VALUES('chua', 'c', '欻', 900);");
        database.execute("CREATE TABLE tbl_2_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_s VALUES('shang''zh', 'sz', '上周', 900);");
        // huanzh/huansh/huanzg 的明文读法都是 huan + 简拼尾，生成式远键读法（head
        // huanz/huans 经 z→g / s→g 成 huang）各放一个单字：明文给 欢，被拒的组合读法
        // 若出现会带来 黄。
        database.execute("CREATE TABLE tbl_1_h(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_h VALUES('huan', 'h', '欢', 900);"
                         "INSERT INTO tbl_1_h VALUES('huang', 'h', '黄', 900);");
        // 自洽明文负例的判别夹具：「明文 head 音节」与「被拒的邻键 head 音节」各一个字。
        // 规则 3 生效时只有前者在场；若组合纠错发生，后者（xia/dia/...）会顶替它。
        // jv/lve 系按词库正键写法登记（jv 归一为 ju、jve 归一为 jue）。
        database.execute("CREATE TABLE tbl_1_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_x VALUES('xi', 'x', '西', 900);"
                         "INSERT INTO tbl_1_x VALUES('xia', 'x', '下', 900);");
        database.execute("CREATE TABLE tbl_1_d(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_d VALUES('di', 'd', '低', 900);"
                         "INSERT INTO tbl_1_d VALUES('dia', 'd', '嗲', 900);");
        database.execute("CREATE TABLE tbl_1_g(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_g VALUES('gu', 'g', '古', 900);"
                         "INSERT INTO tbl_1_g VALUES('gua', 'g', '挂', 900);");
        database.execute("CREATE TABLE tbl_1_j(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_j VALUES('ji', 'j', '机', 900);"
                         "INSERT INTO tbl_1_j VALUES('jia', 'j', '家', 900);"
                         "INSERT INTO tbl_1_j VALUES('ju', 'j', '句', 900);"
                         "INSERT INTO tbl_1_j VALUES('jue', 'j', '决', 900);");
        database.execute("CREATE TABLE tbl_1_q(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_q VALUES('qu', 'q', '去', 900);"
                         "INSERT INTO tbl_1_q VALUES('que', 'q', '却', 900);");
        database.execute("CREATE TABLE tbl_1_z(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_z VALUES('zhu', 'z', '主', 900);"
                         "INSERT INTO tbl_1_z VALUES('zhua', 'z', '抓', 900);");
        database.execute("CREATE TABLE tbl_1_l(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_l VALUES('lv', 'l', '绿', 900);"
                         "INSERT INTO tbl_1_l VALUES('lve', 'l', '略', 900);");
        database.execute("CREATE TABLE tbl_3_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_3_s VALUES('sha''tang''zh', 'stz', '沙汤扎', 900);");
    }

    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
    // chng 是漏字形状（chang 掉了 a）：这一条要产品全开语义，四个开关都开。
    const unsigned all_types = both | quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;

    // 远键替换：shatg → shang（生成对，贵档 15）。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "shatg");
        const auto found = find_candidate_index(session, "上");
        require(found < session.candidates().size() && session.candidates()[found].corrected_from == "shatg",
                "A far-substitution typo must be corrected to 上 with the typed letters recorded.");
    }

    // 远键插入（③）已随本任务移除：schang（c 既非 s 的邻键也非 h 的邻键，且不是
    // 任何合法音节的远键替换）必须不再有纠错读法。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "schang");
        require(quanpin::autocorrect_cut("schang", both).empty(),
                "Far-key insertions were removed: schang must have no correction cut.");
        require(std::none_of(session.candidates().begin(), session.candidates().end(),
                             [](const WordItem &item) { return !item.corrected_from.empty(); }),
                "Far-key insertions were removed: schang must produce no corrected candidate.");
    }

    // 静态优先语义：chng = chang 漏 a。读法竞争——chng→chang（静态漏字 11）与
    // chng→cang（生成远键替换 h→a，15）。最优切分为纯静态时生成切分整体丢弃：
    // 仓不出现（混表加权仲裁依赖训练权重，方向阶段 3），长照常领衔。
    // 漏字位独立后要显式打开，否则这一条断言的是「没开漏字纠错」。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, all_types, true, true, true, paths);
        type(session, "chng");
        require(candidate_index(session, "长") == 0, "The in-table cheaper deletion reading must lead for chng.");
        require(
            find_candidate_index(session, "仓") == session.candidates().size(),
            "Static-priority semantics: the generated cang reading must not surface when a static cut tops the input.");
    }

    // head+简拼尾组合路径的静态优先：shatngzh 在整串 k-best 上无解（尾部 zh 非
    // 完整音节），走 head 重试。head shatng 的读法是 sha+tng→tang（静态漏字 11）。
    // 生成式插入已随本任务移除，shang 读法（旧生成插入 15）不再存在。静态读法的
    // 沙汤扎（sha'tang'zh 精确键）在场，上周（shang+zh）不得出现。
    // 漏字位独立后要显式打开，否则 head 没有读法，组合路径根本进不去。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, all_types, true, true, true, paths);
        type(session, "shatngzh");
        require(find_candidate_index(session, "沙汤扎") < session.candidates().size(),
                "The head-composition path must still correct shatng to sha+tang.");
        require(find_candidate_index(session, "上周") == session.candidates().size(),
                "Static priority must hold on the head path: the generated shang+zh reading must not surface.");
    }

    // head+简拼尾组合路径与整串同掩码：邻键 head 照常组合。shabg 的读法是 b→n
    // 邻键替换（静态表 13），接上简拼尾 z 得 shang'z，命中上周。改动前 head 掩码
    // 剥掉邻键位、shabg 无解，这条读法整个不存在（只剩 'sha' 前缀扫出的上）。
    // 掩码按产品可达值给（engine.cpp 会把漏字/多字位与两个旧开关捆绑）：两个开关
    // 都开的 0xf 组合，只开换位开关的 0xd 不组合。
    {
        const unsigned transposition_only_product =
            quanpin::kAutocorrectTransposition | quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;
        metasequoia::InputSession session(SchemeType::Quanpin, both | transposition_only_product, true, true, true,
                                          paths);
        type(session, "shabgz");
        const auto composed = find_candidate_index(session, "上周");
        require(composed == 0 && session.candidates()[composed].pinyin == "shang'z" &&
                    session.candidates()[composed].corrected_from == "shabgz",
                (std::string("A neighbor head must compose with a jianpin tail: shabgz -> shang'z, preedit ") +
                 session.get_pinyin_segmentation_with_cases())
                    .c_str());

        // 邻键位关掉则 head 无读法，组合不发生：只剩 'sha' 前缀扫出的上，不带纠错标记。
        metasequoia::InputSession without_neighbor(SchemeType::Quanpin, transposition_only_product, true, true, true,
                                                   paths);
        type(without_neighbor, "shabgz");
        require(std::none_of(without_neighbor.candidates().begin(), without_neighbor.candidates().end(),
                             [](const WordItem &item) { return !item.corrected_from.empty(); }),
                "Without the neighbor bit the neighbor-headed input must not compose.");
    }

    // 生成式远键读法不上 head：huanzh/huansh/huanzg 的正确拼写都是「完整音节 huan +
    // 简拼尾」，但邻居位里的生成式空间带 z→g / s→g 远键替换，经最短尾切分（tail "h"、
    // 或 huanzg 的 "g"）会把 head "huanz"/"huans" 读成 huang，组合出 huang'h 并顶掉明文
    // 切分（代价档仲裁不了「未纠正读法」）。组合路径只接受纯静态 top cut——k-best 静态
    // 优先 ⇒ front 带 has_generated 等价于「无静态读法解释 head」——故探到生成读法即跳过。
    // 两个产品可达的全开掩码 0xe（只开邻键开关）/0xf（两个开关都开）都保持明文：欢是
    // 明文读法的候选且无纠错标记，黄只可能来自被拒的组合读法，一个都不许带纠错标记。
    {
        const unsigned neighbor_product =
            quanpin::kAutocorrectNeighbor | quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;
        for (const std::string spelled : {"huanzh", "huansh", "huanzg"})
        {
            for (const unsigned mask : {neighbor_product, both | neighbor_product})
            {
                metasequoia::InputSession session(SchemeType::Quanpin, mask, true, true, true, paths);
                type(session, spelled);
                const auto plain = find_candidate_index(session, "欢");
                require(plain < session.candidates().size() && session.candidates()[plain].corrected_from.empty() &&
                            session.candidates()[plain].pinyin.rfind("huan", 0) == 0,
                        (std::string("A correctly spelled head with a jianpin tail must stay plain (") + spelled +
                         "), preedit " + session.get_pinyin_segmentation_with_cases())
                            .c_str());
                require(find_candidate_index(session, "黄") == session.candidates().size(),
                        (std::string("A generated far-key head reading must not rewrite a correctly spelled input (") +
                         spelled + ").")
                            .c_str());
                require(std::none_of(session.candidates().begin(), session.candidates().end(),
                                     [&](const WordItem &item) { return item.corrected_from == spelled; }),
                        (std::string("No candidate may carry a correction for a correctly spelled ") + spelled + ".")
                            .c_str());
            }
        }
    }

    // 自洽明文读法优先（规则 3）：head 是完整合法音节、尾是合法简拼尾时，输入本身有
    // 明文读法，组合纠错整个让位。判别用词库层切分——两态实测（改动前 = 临时还原规则 3
    // 重建）：0xe/0xf 下「完整音节 + sh/zh」被最短尾拆成 s/h、z/h 再经静态邻键凑成合法
    // 音节（xish→xia'h、jvsh→jve'h 等），规则 3 后回到明文；0x0/0xd 两态逐位一致。会话
    // 层再确认明文候选在场、被拒的邻键候选不在场（fixture 各放了一个字）。
    {
        const unsigned neighbor_product =
            quanpin::kAutocorrectNeighbor | quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;
        struct SelfConsistentCase
        {
            const char *input;
            const char *plain_segmentation;
            const char *plain_word;
            const char *rewritten_word;
        };
        for (const SelfConsistentCase &test :
             {SelfConsistentCase{"xish", "xi'sh", "西", "下"}, SelfConsistentCase{"dish", "di'sh", "低", "嗲"},
              SelfConsistentCase{"gush", "gu'sh", "古", "挂"}, SelfConsistentCase{"jish", "ji'sh", "机", "家"},
              SelfConsistentCase{"qush", "qu'sh", "去", "却"}, SelfConsistentCase{"zhush", "zhu'sh", "主", "抓"},
              SelfConsistentCase{"chush", "chu'sh", "出", "欻"}, SelfConsistentCase{"lvsh", "lv'sh", "绿", "略"},
              SelfConsistentCase{"jvsh", "ju'sh", "句", "决"}})
        {
            for (const unsigned mask : {neighbor_product, both | neighbor_product})
            {
                QuanpinDictionary dictionary(metasequoia::path_to_utf8(directory / "msime.db"));
                (void)dictionary.query(test.input, "", mask);
                require(dictionary.get_pinyin_segmentation() == test.plain_segmentation,
                        (std::string("A self-consistent plain reading must survive the tail path (") + test.input +
                         " -> " + dictionary.get_pinyin_segmentation() + ").")
                            .c_str());
                metasequoia::InputSession session(SchemeType::Quanpin, mask, true, true, true, paths);
                type(session, test.input);
                const auto plain = find_candidate_index(session, test.plain_word);
                require(plain < session.candidates().size() && session.candidates()[plain].corrected_from.empty(),
                        (std::string("The plain head must stay an unmarked candidate (") + test.input + ").").c_str());
                require(find_candidate_index(session, test.rewritten_word) == session.candidates().size(),
                        (std::string("A rewritten neighbour head must not surface (") + test.input + ").").c_str());
            }
        }
    }

    // 不变项、正例护栏与回归集（掩码取四个独立开关的组合；下文的 0xd 读作
    // 「换位 + 漏字 + 多字」、0xe 读作「邻键 + 漏字 + 多字」，它们现在是掩码空间里的
    // 三个位组合，不再是产品的全部可达集合）：
    // - shabgz：无邻键位（head shabg 无读法）不组合，含邻键位时组合出 shang'z ——
    //   本任务的正面目标，必须仍成立（防规则 3 误杀）。
    // - hauzh/hauz：结构性手误的组合不受规则 1/2/3 影响，换位+漏字+多字下仍是 hua'zh/hua'z；只开
    //   邻键（没有换位读法）时 head "hau" 走邻键 u→i 读成 hai，组合出 hai'zh/hai'z ——
    //   规则 1 在纯邻键掩码上的新增面（换位读法更便宜时都不走这条）。
    // - huazh/guazh/bazh：「完整音节 + zh 尾」由规则 3 回到明文。改动前这三条在含多字 /
    //   漏字位的掩码上都是 hua'h/gua'h/ba'h（head "huaz" 这类被多字位读成 hua），与邻键位无关
    //   ——所以纯多字这一档也能判别规则 3，是自洽明文类里唯一在无邻键掩码下有信号的样本。
    // - xibd/tamd/zhemd：「完整音节 + 两个简拼声母」，head（xib = xi + b）本身过简拼形状门，
    //   组合让位。不挡的话含邻键位的掩码下最短尾 d 留下 head xib，静态邻键 b->n 把它读成 xin，
    //   组合出 xin'd 顶掉明文（旧的邻键掩码挡的就是这一类）。
    // - 到不了组合路径的整串输入（sahng/shng/keneng/nbn）四组合掩码与改动前逐位一致。
    {
        const unsigned transposition_product =
            quanpin::kAutocorrectTransposition | quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;
        const unsigned neighbor_product =
            quanpin::kAutocorrectNeighbor | quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;
        struct SegmentationCase
        {
            const char *input;
            unsigned mask;
            const char *segmentation;
        };
        const SegmentationCase cases[] = {
            {"shabgz", transposition_product, "sha'b'g'z"},
            {"shabgz", neighbor_product, "shang'z"},
            {"shabgz", both | neighbor_product, "shang'z"},
            {"hauzh", neighbor_product, "hai'zh"},
            {"hauz", neighbor_product, "hai'z"},
            {"hauzh", both | neighbor_product, "hua'zh"},
            {"hauz", both | neighbor_product, "hua'z"},
            {"hauzh", transposition_product, "hua'zh"},
            {"hauz", transposition_product, "hua'z"},
            {"huazh", both | neighbor_product, "hua'zh"},
            {"huazh", transposition_product, "hua'zh"},
            {"guazh", transposition_product, "gua'zh"},
            {"bazh", transposition_product, "ba'zh"},
            {"xibd", transposition_product, "xi'b'd"},
            {"xibd", neighbor_product, "xi'b'd"},
            {"xibd", both | neighbor_product, "xi'b'd"},
            {"tamd", neighbor_product, "ta'm'd"},
            {"tamd", both | neighbor_product, "ta'm'd"},
            {"zhemd", neighbor_product, "zhe'm'd"},
            {"zhemd", both | neighbor_product, "zhe'm'd"},
            {"sahng", 0u, "sa'h'n'g"},
            {"sahng", transposition_product, "shang"},
            {"sahng", neighbor_product, "shang"},
            {"sahng", both | neighbor_product, "shang"},
            {"shng", 0u, "sh'n'g"},
            {"shng", transposition_product, "shang"},
            {"shng", neighbor_product, "shang"},
            {"shng", both | neighbor_product, "shang"},
            {"keneng", 0u, "ke'neng"},
            {"keneng", transposition_product, "ke'neng"},
            {"keneng", neighbor_product, "ke'neng"},
            {"keneng", both | neighbor_product, "ke'neng"},
            {"nbn", 0u, "n'b'n"},
            {"nbn", transposition_product, "n'b'n"},
            {"nbn", neighbor_product, "nan"},
            {"nbn", both | neighbor_product, "nan"},
        };
        for (const SegmentationCase &test : cases)
        {
            QuanpinDictionary dictionary(metasequoia::path_to_utf8(directory / "msime.db"));
            (void)dictionary.query(test.input, "", test.mask);
            require(dictionary.get_pinyin_segmentation() == test.segmentation,
                    (std::string("Segmentation regression for ") + test.input + ": expected " + test.segmentation +
                     ", got " + dictionary.get_pinyin_segmentation())
                        .c_str());
        }
    }

    // 守卫与合法输入：尾位插入（shangv 类）受简拼尾守卫——纠错不触发由
    // test_pinyin 的 zheg 用例与输入门不变式覆盖，此处不重复；合法拼写原样直出。
    {
        metasequoia::InputSession legal(SchemeType::Quanpin, both, true, true, true, paths);
        type(legal, "shang");
        require(candidate_index(legal, "上") == 0 && legal.candidates()[0].corrected_from.empty(),
                "A legal spelling must stay uncorrected.");
    }
}

// 纯简拼形输入（每个字母都是一个声母）如果词库里真有它的简拼整词，由字面整词领衔：
// nbn/nqn/bqng 分别命中词库 jp 列的 能不能/女青年/并且能够，纠错读法（难/帮）只排在
// 后面，仍可选、仍带纠错标记。预编辑按字面简拼切分，选前缀单字只消耗它自己的字母。
// 漏了元音的 bng 即使有简拼整词「不能够」，「帮」也不能丢。shng 含双字母切分 sh，
// 不是纯简拼，漏字纠错照旧。
void run_autocorrect_pure_jianpin_guard_tests(const std::filesystem::path &data_directory)
{
    const std::filesystem::path directory = data_directory / "autocorrect-pure-jianpin";
    std::filesystem::create_directories(directory);
    {
        Database database(directory / "msime.db");
        database.execute("CREATE TABLE tbl_1_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_s VALUES('shang','s','上',900);"
                         "CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_n VALUES('nan','n','难',900);"
                         "INSERT INTO tbl_1_n VALUES('neng','n','能',800);"
                         "CREATE TABLE tbl_1_b(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_b VALUES('bang','b','帮',900);"
                         "CREATE TABLE tbl_3_b(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_3_b VALUES('bu''neng''gou','bng','不能够',2000);"
                         "CREATE TABLE tbl_3_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_3_n VALUES('neng''bu''neng','nbn','能不能',5000);"
                         "INSERT INTO tbl_3_n VALUES('nv''qing''nian','nqn','女青年',3000);"
                         "CREATE TABLE tbl_4_b(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_4_b VALUES('bing''qie''neng''gou','bqng','并且能够',4000);");
    }

    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;
    const unsigned all = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor |
                         quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;

    struct Case
    {
        const char *input;
        const char *word;
        const char *display;
        const char *corrected;
    };
    for (const Case &test : {Case{"nbn", "能不能", "n'b'n", "难"}, Case{"nqn", "女青年", "n'q'n", "难"},
                             Case{"bqng", "并且能够", "b'q'n'g", "帮"}, Case{"bng", "不能够", "b'n'g", "帮"}})
    {
        metasequoia::InputSession session(SchemeType::Quanpin, all, true, true, true, paths);
        type(session, test.input);
        require(!session.candidates().empty() && session.candidates().front().word == test.word &&
                    session.candidates().front().corrected_from.empty(),
                (std::string("A pure-jianpin whole word must lead, uncorrected: ") + test.input).c_str());
        const auto corrected = find_candidate_index(session, test.corrected);
        require(corrected < session.candidates().size() && session.candidates()[corrected].corrected_from == test.input,
                (std::string("The corrected reading must stay behind the jianpin word: ") + test.input).c_str());
        require(session.get_pinyin_segmentation_with_cases() == test.display,
                (std::string("The preedit must follow the literal jianpin reading: ") + test.input + " -> " +
                 session.get_pinyin_segmentation_with_cases())
                    .c_str());
    }

    // 选纯简拼的前缀单字只消耗它自己的声母，后面的 bn 留着继续输入。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, all, true, true, true, paths);
        type(session, "nbn");
        const auto committed = session.select_candidate(candidate_index(session, "能"));
        require(committed.commit == "能" && session.has_composition() && session.preedit() == "bn",
                "Selecting a jianpin prefix character must consume only its own initial.");
    }

    // 含双字母切分的输入不是纯简拼，漏字纠错照旧命中「上」并记下敲入的字母。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, all, true, true, true, paths);
        type(session, "shng");
        const auto found = find_candidate_index(session, "上");
        require(found < session.candidates().size() && session.candidates()[found].corrected_from == "shng",
                "A non-pure-jianpin typo (shng) must still be corrected with the typed letters recorded.");
    }

    // 纠错关时逐位不变：纯简拼输入仍由字面简拼读法领衔，且无纠错标记。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0u, true, true, true, paths);
        type(session, "bqng");
        require(!session.candidates().empty() && session.candidates().front().word == "并且能够" &&
                    session.candidates().front().corrected_from.empty(),
                "With autocorrection off a pure-jianpin reading must still lead, uncorrected.");
    }

    std::filesystem::remove_all(directory);
}

void run_caret_prefix_session_tests(const std::filesystem::path &data_directory)
{
    const std::filesystem::path directory = data_directory / "caret-prefix";
    std::filesystem::create_directories(directory);
    {
        Database database(directory / "msime.db");
        database.execute("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_n VALUES('ni','n','你',100);"
                         "INSERT INTO tbl_1_n VALUES('ni','n','拟',90);");
        database.execute("CREATE TABLE tbl_2_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_n VALUES('ni''hao','nh','你好',200);"
                         "INSERT INTO tbl_2_n VALUES('ni''hao','nh','拟好',100);");
        database.execute("CREATE TABLE tbl_1_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_s VALUES('shi','sh','是',100);");
        database.execute("CREATE TABLE tbl_1_j(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_j VALUES('jie','j','接',100);");
        database.execute("CREATE TABLE tbl_1_y(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_y VALUES('ya','y','呀',100);"
                         "INSERT INTO tbl_1_y VALUES('ya','y','压',90);");
        database.execute("CREATE TABLE wubi86(key TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO wubi86 VALUES('aaaa','工',100);"
                         "INSERT INTO wubi86 VALUES('aaaa','或',50);");
    }

    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;

    const auto words_of = [](const metasequoia::InputSession &session) {
        std::vector<std::string> words;
        words.reserve(session.candidates().size());
        for (const WordItem &item : session.candidates())
        {
            words.push_back(item.word);
        }
        return words;
    };
    const auto same_word_list = [](const metasequoia::InputSession &session, const std::vector<std::string> &expected) {
        if (session.candidates().size() != expected.size())
        {
            return false;
        }
        return std::equal(session.candidates().begin(), session.candidates().end(), expected.begin(),
                          [](const WordItem &left, const std::string &right) { return left.word == right; });
    };

    // R2/R3/R7：音节内 caret 向下取整到最后一个完整单元边界，候选等于前缀的候选；
    // caret 未设置或落在末尾时与现状整串解码零差异。
    const std::string sentence = "ni'hao'shi'jie";
    const auto quanpin_words = [&](const std::string &typed) {
        metasequoia::InputSession other(SchemeType::Quanpin, 0, true, true, false, paths);
        type(other, typed);
        return words_of(other);
    };
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0, true, true, false, paths);
        type(session, sentence);
        require(session.has_composition() && session.prefix_end() == sentence.size() &&
                    session.pending_suffix().empty(),
                "An unset caret must decode the whole string");
        const auto full_words = words_of(session);
        require(same_word_list(session, full_words),
                "The unset-caret baseline diverged from a plain typed composition");

        for (const std::size_t caret : {std::size_t(5), std::size_t(6)})
        {
            session.set_caret(caret);
            session.recompute_candidates();
            require(session.prefix_end() == 3, "An intra-syllable caret must floor to the last complete unit boundary");
            require(session.pending_suffix() == "hao'shi'jie",
                    "The pending suffix must keep the raw spelling the decode did not consume");
            require(candidate_index(session, "你") < session.candidates().size(),
                    "The 'ni' prefix lost its dictionary candidates");
            require(same_word_list(session, quanpin_words("ni")),
                    "The floored prefix must decode exactly like the typed prefix spelling");
        }

        // 首单元内侧不是串首：向上取整到首单元末尾，与 caret 落在 ni 边界上解码相同。
        for (const std::size_t caret : {std::size_t(1), std::size_t(2)})
        {
            session.set_caret(caret);
            session.recompute_candidates();
            require(session.prefix_end() == 3 && session.pending_suffix() == "hao'shi'jie",
                    "A caret inside the first unit must round up to the end of that unit");
            require(same_word_list(session, quanpin_words("ni")),
                    "The rounded-up first unit must decode exactly like the typed spelling");
        }

        session.set_caret(7);
        session.recompute_candidates();
        require(session.prefix_end() == 7 && session.pending_suffix() == "shi'jie",
                "A caret on the hao boundary must consume ni'hao");
        require(candidate_index(session, "你好") < session.candidates().size(),
                "The ni'hao prefix lost its phrase candidates");
        require(same_word_list(session, quanpin_words("ni'hao")),
                "The ni'hao prefix must decode exactly like a typed ni'hao");
        const auto at_hao = words_of(session);
        session.set_caret(9);
        session.recompute_candidates();
        require(session.prefix_end() == 7 && same_word_list(session, at_hao),
                "A caret inside 'shi' must floor back to the hao boundary");

        session.set_caret(13);
        session.recompute_candidates();
        require(session.prefix_end() == 11 && session.pending_suffix() == "jie",
                "A caret inside 'jie' must floor to the shi boundary");
        require(!session.candidates().empty() && same_word_list(session, quanpin_words("ni'hao'shi")),
                "The ni'hao'shi prefix must decode exactly like the typed spelling");

        // R4：量化后前缀为空 → 无候选，raw/preedit/caret 原样。
        session.set_caret(0);
        session.recompute_candidates();
        require(session.candidates().empty(), "A caret before the first unit must offer no candidate");
        require(session.prefix_end() == 0 && session.pending_suffix() == sentence,
                "An empty prefix must leave the whole string pending");
        require(session.editing_text() == sentence && session.caret_position() == 0 && session.preedit() == sentence,
                "An empty prefix must not disturb the composition or the preedit");

        // 越界 caret 被夹到串尾 → 退化为整串解码（R7）。
        session.set_caret(sentence.size() + 10);
        session.recompute_candidates();
        require(session.caret_position() == sentence.size() && session.prefix_end() == sentence.size(),
                "An out-of-range caret must clamp to the end");
        require(same_word_list(session, full_words), "A caret clamped to the end must restore the full-string decode");
        session.set_caret(std::nullopt);
        session.recompute_candidates();
        require(same_word_list(session, full_words), "Unsetting the caret must restore the full-string decode");
    }

    // 只有一个单元时首单元内侧向上取整就是整串：按整串解码而非空前缀，
    // 候选窗不被 IsCaretPrefixEmpty 收起。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0, true, true, false, paths);
        type(session, "ya");
        require(session.prefix_end() == 2 && !session.candidates().empty(),
                "The unset 'ya' caret must decode the whole string");
        const auto full_words = words_of(session);
        session.set_caret(1);
        session.recompute_candidates();
        require(session.prefix_end() == 2 && session.pending_suffix().empty(),
                "A caret inside the first unit must not be mistaken for the string head");
        require(same_word_list(session, full_words) && same_word_list(session, quanpin_words("ya")),
                "An intra-first-unit caret must decode exactly like the whole 'ya'");
    }

    // pending_suffix 保留原始大小写：大写字母从光标处插入后原样留在后缀里。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0, true, true, false, paths);
        type(session, "ni'hao");
        session.handle_command(metasequoia::Command::MoveHome);
        require(session.handle_character('H').handled, "The uppercase insert at the caret was rejected");
        require(session.editing_text() == "Hni'hao" && session.caret_position() == 1,
                "The uppercase insert lost its case or position");
        session.set_caret(0);
        session.recompute_candidates();
        require(session.prefix_end() == 0 && session.pending_suffix() == "Hni'hao",
                "The pending suffix must preserve the typed casing");
        session.set_caret(std::nullopt);
        session.recompute_candidates();
        require(session.editing_text() == "Hni'hao", "Restoring the end caret altered the raw text");
    }

    // 无单元模型（五笔）：segment_raw_boundaries 为空 → caret 移动不量化，候选零变化。
    {
        metasequoia::InputSession session(SchemeType::Wubi, 0, true, true, false, paths);
        type(session, "aaaa");
        require(session.has_composition() && candidate_index(session, "工") < session.candidates().size(),
                "The wubi fixture lost its candidates");
        const auto native = words_of(session);
        for (const std::size_t caret : {std::size_t(0), std::size_t(2)})
        {
            session.set_caret(caret);
            session.recompute_candidates();
            require(same_word_list(session, native), "A scheme without the unit model must not re-decode by caret");
            require(session.prefix_end() == 4 && session.pending_suffix().empty(),
                    "Without a unit model the caret never shortens the decode");
        }
    }

    // 双拼贪心配对（engine spec #187）：nihkb; → {0,2,4,6}、nihcb; → {0,2,3,5,6}，
    // caret 落在段中间时同样 floor 到完整段边界；首单元内侧向上取整到首单元末尾。
    const auto shuangpin_words = [&](const std::string &typed) {
        metasequoia::InputSession other(SchemeType::Shuangpin, GetMicrosoftShuangpinProfile(), paths);
        type(other, typed);
        return words_of(other);
    };
    {
        metasequoia::InputSession session(SchemeType::Shuangpin, GetMicrosoftShuangpinProfile(), paths);
        type(session, "nihkb;");
        require(candidate_index(session, "你好") < session.candidates().size(),
                "The shuangpin baseline lost its phrase candidates");
        require(session.prefix_end() == 6 && session.pending_suffix().empty(),
                "The unset shuangpin caret must decode the whole string");

        session.set_caret(1);
        session.recompute_candidates();
        require(session.prefix_end() == 2 && session.pending_suffix() == "hkb;" &&
                    same_word_list(session, shuangpin_words("ni")),
                "A caret inside the first shuangpin unit must round up to the end of that unit");

        session.set_caret(3);
        session.recompute_candidates();
        require(session.prefix_end() == 2 && session.pending_suffix() == "hkb;",
                "A caret inside the hk unit must floor to the ni boundary");
        require(same_word_list(session, shuangpin_words("ni")),
                "The shuangpin 'ni' prefix must decode exactly like the typed spelling");

        session.set_caret(5);
        session.recompute_candidates();
        require(session.prefix_end() == 4 && session.pending_suffix() == "b;",
                "A caret inside the b; unit must floor to the hk boundary");
        require(candidate_index(session, "你好") < session.candidates().size(),
                "The shuangpin nihk prefix lost its phrase candidates");
        require(same_word_list(session, shuangpin_words("nihk")),
                "The shuangpin nihk prefix must decode exactly like the typed spelling");

        session.set_caret(6);
        session.recompute_candidates();
        require(session.prefix_end() == 6 && !session.candidates().empty(),
                "A caret on the final shuangpin boundary must restore the full decode");

        session.handle_command(metasequoia::Command::Cancel);
        type(session, "nihcb;");
        session.set_caret(4);
        session.recompute_candidates();
        require(session.prefix_end() == 3 && session.pending_suffix() == "cb;",
                "The greedy pairing boundary must floor the caret to where cb starts");
        require(same_word_list(session, shuangpin_words("nih")),
                "The shuangpin nih prefix must decode exactly like the typed spelling");
    }

    std::filesystem::remove_all(directory);
}
} // namespace

void run_word_final_helpcode_lifecycle_tests(const std::filesystem::path &data_directory)
{
    const auto directory = data_directory / "word-final-lifecycle";
    std::filesystem::create_directories(directory);
    write_file(directory / "helpcodes" / "custom" / "lifecycle.txt", "你=rx\n拟=fr\n好=nz\n湖=gq\n不=ab\n");
    Database database(directory / "msime.db");
    database.execute("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                     "INSERT INTO tbl_1_n VALUES('ni','n','你',200),('ni','n','拟',100);"
                     "CREATE TABLE tbl_1_h(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                     "INSERT INTO tbl_1_h VALUES('hao','h','好',100),('hu','h','湖',100);"
                     "CREATE TABLE tbl_2_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                     "INSERT INTO tbl_2_n VALUES('ni''hao','nh','你好',200);"
                     "CREATE TABLE tbl_2_b(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                     "INSERT INTO tbl_2_b VALUES('bu''hao','bh','不好',100);");
    const metasequoia::RuntimePaths paths{directory, directory, directory, directory};
    const auto make_session = [&](bool direct) {
        auto session =
            std::make_unique<metasequoia::InputSession>(SchemeType::Shuangpin, GetXiaoheShuangpinProfile(), paths);
        session->set_shuangpin_helpcode_enabled(true);
        session->set_shuangpin_helpcode_triggers(direct, !direct);
        require(session->set_helpcode_schema("custom/lifecycle"), "The lifecycle helpcode fixture was not selected.");
        return session;
    };
    const auto check_transition = [&](bool direct, const std::string &raw, const std::string &word,
                                      const std::string &prefix, const std::string &rest, const std::string &tail) {
        auto session = make_session(direct);
        session->set_mid_sentence_helpcode_enabled(true);
        type(*session, raw);
        if (session->candidates().empty())
        {
            throw std::runtime_error("No lifecycle candidates for: " + raw);
        }
        const auto picked = session->candidates()[candidate_index(*session, word)];
        const auto transition = session->advance_composition_after_selection(picked.pinyin, picked.word,
                                                                             picked.canonical_pinyin, picked.scheme);
        require(transition.continues_composition && session->get_pinyin_sequence_with_cases() == rest,
                "Terminal helpcode selection did not preserve the expected suffix.");
        require(transition.consumed_raw_input_with_cases == prefix &&
                    transition.consumed_terminal_helpcode_with_cases == tail && prefix + rest + tail == raw,
                "The prefix/suffix/terminal decomposition was not reversible.");
    };

    for (const bool direct : {false, true})
    {
        // An uppercase first direct code reverses the existing code order.
        for (const std::string codes : {"f", "fR", direct ? "Rf" : "Fr", direct ? "RF" : "FR"})
        {
            const std::string tail = (direct ? std::string() : "|") + codes;
            for (const std::string spelling : {"nihc", "ni'hc"})
            {
                check_transition(direct, spelling + tail, "拟", spelling == "nihc" ? "ni" : "ni'", "hc", tail);
            }
            check_transition(direct, "nihu`ghc" + tail, "拟", "ni", "hu`ghc", tail);
        }
        const std::string marker = direct ? "" : "|";
        check_transition(direct, "nihcbuhc" + marker + "x", "你好", "nihc", "buhc", marker + "x");
        check_transition(direct, "nihcbuhc" + marker + "rN", "你好", "nihc", "buhc", marker + "rN");
        auto missing = make_session(direct);
        type(*missing, "nihc" + marker + "fN");
        require(missing->candidates().empty(), "The lifecycle change generated a new strict-filter candidate.");
    }
    check_transition(false, "nihcFr", "拟", "ni", "hc", "Fr");
    check_transition(false, "nihcF", "拟", "ni", "hc", "F");
    check_transition(false, "nihc", "拟", "ni", "hc", "");
    check_transition(false, "ni`fRhc", "拟", "ni`fR", "hc", "");
    check_transition(false, "nihc|", "拟", "ni", "hc|", "");
    // A block followed by more pinyin is no longer the active terminal block.
    check_transition(false, "nihc|xybuhc|fR", "拟", "ni", "hc|xybuhc", "|fR");

    // A caret-prefix selection keeps the existing suffix layout and consumes
    // no terminal block that belongs to the full-string decode.
    {
        auto session = make_session(false);
        type(*session, "nihc|fR");
        session->set_caret(2);
        session->recompute_candidates();
        const auto picked = session->candidates()[candidate_index(*session, "拟")];
        const auto transition = session->advance_composition_after_selection(picked.pinyin, picked.word,
                                                                             picked.canonical_pinyin, picked.scheme);
        require(transition.continues_composition && transition.consumed_terminal_helpcode_with_cases.empty() &&
                    session->get_pinyin_sequence_with_cases() == "hc|fR",
                "A caret-prefix selection changed the full-string terminal helpcode lifecycle.");
    }

    // Learning must still work when the desired phrase was absent initially.
    for (const bool direct : {false, true})
    {
        auto session = make_session(direct);
        type(*session, direct ? "nihcfR" : "nihc|fR");
        require(session->select_candidate(candidate_index(*session, "拟")).commit == "拟" &&
                    session->get_pinyin_sequence_with_cases() == "hc",
                "The first selection did not continue portable word creation.");
        require(session->select_candidate(candidate_index(*session, "好")).commit == "好" &&
                    !session->has_composition(),
                "The final selection did not complete portable word creation.");
        auto reopened = make_session(direct);
        type(*reopened, "nihc");
        require(candidate_index(*reopened, "拟好") < reopened->candidates().size(),
                "The newly created phrase was not available in the next composition.");
    }
}

int run_test()
{
    const auto unique_suffix = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const std::filesystem::path data_directory =
        std::filesystem::temp_directory_path() / std::filesystem::u8path("metasequoia-session-词库-" + unique_suffix);
    metasequoia::test::ScopedDataDirectoryCleanup cleanup(data_directory);
    std::filesystem::create_directories(data_directory);
    set_data_directory(data_directory);

#ifndef METASEQUOIA_FREQUENCY_TESTS_ONLY
    {
        const std::filesystem::path helpcode_directory = data_directory / "helpcodes";
        write_file(helpcode_directory / "helpcode.txt", "你=ab\n拟=cd\n好=ef\n");
        write_file(helpcode_directory / "zrm_helpcode_big_unique.txt", "你=cb\n拟=ad\n好=ef\n");
        write_file(helpcode_directory / "shouyou2_0_helpcode.txt", "你=ab\n拟=cd\n好=ef\n");
        write_file(helpcode_directory / "shouyouplus_helpcode.txt", "你=ab\n拟=cd\n好=ef\n");
        write_file(helpcode_directory / "xiaohe_helpcode.txt", "你=ab\n拟=cd\n好=ef\n");

        Database database(data_directory / "msime.db");
        database.execute("CREATE TABLE tbl_2_n(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', '你好', 200)");
        database.execute("INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', '拟好', 100)");
        database.execute("CREATE TABLE tbl_2_z(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_2_z VALUES('zhong''guo', 'zg', '中国', 200)");
        database.execute("CREATE TABLE tbl_2_d(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_2_d VALUES('dong''gua', 'dg', '冬瓜', 200)");
        database.execute("INSERT INTO tbl_2_d VALUES('dong''an', 'da', '东安', 200)");
        database.execute("CREATE TABLE tbl_2_b(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_2_b VALUES('bu''hao', 'bh', '不好', 200)");
        database.execute("INSERT INTO tbl_2_b VALUES('bu''hao', 'bh', '补好', 100)");
        database.execute("INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', '𠀀方案𠮷', 90)");
        database.execute("INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', 'C语言 2', 80)");
        database.execute("INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', 'GitHub', 70)");
        database.execute("CREATE TABLE tbl_1_j(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_1_j VALUES('ju', 'j', '居', 100)");
        database.execute("CREATE TABLE tbl_1_q(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_1_q VALUES('qu', 'q', '去', 100)");
        database.execute("CREATE TABLE tbl_1_x(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_1_x VALUES('xu', 'x', '需', 100)");
        database.execute("CREATE TABLE tbl_1_y(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_1_y VALUES('yu', 'y', '与', 100)");

        database.execute("INSERT INTO tbl_1_x VALUES('xi', 'x', '西', 100)");
        database.execute("CREATE TABLE tbl_2_t(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        database.execute("INSERT INTO tbl_2_t VALUES('te''le','tl','特乐',100)");
        database.execute("CREATE TABLE tbl_3_x(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        {
            metasequoia::InputSession portable(SchemeType::Quanpin, true, false);
            type(portable, "xi'te'le");
            const auto first = portable.select_candidate(candidate_index(portable, "西"));
            require(first.commit == "西" && portable.has_composition() && portable.preedit() == "te'le",
                    "Portable selection discarded the unconsumed pinyin suffix.");
            const auto last = portable.select_candidate(candidate_index(portable, "特乐"));
            require(last.commit == "特乐" && !portable.has_composition(),
                    "Portable continuation duplicated the committed prefix or retained completed input.");
            require(database.query_integer("SELECT COUNT(*) FROM tbl_3_x WHERE key='xi''te''le' AND value='西特乐'") ==
                        1,
                    "Portable composition did not learn the canonical phrase across selections.");
            type(portable, "xi'te'le");
            (void)portable.select_candidate(candidate_index(portable, "西"));
            require(portable.handle_command(metasequoia::Command::Cancel).handled && !portable.has_composition(),
                    "Cancel did not clear an incomplete portable phrase.");

            type(portable, "xi'te'le");
            (void)portable.select_candidate(candidate_index(portable, "西"));
            while (portable.has_composition())
            {
                require(portable.handle_command(metasequoia::Command::Backspace).handled,
                        "Backspace did not consume the abandoned portable composition.");
            }
            type(portable, "nihao");
            require(portable.select_candidate(candidate_index(portable, "你好")).commit == "你好",
                    "The composition following an abandoned phrase could not be committed.");
            require(database.query_integer("SELECT COUNT(*) FROM tbl_3_x WHERE value='西你好'") == 0,
                    "A phrase abandoned by Backspace was learned together with the next composition.");
        }
        {
            metasequoia::InputSession unlearned(SchemeType::Quanpin, true, false, true, false);
            type(unlearned, "xi'te'le");
            (void)unlearned.select_candidate(candidate_index(unlearned, "西"));
            const auto punctuation = unlearned.handle_punctuation(',');
            require(punctuation.commit == "特乐，" && !unlearned.has_composition(),
                    "Punctuation failed to finish the remaining portable composition atomically.");
        }

        // 整句候选（词格 / Google 解码器）在词库里没有对应的行，调频无处落笔：用户
        // 选中一条整句多少次，它下次仍然要靠猜，排序也跟着重算。选中即落成用户词组，
        // 之后同样的输入就由词库那一行来回答。
        database.execute("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        database.execute("INSERT INTO tbl_1_n VALUES('na','n','那',100)");
        database.execute("INSERT INTO tbl_1_y VALUES('yi','y','一',100)");
        database.execute("CREATE TABLE tbl_1_t(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        database.execute("INSERT INTO tbl_1_t VALUES('tiao','t','条',100)");
        database.execute("CREATE TABLE tbl_3_n(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        {
            metasequoia::InputSession sentence(SchemeType::Quanpin);
            // 整句联想默认全关，这一段考的就是整句候选，得先把造句那两条打开。
            SentenceAssociationOptions association;
            association.word_lattice = true;
            association.google = true;
            sentence.set_sentence_association(association);
            type(sentence, "na'yi'tiao");
            const auto guessed = candidate_index(sentence, "那一条");
            const auto guessed_source = sentence.candidates()[guessed].source;
            require(guessed_source == CandidateSource::Generated || guessed_source == CandidateSource::Fallback,
                    "The sentence under test must be a guess, not a dictionary row.");
            require(sentence.select_candidate(guessed).commit == "那一条" && !sentence.has_composition(),
                    "Selecting a whole-sentence candidate did not commit it.");
            require(
                database.query_integer("SELECT COUNT(*) FROM tbl_3_n WHERE key='na''yi''tiao' AND value='那一条'") == 1,
                "A selected whole-sentence candidate was not learned as a user phrase.");

            type(sentence, "na'yi'tiao");
            const auto learned = candidate_index(sentence, "那一条");
            require(sentence.candidates()[learned].source != CandidateSource::Generated &&
                        sentence.candidates()[learned].source != CandidateSource::Fallback,
                    "The learned sentence did not come back as a dictionary row.");
            require(learned == 0, "The learned sentence did not outrank the guessed ones.");
            (void)sentence.select_candidate(learned);
        }
        {
            // 学习整句是造词，不是调频：关掉候选学习的会话一条也不该落库。
            metasequoia::InputSession unlearned(SchemeType::Quanpin, 0, true, true, false);
            SentenceAssociationOptions association;
            association.word_lattice = true;
            association.google = true;
            unlearned.set_sentence_association(association);
            type(unlearned, "na'yi'na");
            const auto guessed = candidate_index(unlearned, "那一那");
            (void)unlearned.select_candidate(guessed);
            require(database.query_integer("SELECT COUNT(*) FROM tbl_3_n WHERE value='那一那'") == 0,
                    "A session with candidate learning disabled still stored a whole-sentence candidate.");
        }

        // A seven-syllable key and eight/nine-syllable keys cross the shipping
        // table boundary. Creation, normal query and upgrade replay must agree.
        database.execute("CREATE TABLE tbl_7_n(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        database.execute("CREATE TABLE tbl_others_n(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        const auto replay_path = data_directory / "replay.db";
        const auto journal_path = metasequoia::path_to_utf8(data_directory / "format-journal.db");
        Database replay_database(replay_path);
        replay_database.execute("CREATE TABLE tbl_7_n(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        replay_database.execute("CREATE TABLE tbl_others_n(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        for (const int count : {7, 8, 9})
        {
            std::string key, word;
            for (int i = 0; i < count; ++i)
            {
                if (i)
                    key += "'";
                key += "ni";
                word += "你";
            }
            const std::string expected_table = count == 7 ? "tbl_7_n" : "tbl_others_n";
            require(quanpin::build_table_name(std::vector<std::string>(count, "ni")) == expected_table,
                    "Runtime lookup selected the wrong long-phrase table.");
            metasequoia::InputSession writer(SchemeType::Quanpin);
            require(writer.store_user_phrase_from_canonical_pinyin(key, word) == 0,
                    "Canonical phrase creation could not write the public format.");
            type(writer, key);
            (void)candidate_index(writer, word);
            require(user_dictionary::record_user_insert(journal_path, user_dictionary::DictionaryKind::Pinyin, key,
                                                        word, 10000),
                    "The long-phrase journal operation failed.");
        }
        const auto replay = user_dictionary::replay(journal_path, metasequoia::path_to_utf8(replay_path),
                                                    metasequoia::path_to_utf8(data_directory / "replay-english.db"));
        require(replay.failed == 0 && replay.applied == 3 &&
                    replay_database.query_integer("SELECT COUNT(*) FROM tbl_7_n") == 1 &&
                    replay_database.query_integer("SELECT COUNT(*) FROM tbl_others_n") == 2,
                "Upgrade replay disagreed with the public long-phrase table format.");

        // Hosts that insert text asynchronously use the same composition owner as
        // portable character/command clients. Exercise their sequence boundary here.
        for (const bool raw_dispatch : {false, true})
        {
            metasequoia::InputSession session(SchemeType::Quanpin);
            const std::string input = "xi'te'le";
            if (raw_dispatch)
            {
                session.set_pinyin_sequence(input);
                session.set_pinyin_sequence_with_cases(input);
                session.recompute_candidates();
            }
            else
            {
                type(session, input);
            }
            const auto query_before_selection = session.online_query();
            const auto first = session.advance_composition_after_selection("xi", "西", "xi");
            require(first.continues_composition && session.get_pinyin_sequence() == "te'le",
                    "Partial selection lost the remaining manually delimited input.");
            require(first.consumed_raw_input_with_cases == "xi",
                    "Partial selection did not report the raw input it consumed.");
            const auto progress = session.update_creating_word_progress("", "", "西", first);
            require(!progress.completed && progress.pinyin == "xi" && progress.preedit == "西te'le",
                    "Partial selection produced the wrong phrase progress.");
            require(query_before_selection && !session.apply_online_candidate(*query_before_selection, "旧",
                                                                              CandidateSource::CloudSuggestion),
                    "Partial selection accepted a result for the previous composition.");
            const auto modern_query = session.online_query();
            const auto host_query = session.get_cloud_query_state();
            require(modern_query && modern_query->query_text == host_query.query_text &&
                        modern_query->cache_key == host_query.cache_key,
                    "Portable and asynchronous hosts produced different online queries.");
            const auto last = session.advance_composition_after_selection("te'le", "特乐", "te'le");
            require(!last.continues_composition && last.consumed_raw_input_with_cases == "te'le",
                    "The final selection did not report the raw input it consumed.");
            const auto complete = session.update_creating_word_progress(progress.pinyin, progress.word, "特乐", last);
            require(complete.completed && complete.can_store && complete.pinyin == "xi'te'le" &&
                        complete.word == "西特乐",
                    "The completed phrase did not retain canonical pinyin across selections.");
            const auto invalid = session.update_creating_word_progress("", "西", "特乐", last);
            require(invalid.completed && !invalid.can_store && invalid.pinyin.empty(),
                    "A phrase with an earlier unknown canonical reading became storeable.");
            session.set_pinyin_sequence("pending");
            session.reset_state();
            session.recompute_candidates();
            require(!session.has_composition(), "Reset left a pending host composition alive.");
        }

        // A host that retracts a selected segment replays the reported spelling,
        // so it must keep the casing the user typed rather than reuse the
        // normalized pre-selection raw.
        {
            metasequoia::InputSession session(SchemeType::Quanpin);
            session.set_pinyin_sequence("xi'te'le");
            session.set_pinyin_sequence_with_cases("Xi'Te'Le");
            session.recompute_candidates();
            const auto transition = session.advance_composition_after_selection("xi", "西", "xi");
            require(transition.continues_composition && transition.consumed_raw_input_with_cases == "Xi",
                    "Consumed input lost the user's original casing.");
            // Retracting that selection replays the reported spelling. The
            // restored raw must survive the host-side round trip unchanged.
            session.set_pinyin_sequence(transition.consumed_raw_input_with_cases);
            session.set_pinyin_sequence_with_cases(transition.consumed_raw_input_with_cases);
            session.recompute_candidates();
            require(session.get_pinyin_sequence_with_cases() == "Xi" && session.get_pinyin_sequence() == "xi",
                    "Restoring the consumed spelling did not round-trip through the session.");
        }

        metasequoia::InputSession default_session;
        require(default_session.scheme_type() == SchemeType::Quanpin,
                "The default input scheme should be full pinyin.");
        require(default_session.quanpin_autocorrect_types() == 0,
                "Pinyin autocorrection should default to off (no type bits set).");
        require(default_session.helpcode_enabled(), "Helpcode should be enabled by default.");
        require(default_session.chinese_punctuation_enabled(), "Chinese punctuation should be enabled by default.");
        require(default_session.candidate_learning_enabled(), "Candidate learning should be enabled by default.");

        struct UmlautAliasCase
        {
            const char *pinyin;
            const char *candidate;
        };
        const std::array<UmlautAliasCase, 4> umlaut_alias_cases = {
            {{"jv", "居"}, {"qv", "去"}, {"xv", "需"}, {"yv", "与"}}};
        for (const auto &test_case : umlaut_alias_cases)
        {
            metasequoia::InputSession alias_session;
            type(alias_session, test_case.pinyin);
            require(candidate_index(alias_session, test_case.candidate) == 0,
                    "A v-form umlaut syllable did not query its canonical dictionary key.");
        }

        // The alias layer that folds a dropped final g (zhonguo -> zhong'guo,
        // dongua -> dong'gua) is part of autocorrection and now follows its
        // switch, so these cases run with the switch explicitly on. The default
        // session has it off -- the mask=0 block right below pins that.
        const unsigned alias_mask = quanpin::kAutocorrectTransposition;
        const std::array<UmlautAliasCase, 6> missing_final_g_cases = {{{"zhonguo", "中国"},
                                                                       {"zhon'guo", "中国"},
                                                                       {"zhongguo", "中国"},
                                                                       {"dongua", "冬瓜"},
                                                                       {"donggua", "冬瓜"},
                                                                       {"dongan", "东安"}}};
        for (const auto &test_case : missing_final_g_cases)
        {
            metasequoia::InputSession corrected(SchemeType::Quanpin, alias_mask);
            type(corrected, test_case.pinyin);
            require(candidate_index(corrected, test_case.candidate) == 0,
                    "A missing final g did not resolve to the complete dictionary phrase.");
            require(corrected.select_candidate(0).commit == test_case.candidate && !corrected.has_composition(),
                    "Selecting a corrected phrase left an unconsumed input suffix.");
        }

        // R2: with both switches off the engine must not correct at all. These
        // inputs used to be rewritten by the always-on alias layer (sahng ->
        // shang, zhonguo -> zhong'guo, laing -> liang, ahng -> hang, mihng ->
        // ming); now the alias layer and the correction BFS both follow the
        // mask, so nothing may come back marked as corrected.
        for (const char *typo : {"sahng", "zhonguo", "laing", "ahng", "mihng"})
        {
            metasequoia::InputSession off_session(SchemeType::Quanpin, 0u);
            type(off_session, typo);
            require(std::none_of(off_session.candidates().begin(), off_session.candidates().end(),
                                 [](const WordItem &item) { return !item.corrected_from.empty(); }),
                    "With autocorrection off no candidate may be marked as corrected.");
        }

        // R2 exception: the non-typo ü normalization must still work with the
        // switches off. The run_test fixture carries ju/qu/xu/yu (nue/lve live in
        // run_umlaut_alias_session_tests, which also pins the mask=0 path).
        {
            metasequoia::InputSession umlaut_session(SchemeType::Quanpin, 0u);
            type(umlaut_session, "jv");
            require(candidate_index(umlaut_session, "居") == 0,
                    "The ü normalization (jv -> ju) must stay active with autocorrection off.");
        }

        // PRD R5 regression: a fully legal spelling whose alias reading won the
        // query (haoyonga -> hao'yong'ga) must still preedit the typed letters.
        // The rewritten segmentation used to leak into the preedit verbatim,
        // adding a phantom letter and shifting the TSF caret (haoyongg|a). The
        // typed letters keep separators from their own legal reading, so one
        // more letter does not drop the separators already shown.
        struct PreeditCase
        {
            const char *pinyin;
            const char *expected_preedit;
        };
        const std::array<PreeditCase, 3> legal_spelling_preedit_cases = {
            {{"haoyong", "hao'yong"}, {"haoyonga", "hao'yong'a"}, {"dongan", "dong'an"}}};
        for (const auto &test_case : legal_spelling_preedit_cases)
        {
            metasequoia::InputSession session;
            type(session, test_case.pinyin);
            require(session.get_pinyin_segmentation_with_cases() == test_case.expected_preedit,
                    "A legal spelling with an alias reading must preedit the typed letters.");
        }

        // Inputs the guard does not cover: with autocorrection off the alias
        // layer no longer rewrites the letters, so these preedit their literal
        // (greedy) cut -- the typed letters with a separator at the greedy cut
        // position, and never a phantom letter. (With correction on the same
        // inputs fall back to the typed spelling; that path is covered by the
        // mask-enabled display cases in test_pinyin.cpp.)
        const std::array<PreeditCase, 4> typed_fallback_preedit_cases = {{{"dongua", "don'gua"},
                                                                          {"zhonguo", "zhon'guo"},
                                                                          {"haoyonga'", "hao'yong'a'"},
                                                                          {"hao'yonga", "hao'yong'a"}}};
        for (const auto &test_case : typed_fallback_preedit_cases)
        {
            metasequoia::InputSession session;
            type(session, test_case.pinyin);
            require(session.get_pinyin_segmentation_with_cases() == test_case.expected_preedit,
                    "An uncorrected input must preedit its literal cut.");
        }

        metasequoia::InputSession no_autocorrect_session(SchemeType::Quanpin, 0u);
        require(no_autocorrect_session.quanpin_autocorrect_types() == 0,
                "The requested pinyin autocorrect setting was not retained.");
        const unsigned both_types = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
        no_autocorrect_session.set_quanpin_autocorrect_types(both_types);
        require(no_autocorrect_session.quanpin_autocorrect_types() == both_types,
                "The requested pinyin autocorrect mask was not retained.");
        metasequoia::InputSession no_helpcode_session(SchemeType::Quanpin, true, false);
        require(!no_helpcode_session.helpcode_enabled(), "The requested helpcode setting was not retained.");
        type(no_helpcode_session, "ni");
        require(!no_helpcode_session.handle_character('H').handled && no_helpcode_session.preedit() == "ni",
                "A constructor-disabled Quanpin helpcode key was swallowed.");

        metasequoia::InputSession shuangpin_session(SchemeType::Shuangpin);
        require(shuangpin_session.scheme_type() == SchemeType::Shuangpin,
                "The requested double-pinyin scheme was not retained.");
        require(shuangpin_session.handle_character('n').handled && shuangpin_session.preedit() == "n",
                "A valid Shuangpin letter was rejected.");
        metasequoia::InputSession japanese_session(SchemeType::JapaneseRomaji);
        require(japanese_session.handle_character('k').handled && japanese_session.preedit() == "k",
                "A valid Japanese romaji letter was rejected.");
        metasequoia::InputSession wubi_session(SchemeType::Wubi);
        require(!wubi_session.handle_character('z').handled && wubi_session.preedit().empty(),
                "An unsupported Wubi letter was swallowed.");
        type(wubi_session, "abcd");
        require(!wubi_session.handle_character('e').handled && wubi_session.preedit() == "abcd",
                "A Wubi letter beyond the four-code limit was swallowed.");
        require(!wubi_session.handle_character('\'').handled && wubi_session.preedit() == "abcd",
                "An unsupported Wubi apostrophe was swallowed.");

        metasequoia::InputSession ascii_punctuation_session(SchemeType::Quanpin, true, true, false);
        require(!ascii_punctuation_session.chinese_punctuation_enabled(),
                "The requested punctuation setting was not retained.");
        require(!ascii_punctuation_session.handle_punctuation('.').handled,
                "Disabled Chinese punctuation swallowed ASCII punctuation.");

        metasequoia::InputSession no_learning_session(SchemeType::Quanpin, true, true, true, false);
        require(!no_learning_session.candidate_learning_enabled(),
                "The requested candidate-learning setting was not retained.");
        type(no_learning_session, "buhao");
        require(no_learning_session.candidates().size() >= 2 && no_learning_session.candidates().front().word == "不好",
                "The learning test dictionary did not preserve its initial order.");
        const auto unlearned_selection = no_learning_session.select_candidate(1);
        require(unlearned_selection.handled && unlearned_selection.commit == "补好",
                "Candidate selection failed while learning was disabled.");
        metasequoia::InputSession verify_unlearned_session;
        type(verify_unlearned_session, "buhao");
        require(!verify_unlearned_session.candidates().empty() &&
                    verify_unlearned_session.candidates().front().word == "不好",
                "A selected candidate was learned while candidate learning was disabled.");

        metasequoia::InputSession uppercase_session;
        require(!uppercase_session.handle_character('N').handled,
                "An uppercase letter was swallowed while no composition was active.");
        type(uppercase_session, "ni");
        require(uppercase_session.handle_character('H').handled && uppercase_session.preedit() == "niH",
                "An uppercase helpcode was rejected from an active pinyin composition.");
        uppercase_session.handle_command(metasequoia::Command::Cancel);

        metasequoia::InputSession duplicate_apostrophe_session;
        type(duplicate_apostrophe_session, "ni");
        require(duplicate_apostrophe_session.handle_character('\'').handled,
                "The first Pinyin apostrophe was rejected.");
        require(!duplicate_apostrophe_session.handle_character('\'').handled &&
                    duplicate_apostrophe_session.preedit() == "ni'",
                "A duplicate Pinyin apostrophe was swallowed.");

        metasequoia::InputSession session(SchemeType::Quanpin, true, true, true, false);
        type(session, "nihao");
        require(session.preedit() == "nihao", "The preedit did not mirror the raw pinyin.");
        require(session.raw_segmentation() == "ni'hao" && session.normalized_segmentation() == "ni'hao",
                "Quanpin segmentation was not exposed through the native session API.");
        require(session.has_composition(), "Typing pinyin did not start a composition.");
        require(session.candidates().size() >= 2, "The engine did not return both dictionary candidates.");

        const auto selected = session.select_candidate(static_cast<std::size_t>(1));
        require(selected.handled && selected.commit == "拟好", "Selecting the second candidate committed wrong text.");
        require(!session.has_composition(), "Selecting a candidate did not end the composition.");

        type(session, "nihao");
        const auto by_word = session.select_candidate(std::string("拟好"));
        require(by_word.handled && by_word.commit == "拟好", "Selecting a candidate by word committed the wrong text.");

        type(session, "nihao");
        const auto out_of_range = session.select_candidate(session.candidates().size());
        require(!out_of_range.handled, "An out-of-range candidate index was accepted.");
        const auto unknown_word = session.select_candidate(std::string("没有这个词"));
        require(!unknown_word.handled, "An unknown candidate word was accepted.");

        const auto leading = session.handle_command(metasequoia::Command::CommitCandidate);
        require(leading.handled && leading.commit == "你好", "CommitCandidate did not commit the leading candidate.");

        type(session, "nihao");
        const auto composed_punctuation = session.handle_punctuation(',');
        require(composed_punctuation.handled && composed_punctuation.commit == "你好，",
                "Punctuation did not commit the candidate atomically.");
        require(session.handle_punctuation('.').commit == "。", "Idle Chinese punctuation was not converted.");
        require(session.handle_punctuation('"').commit == "“" && session.handle_punctuation('"').commit == "”",
                "Double quotes did not alternate between opening and closing Chinese quotes.");
        require(session.handle_punctuation('\'').commit == "‘" && session.handle_punctuation('\'').commit == "’",
                "Single quotes did not alternate between opening and closing Chinese quotes.");
        require(session.handle_punctuation('(').commit == "（" && session.handle_punctuation(')').commit == "）",
                "Parentheses were not converted to Chinese punctuation.");
        require(session.handle_punctuation('[').commit == "【" && session.handle_punctuation(']').commit == "】",
                "Square brackets were not converted to Chinese punctuation.");
        require(session.handle_punctuation('`').commit == "·" && session.handle_punctuation('$').commit == "￥" &&
                    session.handle_punctuation('^').commit == "……" && session.handle_punctuation('_').commit == "——",
                "The keys whose Chinese form differs from ASCII were not converted.");

        // The outer pair is 《》 and anything inside it uses 〈〉, so depth decides the mark.
        require(session.handle_punctuation('<').commit == "《" && session.handle_punctuation('<').commit == "〈" &&
                    session.handle_punctuation('>').commit == "〉" && session.handle_punctuation('>').commit == "》",
                "Book title marks did not nest.");
        // An unmatched '>' must not drive the depth below zero, or the next '<' would open with 〈.
        require(session.handle_punctuation('>').commit == "》", "An unmatched closing book title mark was not 》.");
        require(session.handle_punctuation('<').commit == "《",
                "An unmatched closing book title mark left the nesting depth negative.");
        require(session.handle_punctuation('>').commit == "》", "Book title nesting did not return to depth zero.");
        require(session.handle_punctuation('<').commit == "《" && session.handle_punctuation('>').commit == "》",
                "Book-title brackets were not converted to Chinese punctuation.");
        require(session.handle_punctuation('\\').commit == "、", "The enumeration comma was not converted.");

        type(session, "nihao");
        const auto digit = session.handle_candidate_key('2');
        require(digit.handled && digit.commit == "拟好", "The 2 key did not commit the second candidate.");

        metasequoia::InputSession learning_source_session;
        type(learning_source_session, "nihao");
        const auto learned_selection = learning_source_session.select_candidate(1);
        require(learned_selection.handled && learned_selection.commit == "拟好",
                "The learning source candidate was not selected.");
        metasequoia::InputSession learned_session;
        type(learned_session, "nihao");
        require(!learned_session.candidates().empty() && learned_session.candidates().front().word == "拟好",
                "Selecting a candidate did not promote it for the next matching input.");

        type(session, "nihao");
        const auto first_bmp =
            session.select_candidate_edge(candidate_index(session, "拟好"), metasequoia::CandidateEdge::FirstHan);
        require(first_bmp.handled && first_bmp.commit == "拟" && !session.has_composition(),
                "FirstHan did not commit the first BMP Han character and reset the composition.");

        type(session, "nihao");
        const auto last_bmp =
            session.select_candidate_edge(candidate_index(session, "拟好"), metasequoia::CandidateEdge::LastHan);
        require(last_bmp.handled && last_bmp.commit == "好" && !session.has_composition(),
                "LastHan did not commit the last BMP Han character and reset the composition.");

        type(session, "nihao");
        const auto first_supplementary =
            session.select_candidate_edge(candidate_index(session, "𠀀方案𠮷"), metasequoia::CandidateEdge::FirstHan);
        require(first_supplementary.handled && first_supplementary.commit == "𠀀" && !session.has_composition(),
                "FirstHan split a supplementary-plane Han character.");

        type(session, "nihao");
        const auto last_supplementary =
            session.select_candidate_edge(candidate_index(session, "𠀀方案𠮷"), metasequoia::CandidateEdge::LastHan);
        require(last_supplementary.handled && last_supplementary.commit == "𠮷" && !session.has_composition(),
                "LastHan split a supplementary-plane Han character.");

        type(session, "nihao");
        const auto first_mixed =
            session.select_candidate_edge(candidate_index(session, "C语言 2"), metasequoia::CandidateEdge::FirstHan);
        require(first_mixed.handled && first_mixed.commit == "语", "FirstHan did not skip a non-Han candidate prefix.");

        type(session, "nihao");
        const auto last_mixed =
            session.select_candidate_edge(candidate_index(session, "C语言 2"), metasequoia::CandidateEdge::LastHan);
        require(last_mixed.handled && last_mixed.commit == "言", "LastHan did not skip a non-Han candidate suffix.");

        type(session, "nihao");
        const auto no_han =
            session.select_candidate_edge(candidate_index(session, "GitHub"), metasequoia::CandidateEdge::FirstHan);
        require(!no_han.handled && session.has_composition(),
                "A candidate without Han characters was consumed by edge selection.");
        session.handle_command(metasequoia::Command::Cancel);

        type(session, "nihao");
        session.handle_command(metasequoia::Command::Backspace);
        require(session.preedit() == "niha", "Backspace did not remove the last pinyin character.");
        const auto raw = session.handle_command(metasequoia::Command::CommitRaw);
        require(raw.handled && raw.commit == "niha", "CommitRaw did not commit the typed input.");

        type(session, "nihao");
        const auto cancel = session.handle_command(metasequoia::Command::Cancel);
        require(cancel.handled && !cancel.commit.has_value() && !session.has_composition(),
                "Cancel did not discard the composition.");

        type(session, "nihao");
        session.switch_scheme(SchemeType::Wubi);
        require(session.scheme() == SchemeType::Wubi, "Switching to Wubi did not update the active scheme.");
        require(!session.has_composition() && session.candidates().empty(),
                "Switching schemes did not discard the old composition.");

        session.switch_scheme(SchemeType::JapaneseRomaji);
        require(session.scheme() == SchemeType::JapaneseRomaji,
                "Switching without a composition did not update the active scheme.");
        session.switch_scheme(SchemeType::Quanpin);

        const std::vector<std::string> supported_helpcode_schemas{"lantian",     "ziranma", "shouyou2_0",
                                                                  "shouyouplus", "xiaohe",  "jiajia"};
        for (const std::string &schema : supported_helpcode_schemas)
        {
            require(metasequoia::InputSession::is_supported_helpcode_schema(schema) &&
                        metasequoia::InputSession::select_helpcode_schema(schema),
                    "A Windows-supported helpcode schema was rejected.");
        }
        require(!metasequoia::InputSession::is_supported_helpcode_schema("unknown") &&
                    !metasequoia::InputSession::select_helpcode_schema("unknown"),
                "An unknown helpcode schema was accepted.");
        // User tables in helpcodes/custom: UTF-8 file names, a BOM, CRLF and a full-width colon in the header.
        const std::filesystem::path custom_directory = data_directory / "helpcodes" / "custom";
        write_file(custom_directory / std::filesystem::u8path("我的码.txt"),
                   "\xEF\xBB\xBF# name\xEF\xBC\x9A 我的辅助码\n# name_en: Mine\n你=cb\n拟=ab\n好=ef\n");
        write_file(custom_directory / "plain.txt", "你=ab\n");
        write_file(custom_directory / "README.md", "# name: not a schema\n");
        const auto custom_schemas = HelpcodeUtils::list_custom_helpcode_schemas(data_directory);
        require(custom_schemas.size() == 2 && custom_schemas[0].schema == "custom/plain" &&
                    custom_schemas[0].name.empty() && custom_schemas[0].name_en.empty() &&
                    custom_schemas[1].schema == "custom/我的码" && custom_schemas[1].name == "我的辅助码" &&
                    custom_schemas[1].name_en == "Mine",
                "Custom helpcode schemas were not discovered with their header names.");
        require(HelpcodeUtils::is_helpcode_schema_available(data_directory, "custom/我的码") &&
                    HelpcodeUtils::is_helpcode_schema_available(data_directory, "lantian") &&
                    !HelpcodeUtils::is_helpcode_schema_available(data_directory, "custom/missing"),
                "Custom helpcode availability did not follow the files on disk.");
        for (const std::string schema : {"custom/", "custom/../helpcode", "custom/a\\b", "custom/.hidden"})
        {
            require(!metasequoia::InputSession::is_supported_helpcode_schema(schema),
                    "A custom helpcode schema escaping its directory was accepted.");
        }
        const auto custom_keymap = HelpcodeUtils::load_helpcode_keymap(data_directory, "custom/我的码");
        require(custom_keymap->size() == 3 && custom_keymap->at("你") == "cb",
                "A custom helpcode table was not loaded past its header and BOM.");
        {
            metasequoia::InputSession custom_helpcode(SchemeType::Quanpin);
            custom_helpcode.set_quanpin_helpcode_enabled(true);
            require(custom_helpcode.set_helpcode_schema("custom/我的码"), "A custom helpcode schema was not selected.");
            type(custom_helpcode, "nihaoC");
            require(!custom_helpcode.candidates().empty() && custom_helpcode.candidates().front().word == "你好",
                    "A custom helpcode table did not drive candidate reordering.");
        }

        metasequoia::InputSession quanpin_helpcode(SchemeType::Quanpin);
        quanpin_helpcode.set_quanpin_helpcode_enabled(true);
        require(quanpin_helpcode.set_helpcode_schema("lantian"), "The Lantian helpcode fixture was not selected.");
        type(quanpin_helpcode, "nihaoC");
        require(!quanpin_helpcode.candidates().empty() && quanpin_helpcode.candidates().front().word == "拟好",
                "Quanpin helpcode did not reorder candidates after a complete spelling.");
        quanpin_helpcode.handle_command(metasequoia::Command::Cancel);
        type(quanpin_helpcode, "nihC");
        metasequoia::InputSession quanpin_without_helpcode(SchemeType::Quanpin);
        quanpin_without_helpcode.set_quanpin_helpcode_enabled(false);
        require(!quanpin_without_helpcode.helpcode_enabled(),
                "Disabling Quanpin helpcode was not reflected by the session.");
        type(quanpin_without_helpcode, "nih");
        require(!quanpin_without_helpcode.handle_character('C').handled && quanpin_without_helpcode.preedit() == "nih",
                "A setter-disabled Quanpin helpcode key was swallowed.");
        require(same_candidate_words(quanpin_helpcode, quanpin_without_helpcode),
                "Quanpin helpcode changed candidates after an incomplete base spelling.");

        metasequoia::InputSession shuangpin_helpcode(SchemeType::Shuangpin);
        shuangpin_helpcode.set_shuangpin_helpcode_enabled(true);
        type(shuangpin_helpcode, "nihcc");
        require(shuangpin_helpcode.raw_segmentation() == "ni'hc'c" &&
                    shuangpin_helpcode.normalized_segmentation() == "ni'hao'c" &&
                    !shuangpin_helpcode.candidates().empty() && shuangpin_helpcode.candidates().front().word == "拟好",
                "Shuangpin helpcode or exposed segmentation did not match the complete base spelling.");

        metasequoia::InputSession shuangpin_delimited_helpcode(SchemeType::Shuangpin);
        shuangpin_delimited_helpcode.set_shuangpin_helpcode_enabled(true);
        type(shuangpin_delimited_helpcode, "nihcAB'");
        require(shuangpin_delimited_helpcode.raw_segmentation() == "ni'hc'AB" &&
                    shuangpin_delimited_helpcode.normalized_segmentation() == "ni'hao'AB",
                "A manual delimiter next to the Shuangpin help codes leaked into the exposed segmentation.");

        metasequoia::InputSession shuangpin_without_helpcode(SchemeType::Shuangpin);
        shuangpin_without_helpcode.set_shuangpin_helpcode_enabled(false);
        require(!shuangpin_without_helpcode.helpcode_enabled(),
                "Disabling Shuangpin helpcode was not reflected by the session.");
        type(shuangpin_without_helpcode, "ni");
        require(!shuangpin_without_helpcode.handle_character('H').handled &&
                    shuangpin_without_helpcode.preedit() == "ni",
                "A setter-disabled Shuangpin helpcode key was swallowed.");

        // 句中辅助码：反引号后的第一码筛它前面那个音节上的字（lantian 夹具：你=ab、拟=cd、好=ef）。
        {
            const auto has_word = [](const metasequoia::InputSession &target, const std::string &word) {
                const auto &items = target.candidates();
                return std::any_of(items.begin(), items.end(), [&](const WordItem &item) { return item.word == word; });
            };
            const auto enabled_session = []() {
                auto session = std::make_unique<metasequoia::InputSession>(SchemeType::Shuangpin);
                session->set_mid_sentence_helpcode_enabled(true);
                require(session->set_helpcode_schema("lantian"), "The Lantian helpcode fixture was not selected.");
                return session;
            };

            metasequoia::InputSession disabled(SchemeType::Shuangpin);
            type(disabled, "ni");
            require(!disabled.handle_character('`').handled && disabled.preedit() == "ni",
                    "A backquote entered the composition while mid-sentence helpcode was off.");

            auto first_code_session = enabled_session();
            auto &first_code = *first_code_session;
            type(first_code, "n");
            require(!first_code.handle_character('`').handled,
                    "A backquote was accepted after an incomplete shuangpin syllable.");
            type(first_code, "i`c");
            require(!first_code.handle_character('`').handled,
                    "A second backquote was accepted right after a helpcode block.");
            type(first_code, "hc");
            require(first_code.preedit() == "ni`chc" && first_code.get_pinyin_sequence_with_cases() == "ni`chc" &&
                        first_code.get_pinyin_sequence() == "ni'hc",
                    "The mid-sentence helpcode block was not kept in the raw input.");
            require(first_code.get_pinyin_segmentation_with_cases() == "ni`c'hc",
                    "The mid-sentence helpcode block was not shown after its syllable.");
            require(has_word(first_code, "拟好") && !has_word(first_code, "你好"),
                    "The mid-sentence first code did not filter the constrained syllable.");

            auto second_code_session = enabled_session();
            auto &second_code = *second_code_session;
            type(second_code, "ni`cDhc");
            require(has_word(second_code, "拟好") && !has_word(second_code, "你好"),
                    "A matching uppercase second code filtered the constrained syllable away.");
            auto wrong_second_code_session = enabled_session();
            auto &wrong_second_code = *wrong_second_code_session;
            type(wrong_second_code, "ni`cEhc");
            require(!has_word(wrong_second_code, "拟好") && !has_word(wrong_second_code, "你好"),
                    "An uppercase second code did not take part in filtering.");

            // 约束挂在后面的音节上：前面的短候选不受影响，选掉之后约束随剩余部分留下来。
            auto carried_session = enabled_session();
            auto &carried = *carried_session;
            type(carried, "nihcbuhc`x");
            require(has_word(carried, "你好"), "A constraint on a later syllable filtered a shorter candidate.");
            const auto committed = carried.select_candidate(candidate_index(carried, "你好"));
            require(committed.commit == "你好" && carried.preedit() == "buhc`x",
                    "Selecting a prefix dropped the helpcode block of the remaining syllables.");
            require(!has_word(carried, "不好") && !has_word(carried, "补好"),
                    "The carried mid-sentence helpcode no longer filtered the remaining syllables.");
            require(carried.handle_command(metasequoia::Command::Backspace).handled && carried.preedit() == "buhc`",
                    "Backspace did not remove the helpcode letter.");
            require(has_word(carried, "不好") && has_word(carried, "补好"),
                    "A bare backquote still filtered candidates.");

            // 光标移回句中补辅助码：反引号和它后面的码按光标前的部分判断，插在光标处。
            auto caret_session = enabled_session();
            auto &caret = *caret_session;
            type(caret, "nihc");
            require(caret.handle_command(metasequoia::Command::MoveLeft).handled && caret.caret_position() == 3,
                    "The caret did not move into the composition.");
            require(!caret.handle_character('`').handled && caret.preedit() == "nihc",
                    "A backquote was accepted in the middle of a shuangpin syllable.");
            require(caret.handle_command(metasequoia::Command::MoveLeft).handled && caret.caret_position() == 2,
                    "The caret did not move back to the syllable boundary.");
            require(caret.handle_character('`').handled && caret.preedit() == "ni`hc" && caret.caret_position() == 3,
                    "A backquote at a mid-composition syllable boundary was not inserted at the caret.");
            require(!caret.handle_character('`').handled && caret.preedit() == "ni`hc",
                    "A second backquote was accepted right after a mid-composition marker.");
            type(caret, "cD");
            require(caret.preedit() == "ni`cDhc" && caret.caret_position() == 5,
                    "The mid-composition helpcode letters were not inserted at the caret.");
            // 辅码段是音节后面单独的一个单元：按段删除、按段跳光标都能只落在它上面。
            require(caret.segment_raw_boundaries() == std::vector<std::size_t>({0, 2, 5, 7}),
                    "The mid-sentence helpcode block was not a unit of its own.");
            // 光标停在辅码段后面：候选跟着光标前缀走，不再是整串的词；回到串尾再按整串解码。
            require(!has_word(caret, "拟好") && !has_word(caret, "你好"),
                    "The caret prefix after a mid-sentence helpcode block was not decoded on its own.");
            require(caret.handle_command(metasequoia::Command::MoveEnd).handled, "The caret did not move to the end.");
            require(has_word(caret, "拟好") && !has_word(caret, "你好"),
                    "A helpcode inserted in the middle did not filter its syllable.");
            for (int step = 0; step < 2; ++step)
                require(caret.handle_command(metasequoia::Command::MoveLeft).handled, "The caret did not move left.");
            for (int step = 0; step < 3; ++step)
                require(caret.handle_command(metasequoia::Command::MoveLeft).handled, "The caret did not move left.");
            require(caret.caret_position() == 2 && !caret.handle_character('`').handled && caret.preedit() == "ni`cDhc",
                    "A backquote was accepted in front of an existing helpcode block.");
        }

        // 句中辅助码的大写触发：完整音节后的大写字母相当于「反引号 + 这个字母」，第二码仍要大写。
        {
            const auto has_word = [](const metasequoia::InputSession &target, const std::string &word) {
                const auto &items = target.candidates();
                return std::any_of(items.begin(), items.end(), [&](const WordItem &item) { return item.word == word; });
            };
            const auto uppercase_session = [](bool uppercase, const ShuangpinProfile &profile) {
                auto session = std::make_unique<metasequoia::InputSession>(SchemeType::Shuangpin, profile,
                                                                           metasequoia::RuntimePaths::legacy());
                session->set_shuangpin_helpcode_enabled(false);
                session->set_mid_sentence_helpcode_enabled(true);
                session->set_mid_sentence_uppercase_trigger_enabled(uppercase);
                require(session->set_helpcode_schema("lantian"), "The Lantian helpcode fixture was not selected.");
                return session;
            };

            auto first_session = uppercase_session(true, GetXiaoheShuangpinProfile());
            auto &first = *first_session;
            type(first, "niChc");
            require(first.preedit() == "niChc" && first.get_pinyin_sequence_with_cases() == "niChc" &&
                        first.get_pinyin_sequence() == "ni'hc",
                    "An uppercase letter after a complete syllable did not open a mid-sentence helpcode block.");
            require(first.get_pinyin_segmentation_with_cases() == "niC'hc",
                    "The uppercase helpcode block was not shown after its syllable.");
            require(first.segment_raw_boundaries() == std::vector<std::size_t>({0, 2, 3, 5}),
                    "The uppercase helpcode block was not a unit of its own.");
            require(has_word(first, "拟好") && !has_word(first, "你好"),
                    "The uppercase first code did not filter the constrained syllable.");
            // 大写段之后照样能接反引号段，这一节从大写段之后算起。
            require(first.handle_character('`').handled && first.preedit() == "niChc`",
                    "A backquote was rejected after an uppercase helpcode block.");

            auto second_session = uppercase_session(true, GetXiaoheShuangpinProfile());
            type(*second_session, "niCDhc");
            require(has_word(*second_session, "拟好") && !has_word(*second_session, "你好"),
                    "A matching uppercase second code after an uppercase block filtered the syllable away.");
            auto wrong_second_session = uppercase_session(true, GetXiaoheShuangpinProfile());
            type(*wrong_second_session, "niCEhc");
            require(!has_word(*wrong_second_session, "拟好") && !has_word(*wrong_second_session, "你好"),
                    "An uppercase second code after an uppercase block did not take part in filtering.");

            // 只有在完整音节后才开段：音节中间的大写字母不收（辅助码开关关着时）。
            auto middle_session = uppercase_session(true, GetXiaoheShuangpinProfile());
            type(*middle_session, "n");
            require(!middle_session->handle_character('C').handled && middle_session->preedit() == "n",
                    "An uppercase letter in the middle of a syllable was accepted as a helpcode.");

            // 勾选关着：大写字母不开段，原来的规则不变。
            auto off_session = uppercase_session(false, GetXiaoheShuangpinProfile());
            type(*off_session, "ni");
            require(!off_session->handle_character('C').handled && off_session->preedit() == "ni",
                    "An uppercase letter opened a block while the uppercase trigger was off.");

            // 约束挂在后面的音节上：选掉前面的词，大写段随剩余部分留下来。
            auto carried_session = uppercase_session(true, GetXiaoheShuangpinProfile());
            auto &carried = *carried_session;
            type(carried, "nihcbuhcX");
            require(has_word(carried, "你好"), "An uppercase block on a later syllable filtered a shorter candidate.");
            const auto committed = carried.select_candidate(candidate_index(carried, "你好"));
            require(committed.commit == "你好" && carried.preedit() == "buhcX",
                    "Selecting a prefix dropped the uppercase block of the remaining syllables.");
            require(!has_word(carried, "不好") && !has_word(carried, "补好"),
                    "The carried uppercase block no longer filtered the remaining syllables.");

            // 微软双拼的 ; 韵母：大写段不算这一节的键，nihkXb; 里 b; 是一个音节。
            auto microsoft_session = uppercase_session(true, GetMicrosoftShuangpinProfile());
            type(*microsoft_session, "nihkXb");
            require(microsoft_session->handle_character(';').handled && microsoft_session->preedit() == "nihkXb;",
                    "The semicolon final was rejected after an uppercase helpcode block.");
        }

        // 双拼末尾辅助码关掉直接触发：辅码改由引导键（输入串里的 |，宿主把反引号或分号换成它）或大写字母
        // 引出，一码按首末字重排、两码按首末字筛选（lantian 夹具：你=ab、拟=cd、好=ef）。
        {
            const auto has_word = [](const metasequoia::InputSession &target, const std::string &word) {
                const auto &items = target.candidates();
                return std::any_of(items.begin(), items.end(), [&](const WordItem &item) { return item.word == word; });
            };
            const auto indirect_session = [](bool uppercase, const ShuangpinProfile &profile) {
                auto session = std::make_unique<metasequoia::InputSession>(SchemeType::Shuangpin, profile,
                                                                           metasequoia::RuntimePaths::legacy());
                session->set_shuangpin_helpcode_enabled(true);
                session->set_shuangpin_helpcode_triggers(false, uppercase);
                require(session->set_helpcode_schema("lantian"), "The Lantian helpcode fixture was not selected.");
                return session;
            };

            // 关着直接触发：完整音节后多出来的键不再当单码。
            auto plain_session = indirect_session(false, GetXiaoheShuangpinProfile());
            type(*plain_session, "nihcc");
            require(!plain_session->has_active_helpcode(),
                    "A trailing letter was read as a helpcode while the end helpcode was indirect.");

            auto single_session = indirect_session(false, GetXiaoheShuangpinProfile());
            auto &single = *single_session;
            type(single, "n");
            require(!single.handle_character('|').handled && single.preedit() == "n",
                    "An end helpcode marker was accepted after an incomplete shuangpin syllable.");
            type(single, "ihc");
            require(single.handle_character('|').handled && single.preedit() == "nihc|",
                    "The end helpcode marker was not accepted after complete syllables.");
            require(!single.handle_character('|').handled, "A second end helpcode marker was accepted.");
            type(single, "c");
            require(single.get_pinyin_sequence_with_cases() == "nihc|c" && single.get_pinyin_sequence() == "nihc" &&
                        single.has_active_helpcode(),
                    "The end helpcode block was not kept apart from the pinyin.");
            require(single.get_pinyin_segmentation_with_cases() == "ni'hc|c",
                    "The end helpcode block was not shown after the last syllable.");
            require(single.segment_raw_boundaries() == std::vector<std::size_t>({0, 2, 4, 6}),
                    "The end helpcode block was not a unit of its own.");
            require(!single.candidates().empty() && single.candidates().front().word == "拟好" &&
                        has_word(single, "你好"),
                    "A single indirect end helpcode did not reorder candidates by their first character.");

            auto double_session = indirect_session(false, GetXiaoheShuangpinProfile());
            type(*double_session, "nihc|cE");
            require(has_word(*double_session, "拟好") && !has_word(*double_session, "你好"),
                    "Two indirect end helpcodes did not filter by the first and last characters.");

            // Selecting a prefix consumes the active terminal block in either mode.
            database.execute("INSERT INTO tbl_1_n VALUES('ni','n','拟',100)");
            database.execute("CREATE TABLE IF NOT EXISTS tbl_1_h(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
            database.execute("INSERT INTO tbl_1_h VALUES('hao','h','好',100)");
            for (const bool direct : {false, true})
            {
                for (const std::string codes : {"c", "cD"})
                {
                    auto target = indirect_session(false, GetXiaoheShuangpinProfile());
                    target->set_shuangpin_helpcode_triggers(direct, false);
                    type(*target, "nihc" + (direct ? std::string() : "|") + codes);
                    const auto picked = target->candidates()[candidate_index(*target, "拟")];
                    const auto transition = target->advance_composition_after_selection(
                        picked.pinyin, picked.word, picked.canonical_pinyin, picked.scheme);
                    require(transition.continues_composition && target->get_pinyin_sequence_with_cases() == "hc" &&
                                has_word(*target, "好"),
                            "A selected prefix left its terminal helpcode blocking the remaining syllables.");
                    require(transition.consumed_raw_input_with_cases == "ni" &&
                                transition.consumed_terminal_helpcode_with_cases ==
                                    (direct ? std::string() : "|") + codes,
                            "The selection did not record the prefix and terminal block separately.");
                }
            }

            // 引导键只在输入末尾收：光标移回句中时仍按标点处理。
            auto caret_session = indirect_session(false, GetXiaoheShuangpinProfile());
            type(*caret_session, "nihc");
            require(caret_session->handle_command(metasequoia::Command::MoveLeft).handled &&
                        caret_session->handle_command(metasequoia::Command::MoveLeft).handled &&
                        !caret_session->handle_character('|').handled && caret_session->preedit() == "nihc",
                    "An end helpcode marker was accepted in the middle of the composition.");

            // A single code also expires after selecting a nonmatching prefix.
            auto carried_session = indirect_session(false, GetXiaoheShuangpinProfile());
            auto &carried = *carried_session;
            type(carried, "nihcbuhc|x");
            require(has_word(carried, "你好"), "A single end helpcode filtered a shorter candidate away.");
            const auto committed = carried.select_candidate(candidate_index(carried, "你好"));
            require(committed.commit == "你好" && carried.preedit() == "buhc" && !carried.has_active_helpcode(),
                    "Selecting a prefix retained the terminal helpcode on the remaining syllables.");
            require(carried.handle_command(metasequoia::Command::Backspace).handled && carried.preedit() == "buh" &&
                        !carried.has_active_helpcode(),
                    "Portable Backspace did not delete the remaining pinyin letter.");

            // 大写触发：完整音节后的大写字母开一段末尾段，第二码大小写都算。
            auto upper_session = indirect_session(true, GetXiaoheShuangpinProfile());
            auto &upper = *upper_session;
            type(upper, "nihcCe");
            require(upper.get_pinyin_sequence() == "nihc" && upper.get_pinyin_segmentation_with_cases() == "ni'hcCe",
                    "An uppercase letter after complete syllables did not open an end helpcode block.");
            require(has_word(upper, "拟好") && !has_word(upper, "你好"),
                    "An uppercase end helpcode block did not filter candidates.");

            // 微软双拼：末尾段后面的 ; 不是韵母。
            auto microsoft_session = indirect_session(false, GetMicrosoftShuangpinProfile());
            type(*microsoft_session, "nihk|ce");
            require(!microsoft_session->handle_character(';').handled && microsoft_session->preedit() == "nihk|ce",
                    "A semicolon after an end helpcode block was taken as the ing final.");

            // 直接触发开着、或直接辅助码（万象式）接管辅码时，引导键不收。
            auto direct_session = indirect_session(false, GetXiaoheShuangpinProfile());
            direct_session->set_shuangpin_helpcode_triggers(true, false);
            type(*direct_session, "nihc");
            require(!direct_session->handle_character('|').handled,
                    "An end helpcode marker was accepted while the end helpcode was direct.");
            auto wanxiang_session = indirect_session(false, GetXiaoheShuangpinProfile());
            wanxiang_session->set_direct_helpcode_enabled(true);
            type(*wanxiang_session, "nihc");
            require(!wanxiang_session->handle_character('|').handled,
                    "An end helpcode marker was accepted while the direct helpcode took over.");
        }

        require(!session.handle_character('1').handled, "A digit was swallowed instead of passed through.");
        require(!session.handle_command(metasequoia::Command::Backspace).handled,
                "Backspace was swallowed while no composition was active.");
        require(!session.handle_command(metasequoia::Command::CommitRaw).handled,
                "CommitRaw was swallowed while no composition was active.");
        require(!session.select_candidate(static_cast<std::size_t>(0)).handled,
                "A candidate was selected while no composition was active.");

        require(!session.handle_character('\'').handled, "An idle apostrophe was swallowed.");
    }

    run_umlaut_alias_session_tests(data_directory);
    run_word_final_helpcode_lifecycle_tests(data_directory);
    run_caret_prefix_session_tests(data_directory);
    run_autocorrect_mixed_jianpin_tests(data_directory);
    run_literal_mixed_pinyin_tests(data_directory);
    run_single_letter_candidate_order_tests(data_directory);
    run_autocorrect_context_ranking_tests(data_directory);
    run_autocorrect_context_layering_tests(data_directory);
    run_autocorrect_context_user_choice_tests(data_directory);
    run_legal_input_correction_tests(data_directory);
    run_autocorrect_selection_consumption_tests(data_directory);
    run_literal_segmentation_arbitration_tests(data_directory);
    run_autocorrect_costlier_context_tests(data_directory);
    run_autocorrect_off_consistency_tests(data_directory);

    run_autocorrect_generated_space_tests(data_directory);
    run_autocorrect_pure_jianpin_guard_tests(data_directory);
#endif

#ifndef METASEQUOIA_SKIP_FREQUENCY_TESTS
    for (const unsigned autocorrect : {0u, quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor})
    {
        const auto directory = data_directory / ("frequency-ng-" + std::to_string(autocorrect));
        std::filesystem::create_directories(directory);
        {
            Database database(directory / "msime.db");
            database.execute("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                             "INSERT INTO tbl_1_n VALUES('ng','n','嗯',0);"
                             "CREATE TABLE tbl_2_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                             "INSERT INTO tbl_2_n VALUES('neng''gou','ng','能够',501639);"
                             "INSERT INTO tbl_2_n VALUES('na''ge','ng','那个',501011);");
        }
        const metasequoia::RuntimePaths paths{directory, directory, directory, directory};
        metasequoia::InputSession session(SchemeType::Quanpin, autocorrect, true, true, false, paths);
        type(session, "ng");
        const auto &prefix = session.candidates()[candidate_index(session, "嗯")];
        require(prefix.pinyin == "n" && prefix.canonical_pinyin == "ng",
                "The ng fixture must expose the nasal reading as a prefix candidate.");
        const auto pinned = session.pin_candidate(candidate_index(session, "那个"));
        require(pinned.handled && !pinned.commit && !pinned.diagnostic && session.candidates().front().word == "那个",
                "A prefix candidate whose reading matches the full input displaced the manually pinned word.");
        session.handle_command(metasequoia::Command::Cancel);
        type(session, "ng");
        require(session.candidates().front().word == "那个", "Retyping ng lost the manually pinned order.");
        metasequoia::InputSession reopened(SchemeType::Quanpin, autocorrect, true, true, false, paths);
        type(reopened, "ng");
        require(reopened.candidates().front().word == "那个",
                "Reopening the session lost the manually pinned ng word.");
    }

    struct FrequencyCase
    {
        metasequoia::FrequencyAdjustmentMode mode;
        const char *name;
        std::size_t expected_index;
        int linear_step;
    };
    const std::array frequency_cases{
        FrequencyCase{metasequoia::FrequencyAdjustmentMode::Disabled, "disabled", 5, 1},
        FrequencyCase{metasequoia::FrequencyAdjustmentMode::Pin, "pin", 0, 1},
        FrequencyCase{metasequoia::FrequencyAdjustmentMode::Halve, "halve", 2, 1},
        FrequencyCase{metasequoia::FrequencyAdjustmentMode::Linear, "linear", 3, 2},
        FrequencyCase{metasequoia::FrequencyAdjustmentMode::Promote, "promote", 4, 1},
    };
    for (const FrequencyCase &frequency_case : frequency_cases)
    {
        user_dictionary::close_default_user_database();
        const std::filesystem::path directory = data_directory / (std::string("frequency-") + frequency_case.name);
        prepare_frequency_fixture(directory);
        set_data_directory(directory);

        metasequoia::InputSession learning_session(SchemeType::Quanpin);
        require(learning_session.set_frequency_adjustment({frequency_case.mode, 1, frequency_case.linear_step}),
                "A supported frequency adjustment configuration was rejected.");
        type(learning_session, "ni");
        const auto learned = learning_session.select_candidate(std::string("己"));
        require(learned.handled && learned.commit == "己" && !learned.diagnostic.has_value(),
                "Frequency learning changed or diagnosed a successful candidate commit.");

        metasequoia::InputSession reopened(SchemeType::Quanpin);
        type(reopened, "ni");
        require(candidate_index(reopened, "己") == frequency_case.expected_index,
                "A frequency mode did not persist the Windows-compatible ranking transition.");
        const bool journal_exists = std::filesystem::exists(directory / "msime_user.db");
        require(journal_exists == (frequency_case.mode != metasequoia::FrequencyAdjustmentMode::Disabled),
                "Frequency learning wrote an unexpected user journal state.");
    }

    user_dictionary::close_default_user_database();
    const std::filesystem::path trigger_directory = data_directory / "frequency-trigger";
    prepare_frequency_fixture(trigger_directory);
    set_data_directory(trigger_directory);
    for (int selection = 0; selection < 2; ++selection)
    {
        metasequoia::InputSession triggered(SchemeType::Quanpin);
        require(triggered.set_frequency_adjustment({metasequoia::FrequencyAdjustmentMode::Pin, 2, 1}),
                "A valid trigger-count configuration was rejected.");
        type(triggered, "ni");
        require(triggered.select_candidate(std::string("己")).commit == "己",
                "A deferred frequency adjustment blocked candidate commit.");

        metasequoia::InputSession observed(SchemeType::Quanpin);
        type(observed, "ni");
        require(candidate_index(observed, "己") == (selection == 0 ? 5U : 0U),
                "Frequency trigger_count did not defer exactly the configured number of selections.");
    }

    user_dictionary::close_default_user_database();
    const std::filesystem::path first_candidate_directory = data_directory / "frequency-first-candidate";
    prepare_frequency_fixture(first_candidate_directory);
    set_data_directory(first_candidate_directory);
    metasequoia::InputSession first_candidate_session(SchemeType::Quanpin);
    require(first_candidate_session.set_frequency_adjustment({metasequoia::FrequencyAdjustmentMode::Pin, 1, 1}),
            "A valid first-candidate learning configuration was rejected.");
    type(first_candidate_session, "ni");
    require(first_candidate_session.select_candidate(static_cast<std::size_t>(0)).commit == "甲" &&
                !std::filesystem::exists(first_candidate_directory / "msime_user.db"),
            "Selecting the already-leading candidate created frequency state.");

    user_dictionary::close_default_user_database();
    const std::filesystem::path shuangpin_directory = data_directory / "frequency-shuangpin";
    prepare_shuangpin_frequency_fixture(shuangpin_directory);
    set_data_directory(shuangpin_directory);
    metasequoia::InputSession shuangpin_learning(SchemeType::Shuangpin);
    require(shuangpin_learning.set_frequency_adjustment({metasequoia::FrequencyAdjustmentMode::Pin, 1, 1}),
            "A valid Shuangpin frequency configuration was rejected.");
    type(shuangpin_learning, "nihc");
    require(shuangpin_learning.select_candidate(std::string("拟好")).commit == "拟好",
            "Shuangpin frequency learning blocked candidate commit.");
    metasequoia::InputSession reopened_shuangpin(SchemeType::Shuangpin);
    type(reopened_shuangpin, "nihc");
    require(!reopened_shuangpin.candidates().empty() && reopened_shuangpin.candidates().front().word == "拟好",
            "Shuangpin frequency learning did not persist through the canonical pinyin key.");

    // 句中辅助码筛过的列表里，要的词排第一；直接上屏也要按不带约束的排序调频，下次不敲辅助码
    // 它就排在前面（lantian 夹具：你=ab、拟=cd）。
    user_dictionary::close_default_user_database();
    const std::filesystem::path mid_sentence_directory = data_directory / "frequency-shuangpin-mid-sentence";
    prepare_shuangpin_frequency_fixture(mid_sentence_directory);
    write_file(mid_sentence_directory / "helpcodes" / "helpcode.txt", "你=ab\n拟=cd\n好=ef\n");
    set_data_directory(mid_sentence_directory);
    {
        metasequoia::InputSession mid_sentence_learning(SchemeType::Shuangpin);
        mid_sentence_learning.set_mid_sentence_helpcode_enabled(true);
        require(mid_sentence_learning.set_helpcode_schema("lantian") &&
                    mid_sentence_learning.set_frequency_adjustment({metasequoia::FrequencyAdjustmentMode::Pin, 1, 1}),
                "The mid-sentence frequency fixture could not be configured.");
        type(mid_sentence_learning, "ni`chc");
        require(candidate_index(mid_sentence_learning, "拟好") == 0,
                "The mid-sentence helpcode did not put the constrained phrase first.");
        const auto learned = mid_sentence_learning.select_candidate(static_cast<std::size_t>(0));
        require(learned.commit == "拟好" && !learned.diagnostic.has_value(),
                "Committing a mid-sentence helpcode phrase failed.");
    }
    metasequoia::InputSession reopened_mid_sentence(SchemeType::Shuangpin);
    type(reopened_mid_sentence, "nihc");
    require(candidate_index(reopened_mid_sentence, "拟好") == 0,
            "A phrase committed with a mid-sentence helpcode was not promoted for the plain spelling.");

    user_dictionary::close_default_user_database();
    const std::filesystem::path wubi_directory = data_directory / "frequency-wubi";
    prepare_wubi_frequency_fixture(wubi_directory);
    set_data_directory(wubi_directory);
    metasequoia::InputSession wubi_learning(SchemeType::Wubi);
    require(wubi_learning.set_frequency_adjustment({metasequoia::FrequencyAdjustmentMode::Pin, 1, 1}),
            "A valid Wubi frequency configuration was rejected.");
    type(wubi_learning, "aaaa");
    require(wubi_learning.select_candidate(std::string("或")).commit == "或",
            "Wubi frequency learning blocked candidate commit.");
    metasequoia::InputSession reopened_wubi(SchemeType::Wubi);
    type(reopened_wubi, "aaaa");
    require(!reopened_wubi.candidates().empty() && reopened_wubi.candidates().front().word == "或",
            "Wubi frequency learning did not persist through the Wubi table.");

    user_dictionary::close_default_user_database();
    const std::filesystem::path failure_directory = data_directory / "frequency-write-failure";
    prepare_frequency_fixture(failure_directory);
    std::filesystem::create_directory(failure_directory / "msime_user.db");
    set_data_directory(failure_directory);
    metasequoia::InputSession failing_learning_session(SchemeType::Quanpin);
    require(failing_learning_session.set_frequency_adjustment({metasequoia::FrequencyAdjustmentMode::Pin, 1, 1}),
            "A valid write-failure learning configuration was rejected.");
    type(failing_learning_session, "ni");
    const auto failure_commit = failing_learning_session.select_candidate(std::string("己"));
    require(failure_commit.handled && failure_commit.commit == "己" && failure_commit.diagnostic.has_value() &&
                failure_commit.diagnostic->find("己") == std::string::npos &&
                failure_commit.diagnostic->find("ni") == std::string::npos,
            "A frequency write failure blocked commit or exposed input text in its diagnostic.");

    user_dictionary::close_default_user_database();
    const std::filesystem::path partial_write_directory = data_directory / "frequency-partial-write";
    prepare_frequency_fixture(partial_write_directory);
    set_data_directory(partial_write_directory);
    require(user_dictionary::ensure_user_database(user_dictionary::default_user_db_path()),
            "The partial-write fixture could not create the user dictionary schema.");
    user_dictionary::close_default_user_database();
    {
        Database user_database(partial_write_directory / "msime_user.db");
        user_database.execute("CREATE TRIGGER reject_frequency_journal BEFORE INSERT ON user_dictionary_operations "
                              "BEGIN SELECT RAISE(FAIL, 'injected journal failure'); END");
    }
    metasequoia::InputSession partial_write_session(SchemeType::Quanpin);
    require(partial_write_session.set_frequency_adjustment({metasequoia::FrequencyAdjustmentMode::Pin, 1, 1}),
            "A valid partial-write learning configuration was rejected.");
    type(partial_write_session, "ni");
    const auto partial_write_commit = partial_write_session.select_candidate(std::string("己"));
    require(partial_write_commit.handled && partial_write_commit.commit == "己" &&
                partial_write_commit.diagnostic.has_value() &&
                partial_write_commit.diagnostic->find("己") == std::string::npos &&
                partial_write_commit.diagnostic->find("ni") == std::string::npos,
            "A partial frequency write blocked commit or exposed input text in its diagnostic.");
    {
        Database main_database(partial_write_directory / "msime.db");
        require(main_database.query_integer("SELECT weight FROM tbl_1_n WHERE key='ni' AND value='己'") == 50,
                "A failed journal write left a partial frequency update in the main dictionary.");
    }

    metasequoia::FrequencyAdjustmentOptions invalid_frequency;
    invalid_frequency.mode = static_cast<metasequoia::FrequencyAdjustmentMode>(99);
    require(!failing_learning_session.set_frequency_adjustment(invalid_frequency),
            "An unknown frequency mode was accepted.");
    invalid_frequency = {};
    invalid_frequency.trigger_count = 0;
    require(!failing_learning_session.set_frequency_adjustment(invalid_frequency),
            "An out-of-range frequency trigger count was accepted.");
    invalid_frequency = {};
    invalid_frequency.linear_step = 11;
    require(!failing_learning_session.set_frequency_adjustment(invalid_frequency),
            "An out-of-range frequency linear step was accepted.");
    user_dictionary::close_default_user_database();
#endif

#ifndef METASEQUOIA_FREQUENCY_TESTS_ONLY
    // 拆字查表走数据目录下的 radical/decomposition.txt。写一份最小表，测试不依赖部署数据
    // 目录里的真实表，字频只用来验证「同前缀内高频在前」。
    const std::filesystem::path radical_directory = data_directory / "radical-fixture";
    write_file(radical_directory / "radical" / "decomposition.txt", "# 部件拼音连写<Tab>汉字<Tab>字频\n"
                                                                    "ririri\t晶\t106\n"
                                                                    "ririri\t曑\t1\n"
                                                                    "heng\t一\t18584\n"
                                                                    "hengheng\t丁\t900\n");
    set_data_directory(radical_directory);

    metasequoia::InputSession radical_session(SchemeType::Quanpin);
    require(radical_session.handle_character('U', true).handled &&
                radical_session.local_input_mode() == metasequoia::LocalInputMode::Radical &&
                radical_session.preedit() == "U" && radical_session.candidates().empty(),
            "Shift+U did not enter an empty radical composition.");
    // 非小写字母（数字）被吞：预编辑保持 "U"、候选不变，也不让给普通拼音。
    require(radical_session.handle_character('4').handled && radical_session.preedit() == "U" &&
                radical_session.candidates().empty(),
            "Radical mode accepted or forwarded a non-lowercase-letter key.");
    for (const char character : std::string("ririri"))
    {
        require(radical_session.handle_character(character).handled, "Radical mode rejected a lowercase letter.");
    }
    require(radical_session.preedit() == "Uririri" && !radical_session.candidates().empty() &&
                radical_session.candidates().front().word == "晶" &&
                radical_session.candidates().front().source == CandidateSource::Generated,
            "Radical mode did not rank the highest-frequency decomposition of 晶 first.");
    const auto radical_commit = radical_session.select_candidate(0);
    require(radical_commit.handled && radical_commit.commit == "晶" && !radical_session.has_composition() &&
                radical_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Committing a radical candidate did not leave the local mode.");

    // 前缀收窄：连写码 heng 的最高频命中是 一（18584，高于任何 heng* 的更长码）。
    require(radical_session.handle_character('U', true).handled, "Radical mode could not be re-entered for prefixing.");
    type(radical_session, "heng");
    require(radical_session.preedit() == "Uheng" && !radical_session.candidates().empty() &&
                radical_session.candidates().front().word == "一",
            "A radical prefix did not return its highest-frequency character first.");
    require(radical_session.handle_command(metasequoia::Command::Cancel).handled &&
                radical_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Cancel did not leave radical mode.");

    // 大写字母与预编辑里的其它按键都被吞、预编辑不变（沿用「非法字符被吞」的语义）。
    require(radical_session.handle_character('U', true).handled,
            "Radical mode could not be re-entered for illegal-key coverage.");
    require(radical_session.handle_character('A').handled && radical_session.handle_character('9').handled &&
                radical_session.preedit() == "U" && radical_session.candidates().empty(),
            "Radical mode leaked an uppercase letter or digit into the preedit.");
    // Segment boundaries are the engine's unit model: quanpin reports one offset
    // per displayed syllable, and a local mode reports none so the host keeps
    // single-character editing (PRD R4).
    require(radical_session.segment_raw_boundaries().empty(), "A local mode must report no pinyin segment boundaries.");
    require(radical_session.handle_command(metasequoia::Command::Cancel).handled,
            "Cancel did not leave radical mode after the boundary check.");

    metasequoia::InputSession boundary_session(SchemeType::Quanpin);
    for (const char character : std::string("nihaoma"))
    {
        require(boundary_session.handle_character(character).handled, "Quanpin rejected a letter.");
    }
    require(boundary_session.segment_raw_boundaries() == std::vector<std::size_t>({0, 2, 5, 7}),
            "Quanpin unit boundaries did not follow the displayed syllables.");

    require(radical_session.handle_character('U', true).handled,
            "Radical prefix was not handled before Backspace coverage.");
    require(radical_session.handle_command(metasequoia::Command::Backspace).handled &&
                !radical_session.has_composition() &&
                radical_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Backspace on a bare radical prefix did not leave the mode.");

    metasequoia::InputSession plain_uppercase(SchemeType::Quanpin);
    require(!plain_uppercase.handle_character('U').handled && !plain_uppercase.has_composition() &&
                plain_uppercase.local_input_mode() == metasequoia::LocalInputMode::None,
            "An uppercase character without Shift-only was swallowed.");
    metasequoia::LocalModeOptions disabled_local_modes;
    disabled_local_modes.radical = false;
    metasequoia::InputSession disabled_radical(SchemeType::Quanpin);
    disabled_radical.set_local_mode_options(disabled_local_modes);
    require(!disabled_radical.handle_character('U', true).handled && !disabled_radical.has_composition() &&
                disabled_radical.local_input_mode() == metasequoia::LocalInputMode::None,
            "A disabled radical shortcut swallowed Shift+U.");

    metasequoia::InputSession wubi_radical(SchemeType::Wubi);
    require(!wubi_radical.handle_character('U', true).handled && !wubi_radical.has_composition() &&
                wubi_radical.local_input_mode() == metasequoia::LocalInputMode::None,
            "Shift+U was swallowed outside a pinyin scheme.");
    metasequoia::InputSession switch_clears_radical(SchemeType::Shuangpin);
    require(switch_clears_radical.handle_character('U', true).handled &&
                switch_clears_radical.local_input_mode() == metasequoia::LocalInputMode::Radical,
            "Shuangpin could not enter radical mode.");
    switch_clears_radical.switch_scheme(SchemeType::Quanpin);
    require(!switch_clears_radical.has_composition() &&
                switch_clears_radical.local_input_mode() == metasequoia::LocalInputMode::None,
            "Switching schemes did not clear radical mode.");

    metasequoia::InputSession date_time_session(SchemeType::Quanpin);
    date_time_session.set_local_date_time_provider(
        [] { return metasequoia::local_modes::LocalDateTime{2026, 8, 9, 0, 14, 30, 0}; });
    require(date_time_session.handle_character('T', true).handled &&
                date_time_session.local_input_mode() == metasequoia::LocalInputMode::DateTime &&
                date_time_session.preedit() == "T" && date_time_session.candidates().empty(),
            "Shift+T did not enter an empty date/time composition.");
    type(date_time_session, "rq");
    require(date_time_session.preedit() == "Trq" && date_time_session.candidates().size() == 17 &&
                date_time_session.candidates().front().word == "2026年8月9日" &&
                date_time_session.candidates().back().word == "丙午年六月二十七日",
            "Date/time mode did not expose deterministic date candidates.");
    const auto date_commit = date_time_session.select_candidate(0);
    require(date_commit.handled && date_commit.commit == "2026年8月9日" &&
                date_time_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Committing a date candidate did not leave date/time mode.");

    require(date_time_session.handle_character('T', true).handled,
            "Date/time mode could not be re-entered for incomplete-input coverage.");
    type(date_time_session, "r");
    require(date_time_session.candidates().empty(), "An incomplete date keyword produced candidates.");
    const std::string incomplete_date_preedit = date_time_session.preedit();
    require(date_time_session.handle_character('1').handled && date_time_session.preedit() == incomplete_date_preedit,
            "Invalid date/time input leaked into normal composition or changed preedit.");
    require(date_time_session.handle_command(metasequoia::Command::Cancel).handled &&
                date_time_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Cancel did not leave date/time mode.");

    require(date_time_session.handle_character('T', true).handled &&
                date_time_session.handle_command(metasequoia::Command::Backspace).handled &&
                date_time_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Backspace on a bare date/time prefix did not leave the mode.");

    // 指定的日期时间：数字、/ 和 : 接得上形状才收，接不上的吞掉，预编辑保持原样。
    require(date_time_session.handle_character('T', true).handled, "Date/time mode could not be re-entered.");
    type(date_time_session, "2024/12/25");
    require(date_time_session.preedit() == "T2024/12/25" && date_time_session.candidates().size() == 15 &&
                date_time_session.candidates().front().word == "2024年12月25日",
            "A specific date did not produce candidates for that day.");
    require(date_time_session.handle_character('1').handled && date_time_session.handle_character(':').handled &&
                date_time_session.preedit() == "T2024/12/25",
            "A key that cannot extend the date leaked into the preedit.");
    require(date_time_session.handle_command(metasequoia::Command::MoveLeft).handled &&
                date_time_session.handle_command(metasequoia::Command::MoveLeft).handled &&
                date_time_session.handle_command(metasequoia::Command::MoveLeft).handled &&
                date_time_session.handle_command(metasequoia::Command::Backspace).handled &&
                date_time_session.preedit() == "T2024/1/25" && date_time_session.handle_character('1').handled &&
                date_time_session.preedit() == "T2024/11/25" &&
                date_time_session.candidates().front().word == "2024年11月25日",
            "Editing a specific date in the middle did not follow the shared shape rule.");
    const auto specific_commit = date_time_session.select_candidate(1);
    require(specific_commit.handled && specific_commit.commit == "2024-11-25" &&
                date_time_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Committing a specific date candidate did not leave date/time mode.");
    require(date_time_session.handle_character('T', true).handled, "Date/time mode could not be re-entered.");
    type(date_time_session, "9:05");
    require(date_time_session.preedit() == "T9:05" && date_time_session.candidates().front().word == "09:05",
            "A specific time did not produce candidates.");
    require(date_time_session.handle_command(metasequoia::Command::Cancel).handled,
            "Cancel did not leave the specific time.");

    // V 模式由 Server 按原串识别，引擎照常收到整串：运算符和括号混在拼音原串里也不能出错。
    for (const SchemeType scheme : {SchemeType::Quanpin, SchemeType::Shuangpin})
    {
        metasequoia::InputSession v_mode_raw(scheme);
        for (const char *raw : {"V1+2*(3-4)/5.6", "v12.5", "V(", "V9)", "v1//2"})
        {
            v_mode_raw.set_pinyin_sequence(raw);
            v_mode_raw.set_pinyin_sequence_with_cases(raw);
            v_mode_raw.recompute_candidates();
            (void)v_mode_raw.get_candidates();
        }
    }

    metasequoia::LocalModeOptions disabled_date_time_options;
    disabled_date_time_options.date_time = false;
    metasequoia::InputSession disabled_date_time(SchemeType::Quanpin);
    disabled_date_time.set_local_mode_options(disabled_date_time_options);
    require(!disabled_date_time.handle_character('T', true).handled && !disabled_date_time.has_composition() &&
                disabled_date_time.local_input_mode() == metasequoia::LocalInputMode::None,
            "A disabled date/time shortcut swallowed Shift+T.");

    metasequoia::InputSession wubi_date_time(SchemeType::Wubi);
    require(!wubi_date_time.handle_character('T', true).handled && !wubi_date_time.has_composition() &&
                wubi_date_time.local_input_mode() == metasequoia::LocalInputMode::None,
            "Shift+T was swallowed outside a pinyin scheme.");

    // 五笔保留原样串：Server 端的 T/V 形状判定（大写 T 前缀、V＋数字）读的是宿主回读的原样串，
    // 而五笔码表查询和自动上屏用的是归一后的码。宿主把 T/V 模式的串标成字面输入，原样串才
    // 留全用户敲的字符；普通码的原样串就是带大小写的码，与查询码逐位对应、一起截断。
    const std::filesystem::path wubi_raw_directory = data_directory / "wubi-raw";
    std::filesystem::create_directories(wubi_raw_directory);
    {
        Database database(wubi_raw_directory / "msime.db");
        database.execute("CREATE TABLE wubi86(key TEXT, value TEXT, weight INTEGER)");
    }
    set_data_directory(wubi_raw_directory);
    for (const bool mixed_pinyin : {false, true})
    {
        metasequoia::WubiInputOptions wubi_options;
        wubi_options.mixed_pinyin = mixed_pinyin;

        metasequoia::InputSession v_mode_wubi(SchemeType::Wubi);
        v_mode_wubi.set_wubi_input_options(wubi_options);
        v_mode_wubi.set_pinyin_sequence("V1+2");
        v_mode_wubi.set_pinyin_sequence_with_cases("V1+2");
        v_mode_wubi.set_wubi_literal_input(true);
        v_mode_wubi.recompute_candidates();
        require(v_mode_wubi.get_pinyin_sequence_with_cases() == "V1+2" && v_mode_wubi.get_pinyin_sequence() == "v",
                "Wubi did not keep the raw-case V-mode prefix apart from the query code.");

        // 不标字面输入时同一串就是普通码：数字不是码字，回读里也不能有。
        metasequoia::InputSession v_code_wubi(SchemeType::Wubi);
        v_code_wubi.set_wubi_input_options(wubi_options);
        v_code_wubi.set_pinyin_sequence("V1+2");
        v_code_wubi.set_pinyin_sequence_with_cases("V1+2");
        v_code_wubi.recompute_candidates();
        require(v_code_wubi.get_pinyin_sequence_with_cases() == "V" && v_code_wubi.get_pinyin_sequence() == "v",
                "A wubi code kept characters that are not code letters in its raw-case string.");

        metasequoia::InputSession t_mode_wubi(SchemeType::Wubi);
        t_mode_wubi.set_wubi_input_options(wubi_options);
        t_mode_wubi.set_pinyin_sequence("T2024/12/25");
        t_mode_wubi.set_pinyin_sequence_with_cases("T2024/12/25");
        t_mode_wubi.set_wubi_literal_input(true);
        t_mode_wubi.recompute_candidates();
        require(t_mode_wubi.get_pinyin_sequence_with_cases() == "T2024/12/25" &&
                    t_mode_wubi.get_pinyin_sequence() == "t" && t_mode_wubi.preedit() == "t",
                "Wubi did not keep the raw-case date/time prefix apart from the query code.");

        // 反向保护：普通小写码字两个 getter 逐字节相同，预编辑也照旧。
        metasequoia::InputSession plain_wubi(SchemeType::Wubi);
        plain_wubi.set_wubi_input_options(wubi_options);
        plain_wubi.set_pinyin_sequence("wqaa");
        plain_wubi.set_pinyin_sequence_with_cases("wqaa");
        plain_wubi.recompute_candidates();
        require(plain_wubi.get_pinyin_sequence_with_cases() == "wqaa" && plain_wubi.get_pinyin_sequence() == "wqaa" &&
                    plain_wubi.preedit() == "wqaa",
                "A plain wubi code no longer round-trips unchanged.");

        // z 不是码字时（混输与通配都关）必须从原样串里也丢掉：Server 按原样串长度数四码，
        // 留着它会比码多一格，顶字提前一个键触发。混输开启时 z 是合法码字，照旧保留。
        metasequoia::InputSession z_wubi(SchemeType::Wubi);
        z_wubi.set_wubi_input_options(wubi_options);
        z_wubi.set_pinyin_sequence("wqaz");
        z_wubi.set_pinyin_sequence_with_cases("wqaz");
        z_wubi.recompute_candidates();
        require(z_wubi.get_pinyin_sequence_with_cases() == (mixed_pinyin ? "wqaz" : "wqa") &&
                    z_wubi.get_pinyin_sequence() == (mixed_pinyin ? "wqaz" : "wqa"),
                "A dropped wubi z did not stay dropped in the raw-case string.");
    }

    const std::filesystem::path quick_phrase_directory = data_directory / "quick-phrase";
    std::filesystem::create_directories(quick_phrase_directory);
    {
        Database database(quick_phrase_directory / "msime.db");
        database.execute("CREATE TABLE quick_parases(key TEXT,value TEXT,weight INTEGER)");
        database.execute("INSERT INTO quick_parases VALUES('ab','快捷短语一',20)");
        database.execute("INSERT INTO quick_parases VALUES('aa','快捷短语二',10)");
    }
    set_data_directory(quick_phrase_directory);
    metasequoia::InputSession quick_phrase_session(SchemeType::Quanpin);
    require(quick_phrase_session.handle_character('K', true).handled &&
                quick_phrase_session.local_input_mode() == metasequoia::LocalInputMode::QuickPhrase &&
                quick_phrase_session.preedit() == "K" && quick_phrase_session.candidates().empty(),
            "Shift+K did not enter an empty quick-phrase composition.");
    const auto quick_phrase_query = quick_phrase_session.handle_character('a');
    require(quick_phrase_query.handled && !quick_phrase_query.diagnostic.has_value() &&
                quick_phrase_session.preedit() == "Ka" && quick_phrase_session.candidates().size() == 2 &&
                quick_phrase_session.candidates().front().word == "快捷短语一" &&
                quick_phrase_session.candidates().front().source == CandidateSource::QuickPhrase,
            "Quick-phrase mode did not expose prefix candidates.");
    const auto quick_phrase_commit = quick_phrase_session.select_candidate(1);
    require(quick_phrase_commit.handled && quick_phrase_commit.commit == "快捷短语二" &&
                quick_phrase_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Committing a quick phrase did not leave the local mode.");

    require(quick_phrase_session.handle_character('K', true).handled,
            "Quick-phrase mode could not be re-entered for invalid-input coverage.");
    const std::string quick_phrase_prefix = quick_phrase_session.preedit();
    require(quick_phrase_session.handle_character('1').handled && quick_phrase_session.preedit() == quick_phrase_prefix,
            "Invalid quick-phrase input leaked into normal composition or changed preedit.");
    require(quick_phrase_session.handle_command(metasequoia::Command::Backspace).handled &&
                quick_phrase_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Backspace on a bare quick-phrase prefix did not leave the mode.");

    metasequoia::LocalModeOptions disabled_quick_phrase_options;
    disabled_quick_phrase_options.quick_phrase = false;
    metasequoia::InputSession disabled_quick_phrase(SchemeType::Quanpin);
    disabled_quick_phrase.set_local_mode_options(disabled_quick_phrase_options);
    require(!disabled_quick_phrase.handle_character('K', true).handled && !disabled_quick_phrase.has_composition() &&
                disabled_quick_phrase.local_input_mode() == metasequoia::LocalInputMode::None,
            "A disabled quick-phrase shortcut swallowed Shift+K.");

    const std::filesystem::path missing_quick_phrase_directory = data_directory / "quick-phrase-missing";
    std::filesystem::create_directories(missing_quick_phrase_directory);
    set_data_directory(missing_quick_phrase_directory);
    metasequoia::InputSession missing_quick_phrase(SchemeType::Quanpin);
    require(missing_quick_phrase.handle_character('K', true).handled,
            "Quick-phrase mode could not start with a missing database.");
    const auto missing_quick_phrase_result = missing_quick_phrase.handle_character('a');
    require(missing_quick_phrase_result.handled && missing_quick_phrase_result.diagnostic.has_value() &&
                missing_quick_phrase.candidates().empty(),
            "A missing quick-phrase database did not report a non-blocking diagnostic.");

    const std::filesystem::path corrupt_quick_phrase_directory = data_directory / "quick-phrase-corrupt";
    std::filesystem::create_directories(corrupt_quick_phrase_directory);
    write_file(corrupt_quick_phrase_directory / "msime.db", "not a sqlite database");
    set_data_directory(corrupt_quick_phrase_directory);
    metasequoia::InputSession corrupt_quick_phrase(SchemeType::Quanpin);
    require(corrupt_quick_phrase.handle_character('K', true).handled,
            "Quick-phrase mode could not start with a corrupt database.");
    const auto corrupt_quick_phrase_result = corrupt_quick_phrase.handle_character('a');
    require(corrupt_quick_phrase_result.handled && corrupt_quick_phrase_result.diagnostic.has_value() &&
                corrupt_quick_phrase.candidates().empty(),
            "A corrupt quick-phrase database did not report a non-blocking diagnostic.");

    const std::filesystem::path expressive_directory = data_directory / "expressive-modes";
    std::filesystem::create_directories(expressive_directory);
    {
        Database database(expressive_directory / "others.db");
        database.execute("CREATE TABLE emoji_pinyin(key TEXT,emoji TEXT,sort_order INTEGER)");
        database.execute("INSERT INTO emoji_pinyin VALUES('xiaolian','😀',10)");
        database.execute("INSERT INTO emoji_pinyin VALUES('xiao''lian','😄',20)");
        database.execute("CREATE TABLE kaomoji(pinyin TEXT,jianpin TEXT,kaomoji TEXT,sort_order INTEGER)");
        database.execute("INSERT INTO kaomoji VALUES('haixiu','hx','(*/ω＼*)',10)");
    }
    set_data_directory(expressive_directory);

    metasequoia::InputSession emoji_session(SchemeType::Quanpin);
    require(emoji_session.handle_character('E', true).handled &&
                emoji_session.local_input_mode() == metasequoia::LocalInputMode::Emoji &&
                emoji_session.preedit() == "E" && emoji_session.candidates().empty(),
            "Shift+E did not enter an empty Emoji composition.");
    type(emoji_session, "XIAOLIAN");
    require(emoji_session.preedit() == "EXIAOLIAN" && emoji_session.candidates().size() == 1 &&
                emoji_session.candidates().front().word == "😀" &&
                emoji_session.candidates().front().source == CandidateSource::Emoji,
            "Emoji mode did not accept uppercase input or expose its candidate.");
    const auto emoji_commit = emoji_session.handle_command(metasequoia::Command::CommitCandidate);
    require(emoji_commit.handled && emoji_commit.commit == "😀" &&
                emoji_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Committing an Emoji candidate did not leave Emoji mode.");

    require(emoji_session.handle_character('E', true).handled,
            "Emoji mode could not be re-entered for apostrophe coverage.");
    type(emoji_session, "xiao'lian");
    require(emoji_session.preedit() == "Exiao'lian" && emoji_session.candidates().size() == 1 &&
                emoji_session.candidates().front().word == "😄",
            "Emoji mode did not retain and query an apostrophe.");
    const std::string emoji_preedit = emoji_session.preedit();
    require(emoji_session.handle_character('1').handled && emoji_session.preedit() == emoji_preedit,
            "Invalid Emoji input changed the local composition.");
    require(emoji_session.handle_command(metasequoia::Command::Backspace).handled &&
                emoji_session.preedit() == "Exiao'lia",
            "Emoji-mode Backspace did not edit and refresh the composition.");
    require(emoji_session.handle_command(metasequoia::Command::Cancel).handled &&
                emoji_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Cancel did not leave Emoji mode.");

    metasequoia::InputSession kaomoji_session(SchemeType::Shuangpin);
    require(kaomoji_session.handle_character('M', true).handled &&
                kaomoji_session.local_input_mode() == metasequoia::LocalInputMode::Kaomoji,
            "Shift+M did not enter kaomoji mode in Shuangpin.");
    type(kaomoji_session, "hx");
    require(kaomoji_session.candidates().size() == 1 && kaomoji_session.candidates().front().word == "(*/ω＼*)" &&
                kaomoji_session.candidates().front().source == CandidateSource::Kaomoji,
            "Kaomoji mode did not expose its Shuangpin-expanded candidate.");
    const auto kaomoji_commit = kaomoji_session.select_candidate(0);
    require(kaomoji_commit.handled && kaomoji_commit.commit == "(*/ω＼*)" &&
                kaomoji_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Committing a kaomoji did not leave kaomoji mode.");

    metasequoia::LocalModeOptions disabled_expressive_options;
    disabled_expressive_options.emoji = false;
    disabled_expressive_options.kaomoji = false;
    metasequoia::InputSession disabled_expressive(SchemeType::Quanpin);
    disabled_expressive.set_local_mode_options(disabled_expressive_options);
    require(!disabled_expressive.handle_character('E', true).handled && !disabled_expressive.has_composition() &&
                disabled_expressive.local_input_mode() == metasequoia::LocalInputMode::None,
            "A disabled Emoji shortcut swallowed Shift+E.");
    require(!disabled_expressive.handle_character('M', true).handled && !disabled_expressive.has_composition() &&
                disabled_expressive.local_input_mode() == metasequoia::LocalInputMode::None,
            "A disabled kaomoji shortcut swallowed Shift+M.");

    metasequoia::InputSession disabling_active_expressive(SchemeType::Quanpin);
    require(disabling_active_expressive.handle_character('E', true).handled &&
                disabling_active_expressive.local_input_mode() == metasequoia::LocalInputMode::Emoji,
            "Emoji mode could not start before option-reset coverage.");
    auto disable_active_options = disabling_active_expressive.local_mode_options();
    disable_active_options.emoji = false;
    disabling_active_expressive.set_local_mode_options(disable_active_options);
    require(!disabling_active_expressive.has_composition() &&
                disabling_active_expressive.local_input_mode() == metasequoia::LocalInputMode::None,
            "Disabling an active Emoji mode did not reset it.");

    metasequoia::InputSession wubi_expressive(SchemeType::Wubi);
    require(!wubi_expressive.handle_character('E', true).handled && !wubi_expressive.has_composition() &&
                wubi_expressive.local_input_mode() == metasequoia::LocalInputMode::None,
            "Shift+E was swallowed outside a pinyin scheme.");
    require(!wubi_expressive.handle_character('M', true).handled && !wubi_expressive.has_composition() &&
                wubi_expressive.local_input_mode() == metasequoia::LocalInputMode::None,
            "Shift+M was swallowed outside a pinyin scheme.");

    const std::filesystem::path missing_expressive_directory = data_directory / "expressive-missing";
    std::filesystem::create_directories(missing_expressive_directory);
    set_data_directory(missing_expressive_directory);
    metasequoia::InputSession missing_emoji_session(SchemeType::Quanpin);
    require(missing_emoji_session.handle_character('E', true).handled,
            "Emoji mode could not start with a missing database.");
    const auto missing_emoji_result = missing_emoji_session.handle_character('x');
    require(missing_emoji_result.handled && missing_emoji_result.diagnostic.has_value() &&
                missing_emoji_session.candidates().empty(),
            "A missing Emoji database did not report a non-blocking diagnostic.");
#endif

    return 0;
}

int main()
{
    try
    {
        return run_test();
    }
    catch (const std::exception &exception)
    {
        std::fprintf(stderr, "%s\n", exception.what());
        return 1;
    }
    catch (...)
    {
        std::fprintf(stderr, "An exception escaped the test body.\n");
        return 1;
    }
}
