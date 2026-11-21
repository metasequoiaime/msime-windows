#include <metasequoia/dictionary_state.h>
#include "user_dictionary_journal.h"
#include "../core/data_path.h"
#include "../contracts/assets/assets.h"
#include <sqlite3.h>
#include <utf8.h>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace metasequoia
{
namespace
{
using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
void require(bool condition)
{
    if (!condition)
        throw std::runtime_error("Invalid or unavailable dictionary state");
}
Database open(const std::filesystem::path &file, int flags)
{
    sqlite3 *raw = nullptr;
    const int status = sqlite3_open_v2(path_to_utf8(file).c_str(), &raw, flags | SQLITE_OPEN_FULLMUTEX, nullptr);
    Database db(raw, sqlite3_close);
    require(status == SQLITE_OK);
    sqlite3_busy_timeout(db.get(), 5000);
    return db;
}
void execute(sqlite3 *db, const char *sql)
{
    require(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
}
Statement prepare(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *raw = nullptr;
    const int status = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
    Statement result(raw, sqlite3_finalize);
    require(status == SQLITE_OK);
    return result;
}
void bind(sqlite3_stmt *s, int index, const std::string &value)
{
    require(sqlite3_bind_text(s, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK);
}
std::string text(sqlite3_stmt *s, int index)
{
    const auto *bytes = sqlite3_column_text(s, index);
    const auto count = sqlite3_column_bytes(s, index);
    require(bytes != nullptr);
    return {reinterpret_cast<const char *>(bytes), static_cast<std::size_t>(count)};
}
const char *name(PersonalDictionaryKind kind)
{
    switch (kind)
    {
    case PersonalDictionaryKind::Pinyin:
        return "pinyin";
    case PersonalDictionaryKind::Wubi:
        return "wubi";
    case PersonalDictionaryKind::English:
        return "english";
    case PersonalDictionaryKind::QuickPhrase:
        return "quick";
    case PersonalDictionaryKind::Wubi98:
        return "wubi98";
    case PersonalDictionaryKind::WubiXsj:
        return "wubixsj";
    }
    throw std::runtime_error("Invalid dictionary kind");
}
PersonalDictionaryKind kind(const std::string &value)
{
    if (value == "wubi98")
        return PersonalDictionaryKind::Wubi98;
    if (value == "wubixsj")
        return PersonalDictionaryKind::WubiXsj;
    if (value == "pinyin")
        return PersonalDictionaryKind::Pinyin;
    if (value == "wubi")
        return PersonalDictionaryKind::Wubi;
    if (value == "english")
        return PersonalDictionaryKind::English;
    if (value == "quick")
        return PersonalDictionaryKind::QuickPhrase;
    throw std::runtime_error("Invalid dictionary kind");
}
void bounded_text(const std::string &value, std::size_t maximum, bool empty = false)
{
    require((empty || !value.empty()) && value.size() <= maximum && value.find('\0') == std::string::npos &&
            utf8::is_valid(value.begin(), value.end()));
}
bool overlap(const std::filesystem::path &a, const std::filesystem::path &b)
{
    const auto p = std::mismatch(a.begin(), a.end(), b.begin(), b.end());
    return p.first == a.end() || p.second == b.end();
}
} // namespace

void stream_dictionary_state(const RuntimePaths &paths, const std::function<bool(const DictionaryStateRecord &)> &emit)
{
    paths.validate();
    require(static_cast<bool>(emit));
    const auto file = paths.user(assets::user_journal);
    if (!std::filesystem::exists(file))
        return;
    // SQLite may need to create WAL coordination files even for a reader. The
    // connection permits those files but query_only forbids journal mutations.
    auto db = open(file, SQLITE_OPEN_READWRITE);
    execute(db.get(), "PRAGMA query_only=ON");
    execute(db.get(), "BEGIN");
    {
        auto rows = prepare(
            db.get(),
            "SELECT dictionary,key,value,weight,display,operation,user_inserted,learning_count,last_learned_at FROM "
            "user_dictionary_operations ORDER BY dictionary,key,value");
        int status;
        while ((status = sqlite3_step(rows.get())) == SQLITE_ROW)
        {
            DictionaryStateEntry entry{kind(text(rows.get(), 0)),
                                       text(rows.get(), 1),
                                       text(rows.get(), 2),
                                       sqlite3_column_int64(rows.get(), 3),
                                       text(rows.get(), 4),
                                       text(rows.get(), 5) == "delete",
                                       sqlite3_column_int(rows.get(), 6) != 0,
                                       sqlite3_column_int(rows.get(), 7),
                                       sqlite3_column_int64(rows.get(), 8)};
            require(emit(entry));
        }
        require(status == SQLITE_DONE);
    }
    for (bool fixed : {true, false})
    {
        auto rows = prepare(db.get(), fixed ? "SELECT context_key,entry_key,value,position FROM "
                                              "fixed_candidate_positions ORDER BY context_key,entry_key,value"
                                            : "SELECT context_key,entry_key,value,selection_count FROM "
                                              "candidate_selection_state ORDER BY context_key,entry_key,value");
        int status;
        while ((status = sqlite3_step(rows.get())) == SQLITE_ROW)
        {
            if (fixed)
                require(emit(DictionaryStatePosition{text(rows.get(), 0), text(rows.get(), 1), text(rows.get(), 2),
                                                     sqlite3_column_int(rows.get(), 3)}));
            else
                require(emit(DictionaryStateSelection{text(rows.get(), 0), text(rows.get(), 1), text(rows.get(), 2),
                                                      sqlite3_column_int(rows.get(), 3)}));
        }
        require(status == SQLITE_DONE);
    }
    execute(db.get(), "COMMIT");
}

RuntimePaths stage_dictionary_state(const std::filesystem::path &resources, const std::filesystem::path &generation,
                                    const std::string &content_id,
                                    const std::function<bool(DictionaryStateRecord &)> &next,
                                    std::size_t maximum_records)
{
    require(resources.is_absolute() && generation.is_absolute() && static_cast<bool>(next));
    require(!overlap(std::filesystem::weakly_canonical(resources), std::filesystem::weakly_canonical(generation)));
    require(std::filesystem::create_directory(generation));
    try
    {
        std::filesystem::permissions(generation, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        const auto user = generation / "user";
        std::filesystem::create_directory(user);
        const auto journal = user / assets::user_journal;
        require(user_dictionary::ensure_user_database(path_to_utf8(journal)));
        {
            auto db = open(journal, SQLITE_OPEN_READWRITE);
            execute(db.get(), "BEGIN IMMEDIATE");
            auto entry = prepare(
                db.get(),
                "INSERT INTO user_dictionary_operations(dictionary,key,value,operation,weight,display,user_inserted,"
                "learning_count,last_learned_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)");
            auto position = prepare(
                db.get(),
                "INSERT INTO fixed_candidate_positions(context_key,entry_key,value,position) VALUES(?1,?2,?3,?4)");
            auto selection =
                prepare(db.get(), "INSERT INTO candidate_selection_state(context_key,entry_key,value,selection_count) "
                                  "VALUES(?1,?2,?3,?4)");
            DictionaryStateRecord record;
            std::size_t count = 0;
            while (next(record))
            {
                require(++count <= maximum_records);
                std::visit(
                    [&](const auto &value) {
                        using T = std::decay_t<decltype(value)>;
                        bounded_text(value.key, 512);
                        bounded_text(value.value, 4096);
                        sqlite3_stmt *stmt;
                        if constexpr (std::is_same_v<T, DictionaryStateEntry>)
                        {
                            bounded_text(value.display, 4096, true);
                            require(value.weight >= 0 && value.weight <= 100000000);
                            stmt = entry.get();
                            bind(stmt, 1, name(value.kind));
                            bind(stmt, 2, value.key);
                            bind(stmt, 3, value.value);
                            bind(stmt, 4, value.deleted ? "delete" : "upsert");
                            require(sqlite3_bind_int64(stmt, 5, value.weight) == SQLITE_OK);
                            bind(stmt, 6, value.display);
                            require(sqlite3_bind_int(stmt, 7, value.user_inserted) == SQLITE_OK);
                            require(value.learning_count == 0 || (value.kind == PersonalDictionaryKind::Pinyin &&
                                                                  !value.deleted && value.learning_count == 1));
                            require(value.last_learned_at >= 0 &&
                                    (value.learning_count == 0 || value.last_learned_at > 0));
                            require(sqlite3_bind_int(stmt, 8, value.learning_count) == SQLITE_OK);
                            require(sqlite3_bind_int64(stmt, 9, value.last_learned_at) == SQLITE_OK);
                        }
                        else
                        {
                            bounded_text(value.context, 512);
                            if constexpr (std::is_same_v<T, DictionaryStatePosition>)
                            {
                                require(value.position >= 1 && value.position <= 5);
                                stmt = position.get();
                                require(sqlite3_bind_int(stmt, 4, value.position) == SQLITE_OK);
                            }
                            else
                            {
                                require(value.count >= 0 && value.count <= 10);
                                stmt = selection.get();
                                require(sqlite3_bind_int(stmt, 4, value.count) == SQLITE_OK);
                            }
                            bind(stmt, 1, value.context);
                            bind(stmt, 2, value.key);
                            bind(stmt, 3, value.value);
                        }
                        require(sqlite3_step(stmt) == SQLITE_DONE);
                        require(sqlite3_reset(stmt) == SQLITE_OK);
                    },
                    record);
            }
            execute(db.get(), "COMMIT");
        }
        return prepare_runtime_paths(resources, user, generation / "cache", content_id);
    }
    catch (...)
    {
        std::error_code ignored;
        std::filesystem::remove_all(generation, ignored);
        throw;
    }
}
} // namespace metasequoia
