#include "../../core/input_session.h"
#include "../../core/data_path.h"
#include "../../common/helpcode_utils.h"
#include "../../direct_helpcode/spelling_graph.h"
#include "../../shuangpin/shuangpin_profile.h"
#include "test_directory_cleanup.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// 双拼直接辅助码（万象式）。夹具用小鹤双拼：shi = ui。辅助码表（lantian 槽位）：
//   石=ab  狮=qa  事=yb  实=tb  时=rc  世=ss
// 词库：事实 500、时事 300、石狮 100（都读 shi'shi），单字 shi 若干。
namespace
{
using direct_helpcode::SpellingKind;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void run_double_helpcode_filter_tests()
{
    const HelpcodeUtils::Keymap keymap{{"甲", "ab"}, {"乙", "cd"}, {"丙", "ef"}};
    require(HelpcodeUtils::matches_double_helpcodes("甲乙", "xd", &keymap),
            "A phrase was rejected although the second helpcode matched its last character.");
    require(!HelpcodeUtils::matches_double_helpcodes("甲乙", "xc", &keymap),
            "A phrase was accepted although the second helpcode did not match its last character.");
    require(HelpcodeUtils::matches_double_helpcodes("甲乙丙", "xf", &keymap),
            "A sentence was rejected although the second helpcode matched its last character.");
    require(HelpcodeUtils::matches_double_helpcodes("甲", "ab", &keymap),
            "The existing exact two-code match for a single character changed.");
    require(!HelpcodeUtils::matches_double_helpcodes("甲", "xb", &keymap),
            "Single-character matching stopped checking its first helpcode.");
}

class Database
{
  public:
    explicit Database(const std::filesystem::path &path)
    {
        if (sqlite3_open(metasequoia::path_to_utf8(path).c_str(), &database_) != SQLITE_OK)
        {
            throw std::runtime_error("Failed to create the direct-helpcode test dictionary.");
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

  private:
    sqlite3 *database_ = nullptr;
};

void write_file(const std::filesystem::path &path, const std::string &contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path);
    stream << contents;
    if (!stream)
    {
        throw std::runtime_error("Failed to prepare a direct-helpcode fixture.");
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

void type(metasequoia::InputSession &session, const std::string &text)
{
    for (const char character : text)
    {
        if (!session.handle_character(character).handled)
        {
            throw std::runtime_error(std::string("A key was not handled: ") + character);
        }
    }
}

std::size_t candidate_index(const metasequoia::InputSession &session, const std::string &word)
{
    const auto &items = session.candidates();
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        if (items[i].word == word)
            return i;
    }
    return items.size();
}

bool has_word(const metasequoia::InputSession &session, const std::string &word)
{
    return candidate_index(session, word) < session.candidates().size();
}

std::string first_word(const metasequoia::InputSession &session)
{
    return session.candidates().empty() ? std::string{} : session.candidates().front().word;
}

const direct_helpcode::SyllableSpelling *find_edge(const direct_helpcode::SpellingGraph &graph, std::size_t pos,
                                                   SpellingKind kind)
{
    if (pos >= graph.edges.size())
        return nullptr;
    for (const auto &spelling : graph.edges[pos])
    {
        if (spelling.kind == kind)
            return &spelling;
    }
    return nullptr;
}

// 纯拼写图：不碰词库，只验证万象的拼写规则和 librime 的缩写剪枝。
void run_spelling_graph_tests()
{
    const ShuangpinProfile &profile = GetXiaoheShuangpinProfile();
    // shi 下有辅码以 a、q 开头的字，以及完整的 ab、ui。
    const direct_helpcode::AuxPredicate has_aux = [](const std::string &quanpin, char first, char second) {
        if (quanpin != "shi")
            return false;
        if (second == 0)
            return first == 'a' || first == 'q' || first == 'u';
        return (first == 'a' && second == 'b') || (first == 'u' && second == 'i');
    };

    {
        const auto graph = direct_helpcode::build_spelling_graph("uiauiq", profile, has_aux);
        require(!graph.empty(), "uiauiq produced no spelling graph.");
        const auto *first = find_edge(graph, 0, SpellingKind::Aux1);
        const auto *second = find_edge(graph, 3, SpellingKind::Aux1);
        require(first && first->end == 3 && first->first == 'a' && first->quanpin == "shi",
                "uia was not recognised as shi with first code a.");
        require(second && second->end == 6 && second->first == 'q', "uiq was not recognised as shi with code q.");
        require(!find_edge(graph, 0, SpellingKind::Plain),
                "ui at the start survived although it cannot reach the end of uiauiq.");
    }

    {
        // 整串就是四码：没有普通切法能走完，缩写边保留。
        const auto graph = direct_helpcode::build_spelling_graph("uiab", profile, has_aux);
        const auto *edge = find_edge(graph, 0, SpellingKind::Aux2Abbrev);
        require(edge && edge->end == 4 && edge->first == 'a' && edge->second == 'b',
                "A whole-input four-key code was not kept as an abbreviation.");
    }

    {
        // ui|ui 是一条完整的普通切法：四码 uiui（shi + 辅码 ui）是缩写，被剪掉；三码 uiu 之后只剩一个
        // 声母缩写，也走不到底。
        const auto graph = direct_helpcode::build_spelling_graph("uiui", profile, has_aux);
        require(find_edge(graph, 0, SpellingKind::Plain) && !find_edge(graph, 0, SpellingKind::Aux2Abbrev) &&
                    !find_edge(graph, 0, SpellingKind::Aux1),
                "A four-key code survived next to a complete normal segmentation.");
    }

    {
        // 补上 / 之后四码是正常拼写，成为唯一走得通的切法。
        const auto graph = direct_helpcode::build_spelling_graph("uiui/", profile, has_aux);
        const auto *edge = find_edge(graph, 0, SpellingKind::Aux2Slash);
        require(edge && edge->end == 5 && edge->first == 'u' && edge->second == 'i',
                "The slash did not turn the four-key code into a normal spelling.");
        require(!find_edge(graph, 0, SpellingKind::Plain), "ui|ui survived although the slash cannot be consumed.");
    }

    {
        // 第二位辅码大写也结束四码：和补 / 一样是正常拼写，uiaB 之后接着打。
        direct_helpcode::SpellingOptions options;
        options.uppercase_marker = true;
        const auto graph = direct_helpcode::build_spelling_graph("uiaBui", profile, has_aux, options);
        const auto *edge = find_edge(graph, 0, SpellingKind::Aux2Upper);
        require(edge && edge->end == 4 && edge->first == 'a' && edge->second == 'b',
                "An uppercase second code did not end the four-key code.");
        require(!find_edge(graph, 0, SpellingKind::Plain) && !find_edge(graph, 0, SpellingKind::Aux1),
                "A segmentation consuming the uppercase letter elsewhere survived.");
        // 大写标记关着时同一串里的大写字母不算标记。
        const auto off = direct_helpcode::build_spelling_graph("uiaB", profile, has_aux);
        require(!find_edge(off, 0, SpellingKind::Aux2Upper) && find_edge(off, 0, SpellingKind::Aux2Abbrev),
                "An uppercase second code was a marker with the uppercase marker off.");
    }

    {
        // 大写标记开着但大写字母放不进第二位辅码（第一位辅码敲成大写）：退回不分大小写的旧规则。
        direct_helpcode::SpellingOptions options;
        options.uppercase_marker = true;
        const auto graph = direct_helpcode::build_spelling_graph("uiAuiQ", profile, has_aux, options);
        require(find_edge(graph, 0, SpellingKind::Aux1) && find_edge(graph, 3, SpellingKind::Aux1),
                "Uppercase first codes stopped working with the uppercase marker on.");
    }

    {
        // / 标记关着：uiui/ 不再是四码，/ 退回成分隔符。
        direct_helpcode::SpellingOptions options;
        options.slash_marker = false;
        options.uppercase_marker = true;
        const auto graph = direct_helpcode::build_spelling_graph("uiui/", profile, has_aux, options);
        require(!find_edge(graph, 0, SpellingKind::Aux2Slash) && graph.skip[4] &&
                    find_edge(graph, 0, SpellingKind::Plain),
                "The slash still ended a four-key code with the slash marker off.");
    }

    {
        // 凑不成四码的 / 退回成分隔符，整串不作废。
        const auto graph = direct_helpcode::build_spelling_graph("ui/", profile, has_aux);
        require(!graph.empty() && graph.skip[2] && find_edge(graph, 0, SpellingKind::Plain),
                "An unusable slash invalidated the whole input.");
    }

    {
        // 打到一半剩一个声母：没有完整切法时声母缩写保留。
        const auto graph = direct_helpcode::build_spelling_graph("uix", profile, has_aux);
        require(find_edge(graph, 0, SpellingKind::Plain) && find_edge(graph, 2, SpellingKind::Initial),
                "A trailing initial was not kept when nothing else reaches the end.");
    }

    {
        // 只有一条完整切分时不必整句解码：uiauiq 只能切成 uia|uiq。
        std::vector<direct_helpcode::SyllableSpelling> path;
        const auto graph = direct_helpcode::build_spelling_graph("uiauiq", profile, has_aux);
        require(direct_helpcode::single_path(graph, path) && path.size() == 2 && path[0].end == 3 && path[1].end == 6,
                "uiauiq was not recognised as a single segmentation.");
        // uiu 能切成 ui|u（声母缩写）或 uiu（三码），剪枝后只剩三码，同样唯一。
        const auto three = direct_helpcode::build_spelling_graph("uiu", profile, has_aux);
        require(direct_helpcode::single_path(three, path) && path.size() == 1 && path[0].kind == SpellingKind::Aux1,
                "uiu did not reduce to its three-key spelling.");
        // 走不通的图没有唯一切分。
        require(!direct_helpcode::single_path(direct_helpcode::SpellingGraph{}, path) && path.empty(),
                "An empty graph was reported as a single segmentation.");
    }

    {
        // 没有辅码可用时（码表里没有）三码不成立。
        const direct_helpcode::AuxPredicate none = [](const std::string &, char, char) { return false; };
        const auto graph = direct_helpcode::build_spelling_graph("uia", profile, none);
        require(!find_edge(graph, 0, SpellingKind::Aux1), "A three-key code was accepted without any matching char.");
    }
}

void prepare_fixture(const std::filesystem::path &directory)
{
    write_file(directory / "helpcodes" / "helpcode.txt", "石=ab\n狮=qa\n事=yb\n实=tb\n时=rc\n世=ss\n");
    Database database(directory / "msime.db");
    database.execute("BEGIN;"
                     "CREATE TABLE tbl_1_s(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                     "INSERT INTO tbl_1_s VALUES('shi', 's', '是', 900);"
                     "INSERT INTO tbl_1_s VALUES('shi', 's', '时', 800);"
                     "INSERT INTO tbl_1_s VALUES('shi', 's', '事', 700);"
                     "INSERT INTO tbl_1_s VALUES('shi', 's', '实', 600);"
                     "INSERT INTO tbl_1_s VALUES('shi', 's', '世', 500);"
                     "INSERT INTO tbl_1_s VALUES('shi', 's', '石', 400);"
                     "INSERT INTO tbl_1_s VALUES('shi', 's', '狮', 300);"
                     "CREATE TABLE tbl_2_s(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                     "INSERT INTO tbl_2_s VALUES('shi''shi', 'ss', '事实', 500);"
                     "INSERT INTO tbl_2_s VALUES('shi''shi', 'ss', '时事', 300);"
                     "INSERT INTO tbl_2_s VALUES('shi''shi', 'ss', '石狮', 100);"
                     "COMMIT;");
}

std::unique_ptr<metasequoia::InputSession> direct_session(bool enabled = true)
{
    auto session = std::make_unique<metasequoia::InputSession>(SchemeType::Shuangpin);
    session->set_shuangpin_helpcode_enabled(false);
    session->set_direct_helpcode_enabled(enabled);
    require(session->set_helpcode_schema("lantian"), "The fixture helpcode table was not selected.");
    return session;
}

void run_session_tests()
{
    {
        // 不带辅码的输入：与开关关着时完全一样。
        auto on = direct_session(true);
        auto off = direct_session(false);
        type(*on, "uiui");
        type(*off, "uiui");
        require(first_word(*on) == "事实" && first_word(*on) == first_word(*off) &&
                    on->candidates().size() == off->candidates().size(),
                "Plain shuangpin changed when direct helpcode was turned on.");
        require(on->get_pinyin_sequence() == "uiui", "Plain shuangpin was rewritten by the resolver.");
        require(on->segment_raw_boundaries() == std::vector<std::size_t>({0, 2, 4}) &&
                    on->segment_raw_boundaries() == off->segment_raw_boundaries(),
                "Plain shuangpin lost its unit boundaries when direct helpcode was turned on.");
    }

    {
        // 三码自动识别，不要引导键：uia = 石（ab），uiq = 狮（qa）。
        auto session = direct_session();
        type(*session, "uiauiq");
        require(first_word(*session) == "石狮", "uiauiq did not decode to 石狮.");
        require(!has_word(*session, "事实") && !has_word(*session, "时事"),
                "Candidates that violate the direct helpcodes were kept.");
        require(session->preedit() == "uiauiq" && session->get_pinyin_sequence_with_cases() == "uiauiq",
                "The typed direct helpcode letters were not kept as typed.");
        require(session->get_pinyin_sequence() == "ui'ui", "The resolver did not strip the helpcode letters.");
        require(session->get_pinyin_segmentation_with_cases() == "uia'uiq",
                "The helpcode letters were not shown after their syllables.");
        // 按段删除 / 跳光标的单元：每个音节和它后面的辅码各算一个。
        require(session->segment_raw_boundaries() == std::vector<std::size_t>({0, 2, 3, 5, 6}),
                "The direct helpcode letters were not units of their own.");
    }

    {
        // 单独三码：石。
        auto session = direct_session();
        type(*session, "uia");
        require(first_word(*session) == "石", "uia did not put 石 first.");
        require(!has_word(*session, "是"), "A char without the typed helpcode survived uia.");
    }

    {
        // 大写辅码与小写等价（万象的大写变体）。
        auto session = direct_session();
        type(*session, "uiAuiQ");
        require(first_word(*session) == "石狮", "Uppercase direct helpcodes were not accepted.");
    }

    {
        // 整串四码：两位辅码定字。
        auto session = direct_session();
        type(*session, "uiab");
        require(first_word(*session) == "石", "A whole-input four-key code did not select 石.");
    }

    {
        // / 的门槛：前面不是「两键 + 两码」时不收。
        auto session = direct_session();
        type(*session, "uia");
        require(!session->handle_character('/').handled && session->preedit() == "uia",
                "A slash was accepted after a three-key code.");
        type(*session, "b");
        require(session->handle_character('/').handled && session->preedit() == "uiab/",
                "A slash after a four-key code was not accepted.");
        require(!session->handle_character('/').handled, "A second slash was accepted.");
        // 四码 + / 之后接着打：/ 是终止键，后面的音节照常解码。
        type(*session, "ui");
        require(first_word(*session) == "石狮", "uiab/ui did not keep 石 on the first syllable.");
        require(session->get_pinyin_segmentation_with_cases() == "uiab/'ui",
                "The four-key code and its slash were not shown after their syllable.");
        require(session->segment_raw_boundaries() == std::vector<std::size_t>({0, 2, 5, 7}),
                "The four-key code and its slash were not one unit.");
    }

    {
        // 只留大写标记：/ 不再收，第二位辅码大写结束四码，后面的音节照常解码。
        auto session = direct_session();
        session->set_direct_helpcode_markers(false, true);
        type(*session, "uiab");
        require(!session->handle_character('/').handled, "A slash was accepted with the slash marker off.");
        require(session->handle_command(metasequoia::Command::Backspace).handled && session->preedit() == "uia",
                "Backspace did not remove the second code.");
        type(*session, "Bui");
        require(first_word(*session) == "石狮", "uiaBui did not keep 石 on the first syllable.");
        require(session->get_pinyin_segmentation_with_cases() == "uiaB'ui",
                "The uppercase four-key code was not shown after its syllable.");
        require(session->segment_raw_boundaries() == std::vector<std::size_t>({0, 2, 4, 6}),
                "The uppercase four-key code was not one unit.");
    }

    {
        // 开关关着：/ 不收，三码不当辅码。
        auto session = direct_session(false);
        type(*session, "uiab");
        require(!session->handle_character('/').handled, "A slash was accepted with direct helpcode off.");
    }

    {
        // 选掉前一个字，后面的辅码随剩余部分留下来。
        auto session = direct_session();
        type(*session, "uiauiq");
        const std::size_t index = candidate_index(*session, "石");
        require(index < session->candidates().size(), "The single char 石 was not offered for uiauiq.");
        const auto committed = session->select_candidate(index);
        require(committed.commit == "石" && session->preedit() == "uiq",
                "Selecting the first char did not leave the remaining syllable with its helpcode.");
        require(first_word(*session) == "狮", "The carried helpcode no longer filtered the remaining syllable.");
    }

    {
        // 退格删掉辅码字母后回到普通双拼。
        auto session = direct_session();
        type(*session, "uia");
        require(session->handle_command(metasequoia::Command::Backspace).handled && session->preedit() == "ui",
                "Backspace did not remove the helpcode letter.");
        // 前面的用例选过「石」，调频已经把它提上来了，所以和关着开关的会话比，而不是写死首选。
        auto off = direct_session(false);
        type(*off, "ui");
        require(session->candidates().size() == off->candidates().size() && first_word(*session) == first_word(*off),
                "Removing the helpcode letter did not restore plain candidates.");
    }

    {
        // 直接辅助码开着时反引号不再是编码键。
        auto session = direct_session();
        session->set_mid_sentence_helpcode_enabled(true);
        type(*session, "ui");
        require(!session->handle_character('`').handled, "A backquote was accepted while direct helpcode was on.");
    }
}

int run()
{
    run_double_helpcode_filter_tests();
    run_spelling_graph_tests();

    const auto unique_suffix = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const std::filesystem::path data_directory =
        std::filesystem::temp_directory_path() / ("metasequoia-direct-helpcode-" + unique_suffix);
    metasequoia::test::ScopedDataDirectoryCleanup cleanup(data_directory);
    std::filesystem::create_directories(data_directory);
    set_data_directory(data_directory);
    prepare_fixture(data_directory);
    run_session_tests();
    return 0;
}
} // namespace

int main()
{
    try
    {
        return run();
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "test_direct_helpcode failed: %s\n", error.what());
        return 1;
    }
}
