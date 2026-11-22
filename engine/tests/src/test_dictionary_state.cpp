#include <metasequoia/dictionary_state.h>
#include <metasequoia/session.h>
#include "../../core/data_path.h"
#include "../../contracts/assets/assets.h"
#include "../../english/english_dictionary.h"
#include "../../user_dictionary/user_dictionary_journal.h"
#include "test_directory_cleanup.h"
#include <sqlite3.h>
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace metasequoia;
namespace
{
void check(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
void sql(const std::filesystem::path &path, const char *query)
{
    sqlite3 *db = nullptr;
    check(sqlite3_open(path_to_utf8(path).c_str(), &db) == SQLITE_OK, "open fixture");
    int status = sqlite3_exec(db, query, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    check(status == SQLITE_OK, "fixture SQL");
}
RuntimePaths stage(const std::filesystem::path &resources, const std::filesystem::path &root,
                   const std::vector<DictionaryStateRecord> &records)
{
    std::size_t i = 0;
    return stage_dictionary_state(resources, root, "fixture", [&](DictionaryStateRecord &out) {
        if (i == records.size())
            return false;
        out = records[i++];
        return true;
    });
}
void run(bool capacity)
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("dictionary-state-" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    test::ScopedDataDirectoryCleanup cleanup(root);
    const auto resources = root / "resources";
    std::filesystem::create_directories(resources);
    sql(resources / assets::main_dictionary,
        "CREATE TABLE tbl_2_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
        "INSERT INTO tbl_2_n VALUES('ni''hao','nh','你好',100),('ni''hao','nh','拟好',80);"
        "CREATE TABLE wubi86(key TEXT,value TEXT,weight INTEGER);"
        "CREATE TABLE quick_parases(key TEXT,value TEXT,weight INTEGER);"
        "CREATE INDEX idx_quick_parases_key_weight ON quick_parases(key,weight DESC);");
    check(EnglishDictionary::ensure_schema(path_to_utf8(resources / assets::english_dictionary)), "English schema");
    if (capacity)
    {
        std::size_t count = 0;
        const auto staged =
            stage_dictionary_state(resources, root / "capacity", "fixture", [&](DictionaryStateRecord &out) {
                if (count == 100000)
                    return false;
                const auto suffix = std::to_string(++count);
                out = DictionaryStateEntry{
                    PersonalDictionaryKind::QuickPhrase, "bulk" + suffix, "合成短语" + suffix, 100, "", false, true};
                return true;
            });
        std::size_t received = 0;
        stream_dictionary_state(staged, [&](const auto &record) {
            const auto *entry = std::get_if<DictionaryStateEntry>(&record);
            check(entry && entry->user_inserted && entry->weight == 100, "capacity state damaged");
            ++received;
            return true;
        });
        check(received == 100000, "capacity state truncated");
        return;
    }
    std::vector<DictionaryStateRecord> records{
        DictionaryStateEntry{PersonalDictionaryKind::Pinyin, "ni'hao", "你好", 0, "", true, true},
        DictionaryStateEntry{PersonalDictionaryKind::Pinyin, "ni'hao", "拟好", 1000, "", false, true, 1, 12345},
        DictionaryStateEntry{PersonalDictionaryKind::Pinyin, "ni'hao", "拟蒿", 150, "", false, true},
        DictionaryStateEntry{PersonalDictionaryKind::QuickPhrase, "fixture", "合成短语", 100, "", false, true},
        DictionaryStateEntry{PersonalDictionaryKind::English, "cloudfixture", "Cloudfixture", 100, "Cloudfixture",
                             false, true},
        DictionaryStatePosition{"ni'hao", "ni'hao", "拟蒿", 1},
        DictionaryStateSelection{"ni'hao", "ni'hao", "拟好", 1}};
    const auto first = stage(resources, root / "first", records);
    sql(first.user(assets::user_journal), "PRAGMA journal_mode=WAL");
    bool wrote = false;
    std::size_t snapshot_count = 0;
    stream_dictionary_state(first, [&](const auto &) {
        ++snapshot_count;
        if (!wrote)
        {
            wrote = true;
            check(user_dictionary::set_fixed_position(path_to_utf8(first.user(assets::user_journal)), "ni'hao",
                                                      "ni'hao", "拟好", 2),
                  "concurrent writer failed");
        }
        return true;
    });
    check(snapshot_count == records.size(), "export mixed concurrent journal versions");
    check(user_dictionary::clear_fixed_position(path_to_utf8(first.user(assets::user_journal)), "ni'hao", "ni'hao",
                                                "拟好"),
          "clear concurrent fixture");
    std::vector<DictionaryStateRecord> exported;
    stream_dictionary_state(first, [&](const auto &record) {
        exported.push_back(record);
        return true;
    });
    check(exported.size() == records.size(), "complete state not exported");
    const auto second = stage(resources, root / "second", exported);
    std::vector<DictionaryStateRecord> roundtrip;
    stream_dictionary_state(second, [&](const auto &record) {
        roundtrip.push_back(record);
        return true;
    });
    check(roundtrip.size() == records.size(), "state record lost");
    bool selection = false, ownership = false, deleted_ownership = false, learning_state = false;
    for (const auto &record : roundtrip)
    {
        if (auto p = std::get_if<DictionaryStateSelection>(&record))
            selection = p->count == 1;
        if (auto p = std::get_if<DictionaryStateEntry>(&record); p && p->value == "拟蒿")
            ownership = p->user_inserted;
        if (auto p = std::get_if<DictionaryStateEntry>(&record); p && p->value == "拟好")
            learning_state = p->learning_count == 1 && p->last_learned_at == 12345;
        if (auto p = std::get_if<DictionaryStateEntry>(&record); p && p->value == "你好")
            deleted_ownership = p->deleted && p->user_inserted;
    }
    check(selection && ownership && deleted_ownership && learning_state, "counter or ownership lost");
    SessionOptions options;
    options.paths = second;
    options.learning = false;
    Session session(options);
    for (char ch : std::string("nihao"))
        session.character(ch);
    const auto candidates = session.snapshot().candidates;
    check(!candidates.empty() && candidates.front().word == "拟蒿", "fixed position not applied");
    for (const auto &candidate : candidates)
        check(candidate.word != "你好", "deleted candidate returned");
    // A complete replacement starts from resources, not the previously overlaid dictionaries.
    const auto empty = stage(resources, root / "empty", {});
    SessionOptions plain;
    plain.paths = empty;
    plain.learning = false;
    Session baseline(plain);
    for (char ch : std::string("nihao"))
        baseline.character(ch);
    check(baseline.snapshot().candidates.front().word == "你好", "old deletion leaked into replacement");
    auto rejected = [&](const char *name, std::vector<DictionaryStateRecord> invalid) {
        const auto path = root / name;
        bool failed = false;
        try
        {
            stage(resources, path, invalid);
        }
        catch (const std::exception &)
        {
            failed = true;
        }
        check(failed && !std::filesystem::exists(path), "failed staging left partial generation");
    };
    bool limited = false;
    try
    {
        std::size_t index = 0;
        stage_dictionary_state(
            resources, root / "limited", "fixture",
            [&](DictionaryStateRecord &out) {
                if (index == records.size())
                    return false;
                out = records[index++];
                return true;
            },
            1);
    }
    catch (const std::exception &)
    {
        limited = true;
    }
    check(limited && !std::filesystem::exists(root / "limited"), "explicit record limit ignored");
    auto duplicate = records;
    duplicate.push_back(records.front());
    rejected("duplicate", duplicate);
    auto duplicate_slot = records;
    duplicate_slot.push_back(DictionaryStatePosition{"ni'hao", "ni'hao", "拟好", 1});
    rejected("slot", duplicate_slot);
    rejected("bad-position", {DictionaryStatePosition{"ni'hao", "ni'hao", "拟好", 6}});
    bool failed = false;
    try
    {
        stage(resources, root / "first", {});
    }
    catch (const std::exception &)
    {
        failed = true;
    }
    check(failed && std::filesystem::exists(first.user(assets::user_journal)), "existing generation was overwritten");
    failed = false;
    try
    {
        stage_dictionary_state(resources, root / "cancelled", "fixture", [](DictionaryStateRecord &) -> bool {
            throw std::runtime_error("cancelled or invalid transport checksum");
        });
    }
    catch (const std::exception &)
    {
        failed = true;
    }
    check(failed && !std::filesystem::exists(root / "cancelled"), "cancelled generation leaked");
}
} // namespace
int main(int argc, char **argv)
{
    try
    {
        run(argc == 2 && std::string(argv[1]) == "--capacity");
        std::cout << "Dictionary state tests passed\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
    catch (...)
    {
        std::cerr << "An exception escaped the test body.\n";
        return 1;
    }
}
