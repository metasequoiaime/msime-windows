#include <metasequoia/personal_dictionary.h>
#include <metasequoia/session.h>
#include "../../core/data_path.h"
#include "../../contracts/assets/assets.h"
#include "../../english/english_dictionary.h"
#include "test_directory_cleanup.h"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace
{
using namespace metasequoia;
void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
void execute(const std::filesystem::path &path, const std::string &sql)
{
    sqlite3 *db = nullptr;
    require(sqlite3_open(path_to_utf8(path).c_str(), &db) == SQLITE_OK, "Cannot open fixture");
    int result = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
    require(result == SQLITE_OK, "Cannot execute fixture SQL");
}
bool has(const RuntimePaths &paths, const std::string &key, const std::string &word,
         SchemeType scheme = SchemeType::Quanpin, bool english = false)
{
    SessionOptions options;
    options.paths = paths;
    options.scheme = scheme;
    options.learning = false;
    Session session(options);
    session.set_dedicated_english(english);
    for (char ch : key)
        session.character(ch);
    const auto snapshot = session.snapshot();
    return std::any_of(snapshot.candidates.begin(), snapshot.candidates.end(),
                       [&](const auto &candidate) { return candidate.word == word; });
}
void run()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("personal-dictionary-" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    test::ScopedDataDirectoryCleanup cleanup(root);
    const auto resources = root / "resources";
    std::filesystem::create_directories(resources);
    execute(resources / assets::main_dictionary,
            "CREATE TABLE tbl_2_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
            "CREATE TABLE tbl_7_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
            "CREATE TABLE tbl_others_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
            "CREATE TABLE wubi86(key TEXT,value TEXT,weight INTEGER);"
            "CREATE TABLE quick_parases(key TEXT,value TEXT,weight INTEGER);");
    require(EnglishDictionary::ensure_schema(path_to_utf8(resources / assets::english_dictionary)),
            "English fixture failed");
    auto paths = prepare_runtime_paths(resources, root / "user", root / "cache", "first");
    require(personal_dictionary_entries(paths).entries.empty(), "New dictionary is not empty");
    PersonalDictionaryEntry pinyin{PersonalDictionaryKind::Pinyin, "NI HAO", "拟好", 12345};
    auto normalized = validate_personal_dictionary_entry(pinyin);
    require(normalized.entry && normalized.entry->key == "ni'hao", "Pinyin normalization failed");
    pinyin = *normalized.entry;
    for (auto invalid : {PersonalDictionaryEntry{PersonalDictionaryKind::Pinyin, "nihao", "你好"},
                         PersonalDictionaryEntry{PersonalDictionaryKind::Pinyin, "ni''hao", "你好"},
                         PersonalDictionaryEntry{PersonalDictionaryKind::Pinyin, "ni'hao", "你"},
                         PersonalDictionaryEntry{PersonalDictionaryKind::Wubi, "abcde", "词"},
                         PersonalDictionaryEntry{PersonalDictionaryKind::QuickPhrase, "bad;code", "text"},
                         PersonalDictionaryEntry{PersonalDictionaryKind::English, "hello", "different"},
                         PersonalDictionaryEntry{PersonalDictionaryKind::English, "hello", std::string("a\0b", 3)},
                         PersonalDictionaryEntry{PersonalDictionaryKind::Pinyin, "ni", std::string("\xff", 1)},
                         PersonalDictionaryEntry{static_cast<PersonalDictionaryKind>(99), "a", "a"}})
        require(!edit_personal_dictionary(paths, std::nullopt, invalid).success, "Invalid entry accepted");
    require(edit_personal_dictionary(paths, std::nullopt, pinyin).success, "Pinyin add failed");
    require(has(paths, "nihao", "拟好") && has(paths, "nihc", "拟好", SchemeType::Shuangpin),
            "Personal pinyin is missing from input schemes");
    {
        // 新写入的拼音词按词库写法归一：nue 存成 nve，查询把 nue 归一后才查得到这一行。
        PersonalDictionaryEntry umlaut{PersonalDictionaryKind::Pinyin, "nue'nv", "虐女", 10};
        require(edit_personal_dictionary(paths, std::nullopt, umlaut).success, "Umlaut alias add failed");
        const auto stored = personal_dictionary_entries(paths).entries;
        const auto found = std::find_if(stored.begin(), stored.end(),
                                        [](const PersonalDictionaryEntry &entry) { return entry.value == "虐女"; });
        require(found != stored.end() && found->key == "nve'nv" && has(paths, "nuenv", "虐女"),
                "Umlaut alias was not stored under the dictionary spelling");
        require(edit_personal_dictionary(paths, *found, std::nullopt).success, "Umlaut alias delete failed");
    }
    PersonalDictionaryEntry wubi{PersonalDictionaryKind::Wubi, "wq", "拟好", 12345};
    PersonalDictionaryEntry english{PersonalDictionaryKind::English, "metasequoia", "Metasequoia", 12345};
    PersonalDictionaryEntry quick{PersonalDictionaryKind::QuickPhrase, "test1", "fixture\nsecond line", 12345};
    for (auto entry : {wubi, english, quick})
        require(edit_personal_dictionary(paths, std::nullopt, entry).success, "Non-pinyin add failed");
    require(has(paths, "wq", "拟好", SchemeType::Wubi), "Wubi entry is missing");
    require(has(paths, "metasequoia", "Metasequoia", SchemeType::Quanpin, true), "English entry is missing");
    {
        SessionOptions options;
        options.paths = paths;
        options.local_modes.quick_phrase = true;
        Session session(options);
        session.character('K', true);
        for (char ch : quick.key)
            session.character(ch);
        const auto snapshot = session.snapshot();
        require(std::any_of(snapshot.candidates.begin(), snapshot.candidates.end(),
                            [&](const auto &candidate) { return candidate.word == quick.value; }),
                "Quick phrase is missing from the local input mode");
    }
    {
        // 快捷短语按编码精确匹配、混在普通候选首位。
        PersonalDictionaryEntry letters{PersonalDictionaryKind::QuickPhrase, "qpfixture", "letters phrase", 10};
        require(edit_personal_dictionary(paths, std::nullopt, letters).success, "Quick phrase add failed");
        SessionOptions options;
        options.paths = paths;
        Session session(options);
        for (char ch : letters.key)
            session.character(ch);
        const auto snapshot = session.snapshot();
        require(!snapshot.candidates.empty() && snapshot.candidates.front().word == letters.value,
                "Quick phrase is not mixed first into the candidate list");
        require(edit_personal_dictionary(paths, letters, std::nullopt).success, "Quick phrase delete failed");
    }
    // Moving between dictionaries must commit the old tombstone and new candidate together.
    PersonalDictionaryEntry moved{PersonalDictionaryKind::English, "fixtureword", "FixtureWord", quick.weight};
    require(edit_personal_dictionary(paths, quick, moved).success &&
                has(paths, "fixtureword", "FixtureWord", SchemeType::Quanpin, true),
            "Cross-kind replacement failed");
    require(edit_personal_dictionary(paths, moved, quick).success &&
                !has(paths, "fixtureword", "FixtureWord", SchemeType::Quanpin, true),
            "Cross-kind replacement left the previous candidate");
    auto page = personal_dictionary_entries(paths, 0, 2);
    require(page.error.empty() && page.entries.size() == 2 && page.has_more, "First page failed");
    auto rest = personal_dictionary_entries(paths, 2, 2);
    require(rest.error.empty() && rest.entries.size() == 2 && !rest.has_more, "Second page failed");
    require(!personal_dictionary_entries(paths, 0, 1001).error.empty(), "Unbounded page accepted");
    // Dictionary format contract: 7 uses a numbered table; 8 and 9 must use tbl_others_n.
    for (int count : {7, 8, 9})
    {
        PersonalDictionaryEntry long_word;
        for (int i = 0; i < count; ++i)
        {
            long_word.key += i ? "'ni" : "ni";
            long_word.value += "你";
        }
        require(edit_personal_dictionary(paths, std::nullopt, long_word).success, "Long pinyin entry failed");
        require(has(paths, long_word.key, long_word.value), "Long pinyin entry cannot be queried");
    }
    auto changed = pinyin;
    changed.value = "你好";
    auto stale = pinyin;
    --stale.weight;
    require(!edit_personal_dictionary(paths, stale, changed).success && has(paths, "nihao", "拟好"),
            "Stale edit overwrote entry");
    auto missing_table = pinyin;
    missing_table.key = "bu'hao";
    missing_table.value = "补好";
    require(!edit_personal_dictionary(paths, pinyin, missing_table).success && has(paths, "nihao", "拟好"),
            "Failed replacement lost the previous entry");
    require(edit_personal_dictionary(paths, pinyin, changed, "edit-1").success && !has(paths, "nihao", "拟好") &&
                has(paths, "nihao", "你好"),
            "Replacement did not update candidates");
    require(edit_personal_dictionary(paths, pinyin, changed, "edit-1").success,
            "Retry of a committed edit was not idempotent");
    require(!edit_personal_dictionary(paths, changed, pinyin, "edit-1").success && has(paths, "nihao", "你好"),
            "Request ID reuse applied different content");
    require(!edit_personal_dictionary(paths, changed, pinyin, "bad id").success, "Malformed request ID accepted");
    // Inject a journal write failure after the candidate update; both sides must roll back.
    execute(paths.user(assets::user_journal), "CREATE TRIGGER reject_edit BEFORE INSERT ON user_dictionary_operations"
                                              " BEGIN SELECT RAISE(ABORT,'fixture failure'); END;");
    require(!edit_personal_dictionary(paths, changed, pinyin, "failure-retry").success && has(paths, "nihao", "你好"),
            "Journal failure lost live candidate");
    execute(paths.user(assets::user_journal), "DROP TRIGGER reject_edit;");
    require(edit_personal_dictionary(paths, changed, pinyin, "failure-retry").success,
            "Failed transaction left a receipt that prevented retry");
    require(edit_personal_dictionary(paths, pinyin, changed).success, "Cannot restore removal fixture");
    require(edit_personal_dictionary(paths, changed, std::nullopt).success, "Removal failed");
    auto upgraded = prepare_runtime_paths(resources, paths.user_data, paths.cache, "second");
    require(!has(upgraded, "nihao", "你好") && !has(upgraded, "nihao", "拟好") &&
                has(upgraded, "wq", "拟好", SchemeType::Wubi) &&
                has(upgraded, "metasequoia", "Metasequoia", SchemeType::Quanpin, true),
            "Personal edits did not survive dictionary upgrade");
    require(edit_personal_dictionary(upgraded, pinyin, changed, "edit-1").success && !has(upgraded, "nihao", "你好"),
            "Retry after upgrade resurrected a subsequently removed entry");
    auto entries = personal_dictionary_entries(upgraded);
    require(entries.error.empty() && entries.entries.size() == 6, "Deleted entries leaked into personal list");
}
} // namespace
int main()
{
    try
    {
        run();
        return 0;
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
