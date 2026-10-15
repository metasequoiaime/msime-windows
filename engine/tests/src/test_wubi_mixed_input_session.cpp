#include <metasequoia/session.h>

#include "../../contracts/assets/assets.h"
#include "../../core/data_path.h"
#include "../../english/english_dictionary.h"
#include "../../core/input_session.h"
#include "../../core/runtime_paths.h"
#include "../../schemes/wubi_scheme.h"
#include "../../user_dictionary/user_dictionary_journal.h"
#include "test_directory_cleanup.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace metasequoia;

void require(bool value, const char *message)
{
    if (!value)
    {
        throw std::runtime_error(message);
    }
}

void execute(const std::filesystem::path &path, const std::string &sql)
{
    sqlite3 *database = nullptr;
    require(sqlite3_open(path_to_utf8(path).c_str(), &database) == SQLITE_OK, "Fixture open failed");
    const int result = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(database);
    require(result == SQLITE_OK, "Fixture SQL failed");
}

std::vector<std::string> words(const InputSession &session)
{
    std::vector<std::string> result;
    result.reserve(session.candidates().size());
    for (const auto &candidate : session.candidates())
    {
        result.push_back(candidate.word);
    }
    return result;
}

// The scheme that produced the candidate at `index`: the merge puts wubi rows first and appends
// the pinyin ones, so callers assert both the order and the owner.
SchemeType scheme_of(const InputSession &session, std::size_t index)
{
    return session.candidates()[index].scheme;
}

std::size_t wubi_count(const InputSession &session)
{
    return static_cast<std::size_t>(
        std::count_if(session.candidates().begin(), session.candidates().end(),
                      [](const WordItem &item) { return item.scheme == SchemeType::Wubi; }));
}

std::vector<std::string> type(InputSession &session, const std::string &code)
{
    for (const char letter : code)
    {
        session.handle_character(letter);
    }
    return words(session);
}

// The fixture answers ni'hao and zi in quanpin, ta and hao in both dictionaries, and leaves nihao
// unanswered by the wubi table. Hardcoding codes against the shipped dictionary would tie these
// assertions to what a checkout happens to have built.
std::filesystem::path prepare_resources(const std::filesystem::path &root)
{
    const auto resources = root / "resources";
    std::filesystem::create_directories(resources);
    execute(resources / assets::main_dictionary,
            "CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
            "INSERT INTO tbl_1_n VALUES('ni','n','你',10000);"
            "CREATE TABLE tbl_1_z(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
            "INSERT INTO tbl_1_z VALUES('zi','z','子',10000),('zu','z','组',10000);"
            "CREATE TABLE tbl_2_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
            "INSERT INTO tbl_2_n VALUES('ni''hao','nh','你好',10000),('ni''hao','nh','拟好',9000);"
            "CREATE TABLE tbl_1_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
            "INSERT INTO tbl_1_x VALUES('xiao','x','笑',10000);"
            "CREATE TABLE tbl_1_t(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
            "INSERT INTO tbl_1_t VALUES('ta','t','他',10000);"
            "CREATE TABLE tbl_1_h(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
            "INSERT INTO tbl_1_h VALUES('hao','h','好',9500),('hao','h','号',9000);"
            "CREATE TABLE tbl_1_a(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
            "INSERT INTO tbl_1_a VALUES('a','a','啊',9000),('a','a','工',100);"
            "CREATE TABLE wubi86(key TEXT,value TEXT,weight INTEGER);"
            "INSERT INTO wubi86 VALUES('wq','你好',10000),('wqaa','众人',9000),"
            "('wqab','甲',8000),('wqab','乙',7000),('taaa','笔',6000),"
            "('a','工',10000),('aaaa','工',5000),('aaab','苛',4000),"
            "('hao','号',9000),('au','乐',20000);");
    require(EnglishDictionary::ensure_schema(path_to_utf8(resources / assets::english_dictionary)),
            "English schema failed");
    return resources;
}

// InputSession is neither copyable nor movable, so a session is built where it is used.
RuntimePaths paths_for(const std::filesystem::path &resources, const std::filesystem::path &root,
                       const std::string &tag)
{
    return prepare_runtime_paths(resources, root / ("user-" + tag), root / ("cache-" + tag), "v1");
}
} // namespace

int main()
{
    try
    {
        const auto root =
            std::filesystem::temp_directory_path() /
            ("msime-wubi-mixed-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        test::ScopedDataDirectoryCleanup cleanup(root);
        const auto resources = prepare_resources(root);
        int counter = 0;
        const auto next = [&counter] { return std::to_string(counter++); };

        // Off is pure wubi: an unmatched code stays empty, a prefix hint is all it shows.
        {
            InputSession plain(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            plain.set_wubi_input_options(WubiInputOptions{false});
            require(type(plain, "nihao").empty(), "The fixture answered nihao in plain wubi.");

            InputSession hints(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            hints.set_wubi_input_options(WubiInputOptions{false});
            const auto ta = type(hints, "ta");
            require(std::find(ta.begin(), ta.end(), "他") == ta.end(),
                    "Plain wubi offered pinyin for a code the table did not answer.");
            require(std::find(ta.begin(), ta.end(), "笔") != ta.end(), "Plain wubi dropped the prefix hint.");
        }

        // Simultaneous mixed input: a code the wubi table knows keeps its rows first and the same
        // letters are also offered to quanpin. A word both dictionaries answer is listed once.
        {
            InputSession mixed(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            mixed.set_wubi_input_options(WubiInputOptions{true});
            const auto candidates = type(mixed, "hao");
            require(candidates.size() == 2, "The fixture did not answer hao with both dictionaries.");
            require(candidates[0] == "号" && candidates[1] == "好",
                    "Mixed input did not put the wubi candidate first.");
            require(scheme_of(mixed, 0) == SchemeType::Wubi, "The exact code row was not tagged as wubi.");
            require(scheme_of(mixed, 1) == SchemeType::Quanpin, "The appended candidate was not tagged as pinyin.");
            require(std::count(candidates.begin(), candidates.end(), "号") == 1,
                    "A word answered by both dictionaries was listed twice.");
            require(wubi_count(mixed) == 1, "The wubi candidate count was wrong.");
        }

        // The table failing the code does not drop its prefix hints: they stay in front of the
        // pinyin candidates, which is what "candidates prefer wubi" means for a partial code.
        {
            InputSession mixed(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            mixed.set_wubi_input_options(WubiInputOptions{true});
            const auto candidates = type(mixed, "ta");
            const auto hint = std::find(candidates.begin(), candidates.end(), "笔");
            const auto pinyin = std::find(candidates.begin(), candidates.end(), "他");
            require(hint != candidates.end(), "Mixed input dropped the wubi prefix hint.");
            require(pinyin != candidates.end(), "Mixed input did not offer pinyin for ta.");
            require(hint < pinyin, "Mixed input put the pinyin candidate before the wubi hint.");
            require(wubi_count(mixed) == 1, "A prefix hint alone was counted as the code being answered.");
        }

        // A code only quanpin answers still reaches the user through the usual pinyin path.
        {
            InputSession mixed(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            mixed.set_wubi_input_options(WubiInputOptions{true});
            InputSession reference(SchemeType::Quanpin, GetXiaoheShuangpinProfile(),
                                   paths_for(resources, root, next()));
            reference.set_wubi_input_options(WubiInputOptions{false});
            require(type(mixed, "nihao") == type(reference, "nihao"),
                    "Mixed input answered an unmatched code with something other than quanpin.");
        }

        // A word with both a short and a full code appears once, under the code actually typed.
        {
            InputSession session(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            session.set_wubi_input_options(WubiInputOptions{false});
            const auto candidates = type(session, "a");
            require(std::count(candidates.begin(), candidates.end(), "工") == 1,
                    "A word with a short and a full code was listed twice.");
            require(session.candidates().front().word == "工" && session.candidates().front().pinyin == "a",
                    "The deduplicated word did not keep the exact-code row.");
        }

        // Auto-commit facts count only the code's own rows: a pinyin spelling mixed input appends
        // is not a wubi candidate, and a recoded four-letter code is complete but not unique.
        {
            InputSession unique(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            unique.set_wubi_input_options(WubiInputOptions{false});
            const auto unique_candidates = type(unique, "wqaa");
            require(unique_candidates.size() == 1, "The fixture did not answer wqaa with one candidate.");
            require(unique.wubi_unique_four_code(), "A unique four-letter wubi code was not reported as one.");
            require(unique.wubi_four_code_is_complete(), "A complete wubi code was not reported as complete.");

            InputSession recoded(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            recoded.set_wubi_input_options(WubiInputOptions{false});
            require(type(recoded, "wqab").size() == 2, "The fixture did not recode wqab.");
            require(!recoded.wubi_unique_four_code(), "A recoded four-letter wubi code was called unique.");
            // Completeness is deliberately not uniqueness: the recoded code still commits its first
            // candidate when the user types past the fourth letter instead of losing that letter.
            require(recoded.wubi_four_code_is_complete(),
                    "A recoded four-letter wubi code was not reported as complete.");

            InputSession short_code(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            short_code.set_wubi_input_options(WubiInputOptions{false});
            type(short_code, "wq");
            require(!short_code.wubi_unique_four_code(), "A two-letter code was called a complete wubi code.");
            require(!short_code.wubi_four_code_is_complete(),
                    "A two-letter code was reported as a complete wubi code.");

            // The fixture gives xiao exactly one quanpin candidate, so candidate count alone would
            // call it unique: the wubi-native guard has to reject the appended pinyin row on its own.
            InputSession pinyin_only(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            pinyin_only.set_wubi_input_options(WubiInputOptions{true});
            const auto pinyin_candidates = type(pinyin_only, "xiao");
            require(pinyin_candidates.size() == 1, "The fixture did not answer xiao with one candidate.");
            require(pinyin_only.candidates().front().scheme == SchemeType::Quanpin,
                    "A pinyin-only answer was tagged as wubi.");
            require(!pinyin_only.wubi_unique_four_code(), "A pinyin-only answer was called a unique wubi code.");
            require(!pinyin_only.wubi_four_code_is_complete(),
                    "A pinyin-only answer was reported as a complete wubi code.");

            InputSession mixed_unique(SchemeType::Wubi, GetXiaoheShuangpinProfile(),
                                      paths_for(resources, root, next()));
            mixed_unique.set_wubi_input_options(WubiInputOptions{true});
            require(type(mixed_unique, "wqaa").size() == 1,
                    "The fixture answered wqaa with more than the one wubi row.");
            require(mixed_unique.wubi_unique_four_code(), "A unique wubi code was not reported unique in mixed input.");
        }

        // A host that drives the session by raw string (the Server) counts the cased raw it reads
        // back to find the fourth letter, and reads its shape to tell T and V mode from a code. For a
        // code that raw is the cased code itself; a special mode's literal input is kept as typed and
        // is never a code, even when its letters spell one (Taaab -> taaa, a unique code here).
        {
            const auto set_raw = [](InputSession &session, const std::string &raw, bool literal) {
                session.set_pinyin_sequence(raw);
                session.set_pinyin_sequence_with_cases(raw);
                session.set_wubi_literal_input(literal);
                session.recompute_candidates();
            };

            InputSession clipped(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            clipped.set_wubi_input_options(WubiInputOptions{false});
            set_raw(clipped, "Wqaab", false);
            require(clipped.get_pinyin_sequence_with_cases() == "Wqaa" && clipped.get_pinyin_sequence() == "wqaa",
                    "The raw-case string of a wubi code was not clipped with the code.");
            require(clipped.wubi_unique_four_code(), "An uppercase letter stopped a wubi code from being a code.");

            InputSession wildcard(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            wildcard.set_wubi_input_options(WubiInputOptions{false, true});
            set_raw(wildcard, "wqazb", false);
            require(wildcard.get_pinyin_sequence_with_cases() == "wqaz" && wildcard.get_pinyin_sequence() == "wqaz",
                    "A wildcard code kept a letter past the fourth in its raw-case string.");

            InputSession code(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            code.set_wubi_input_options(WubiInputOptions{false});
            set_raw(code, "Taaa", false);
            require(code.wubi_unique_four_code(), "The fixture did not answer taaa with one candidate.");

            for (const bool mixed : {false, true})
            {
                InputSession literal(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
                literal.set_wubi_input_options(WubiInputOptions{mixed});
                set_raw(literal, "Taaab", true);
                require(literal.get_pinyin_sequence_with_cases() == "Taaab",
                        "Literal input was not kept as typed in the raw-case string.");
                require(!literal.wubi_unique_four_code() && !literal.wubi_four_code_is_complete(),
                        "Literal input whose letters spell a code was reported as a complete code.");
                require(literal.candidates().empty(), "Literal input was queried as a code.");

                // The flag belongs to the sequence it was set with: the next one is a code again.
                literal.set_pinyin_sequence("taaa");
                literal.set_pinyin_sequence_with_cases("taaa");
                literal.recompute_candidates();
                require(literal.wubi_unique_four_code(), "The literal flag outlived the sequence it was set with.");
            }
        }

        // The scheme's two views stay in step under key-driven editing too: Backspace after literal
        // input drops one typed character and rederives the code, and a letter key takes the letter
        // the key produced for both views.
        {
            WubiScheme scheme;
            scheme.set_literal_input(true);
            scheme.set_raw_input("V1+2", "V1+2");
            scheme.handle_key(ImeKey::Backspace, 0, 0);
            require(scheme.build_request().raw_input_with_cases == "V1+" && scheme.build_request().raw_input == "v",
                    "Backspace after literal input drifted the raw-case string from the code.");
            scheme.handle_key(ImeKey::Backspace, 0, 0);
            scheme.handle_key(ImeKey::Backspace, 0, 0);
            scheme.handle_key(ImeKey::Backspace, 0, 0);
            require(scheme.build_request().raw_input_with_cases.empty() && scheme.build_request().raw_input.empty(),
                    "Backspace left part of the literal input behind.");

            WubiScheme layout;
            layout.handle_key('A', 0, u'q');
            layout.handle_key('B', 0, 0);
            require(layout.build_request().raw_input_with_cases == "qb" && layout.build_request().raw_input == "qb",
                    "A letter key put different letters in the raw-case string and the code.");
        }

        // z is a pinyin letter in mixed input and a wildcard only in wildcard mode -- two
        // independent settings, never a single tri-state.
        {
            InputSession plain(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            plain.set_wubi_input_options(WubiInputOptions{false});
            type(plain, "zi");
            require(plain.preedit() == "i", "Plain wubi stopped dropping z.");

            InputSession mixed(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            mixed.set_wubi_input_options(WubiInputOptions{true, false});
            const auto candidates = type(mixed, "zi");
            require(mixed.preedit() == "zi", "Mixed input dropped the z from a spelling.");
            require(std::find(candidates.begin(), candidates.end(), "子") != candidates.end(),
                    "Mixed input did not reach the spelling that needed z.");

            // Mixed input reads z as an ordinary pinyin letter, not as a wildcard.
            InputSession mixed_z(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            mixed_z.set_wubi_input_options(WubiInputOptions{true, false});
            require(type(mixed_z, "wz").empty(), "Mixed input read z as a wildcard.");

            InputSession wildcard(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            wildcard.set_wubi_input_options(WubiInputOptions{false, true});
            require(type(wildcard, "zi").empty(), "Wildcard mode answered zi without a z in the fixture code.");
            require(wildcard.preedit() == "zi", "Wildcard mode dropped the z instead of keeping it.");
            // wz matches wq (and longer codes starting with it) by standing for q.
            InputSession match(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            match.set_wubi_input_options(WubiInputOptions{false, true});
            const auto matched = type(match, "wz");
            require(std::find(matched.begin(), matched.end(), "你好") != matched.end(),
                    "The wildcard code wz did not match the code wq.");
            // Wildcard keeps the four-letter limit: mixed input is what lifts it, and this mode
            // does not have it.
            InputSession limit(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            limit.set_wubi_input_options(WubiInputOptions{false, true});
            type(limit, "wqaa");
            limit.handle_character('a');
            require(limit.preedit() == "wqaa", "Wildcard mode accepted a fifth letter.");

            // With both on, a z code is also a pinyin spelling: the wildcard rows (zu matches au) are
            // guesses and must not push the spelling's answer off the first slot.
            InputSession both(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            both.set_wubi_input_options(WubiInputOptions{true, true});
            const auto both_candidates = type(both, "zu");
            require(!both_candidates.empty() && both_candidates.front() == "组",
                    "Wildcard rows were ranked ahead of the pinyin spelling in mixed input.");
            require(std::find(both_candidates.begin(), both_candidates.end(), "乐") != both_candidates.end(),
                    "Mixed input dropped the wildcard rows instead of ranking them after pinyin.");
        }

        // A pinyin word pinned under the pinyin context is pulled back in by include_missing; when the
        // wubi half already lists that word it must still appear only once.
        {
            const auto paths = paths_for(resources, root, next());
            InputSession session(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths);
            session.set_wubi_input_options(WubiInputOptions{true});
            require(
                user_dictionary::set_fixed_position(path_to_utf8(paths.user(assets::user_journal)), "a", "a", "工", 1),
                "Pinning the fixture word failed.");
            session.enable_fixed_positions();
            const auto candidates = type(session, "a");
            require(std::count(candidates.begin(), candidates.end(), "工") == 1,
                    "A pinned pinyin word duplicated the same word from the wubi table.");
        }

        // Pinyin candidates that bypass query() still carry their scheme, so a shuangpin session's
        // learning and removal reach the shuangpin engine instead of the default quanpin one.
        {
            InputSession shuangpin(SchemeType::Shuangpin, GetXiaoheShuangpinProfile(),
                                   paths_for(resources, root, next()));
            const auto found = shuangpin.find_candidate("ni", "你");
            require(found.has_value(), "The fixture did not find ni in shuangpin.");
            require(found->scheme == SchemeType::Shuangpin, "A looked-up shuangpin candidate was tagged as quanpin.");

            type(shuangpin, "n");
            if (shuangpin.expand_initial_candidates())
            {
                require(std::all_of(shuangpin.candidates().begin(), shuangpin.candidates().end(),
                                    [](const WordItem &item) { return item.scheme == SchemeType::Shuangpin; }),
                        "An expanded shuangpin candidate was tagged as quanpin.");
            }
        }

        // The four-letter limit holds until the table has failed the code in hand.
        {
            InputSession matched(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            matched.set_wubi_input_options(WubiInputOptions{true});
            const auto four = type(matched, "wqaa");
            require(!four.empty(), "The fixture did not answer the four-letter code wqaa.");
            matched.handle_character('a');
            require(matched.preedit() == "wqaa", "A matched four-letter code accepted a fifth letter.");
            require(words(matched) == four, "A refused fifth letter still changed the candidates.");

            InputSession unmatched(SchemeType::Wubi, GetXiaoheShuangpinProfile(), paths_for(resources, root, next()));
            unmatched.set_wubi_input_options(WubiInputOptions{true});
            type(unmatched, "nihao");
            require(unmatched.preedit() == "nihao", "Mixed input could not reach a spelling past four letters.");
            unmatched.handle_command(Command::Backspace);
            require(unmatched.preedit() == "niha", "Backspace did not shorten an extended composition.");
        }

        // The public session carries the setting from its options and at runtime, and tags the
        // mixed candidates so a host can tell which dictionary answered.
        {
            SessionOptions options;
            options.paths = prepare_runtime_paths(resources, root / "user-public", root / "cache-public", "v1");
            options.scheme = SchemeType::Wubi;
            options.wubi.mixed_pinyin = true;
            Session session(options);
            for (const char letter : std::string("nihao"))
            {
                session.character(letter);
            }
            require(!session.snapshot().candidates.empty(), "SessionOptions did not carry the mixed setting.");
            require(session.snapshot().candidates.front().scheme == SchemeType::Quanpin,
                    "A pinyin-only answer was not tagged as pinyin.");

            SessionOptions plain;
            plain.paths = prepare_runtime_paths(resources, root / "user-toggle", root / "cache-toggle", "v1");
            plain.scheme = SchemeType::Wubi;
            Session toggled(plain);
            for (const char letter : std::string("nihao"))
            {
                toggled.character(letter);
            }
            require(toggled.snapshot().candidates.empty(), "Mixed input was on without being asked for.");
            toggled.set_wubi_mixed_pinyin(true);
            require(!toggled.snapshot().candidates.empty(), "set_wubi_mixed_pinyin did not reach the composition.");

            SessionOptions native;
            native.paths = prepare_runtime_paths(resources, root / "user-native", root / "cache-native", "v1");
            native.scheme = SchemeType::Wubi;
            native.wubi.mixed_pinyin = true;
            Session matched(native);
            for (const char letter : std::string("wqaa"))
            {
                matched.character(letter);
            }
            require(!matched.snapshot().candidates.empty(), "The four-letter code answered with nothing.");
            require(matched.snapshot().candidates.front().scheme == SchemeType::Wubi,
                    "A code the wubi table answered was not tagged as wubi.");

            // set_wubi_mixed_pinyin must leave the sibling z-wildcard setting alone: a host that
            // enabled wildcard at construction and toggles mixed at runtime keeps wildcard working.
            SessionOptions wildcard;
            wildcard.paths = prepare_runtime_paths(resources, root / "user-z", root / "cache-z", "v1");
            wildcard.scheme = SchemeType::Wubi;
            wildcard.wubi.z_wildcard = true;
            Session z_session(wildcard);
            z_session.set_wubi_mixed_pinyin(true);
            for (const char letter : std::string("wz"))
            {
                z_session.character(letter);
            }
            const auto &z_candidates = z_session.snapshot().candidates;
            require(std::any_of(z_candidates.begin(), z_candidates.end(),
                                [](const WordItem &item) { return item.word == "你好"; }),
                    "Toggling mixed pinyin reset the z wildcard setting.");
        }
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
    return 0;
}
