#include <metasequoia/session.h>
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include "contracts/assets/assets.h"
#include <cstdio>

using namespace metasequoia;
void require(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
std::size_t candidate(Session &session, const std::string &word)
{
    const auto view = session.snapshot();
    for (std::size_t i = 0; i < view.candidates.size(); ++i)
        if (view.candidates[i].word == word)
            return i;
    throw std::runtime_error("Missing candidate: " + word);
}
void type(Session &session, const std::string &digits)
{
    for (char digit : digits)
        require(session.character(digit).handled, "digit unhandled");
}
int run_test()
{
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("msime-nine-key-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{directory};
    sqlite3 *db = nullptr;
    require(sqlite3_open((directory / "msime.db").u8string().c_str(), &db) == SQLITE_OK, "open fixture");
    require(sqlite3_exec(db,
                         "CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_n VALUES('ni','n','你',100);"
                         "CREATE TABLE tbl_1_m(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_m VALUES('mi','m','米',50);"
                         "CREATE TABLE tbl_1_h(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_h VALUES('hao','h','好',100);"
                         "CREATE TABLE tbl_1_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_x VALUES('xuan','x','选',100);"
                         "INSERT INTO tbl_1_x VALUES('xun','x','巡',100);"
                         "CREATE TABLE tbl_4_w(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_4_w VALUES('wu''an''quan''yuan','waqy','无安全员',100);"
                         "CREATE TABLE tbl_2_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_n VALUES('ni''hao','nh','你好',1000);",
                         nullptr, nullptr, nullptr) == SQLITE_OK,
            "populate fixture");
    sqlite3_close(db);
    SessionOptions options;
    options.paths = {directory, directory, directory, directory};
    options.learning = false;
    Session session(options);
    require(!session.character('6').handled, "ordinary pinyin swallowed digit");
    session.set_nine_key_enabled(true);
    require(!session.character('0').handled && !session.character('1').handled, "invalid digits accepted");
    const struct
    {
        const char *digits;
        const char *spelling;
        const char *word;
    } umlauts[] = {{"9826", "xuan", "选"}, {"986", "xun", "巡"}};
    for (const auto &test : umlauts)
    {
        type(session, test.digits);
        candidate(session, test.word);
        const auto view = session.snapshot();
        for (const auto &spelling : view.nine_key_spellings)
            require(spelling.size() < 2 || spelling[0] != 'x' || spelling[1] != 'v',
                    "nine-key spelling choices must not duplicate canonical u with v aliases");
        const auto found = std::find(view.nine_key_spellings.begin(), view.nine_key_spellings.end(), test.spelling);
        require(found != view.nine_key_spellings.end(), "missing canonical umlaut spelling");
        require(session.choose_nine_key_spelling(found - view.nine_key_spellings.begin()).handled,
                "choose canonical umlaut spelling");
        require(session.select(candidate(session, test.word)).commit == test.word && session.snapshot().preedit.empty(),
                "locking an umlaut spelling must preserve its candidates");
    }
    type(session, "982678269826");
    require(session.select(candidate(session, "无安全员")).commit == "无安全员",
            "duplicate umlaut aliases must not crowd out distinct nine-key paths");
    type(session, "64");
    candidate(session, "你");
    candidate(session, "米");
    const auto before = session.snapshot().preedit;
    require(!session.select(999).handled && !session.choose_nine_key_spelling(999).handled &&
                session.snapshot().preedit == before,
            "invalid index changed composition");
    auto view = session.snapshot();
    require(view.nine_key_spellings.front() == "ni", "preferred spelling was offscreen");
    const auto ni = std::find(view.nine_key_spellings.begin(), view.nine_key_spellings.end(), "ni");
    require(ni != view.nine_key_spellings.end(), "missing disambiguation");
    require(session.choose_nine_key_spelling(ni - view.nine_key_spellings.begin()).handled, "choose spelling");
    require(session.snapshot().preedit == "ni", "locked preedit");
    for (const auto &item : session.snapshot().candidates)
        require(item.word != "米", "lock failed");
    type(session, "426");
    require(session.snapshot().nine_key_spellings.front() == "hao", "locked suffix spelling order");
    require(session.select(candidate(session, "你好")).commit == "你好" && session.snapshot().preedit.empty(),
            "phrase selection");
    type(session, "64426");
    view = session.snapshot();
    auto spelling = std::find(view.nine_key_spellings.begin(), view.nine_key_spellings.end(), "ni");
    session.choose_nine_key_spelling(spelling - view.nine_key_spellings.begin());
    session.choose_nine_key_spelling(0); // hao, the preferred remaining syllable
    require(session.select(candidate(session, "你")).commit == "你" && session.snapshot().editing_text == "426",
            "partial selection");
    require(session.snapshot().preedit == "hao", "partial selection lost the locked suffix");
    require(session.finish().commit == "好" && session.snapshot().preedit.empty(), "finish residual");
    type(session, "64");
    session.command(Command::Backspace);
    require(session.snapshot().editing_text == "6", "backspace");
    session.command(Command::Backspace);
    require(!session.command(Command::Backspace).handled, "idle backspace");
    type(session, "64426");
    require(session.punctuation(',').commit == "你好，" && session.snapshot().preedit.empty(), "punctuation commit");
    type(session, "64");
    require(session.command(Command::CommitRaw).commit == "64", "raw commit");
    type(session, "64");
    session.command(Command::Cancel);
    require(session.snapshot().preedit.empty() && session.snapshot().nine_key_spellings.empty(), "cancel");
    type(session, "64");
    session.switch_scheme(SchemeType::Shuangpin);
    require(session.snapshot().preedit.empty() && !session.character('6').handled, "shuangpin isolation");
    session.switch_scheme(SchemeType::Quanpin);
    session.set_nine_key_enabled(false);
    require(session.character('n').handled && session.snapshot().editing_text == "n", "restore qwerty");
    session.command(Command::Cancel);
    session.set_nine_key_enabled(true);
    type(session, std::string(32, '7'));
    require(session.character('7').diagnostic.has_value() && session.snapshot().editing_text.size() == 32,
            "digit limit did not preserve composition");
    session.command(Command::Cancel);
    // Learning and explicit management use canonical dictionary keys, never digit strings.
    type(session, "64");
    require(session.select(candidate(session, "米")).commit == "米", "disabled learning selection");
    Session unchanged(options);
    unchanged.set_nine_key_enabled(true);
    type(unchanged, "64");
    require(unchanged.snapshot().candidates.front().word == "你", "disabled learning changed ranking");
    auto learning_options = options;
    learning_options.learning = true;
    learning_options.frequency.mode = FrequencyAdjustmentMode::Promote;
    Session learner(learning_options);
    learner.set_nine_key_enabled(true);
    type(learner, "64");
    auto learned = learner.select(candidate(learner, "米"));
    require(learned.commit == "米" && !learned.diagnostic, "learning failed");
    Session managed(options);
    managed.set_nine_key_enabled(true);
    type(managed, "64");
    require(managed.snapshot().candidates.front().word == "米", "learning did not persist");
    auto pinned = managed.pin(candidate(managed, "你"));
    require(pinned.handled && !pinned.commit && !pinned.diagnostic && managed.snapshot().editing_text == "64" &&
                managed.snapshot().candidates.front().word == "你",
            "manual pin failed or committed");
    require(!managed.remove(candidate(managed, "你")).handled, "single character was removed");
    require(!managed.pin(999).handled && !managed.remove(999).handled && !managed.fix_position(0, 6).handled &&
                !managed.clear_position(999).handled,
            "invalid management action handled");
    require(managed.fix_position(candidate(managed, "米"), 1).handled &&
                managed.snapshot().candidates.front().word == "米",
            "fixed position not applied");
    Session fixed(options);
    fixed.set_nine_key_enabled(true);
    type(fixed, "64");
    require(fixed.snapshot().candidates.front().word == "米", "fixed position not persisted");
    const auto fixed_view = fixed.snapshot();
    const auto lock = std::find(fixed_view.nine_key_spellings.begin(), fixed_view.nine_key_spellings.end(), "ni");
    fixed.choose_nine_key_spelling(lock - fixed_view.nine_key_spellings.begin());
    require(fixed.snapshot().candidates.front().word == "你", "fixed position escaped spelling constraint");
    require(managed.clear_position(candidate(managed, "米")).handled &&
                managed.snapshot().candidates.front().word == "你",
            "clear fixed position failed");
    managed.command(Command::Cancel);
    type(managed, "64426");
    auto removed = managed.remove(candidate(managed, "你好"));
    require(removed.handled && !removed.commit && !removed.diagnostic && managed.snapshot().editing_text == "64426",
            "phrase removal failed or changed composition");
    Session after_removal(options);
    after_removal.set_nine_key_enabled(true);
    type(after_removal, "64426");
    // Removal drops the dictionary row; the sentence composer may still build the same text out of
    // the surviving single-character entries, so only a database-backed 你好 counts as a leak.
    for (const auto &item : after_removal.snapshot().candidates)
        require(item.word != "你好" ||
                    (item.source != CandidateSource::Database && item.source != CandidateSource::UserDatabase),
                "phrase removal did not persist");

    // A persistence failure must preserve the user's commit and surface a diagnostic.
    auto failure_options = learning_options;
    failure_options.paths.user_data = directory / "blocked";
    std::filesystem::create_directories(failure_options.paths.user_data / assets::user_journal);
    Session failing(failure_options);
    failing.set_nine_key_enabled(true);
    type(failing, "64");
    auto failed_learning = failing.finish(candidate(failing, "米"));
    require(failed_learning.commit == "米" && failed_learning.diagnostic && failing.snapshot().preedit.empty(),
            "failed learning lost commit or diagnostic");
    std::cout << "Nine-key input contract passed\n";
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
