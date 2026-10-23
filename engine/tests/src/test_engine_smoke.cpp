#include "../../core/ime_session.h"
#include "../../core/data_path.h"
#include "../../english/english_dictionary.h"
#include "../../japanese/romaji_converter.h"
#include "../../neural/neural_decoder.h"
#include "../../quanpin/quanpin_query.h"
#include "../../quanpin/word_lattice.h"
#include "../../user_dictionary/user_dictionary_journal.h"
#include "test_directory_cleanup.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
class Database
{
  public:
    explicit Database(const std::filesystem::path &path)
    {
        if (sqlite3_open(metasequoia::path_to_utf8(path).c_str(), &database_) != SQLITE_OK)
        {
            throw std::runtime_error("Failed to create the test dictionary.");
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

    bool containsUserDictionaryOperation(const std::string &value)
    {
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(database_, "SELECT 1 FROM user_dictionary_operations WHERE value=?1 LIMIT 1", -1,
                               &statement, nullptr) != SQLITE_OK)
        {
            throw std::runtime_error("Failed to query the user dictionary journal.");
        }
        const bool bound = sqlite3_bind_text(statement, 1, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
        const bool found = bound && sqlite3_step(statement) == SQLITE_ROW;
        sqlite3_finalize(statement);
        return found;
    }

    std::int64_t queryInteger(const char *sql)
    {
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
            sqlite3_step(statement) != SQLITE_ROW)
        {
            sqlite3_finalize(statement);
            throw std::runtime_error("Failed to query the test dictionary.");
        }
        const std::int64_t value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return value;
    }

  private:
    sqlite3 *database_ = nullptr;
};
} // namespace

int run_test()
{
    if (japanese::HiraganaToKatakana("かな") != "カナ" || japanese::HiraganaToRomaji("カナ") != "kana")
    {
        throw std::runtime_error("Kana conversion changed during the platform port.");
    }

    {
        // 多音字的生僻读音：大 读 tai 只占 大 全部权重的 0.14%，单字行要在 penalty 上扣分，
        // 否则 tai'zhuan'qian'le 会被语言模型带成「大赚钱了」。太/台 只有这一个读音，不扣；
        // 长 读 chang 占四成，是常见读音，也不扣。权重取自出厂词库。
        sqlite3 *db = nullptr;
        if (sqlite3_open(":memory:", &db) != SQLITE_OK)
            throw std::runtime_error("Failed to open the in-memory dictionary.");
        const char *fixture = "CREATE TABLE tbl_1_d(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                              "CREATE TABLE tbl_1_t(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                              "CREATE TABLE tbl_1_c(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                              "CREATE TABLE tbl_1_z(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                              "INSERT INTO tbl_1_d VALUES('da','d','大',3273479),('dai','d','大',4993);"
                              "INSERT INTO tbl_1_t VALUES('tai','t','太',768757),('tai','t','台',51622),"
                              "('tai','t','大',4719);"
                              "INSERT INTO tbl_1_c VALUES('chang','c','长',340269);"
                              "INSERT INTO tbl_1_z VALUES('zhang','z','长',462258);";
        if (sqlite3_exec(db, fixture, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Failed to build the polyphone fixture.");

        const auto totals = quanpin::load_char_reading_totals(db);
        if (totals->size() != 2 || totals->count("大") != 1 || totals->count("长") != 1)
            throw std::runtime_error("Reading totals must keep exactly the polyphonic characters.");

        std::unordered_map<std::string, sqlite3_stmt *> statements;
        const auto penalty_of = [&](const std::string &syllable, const std::string &word) {
            const auto lookup = quanpin::make_lattice_db_lookup(db, statements, 32, {}, totals);
            for (const auto &row : lookup({syllable}))
            {
                if (row.value == word)
                    return row.penalty;
            }
            throw std::runtime_error("Polyphone fixture row missing: " + word);
        };
        const double rare = penalty_of("tai", "大");
        if (rare < 1.5 || penalty_of("tai", "太") != 0.0 || penalty_of("tai", "台") != 0.0 ||
            penalty_of("chang", "长") != 0.0 || penalty_of("da", "大") != 0.0)
            throw std::runtime_error("Rare polyphone readings must be penalized, common ones must not: " +
                                     std::to_string(rare));
        // 不给 totals 时保持旧行为，一分不扣。
        for (const auto &row : quanpin::make_lattice_db_lookup(db, statements, 32)({"tai"}))
        {
            if (row.penalty != 0.0)
                throw std::runtime_error("Lookup without reading totals must not penalize.");
        }
        for (auto &[sql, statement] : statements)
            sqlite3_finalize(statement);
        sqlite3_close(db);
    }

    {
        // Neural-only mode still asks the lattice for twelve internal alternatives, but it must not
        // expose the lattice's own pick while the asynchronous neural result is pending.
        std::unordered_map<std::string, std::vector<quanpin::LatticeLexeme>> table;
        table["ni"] = {{"ni", "你", 4000}, {"ni", "拟", 3000}, {"ni", "泥", 2000}, {"ni", "逆", 1000}};
        table["hao"] = {{"hao", "好", 4000}, {"hao", "号", 3000}, {"hao", "浩", 2000}, {"hao", "耗", 1000}};
        const auto lookup = [&](const quanpin::Segments &span) {
            std::string key;
            for (const std::string &syllable : span)
            {
                if (!key.empty())
                    key.push_back('\'');
                key += syllable;
            }
            const auto found = table.find(key);
            return found == table.end() ? std::vector<quanpin::LatticeLexeme>{} : found->second;
        };

        quanpin::WordLatticeOptions options;
        options.nbest = static_cast<int>(neural::RerankOptions{}.max_paths);
        options.include_lattice_best = false;
        options.show_next_on_duplicate = true;
        const std::vector<quanpin::LatticePath> neural_paths =
            quanpin::decode_word_lattice({"ni", "hao"}, lookup, options);
        std::size_t desktop_alternatives = 0;
        std::size_t keyboard_alternatives = 0;
        std::vector<quanpin::SourcedLatticeReranker> rerankers;
        rerankers.push_back({[&](std::vector<quanpin::LatticePath> &paths) {
                                 desktop_alternatives = paths.size();
                                 return true;
                             },
                             CandidateSource::NeuralDesktop});
        rerankers.push_back({[&](std::vector<quanpin::LatticePath> &paths) {
                                 keyboard_alternatives = paths.size();
                                 return true;
                             },
                             CandidateSource::NeuralKeyboard});
        std::vector<WordItem> selected;
        quanpin::merge_lattice_candidates(selected, {"ni", "hao"}, lookup, "ni'hao", options, rerankers);
        if (desktop_alternatives != 12 || keyboard_alternatives != 12 || selected.size() != 2 ||
            selected[0].source != CandidateSource::NeuralKeyboard || selected[0].word != neural_paths[0].sentence ||
            selected[1].source != CandidateSource::NeuralDesktop || selected[1].word != neural_paths[1].sentence)
        {
            throw std::runtime_error("Keyboard did not keep the shared neural best before desktop's next choice.");
        }

        std::vector<WordItem> different_neural_best;
        const std::vector<quanpin::SourcedLatticeReranker> different_best_rerankers = {
            {[](std::vector<quanpin::LatticePath> &paths) {
                 std::swap(paths[0], paths[1]);
                 return true;
             },
             CandidateSource::NeuralDesktop},
            {[](std::vector<quanpin::LatticePath> &) { return true; }, CandidateSource::NeuralKeyboard}};
        quanpin::merge_lattice_candidates(different_neural_best, {"ni", "hao"}, lookup, "ni'hao", options,
                                          different_best_rerankers);
        if (different_neural_best.size() != 2 || different_neural_best[0].source != CandidateSource::NeuralDesktop ||
            different_neural_best[0].word != neural_paths[1].sentence ||
            different_neural_best[1].source != CandidateSource::NeuralKeyboard ||
            different_neural_best[1].word != neural_paths[0].sentence)
        {
            throw std::runtime_error("Desktop did not precede keyboard when their neural best rows differed.");
        }

        std::vector<WordItem> pending;
        const std::vector<quanpin::SourcedLatticeReranker> pending_rerankers = {
            {[](std::vector<quanpin::LatticePath> &) { return false; }, CandidateSource::NeuralDesktop},
            {[](std::vector<quanpin::LatticePath> &) { return false; }, CandidateSource::NeuralKeyboard}};
        quanpin::merge_lattice_candidates(pending, {"ni", "hao"}, lookup, "ni'hao", options, pending_rerankers);
        if (!pending.empty())
        {
            throw std::runtime_error("Neural-only sentence selection exposed a lattice row before scoring finished.");
        }

        std::vector<WordItem> keyboard_ready;
        const std::vector<quanpin::SourcedLatticeReranker> keyboard_ready_rerankers = {
            {[](std::vector<quanpin::LatticePath> &) { return false; }, CandidateSource::NeuralDesktop},
            {[](std::vector<quanpin::LatticePath> &) { return true; }, CandidateSource::NeuralKeyboard}};
        quanpin::merge_lattice_candidates(keyboard_ready, {"ni", "hao"}, lookup, "ni'hao", options,
                                          keyboard_ready_rerankers);
        if (keyboard_ready.size() != 1 || keyboard_ready[0].source != CandidateSource::NeuralKeyboard ||
            keyboard_ready[0].word != neural_paths[0].sentence)
        {
            throw std::runtime_error("Keyboard neural output waited for the desktop model.");
        }

        quanpin::WordLatticeOptions trigram_consensus_options = options;
        trigram_consensus_options.include_lattice_best = true;
        std::vector<WordItem> trigram_consensus;
        const std::vector<quanpin::SourcedLatticeReranker> trigram_consensus_rerankers = {
            {[](std::vector<quanpin::LatticePath> &) { return true; }, CandidateSource::NeuralKeyboard}};
        quanpin::merge_lattice_candidates(trigram_consensus, {"ni", "hao"}, lookup, "ni'hao", trigram_consensus_options,
                                          trigram_consensus_rerankers);
        if (trigram_consensus.size() != 2 || trigram_consensus[0].source != CandidateSource::Generated ||
            trigram_consensus[0].word != neural_paths[0].sentence ||
            trigram_consensus[1].source != CandidateSource::NeuralKeyboard ||
            trigram_consensus[1].word != neural_paths[1].sentence)
        {
            throw std::runtime_error("Trigram consensus was not promoted ahead of the neural rows.");
        }

        // Simulate an earlier Unigram row equal to every later source's first choice. Ownership must
        // remain Unigram, Trigram, keyboard, desktop regardless of reranker input or completion order.
        const std::vector<quanpin::LatticePath> static_paths =
            quanpin::decode_word_lattice({"ni", "hao"}, lookup, options);
        std::vector<WordItem> deduplicated;
        deduplicated.emplace_back("ni'hao", static_paths.front().sentence, 1, CandidateSource::Fallback, "ni'hao");
        options.include_lattice_best = true;
        options.show_next_on_duplicate = true;
        const std::vector<quanpin::SourcedLatticeReranker> duplicate_rerankers = {
            {[](std::vector<quanpin::LatticePath> &) { return true; }, CandidateSource::NeuralDesktop},
            {[](std::vector<quanpin::LatticePath> &paths) {
                 std::swap(paths[0], paths[1]);
                 return true;
             },
             CandidateSource::NeuralKeyboard}};
        quanpin::merge_lattice_candidates(deduplicated, {"ni", "hao"}, lookup, "ni'hao", options, duplicate_rerankers);
        if (deduplicated.size() != 4 || deduplicated[0].source != CandidateSource::Fallback ||
            deduplicated[0].word != static_paths[0].sentence ||
            deduplicated[1].source != CandidateSource::NeuralKeyboard ||
            deduplicated[1].word != static_paths[2].sentence ||
            deduplicated[2].source != CandidateSource::NeuralDesktop ||
            deduplicated[2].word != static_paths[3].sentence || deduplicated[3].source != CandidateSource::Generated ||
            deduplicated[3].word != static_paths[1].sentence)
        {
            throw std::runtime_error("Unigram consensus was not promoted before distinct fallback rows.");
        }

        // Desktop must wait while an enabled keyboard model is pending. Trigram can already appear
        // because its ownership was fixed before either neural model.
        std::vector<WordItem> partially_ready;
        partially_ready.emplace_back("ni'hao", static_paths.front().sentence, 1, CandidateSource::Fallback, "ni'hao");
        const std::vector<quanpin::SourcedLatticeReranker> partially_ready_rerankers = {
            {[](std::vector<quanpin::LatticePath> &) { return true; }, CandidateSource::NeuralDesktop},
            {[](std::vector<quanpin::LatticePath> &) { return false; }, CandidateSource::NeuralKeyboard}};
        quanpin::merge_lattice_candidates(partially_ready, {"ni", "hao"}, lookup, "ni'hao", options,
                                          partially_ready_rerankers);
        if (partially_ready.size() != 2 || partially_ready[0].source != CandidateSource::Fallback ||
            partially_ready[0].word != static_paths[0].sentence ||
            partially_ready[1].source != CandidateSource::Generated ||
            partially_ready[1].word != static_paths[1].sentence)
        {
            throw std::runtime_error("Desktop neural output appeared before keyboard ownership was fixed.");
        }
    }

    {
        // 句中辅助码：约束在建图时筛边。整词「你好」覆盖受约束的音节，同样要过约束；受约束的
        // 跨度改用不截断的查询，普通查询里没有的生僻字也能被锁上。
        std::unordered_map<std::string, std::vector<quanpin::LatticeLexeme>> table;
        table["ni"] = {{"ni", "你", 4000}, {"ni", "拟", 3000}, {"ni", "泥", 2000}};
        table["hao"] = {{"hao", "好", 4000}, {"hao", "号", 3000}};
        table["ni'hao"] = {{"ni'hao", "你好", 9000}};
        const auto lookup_from = [](const std::unordered_map<std::string, std::vector<quanpin::LatticeLexeme>> &rows) {
            return [&rows](const quanpin::Segments &span) {
                std::string key;
                for (const std::string &syllable : span)
                {
                    if (!key.empty())
                        key.push_back('\'');
                    key += syllable;
                }
                const auto found = rows.find(key);
                return found == rows.end() ? std::vector<quanpin::LatticeLexeme>{} : found->second;
            };
        };
        auto unbounded = table;
        unbounded["ni"].push_back({"ni", "尼", 10});

        quanpin::WordLatticeOptions options;
        options.nbest = 5;
        options.char_constraints.push_back({0, [](const std::string &hanzi) { return hanzi == "泥"; }});
        auto paths = quanpin::decode_word_lattice({"ni", "hao"}, lookup_from(table), options);
        if (paths.empty() || std::any_of(paths.begin(), paths.end(), [](const quanpin::LatticePath &path) {
                return path.sentence.rfind("泥", 0) != 0;
            }))
        {
            throw std::runtime_error("A lattice path ignored the constrained syllable.");
        }

        options.char_constraints = {{0, [](const std::string &hanzi) { return hanzi == "尼"; }}};
        if (!quanpin::decode_word_lattice({"ni", "hao"}, lookup_from(table), options).empty())
        {
            throw std::runtime_error("A constraint nothing satisfies still produced a sentence.");
        }
        options.constrained_lookup = lookup_from(unbounded);
        paths = quanpin::decode_word_lattice({"ni", "hao"}, lookup_from(table), options);
        if (paths.empty() || paths.front().sentence != "尼好")
        {
            throw std::runtime_error("The constrained span did not use the unbounded lookup.");
        }
    }

    const auto unique_suffix = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const std::filesystem::path data_directory =
        std::filesystem::temp_directory_path() / std::filesystem::u8path("metasequoia-engine-词库-" + unique_suffix);
    metasequoia::test::ScopedDataDirectoryCleanup cleanup(data_directory);
    std::filesystem::create_directories(data_directory);
#ifdef _WIN32
    if (_wputenv_s(L"METASEQUOIA_IME_DATA_DIR", data_directory.c_str()) != 0)
#else
    if (setenv("METASEQUOIA_IME_DATA_DIR", metasequoia::path_to_utf8(data_directory).c_str(), 1) != 0)
#endif
    {
        throw std::runtime_error("Failed to set the data directory override.");
    }

    {
        Database database(data_directory / "msime.db");
        database.execute("CREATE TABLE tbl_2_n(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', '你好', 100)");

        ImeSession session(SchemeType::Quanpin);
        for (const char character : std::string("nihao"))
        {
            const ImeKeyCode key_code = static_cast<ImeKeyCode>(character - ('a' - 'A'));
            session.handle_key(key_code, 0, static_cast<ImeCharacter>(character));
        }

        if (session.get_preedit() != "nihao")
        {
            throw std::runtime_error("Quanpin preedit does not contain the typed input.");
        }
        const auto &candidates = session.get_candidates();
        if (std::none_of(candidates.begin(), candidates.end(),
                         [](const WordItem &item) { return item.word == "你好"; }))
        {
            throw std::runtime_error("Quanpin did not return the candidate stored in the dictionary.");
        }

        session.handle_key(ImeKey::Backspace);
        if (session.get_preedit() != "niha")
        {
            throw std::runtime_error("Backspace did not update the quanpin preedit.");
        }
    }

    const std::filesystem::path user_database = data_directory / "msime_user.db";
    const std::filesystem::path english_database = data_directory / "english.db";
    if (!EnglishDictionary::ensure_schema(metasequoia::path_to_utf8(english_database)) ||
        !std::filesystem::exists(english_database))
    {
        throw std::runtime_error("English schema initialization did not create a missing database.");
    }
    if (!user_dictionary::record_upsert(metasequoia::path_to_utf8(user_database),
                                        user_dictionary::DictionaryKind::Pinyin, "ni'hao", "首次", 100))
    {
        throw std::runtime_error("Failed to open the persistent default user dictionary.");
    }
    user_dictionary::close_default_user_database();
    const std::filesystem::path previous_user_database = data_directory / "msime_user.previous.db";
    std::filesystem::rename(user_database, previous_user_database);
    if (!user_dictionary::record_upsert(metasequoia::path_to_utf8(user_database),
                                        user_dictionary::DictionaryKind::Pinyin, "ni'hao", "重开", 200))
    {
        throw std::runtime_error("The persistent default user dictionary did not reopen after close.");
    }
    user_dictionary::close_default_user_database();
    {
        Database previous(previous_user_database);
        Database current(user_database);
        if (!previous.containsUserDictionaryOperation("首次") || previous.containsUserDictionaryOperation("重开") ||
            !current.containsUserDictionaryOperation("重开") || current.containsUserDictionaryOperation("首次"))
        {
            throw std::runtime_error("The reopened default user dictionary wrote through the stale handle.");
        }
    }

    if (!user_dictionary::record_upsert(metasequoia::path_to_utf8(user_database),
                                        user_dictionary::DictionaryKind::English, "hello", "Hello", 300, "Hello"))
    {
        throw std::runtime_error("Failed to record the English replay fixture.");
    }
    user_dictionary::close_default_user_database();
    const auto successful_replay = user_dictionary::replay(metasequoia::path_to_utf8(user_database),
                                                           metasequoia::path_to_utf8(data_directory / "msime.db"),
                                                           metasequoia::path_to_utf8(english_database));
    if (!successful_replay.error.empty() || successful_replay.applied != 2)
    {
        throw std::runtime_error("Replay did not apply the Pinyin and attached English operations: error=" +
                                 successful_replay.error + ", applied=" + std::to_string(successful_replay.applied) +
                                 ", failed=" + std::to_string(successful_replay.failed) +
                                 ", skipped=" + std::to_string(successful_replay.skipped));
    }
    {
        Database english(english_database);
        if (english.queryInteger("SELECT weight FROM english_words WHERE word='hello' AND display='Hello'") != 300)
        {
            throw std::runtime_error("Replay did not persist the attached English operation.");
        }
    }

    const std::filesystem::path empty_user_database = data_directory / "msime_user.empty.db";
    if (!user_dictionary::ensure_user_database(metasequoia::path_to_utf8(empty_user_database)))
    {
        throw std::runtime_error("Failed to create the empty replay journal.");
    }
    {
        Database locked_main(data_directory / "msime.db");
        locked_main.execute("BEGIN EXCLUSIVE");
        const auto locked_replay = user_dictionary::replay(metasequoia::path_to_utf8(empty_user_database),
                                                           metasequoia::path_to_utf8(data_directory / "msime.db"),
                                                           metasequoia::path_to_utf8(english_database));
        locked_main.execute("ROLLBACK");
        if (locked_replay.error.empty())
        {
            throw std::runtime_error("Replay reported success after its main transaction failed to start.");
        }
    }

    const std::filesystem::path unreadable_user_database = data_directory / "msime_user.unreadable.db";
    {
        Database unreadable_journal(unreadable_user_database);
        unreadable_journal.execute("CREATE VIEW user_dictionary_operations AS "
                                   "SELECT 'pinyin' AS dictionary,'ni' AS key,'你' AS value,'upsert' AS operation,"
                                   "100 AS weight,'' AS display,1 AS updated_at "
                                   "UNION ALL "
                                   "SELECT 'pinyin','bad',value,'upsert',1,'',2 FROM json_each('not-json')");
    }
    const auto unreadable_replay = user_dictionary::replay(metasequoia::path_to_utf8(unreadable_user_database),
                                                           metasequoia::path_to_utf8(data_directory / "msime.db"),
                                                           metasequoia::path_to_utf8(english_database));
    if (unreadable_replay.error.empty())
    {
        throw std::runtime_error("Replay reported success after the journal cursor failed.");
    }

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
