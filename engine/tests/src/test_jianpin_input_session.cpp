#include "../../core/data_path.h"
#include "../../core/input_session.h"
#include "../../local_modes/jianpin_query.h"
#include "../../user_dictionary/user_dictionary_journal.h"
#include "test_directory_cleanup.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <cstdio>

namespace
{
class Database
{
  public:
    explicit Database(const std::filesystem::path &path)
    {
        if (sqlite3_open(metasequoia::path_to_utf8(path).c_str(), &database_) != SQLITE_OK)
        {
            throw std::runtime_error("Failed to create a super-jianpin test dictionary.");
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

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool contains(const std::vector<WordItem> &candidates, const std::string &word)
{
    return std::any_of(candidates.begin(), candidates.end(),
                       [&](const WordItem &candidate) { return candidate.word == word; });
}

void type(metasequoia::InputSession &session, const std::string &text)
{
    for (const char character : text)
    {
        require(session.handle_character(character).handled, "A super-jianpin test character was not handled.");
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
        throw std::runtime_error("Failed to set the super-jianpin test data directory.");
    }
}

void prepare_database(const std::filesystem::path &directory)
{
    std::filesystem::create_directories(directory);
    Database database(directory / "msime.db");
    database.execute("BEGIN;"
                     "CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                     "INSERT INTO tbl_1_n VALUES('ni','n','你',300);"
                     "INSERT INTO tbl_1_n VALUES('na','n','拿',200);"
                     "CREATE TABLE tbl_2_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                     "INSERT INTO tbl_2_n VALUES('ni''hao','nh','你好',300);"
                     "INSERT INTO tbl_2_n VALUES('ni''hao','nh','拟好',200);"
                     "INSERT INTO tbl_2_n VALUES('na''han','nh','呐喊',100);"
                     "INSERT INTO tbl_2_n VALUES('ni''shuo','ns','你说',290);"
                     "INSERT INTO tbl_2_n VALUES('ni''si','ns','你思',280);"
                     "INSERT INTO tbl_2_n VALUES('ni''u','nu','你屋',100);"
                     "CREATE TABLE tbl_2_a(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                     "INSERT INTO tbl_2_a VALUES('ai''ni','an','爱你',250);"
                     "CREATE TABLE tbl_2_z(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                     "INSERT INTO tbl_2_z VALUES('zhi''chi','zc','知耻',240);"
                     "COMMIT;");
}

void write_file(const std::filesystem::path &path, const std::string &contents)
{
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
    if (!stream)
    {
        throw std::runtime_error("Failed to prepare a corrupt super-jianpin dictionary.");
    }
}
} // namespace

int run_test()
{
    const auto suffix = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("metasequoia-jianpin-" + suffix);
    metasequoia::test::ScopedDataDirectoryCleanup cleanup(root);
    const std::filesystem::path complete = root / "complete";
    prepare_database(complete);

    const auto quanpin = metasequoia::local_modes::query_jianpin("nh", SchemeType::Quanpin, complete / "msime.db", 50);
    require(!quanpin.diagnostic.has_value() && contains(quanpin.candidates, "你好") &&
                contains(quanpin.candidates, "呐喊"),
            "Quanpin super-jianpin did not match two-initial candidates.");
    require(std::all_of(quanpin.candidates.begin(), quanpin.candidates.end(),
                        [](const WordItem &candidate) {
                            return candidate.source == CandidateSource::Database && !candidate.canonical_pinyin.empty();
                        }),
            "Super-jianpin candidates did not retain database source and canonical keys.");
    const auto uppercase = metasequoia::local_modes::query_jianpin("NH", SchemeType::Quanpin, complete / "msime.db", 1);
    require(uppercase.candidates.size() == 1 && uppercase.candidates.front().word == "你好",
            "Super-jianpin did not normalize case or honor the result limit.");
    const auto two_initials =
        metasequoia::local_modes::query_jianpin("an", SchemeType::Quanpin, complete / "msime.db", 50);
    require(contains(two_initials.candidates, "爱你") && !contains(two_initials.candidates, "安"),
            "Super-jianpin treated multiple letters as one full syllable.");
    require(metasequoia::local_modes::query_jianpin("n'", SchemeType::Quanpin, complete / "msime.db", 50)
                .candidates.empty(),
            "Super-jianpin accepted non-letter input.");

    require(metasequoia::local_modes::jianpin_ranking_context("nh", SchemeType::Quanpin) == "n'h" &&
                metasequoia::local_modes::jianpin_ranking_context("nu", SchemeType::Shuangpin,
                                                                  GetXiaoheShuangpinProfile()) == "n'sh" &&
                metasequoia::local_modes::jianpin_ranking_context("ns", SchemeType::Shuangpin,
                                                                  GetXiaoheShuangpinProfile()) == "n's" &&
                metasequoia::local_modes::jianpin_ranking_context("vi", SchemeType::Shuangpin,
                                                                  GetXiaoheShuangpinProfile()) == "zh'ch" &&
                metasequoia::local_modes::jianpin_ranking_context("ne", SchemeType::Shuangpin,
                                                                  GetShoudaoShuangpinProfile()) == "n'sh" &&
                metasequoia::local_modes::jianpin_ranking_context("nu", SchemeType::Shuangpin,
                                                                  GetShoudaoShuangpinProfile()) == "n'u" &&
                metasequoia::local_modes::jianpin_ranking_context("nu", SchemeType::Shuangpin,
                                                                  GetZiranmaShuangpinProfile()) == "n'sh" &&
                metasequoia::local_modes::jianpin_ranking_context("nu", SchemeType::Shuangpin,
                                                                  GetMicrosoftShuangpinProfile()) == "n'sh",
            "Super-jianpin did not decode the supported Shuangpin initial maps.");
    const auto xiaohe_nu = metasequoia::local_modes::query_jianpin("nu", SchemeType::Shuangpin, complete / "msime.db",
                                                                   50, GetXiaoheShuangpinProfile());
    const auto xiaohe_ns = metasequoia::local_modes::query_jianpin("ns", SchemeType::Shuangpin, complete / "msime.db",
                                                                   50, GetXiaoheShuangpinProfile());
    const auto shoudao_ne = metasequoia::local_modes::query_jianpin("ne", SchemeType::Shuangpin, complete / "msime.db",
                                                                    50, GetShoudaoShuangpinProfile());
    require(contains(xiaohe_nu.candidates, "你说") && !contains(xiaohe_nu.candidates, "你思") &&
                contains(xiaohe_ns.candidates, "你思") && !contains(xiaohe_ns.candidates, "你说") &&
                contains(shoudao_ne.candidates, "你说"),
            "Ambiguous Shuangpin super-jianpin inputs were not distinguished by canonical initials.");

    const std::filesystem::path missing = root / "missing" / "msime.db";
    const auto missing_result = metasequoia::local_modes::query_jianpin("nh", SchemeType::Quanpin, missing, 50);
    require(missing_result.candidates.empty() && missing_result.diagnostic.has_value() &&
                !std::filesystem::exists(missing),
            "A missing super-jianpin database was created or not diagnosed.");
    const std::filesystem::path corrupt = root / "corrupt" / "msime.db";
    std::filesystem::create_directories(corrupt.parent_path());
    write_file(corrupt, "not a sqlite database");
    const auto corrupt_result = metasequoia::local_modes::query_jianpin("nh", SchemeType::Quanpin, corrupt, 50);
    require(corrupt_result.candidates.empty() && corrupt_result.diagnostic.has_value(),
            "A corrupt super-jianpin database was not isolated.");

    set_data_directory(complete);
    // 超级简拼只保留给双拼：全拼直接打声母就是简拼，Shift+J 在全拼下不再是特殊模式。
    metasequoia::InputSession session(SchemeType::Shuangpin);
    require(session.handle_character('J', true).handled &&
                session.local_input_mode() == metasequoia::LocalInputMode::SuperJianpin && session.preedit() == "J" &&
                session.candidates().empty(),
            "Shift+J did not enter an empty super-jianpin composition.");
    type(session, "nh");
    require(session.preedit() == "Jnh" && contains(session.candidates(), "你好"),
            "The super-jianpin session did not expose matching candidates.");
    const auto committed = session.select_candidate(std::string("你好"));
    require(committed.handled && committed.commit == "你好" &&
                session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Committing a super-jianpin candidate did not reset the mode.");
    require(session.handle_character('J', true).handled &&
                session.handle_command(metasequoia::Command::Backspace).handled &&
                session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Backspace on a bare super-jianpin prefix did not exit the mode.");

    metasequoia::LocalModeOptions disabled_options;
    disabled_options.super_jianpin = false;
    metasequoia::InputSession disabled(SchemeType::Shuangpin);
    disabled.set_local_mode_options(disabled_options);
    require(!disabled.handle_character('J', true).handled && !disabled.has_composition() &&
                disabled.local_input_mode() == metasequoia::LocalInputMode::None,
            "A disabled super-jianpin shortcut swallowed Shift+J.");
    metasequoia::InputSession quanpin_session(SchemeType::Quanpin);
    (void)quanpin_session.handle_character('J', true);
    require(quanpin_session.local_input_mode() != metasequoia::LocalInputMode::SuperJianpin,
            "Shift+J entered super-jianpin in quanpin; the shortcut is shuangpin-only.");
    metasequoia::InputSession wubi(SchemeType::Wubi);
    require(!wubi.handle_character('J', true).handled && !wubi.has_composition() &&
                wubi.local_input_mode() == metasequoia::LocalInputMode::None,
            "Shift+J was swallowed outside a pinyin scheme.");

    user_dictionary::close_default_user_database();
    metasequoia::InputSession learning(SchemeType::Shuangpin);
    require(learning.set_frequency_adjustment({metasequoia::FrequencyAdjustmentMode::Pin, 1, 1}),
            "A valid super-jianpin frequency configuration was rejected.");
    require(learning.handle_character('J', true).handled, "Super-jianpin learning mode did not start.");
    type(learning, "nh");
    const auto learned = learning.select_candidate(std::string("拟好"));
    require(learned.handled && learned.commit == "拟好" && !learned.diagnostic.has_value(),
            "Super-jianpin frequency learning blocked candidate commit.");
    metasequoia::InputSession reopened(SchemeType::Shuangpin);
    require(reopened.handle_character('J', true).handled, "Super-jianpin mode did not reopen after learning.");
    type(reopened, "nh");
    require(!reopened.candidates().empty() && reopened.candidates().front().word == "拟好",
            "Super-jianpin frequency learning did not persist through its canonical ranking key.");
    user_dictionary::close_default_user_database();

    return 0;
}

int main()
{
    try
    {
        return run_test();
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    catch (...)
    {
        std::fprintf(stderr, "An exception escaped the test body.\n");
        return 1;
    }
}
