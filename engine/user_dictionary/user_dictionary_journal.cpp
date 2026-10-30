#include "../contracts/assets/assets.h"
#include "../contracts/dictionary/format.h"
#include "user_dictionary_journal.h"
#include <metasequoia/personal_dictionary.h>

#include <sqlite3.h>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>
#include <algorithm>
#include <limits>
#include <mutex>
#include <numeric>
#include "../core/data_path.h"
#include "../english/english_dictionary.h"
#include "../local_modes/local_database.h"
#include "../neural/json.h"
#include "../quanpin/quanpin_utils.h"

namespace user_dictionary
{
namespace
{
struct DbCloser
{
    void operator()(sqlite3 *db) const
    {
        if (db != nullptr)
            sqlite3_close(db);
    }
};
using Db = std::unique_ptr<sqlite3, DbCloser>;

struct StmtCloser
{
    void operator()(sqlite3_stmt *stmt) const
    {
        if (stmt != nullptr)
            sqlite3_finalize(stmt);
    }
};
using Stmt = std::unique_ptr<sqlite3_stmt, StmtCloser>;

const char *kind_name(DictionaryKind kind)
{
    switch (kind)
    {
    case DictionaryKind::Pinyin:
        return "pinyin";
    case DictionaryKind::Wubi:
        return "wubi";
    case DictionaryKind::QuickPhrase:
        return "quick";
    case DictionaryKind::English:
        return "english";
    }
    return "";
}

Db open_database(const std::string &path, int flags)
{
    sqlite3 *raw = nullptr;
    if (sqlite3_open_v2(path.c_str(), &raw, flags | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
    {
        if (raw != nullptr)
            sqlite3_close(raw);
        return {};
    }
    sqlite3_busy_timeout(raw, kJournalBusyTimeoutMs);
    return Db(raw);
}

Stmt prepare(sqlite3 *db, const std::string &sql)
{
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
        return {};
    return Stmt(raw);
}

bool execute_sql(sqlite3 *db, const char *sql)
{
    return db != nullptr && sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

// Learning writes run on the dictionary writer thread while the key thread reads
// the same files. In the default rollback-journal mode every commit takes an
// exclusive lock, so a read on the key path waits out the whole commit and its
// fsync -- seconds on a busy disk or under an antivirus scan of the -journal
// file, and long enough for the client's commit-reply wait to time out and drop
// the composition. WAL lets readers proceed against the last committed snapshot.
// The mode is persistent; switching needs a moment with no other connection
// mid-transaction, so a busy database simply stays as it is until the next try.
bool use_write_ahead_log(sqlite3 *db)
{
    auto stmt = prepare(db, "PRAGMA journal_mode=WAL");
    if (!stmt || sqlite3_step(stmt.get()) != SQLITE_ROW)
        return false;
    const unsigned char *mode = sqlite3_column_text(stmt.get(), 0);
    return mode != nullptr && std::string(reinterpret_cast<const char *>(mode)) == "wal";
}

// WAL on its own does not stop a commit from paying for durability. SQLite defaults to synchronous=FULL, which fsyncs
// the WAL on every COMMIT, and on the spinning disks #1012 reports that fsync queues behind whatever else is reading
// the disk: measured at 14.4 ms median and 38.8 ms peak for a single 4 KiB write on that machine. Learning writes run
// at a rate the user controls by typing, so a burst of selections is a burst of fsyncs, and the read on the key path
// that lands in one of them waits.
//
// NORMAL keeps the database consistent and never corrupt, and only gives up durability of the last commits if the
// machine loses power: a process crash still leaves them on disk. What can be lost is the last few frequency
// adjustments, which are learning data, not the text the user typed; the text was committed through the pipe long
// before this write started. Trading that for a twentyfold cut in commit cost is the right way round.
//
// The cost moves to the checkpoint, which runs on whichever connection completes the commit that crosses the
// threshold. The default threshold is 1000 pages, and the WAL of a real install was already sitting at 4,132,392
// bytes, i.e. the threshold itself, so that checkpoint was landing inside an ordinary keystroke's write. The
// threshold is lowered so each one copies less, and the writer drains the rest explicitly when it is idle instead
// (see the checkpoint the dictionary writer runs after its queue empties).
bool tune_wal(sqlite3 *db)
{
    // Both pragmas are cheap and idempotent, and a database that refuses one is still usable at the default, so this
    // retries once rather than escalating: the engine has no logging channel, and a failure here costs the #1012
    // optimisation rather than correctness. The retry covers a transient lock the caller cannot do anything about.
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (execute_sql(db, "PRAGMA synchronous=NORMAL") &&
            execute_sql(db, ("PRAGMA wal_autocheckpoint=" + std::to_string(kWalAutocheckpointPages)).c_str()))
            return true;
    }
    return false;
}

bool bind_text(sqlite3_stmt *stmt, int index, const std::string &value)
{
    return sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

Stmt prepare_upsert_journal(sqlite3 *db)
{
    // 调频写下的权重会覆盖设置页改过的权重，这一行从此按自动调频对待（见 replay）。
    return prepare(db, "INSERT INTO user_dictionary_operations(dictionary,key,value,operation,weight,display)"
                       " VALUES(?1,?2,?3,'upsert',?4,?5)"
                       " ON CONFLICT(dictionary,key,value) DO UPDATE SET operation='upsert',weight=excluded.weight,"
                       " display=excluded.display,weight_edited=0,updated_at=unixepoch()");
}

bool write_upsert_journal(sqlite3_stmt *stmt, DictionaryKind kind, const std::string &key, const std::string &value,
                          std::int64_t weight, const std::string &display = {})
{
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return bind_text(stmt, 1, kind_name(kind)) && bind_text(stmt, 2, key) && bind_text(stmt, 3, value) &&
           sqlite3_bind_int64(stmt, 4, weight) == SQLITE_OK && bind_text(stmt, 5, display) &&
           sqlite3_step(stmt) == SQLITE_DONE;
}

// 旧版日文模式的调频把罗马音当拼音键写进了拼音日志（#869），回放会把这些日文词插进拼音表。
// 拼音的自动学习不会产生带假名的词，带假名的非手动条目一律视为这类残留。SQLite 的 GLOB
// 字符类按码位比较 UTF-8：平假名、片假名、片假名音标扩展、半角片假名。
constexpr const char *kLearnedKanaPinyinCondition =
    "dictionary='pinyin' AND operation='upsert' AND user_inserted=0 AND value GLOB '*[ぁ-ゖァ-ヺㇰ-ㇿｦ-ﾝ]*'";

bool has_column(sqlite3 *db, const std::string &table, const std::string &column)
{
    auto columns = prepare(db, "PRAGMA table_info(\"" + table + "\")");
    if (!columns)
        return false;
    while (sqlite3_step(columns.get()) == SQLITE_ROW)
    {
        const unsigned char *name = sqlite3_column_text(columns.get(), 1);
        if (name != nullptr && column == reinterpret_cast<const char *>(name))
            return true;
    }
    return false;
}

bool ensure_schema(sqlite3 *db)
{
    constexpr const char *sql =
        "CREATE TABLE IF NOT EXISTS user_dictionary_operations("
        "dictionary TEXT NOT NULL,"
        "key TEXT NOT NULL,"
        "value TEXT NOT NULL,"
        "operation TEXT NOT NULL CHECK(operation IN ('upsert','delete')),"
        "weight INTEGER NOT NULL DEFAULT 0,"
        "display TEXT NOT NULL DEFAULT '',"
        "user_inserted INTEGER NOT NULL DEFAULT 0,"
        "weight_edited INTEGER NOT NULL DEFAULT 0,"
        "updated_at INTEGER NOT NULL DEFAULT(unixepoch()),"
        "PRIMARY KEY(dictionary,key,value));"
        "CREATE TABLE IF NOT EXISTS personal_dictionary_receipts( request_id TEXT PRIMARY KEY, payload TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS candidate_selection_state("
        "context_key TEXT NOT NULL,entry_key TEXT NOT NULL,value TEXT NOT NULL,"
        "selection_count INTEGER NOT NULL DEFAULT 0,"
        "PRIMARY KEY(context_key,entry_key,value));"
        "CREATE TABLE IF NOT EXISTS fixed_candidate_positions("
        "context_key TEXT NOT NULL,entry_key TEXT NOT NULL,value TEXT NOT NULL,"
        "position INTEGER NOT NULL CHECK(position BETWEEN 1 AND 5),"
        "PRIMARY KEY(context_key,entry_key,value),UNIQUE(context_key,position));"
        "CREATE TABLE IF NOT EXISTS quick_phrase_slots("
        "code TEXT PRIMARY KEY,slot INTEGER NOT NULL CHECK(slot>=0));"
        "CREATE TABLE IF NOT EXISTS english_slots("
        "code TEXT PRIMARY KEY,slot INTEGER NOT NULL CHECK(slot>=0));"
        "CREATE TABLE IF NOT EXISTS date_time_format_order("
        "category TEXT NOT NULL,format TEXT NOT NULL,position INTEGER NOT NULL CHECK(position>=0),"
        "PRIMARY KEY(category,format));"
        "CREATE TABLE IF NOT EXISTS dictionary_state(name TEXT PRIMARY KEY,value TEXT NOT NULL);";
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        return false;

    if (!has_column(db, "user_dictionary_operations", "user_inserted") &&
        sqlite3_exec(db,
                     "ALTER TABLE user_dictionary_operations "
                     "ADD COLUMN user_inserted INTEGER NOT NULL DEFAULT 0",
                     nullptr, nullptr, nullptr) != SQLITE_OK)
        return false;
    if (!has_column(db, "user_dictionary_operations", "weight_edited") &&
        sqlite3_exec(db,
                     "ALTER TABLE user_dictionary_operations "
                     "ADD COLUMN weight_edited INTEGER NOT NULL DEFAULT 0",
                     nullptr, nullptr, nullptr) != SQLITE_OK)
        return false;
    // Setting user_version is a write, even to the same value. Skip it when it is
    // already current, so opening a connection never waits for another writer.
    auto version = prepare(db, "PRAGMA user_version");
    if (version && sqlite3_step(version.get()) == SQLITE_ROW && sqlite3_column_int(version.get(), 0) == 3)
        return true;
    version.reset();
    return sqlite3_exec(db, "PRAGMA user_version=3", nullptr, nullptr, nullptr) == SQLITE_OK;
}

std::vector<std::string> pinyin_segments(const std::string &key)
{
    std::vector<std::string> segments;
    size_t start = 0;
    while (start <= key.size())
    {
        const size_t delimiter = key.find('\'', start);
        const std::string segment =
            key.substr(start, delimiter == std::string::npos ? std::string::npos : delimiter - start);
        if (segment.empty())
            return {};
        segments.push_back(segment);
        if (delimiter == std::string::npos)
            break;
        start = delimiter + 1;
    }
    return segments;
}

std::string pinyin_table(const std::string &key)
{
    const auto segments = pinyin_segments(key);
    if (segments.empty())
        return {};
    return metasequoia::dictionary_format::quanpin_table(segments.size(), segments.front().front());
}

constexpr std::int64_t kManagedWeightCeiling = 100000000LL;
constexpr std::int64_t kManagedWeightFloor = 1LL;
constexpr std::int64_t kRebalanceGap = 1000LL;
constexpr size_t kRebalanceCount = 16;

std::int64_t clamp_managed_weight(std::int64_t weight)
{
    if (weight < kManagedWeightFloor)
        return kManagedWeightFloor;
    if (weight > kManagedWeightCeiling)
        return kManagedWeightCeiling;
    return weight;
}

size_t utf8_char_count(const std::string &text)
{
    return static_cast<size_t>(
        std::count_if(text.begin(), text.end(), [](unsigned char ch) { return (ch & 0xC0) != 0x80; }));
}

std::string candidate_dictionary_key(const WordItem &item, const std::string &context_key)
{
    if (!item.canonical_pinyin.empty())
        return item.canonical_pinyin;
    std::string item_key = item.pinyin;
    const auto context_segments = pinyin_segments(context_key);
    if (context_segments.size() <= 1)
        return item_key;
    auto item_segments = context_segments;
    const size_t char_count = utf8_char_count(item.word);
    if (char_count > 0 && item_segments.size() > char_count)
        item_segments.resize(char_count);
    item_key.clear();
    for (size_t j = 0; j < item_segments.size(); ++j)
    {
        if (j)
            item_key += '\'';
        item_key += item_segments[j];
    }
    return item_key;
}

bool update_pinyin_weight(sqlite3 *main_db, sqlite3_stmt *journal_upsert, const std::string &key,
                          const std::string &value, std::int64_t weight)
{
    weight = clamp_managed_weight(weight);
    const std::string table = pinyin_table(key);
    if (table.empty() || main_db == nullptr || journal_upsert == nullptr)
        return false;
    auto stmt = prepare(main_db, "UPDATE \"" + table + "\" SET weight=?1 WHERE key=?2 AND value=?3");
    if (!stmt || sqlite3_bind_int64(stmt.get(), 1, weight) != SQLITE_OK || !bind_text(stmt.get(), 2, key) ||
        !bind_text(stmt.get(), 3, value) || sqlite3_step(stmt.get()) != SQLITE_DONE)
        return false;
    // 调频只改已有的行。没改到行时不能记日志：回放的 apply_pinyin 找不到行就插入，
    // 词库里本不存在的词（如日文候选，#869）会在升级后凭空出现。
    if (sqlite3_changes(main_db) == 0)
        return true;
    return write_upsert_journal(journal_upsert, DictionaryKind::Pinyin, key, value, weight);
}

bool update_wubi_weight(sqlite3 *main_db, sqlite3_stmt *journal_upsert, const std::string &key,
                        const std::string &value, std::int64_t weight)
{
    weight = clamp_managed_weight(weight);
    if (main_db == nullptr || journal_upsert == nullptr || key.empty() || value.empty())
        return false;
    auto stmt = prepare(main_db, "UPDATE wubi86 SET weight=?1 WHERE key=?2 AND value=?3");
    return stmt && sqlite3_bind_int64(stmt.get(), 1, weight) == SQLITE_OK && bind_text(stmt.get(), 2, key) &&
           bind_text(stmt.get(), 3, value) && sqlite3_step(stmt.get()) == SQLITE_DONE && sqlite3_changes(main_db) > 0 &&
           write_upsert_journal(journal_upsert, DictionaryKind::Wubi, key, value, weight);
}

bool update_ranked_weight(sqlite3 *main_db, sqlite3_stmt *journal_upsert, DictionaryKind kind, const std::string &key,
                          const std::string &value, std::int64_t weight)
{
    if (kind == DictionaryKind::Wubi)
        return update_wubi_weight(main_db, journal_upsert, key, value, weight);
    return update_pinyin_weight(main_db, journal_upsert, key, value, weight);
}

size_t ranking_target(size_t rank, const std::string &mode, int linear_step, bool force_top)
{
    if (force_top)
        return 0;
    if (mode == "halve")
        return rank / 2;
    if (mode == "linear")
        return rank > static_cast<size_t>(linear_step) ? rank - static_cast<size_t>(linear_step) : 0;
    if (mode == "promote")
        return rank > 4 ? 4 : rank - 1;
    return 0;
}

std::string jianpin(const std::vector<std::string> &segments)
{
    std::string result;
    result.reserve(segments.size());
    for (const auto &segment : segments)
        result.push_back(segment.front());
    return result;
}

bool apply_pinyin(sqlite3 *db, const std::string &key, const std::string &value, const std::string &operation,
                  std::int64_t weight)
{
    const auto segments = pinyin_segments(key);
    if (segments.empty())
        return false;
    const std::string table = pinyin_table(key);
    if (operation == "delete")
    {
        auto stmt = prepare(db, "DELETE FROM \"" + table + "\" WHERE key=?1 AND value=?2");
        return stmt && bind_text(stmt.get(), 1, key) && bind_text(stmt.get(), 2, value) &&
               sqlite3_step(stmt.get()) == SQLITE_DONE;
    }

    auto update = prepare(db, "UPDATE \"" + table + "\" SET jp=?1,weight=?2 WHERE key=?3 AND value=?4");
    if (!update || !bind_text(update.get(), 1, jianpin(segments)) ||
        sqlite3_bind_int64(update.get(), 2, weight) != SQLITE_OK || !bind_text(update.get(), 3, key) ||
        !bind_text(update.get(), 4, value) || sqlite3_step(update.get()) != SQLITE_DONE)
        return false;
    if (sqlite3_changes(db) > 0)
        return true;

    auto insert = prepare(db, "INSERT INTO \"" + table + "\"(key,jp,value,weight) VALUES(?1,?2,?3,?4)");
    return insert && bind_text(insert.get(), 1, key) && bind_text(insert.get(), 2, jianpin(segments)) &&
           bind_text(insert.get(), 3, value) && sqlite3_bind_int64(insert.get(), 4, weight) == SQLITE_OK &&
           sqlite3_step(insert.get()) == SQLITE_DONE;
}

bool apply_simple(sqlite3 *db, const std::string &table, const std::string &key_column, const std::string &value_column,
                  const std::string &key, const std::string &value, const std::string &operation, std::int64_t weight)
{
    if (operation == "delete")
    {
        auto stmt = prepare(db, "DELETE FROM \"" + table + "\" WHERE \"" + key_column + "\"=?1 AND \"" + value_column +
                                    "\"=?2");
        return stmt && bind_text(stmt.get(), 1, key) && bind_text(stmt.get(), 2, value) &&
               sqlite3_step(stmt.get()) == SQLITE_DONE;
    }
    auto update = prepare(db, "UPDATE \"" + table + "\" SET weight=?1 WHERE \"" + key_column + "\"=?2 AND \"" +
                                  value_column + "\"=?3");
    if (!update || sqlite3_bind_int64(update.get(), 1, weight) != SQLITE_OK || !bind_text(update.get(), 2, key) ||
        !bind_text(update.get(), 3, value) || sqlite3_step(update.get()) != SQLITE_DONE)
        return false;
    if (sqlite3_changes(db) > 0)
        return true;
    auto insert = prepare(db, "INSERT INTO \"" + table + "\"(\"" + key_column + "\",\"" + value_column +
                                  "\",weight) VALUES(?1,?2,?3)");
    return insert && bind_text(insert.get(), 1, key) && bind_text(insert.get(), 2, value) &&
           sqlite3_bind_int64(insert.get(), 3, weight) == SQLITE_OK && sqlite3_step(insert.get()) == SQLITE_DONE;
}

bool apply_english(sqlite3 *db, const std::string &key, const std::string &value, const std::string &operation,
                   std::int64_t weight, const std::string &display)
{
    const std::string effective_display = display.empty() ? value : display;
    if (operation == "delete")
    {
        auto stmt = prepare(db, "DELETE FROM replay_english.english_words WHERE word=?1 AND display=?2");
        return stmt && bind_text(stmt.get(), 1, key) && bind_text(stmt.get(), 2, effective_display) &&
               sqlite3_step(stmt.get()) == SQLITE_DONE;
    }
    auto upsert = prepare(db, "INSERT INTO replay_english.english_words(word,display,weight) VALUES(?1,?2,?3) "
                              "ON CONFLICT(word,display) DO UPDATE SET weight=excluded.weight");
    return upsert && bind_text(upsert.get(), 1, key) && bind_text(upsert.get(), 2, effective_display) &&
           sqlite3_bind_int64(upsert.get(), 3, weight) == SQLITE_OK && sqlite3_step(upsert.get()) == SQLITE_DONE;
}
} // namespace

std::string default_user_db_path()
{
    return metasequoia::path_to_utf8(metasequoia::data_file_path(metasequoia::assets::user_journal));
}

namespace
{
// Opening the journal costs a file open plus a full ensure_schema() pass, and
// callers on the hot path pay it per keystroke: apply_fixed_positions runs while
// the candidate list for every input is being built. Measured on Windows with a
// busy disk, that open dominated the candidate build -- 90% of its time, with
// single calls reaching 737 ms -- which stalled the IME server's task thread long
// enough for the client's in-edit-session reply wait to time out and drop the
// commit key. Keep the connection alive instead -- one per thread. FULLMUTEX makes
// a shared handle safe for single calls, but not for transactions: a connection
// has one transaction state, so a thread writing BEGIN IMMEDIATE..COMMIT on it
// would have another thread's reads land inside that transaction, and the other
// thread's UserDatabase destructor would roll it back. Separate connections keep
// the transactions apart and leave the concurrency to SQLite's file locking and
// the busy timeout.
struct CachedDatabase
{
    std::string path;
    std::uint64_t generation = 0;
    std::shared_ptr<sqlite3> connection;
};

// Bumped by close_default_user_database(); every thread drops its cached
// connection the next time it sees a newer generation.
std::atomic<std::uint64_t> &database_cache_generation()
{
    static std::atomic<std::uint64_t> generation{0};
    return generation;
}

CachedDatabase &default_database_cache()
{
    thread_local CachedDatabase cache;
    return cache;
}

// The handle is shared rather than borrowed so that close_default_user_database()
// can drop the cache entry at any time without invalidating a connection an
// operation is still running on; the file is released once the last user is done.
std::shared_ptr<sqlite3> open_shared_database(const std::string &path)
{
    Db opened = open_database(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    if (!opened)
        return {};
    if (use_write_ahead_log(opened.get()))
        (void)tune_wal(opened.get());
    if (!ensure_schema(opened.get()))
        return {};
    return std::shared_ptr<sqlite3>(opened.release(), [](sqlite3 *db) { sqlite3_close(db); });
}

std::shared_ptr<sqlite3> acquire_database(const std::string &path)
{
    // Only the default journal is worth caching, and only it is safe to cache:
    // every other path belongs to an import tool or a test that works on a
    // database and then deletes the directory holding it, which on Windows fails
    // while a connection is still open. Those keep the original one-per-call
    // connection, so nothing but the hot path changes behaviour.
    if (path != default_user_db_path())
        return open_shared_database(path);

    auto &cache = default_database_cache();
    const std::uint64_t generation = database_cache_generation().load(std::memory_order_acquire);
    if (cache.connection && cache.path == path && cache.generation == generation)
        return cache.connection;
    // Release a connection cached for an earlier data directory or generation
    // before opening, so a thread never holds two journal files open.
    cache = {};
    auto connection = open_shared_database(path);
    if (!connection)
        return {};
    cache.path = path;
    cache.generation = generation;
    cache.connection = connection;
    return connection;
}

class UserDatabase
{
  public:
    explicit UserDatabase(const std::string &path) : db_(acquire_database(path))
    {
    }

    // A caller that returns between BEGIN IMMEDIATE and COMMIT used to be rescued
    // by its private connection closing, which rolls back implicitly. A cached
    // connection outlives the call, so an abandoned transaction would keep its
    // write lock and fail every later operation. End it here instead.
    ~UserDatabase()
    {
        if (db_ && sqlite3_get_autocommit(db_.get()) == 0)
            sqlite3_exec(db_.get(), "ROLLBACK", nullptr, nullptr, nullptr);
    }

    UserDatabase(const UserDatabase &) = delete;
    UserDatabase &operator=(const UserDatabase &) = delete;

    explicit operator bool() const
    {
        return db_ != nullptr;
    }
    sqlite3 *get() const
    {
        return db_.get();
    }

  private:
    std::shared_ptr<sqlite3> db_;
};
} // namespace

void close_default_user_database()
{
    // Callers use this to let go of the journal before deleting or replacing the
    // data directory; a connection left open would block that on Windows. The
    // calling thread's connection closes now (once no operation still holds it);
    // other threads' connections close on their next journal access.
    database_cache_generation().fetch_add(1, std::memory_order_acq_rel);
    default_database_cache() = {};
    // The local-mode queries share connections to the dictionaries in the same
    // data directory; they have to let go too.
    metasequoia::local_modes::close_cached_local_databases();
}

bool enable_write_ahead_log(const std::string &db_path)
{
    auto db = open_database(db_path, SQLITE_OPEN_READWRITE);
    if (!db || !use_write_ahead_log(db.get()))
        return false;
    // The same durability setting the journal uses: this connection is opened by the writer, and its commits are the
    // other half of what a selection costs on a slow disk. A database that refuses the setting is still usable, so the
    // failure is not reported as "not in WAL".
    (void)tune_wal(db.get());
    return true;
}

void checkpoint_user_journal_if_needed()
{
    auto db = open_database(default_user_db_path(), SQLITE_OPEN_READWRITE);
    if (!db)
        return;
    // Ask first: this runs every time the writer drains, which is every time the user stops typing, and checkpointing a
    // log that is already small would still walk its frames for nothing.
    auto progress = prepare(db.get(), "PRAGMA wal_checkpoint(PASSIVE)");
    if (!progress || sqlite3_step(progress.get()) != SQLITE_ROW)
        return;
    const int blocked = sqlite3_column_int(progress.get(), 0);
    const int logged = sqlite3_column_int(progress.get(), 1);
    progress.reset();
    if (blocked != 0 || logged < kWalAutocheckpointPages)
        return;
    // PASSIVE did not manage to move everything, which happens while a read is in progress. TRUNCATE gives the log back
    // its starting size, which is the point of running this at all; it returns without doing so if a reader is still
    // holding a frame, and the next drain tries again.
    (void)execute_sql(db.get(), "PRAGMA wal_checkpoint(TRUNCATE)");
}

bool ensure_user_database(const std::string &user_db_path)
{
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    // Drop ranking rows left by the old rebalance staircase so installer replay
    // cannot bury shipped frequencies such as 先/xian under negative weights.
    sqlite3_exec(db.get(),
                 "DELETE FROM user_dictionary_operations "
                 "WHERE dictionary='pinyin' AND operation='upsert' AND weight < 1",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db.get(),
                 (std::string("DELETE FROM user_dictionary_operations WHERE ") + kLearnedKanaPinyinCondition).c_str(),
                 nullptr, nullptr, nullptr);
    return true;
}

bool record_upsert(const std::string &user_db_path, DictionaryKind kind, const std::string &key,
                   const std::string &value, std::int64_t weight, const std::string &display)
{
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    auto stmt = prepare_upsert_journal(db.get());
    return stmt && write_upsert_journal(stmt.get(), kind, key, value, weight, display);
}

bool record_weight_edit(const std::string &user_db_path, DictionaryKind kind, const std::string &key,
                        const std::string &value, std::int64_t weight, const std::string &display)
{
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    auto stmt =
        prepare(db.get(), "INSERT INTO user_dictionary_operations("
                          "dictionary,key,value,operation,weight,display,weight_edited)"
                          " VALUES(?1,?2,?3,'upsert',?4,?5,1)"
                          " ON CONFLICT(dictionary,key,value) DO UPDATE SET operation='upsert',weight=excluded.weight,"
                          " display=excluded.display,weight_edited=1,updated_at=unixepoch()");
    return stmt && bind_text(stmt.get(), 1, kind_name(kind)) && bind_text(stmt.get(), 2, key) &&
           bind_text(stmt.get(), 3, value) && sqlite3_bind_int64(stmt.get(), 4, weight) == SQLITE_OK &&
           bind_text(stmt.get(), 5, display) && sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool record_user_insert(const std::string &user_db_path, DictionaryKind kind, const std::string &key,
                        const std::string &value, std::int64_t weight, const std::string &display)
{
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    auto stmt =
        prepare(db.get(), "INSERT INTO user_dictionary_operations("
                          "dictionary,key,value,operation,weight,display,user_inserted)"
                          " VALUES(?1,?2,?3,'upsert',?4,?5,1)"
                          " ON CONFLICT(dictionary,key,value) DO UPDATE SET operation='upsert',weight=excluded.weight,"
                          " display=excluded.display,user_inserted=1,updated_at=unixepoch()");
    return stmt && bind_text(stmt.get(), 1, kind_name(kind)) && bind_text(stmt.get(), 2, key) &&
           bind_text(stmt.get(), 3, value) && sqlite3_bind_int64(stmt.get(), 4, weight) == SQLITE_OK &&
           bind_text(stmt.get(), 5, display) && sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool record_delete(const std::string &user_db_path, DictionaryKind kind, const std::string &key,
                   const std::string &value)
{
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    auto stmt =
        prepare(db.get(), "INSERT INTO user_dictionary_operations(dictionary,key,value,operation)"
                          " VALUES(?1,?2,?3,'delete')"
                          " ON CONFLICT(dictionary,key,value) DO UPDATE SET operation='delete',weight=0,display='',"
                          " updated_at=unixepoch()");
    return stmt && bind_text(stmt.get(), 1, kind_name(kind)) && bind_text(stmt.get(), 2, key) &&
           bind_text(stmt.get(), 3, value) && sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool is_user_inserted(const std::string &user_db_path, DictionaryKind kind, const std::string &key,
                      const std::string &value)
{
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    auto stmt = prepare(db.get(), "SELECT user_inserted FROM user_dictionary_operations "
                                  "WHERE dictionary=?1 AND key=?2 AND value=?3 LIMIT 1");
    return stmt && bind_text(stmt.get(), 1, kind_name(kind)) && bind_text(stmt.get(), 2, key) &&
           bind_text(stmt.get(), 3, value) && sqlite3_step(stmt.get()) == SQLITE_ROW &&
           sqlite3_column_int(stmt.get(), 0) != 0;
}

bool has_user_upsert_for_key(const std::string &user_db_path, DictionaryKind kind, const std::string &key)
{
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    auto stmt = prepare(db.get(), "SELECT 1 FROM user_dictionary_operations "
                                  "WHERE dictionary=?1 AND key=?2 AND operation='upsert' LIMIT 1");
    return stmt && bind_text(stmt.get(), 1, kind_name(kind)) && bind_text(stmt.get(), 2, key) &&
           sqlite3_step(stmt.get()) == SQLITE_ROW;
}

bool record_pinyin_upsert_from_database(const std::string &main_db_path, const std::string &key,
                                        const std::string &value, const std::string &user_db_path)
{
    auto db = open_database(main_db_path, SQLITE_OPEN_READONLY);
    const std::string table = pinyin_table(key);
    if (!db || table.empty())
        return false;
    auto stmt = prepare(db.get(), "SELECT weight FROM \"" + table + "\" WHERE key=?1 AND value=?2 LIMIT 1");
    if (!stmt || !bind_text(stmt.get(), 1, key) || !bind_text(stmt.get(), 2, value) ||
        sqlite3_step(stmt.get()) != SQLITE_ROW)
        return false;
    return record_upsert(user_db_path, DictionaryKind::Pinyin, key, value, sqlite3_column_int64(stmt.get(), 0));
}

bool set_fixed_position(const std::string &user_db_path, const std::string &context_key, const std::string &entry_key,
                        const std::string &value, int position)
{
    if (position < 1 || position > 5 || context_key.empty() || entry_key.empty() || value.empty())
        return false;
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    // Checked, not assumed: a short busy timeout means the write lock can be unavailable, and carrying on without a
    // transaction would apply the two statements as separately auto-committed changes.
    if (!execute_sql(db.get(), "BEGIN IMMEDIATE"))
        return false;
    auto clear_slot = prepare(db.get(), "DELETE FROM fixed_candidate_positions WHERE context_key=?1 AND position=?2");
    auto upsert = prepare(
        db.get(), "INSERT INTO fixed_candidate_positions(context_key,entry_key,value,position) VALUES(?1,?2,?3,?4)"
                  " ON CONFLICT(context_key,entry_key,value) DO UPDATE SET position=excluded.position");
    const bool ok = clear_slot && upsert && bind_text(clear_slot.get(), 1, context_key) &&
                    sqlite3_bind_int(clear_slot.get(), 2, position) == SQLITE_OK &&
                    sqlite3_step(clear_slot.get()) == SQLITE_DONE && bind_text(upsert.get(), 1, context_key) &&
                    bind_text(upsert.get(), 2, entry_key) && bind_text(upsert.get(), 3, value) &&
                    sqlite3_bind_int(upsert.get(), 4, position) == SQLITE_OK &&
                    sqlite3_step(upsert.get()) == SQLITE_DONE;
    sqlite3_exec(db.get(), ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);
    return ok;
}

bool adjust_english_candidate_ranking(const std::string &english_db_path, const std::string &user_db_path,
                                      const std::string &context_key, const std::vector<WordItem> &ordered_candidates,
                                      const std::string &entry_key, const std::string &value, const std::string &mode,
                                      int linear_step, int trigger_count, bool force_top, bool *ranking_changed)
{
    if (ranking_changed)
        *ranking_changed = false;
    if (entry_key.empty() || value.empty() || ordered_candidates.empty() ||
        is_fixed(user_db_path, context_key, entry_key, value))
        return false;
    if (!force_top && mode == "disabled")
        return true;

    UserDatabase user_db(user_db_path);
    if (!user_db)
        return false;
    auto journal = prepare_upsert_journal(user_db.get());
    if (!journal)
        return false;
    if (!execute_sql(user_db.get(), "BEGIN IMMEDIATE"))
        return false;
    const auto rollback_user = [&]() { (void)execute_sql(user_db.get(), "ROLLBACK"); };
    trigger_count = (std::max)(1, (std::min)(10, trigger_count));
    if (!force_top)
    {
        auto counter = prepare(
            user_db.get(),
            "INSERT INTO candidate_selection_state(context_key,entry_key,value,selection_count) VALUES(?1,?2,?3,1)"
            " ON CONFLICT(context_key,entry_key,value) DO UPDATE SET selection_count=selection_count+1"
            " RETURNING selection_count");
        if (!counter || !bind_text(counter.get(), 1, context_key) || !bind_text(counter.get(), 2, entry_key) ||
            !bind_text(counter.get(), 3, value) || sqlite3_step(counter.get()) != SQLITE_ROW)
        {
            rollback_user();
            return false;
        }
        if (sqlite3_column_int(counter.get(), 0) < trigger_count)
        {
            counter.reset();
            const bool committed = execute_sql(user_db.get(), "COMMIT");
            if (!committed)
                rollback_user();
            return committed;
        }
    }

    // 只和英文候选比。混输列表里还有中文，它们的权重是另一个词库的量纲：拿「全表最高 + 1000」
    // 当新权重，选一次英文就会压过「在」这种一千多万的高频字，而且权重按词全局生效，之后凡是
    // 前缀能查出它的输入都受影响。英文排在中文的第几位由按输入记的英文槽位决定（english_slot）。
    std::vector<WordItem> english_candidates;
    std::copy_if(ordered_candidates.begin(), ordered_candidates.end(), std::back_inserter(english_candidates),
                 [](const WordItem &item) { return item.source == CandidateSource::EnglishDictionary; });
    const auto selected = std::find_if(english_candidates.begin(), english_candidates.end(), [&](const WordItem &item) {
        return item.pinyin == entry_key && item.word == value;
    });
    if (selected == english_candidates.end())
    {
        rollback_user();
        return false;
    }
    const size_t rank = static_cast<size_t>(selected - english_candidates.begin());
    if (rank == 0)
    {
        const bool committed = execute_sql(user_db.get(), "COMMIT");
        if (!committed)
            rollback_user();
        return committed;
    }
    size_t target = 0;
    if (!force_top && mode == "halve")
        target = rank / 2;
    else if (!force_top && mode == "linear")
        target = rank > static_cast<size_t>((std::max)(1, linear_step))
                     ? rank - static_cast<size_t>((std::max)(1, linear_step))
                     : 0;
    else if (!force_top && mode == "promote")
        target = rank > 4 ? 4 : rank - 1;
    const std::int64_t maximum_weight =
        std::accumulate(english_candidates.begin(), english_candidates.end(), std::int64_t{0},
                        [](std::int64_t maximum, const WordItem &item) { return (std::max)(maximum, item.weight); });
    const std::int64_t new_weight =
        target == 0 ? (std::max)(maximum_weight, selected->weight) + 1000 : english_candidates[target - 1].weight + 1;

    auto english_db = open_database(english_db_path, SQLITE_OPEN_READWRITE);
    auto update = english_db
                      ? prepare(english_db.get(), "UPDATE english_words SET weight=?1 WHERE word=?2 AND display=?3")
                      : Stmt{};
    auto existing =
        english_db ? prepare(english_db.get(), "SELECT 1 FROM english_words WHERE word=?1 AND display=?2") : Stmt{};
    // Journal the new weight before english.db carries it. The journal upsert is idempotent and replay reapplies it, so
    // a journal row one step ahead of the dictionary is recoverable, while a boosted weight that never reached the
    // journal is silently reverted at the next dictionary upgrade. The row must already exist, otherwise replay would
    // insert a word the user never learned.
    if (!update || !existing || !bind_text(existing.get(), 1, entry_key) || !bind_text(existing.get(), 2, value) ||
        sqlite3_step(existing.get()) != SQLITE_ROW ||
        !write_upsert_journal(journal.get(), DictionaryKind::English, entry_key, value, new_weight, value))
    {
        rollback_user();
        return false;
    }
    existing.reset();
    auto reset = prepare(user_db.get(),
                         "DELETE FROM candidate_selection_state WHERE context_key=?1 AND entry_key=?2 AND value=?3");
    const bool reset_ok = reset && bind_text(reset.get(), 1, context_key) && bind_text(reset.get(), 2, entry_key) &&
                          bind_text(reset.get(), 3, value) && sqlite3_step(reset.get()) == SQLITE_DONE;
    reset.reset();
    if (!reset_ok || !execute_sql(user_db.get(), "COMMIT"))
    {
        rollback_user();
        return false;
    }
    const bool ok = sqlite3_bind_int64(update.get(), 1, new_weight) == SQLITE_OK &&
                    bind_text(update.get(), 2, entry_key) && bind_text(update.get(), 3, value) &&
                    sqlite3_step(update.get()) == SQLITE_DONE && sqlite3_changes(english_db.get()) > 0;
    if (ok && ranking_changed)
        *ranking_changed = true;
    return ok;
}

bool delete_dictionary_candidate(const std::string &dictionary_db_path, const std::string &user_db_path,
                                 DictionaryKind kind, const std::string &entry_key, const std::string &value)
{
    if (entry_key.empty() || value.empty())
        return false;
    std::string table;
    switch (kind)
    {
    case DictionaryKind::Pinyin:
        table = pinyin_table(entry_key);
        break;
    case DictionaryKind::Wubi:
        table = "wubi86";
        break;
    case DictionaryKind::English:
        table = "english_words";
        break;
    default:
        return false;
    }
    if (table.empty())
        return false;
    // Initialize the journal before attaching it; every operation owns its connections.
    if (!ensure_user_database(user_db_path))
        return false;
    auto database = open_database(dictionary_db_path, SQLITE_OPEN_READWRITE);
    auto attach = database ? prepare(database.get(), "ATTACH DATABASE ?1 AS candidate_journal") : Stmt{};
    if (!attach || !bind_text(attach.get(), 1, user_db_path) || sqlite3_step(attach.get()) != SQLITE_DONE)
        return false;
    attach.reset();
    if (!execute_sql(database.get(), "BEGIN IMMEDIATE"))
        return false;
    const auto rollback = [&]() {
        (void)execute_sql(database.get(), "ROLLBACK");
        return false;
    };
    const std::string columns = kind == DictionaryKind::English ? "word=?1 AND display=?2" : "key=?1 AND value=?2";
    auto remove = prepare(database.get(), "DELETE FROM main.\"" + table + "\" WHERE " + columns);
    if (!remove || !bind_text(remove.get(), 1, entry_key) || !bind_text(remove.get(), 2, value) ||
        sqlite3_step(remove.get()) != SQLITE_DONE || sqlite3_changes(database.get()) == 0)
        return rollback();
    remove.reset();
    auto journal = prepare(database.get(),
                           "INSERT INTO candidate_journal.user_dictionary_operations(dictionary,key,value,operation)"
                           " VALUES(?1,?2,?3,'delete')"
                           " ON CONFLICT(dictionary,key,value) DO UPDATE SET operation='delete',weight=0,display='',"
                           " updated_at=unixepoch()");
    if (!journal || !bind_text(journal.get(), 1, kind_name(kind)) || !bind_text(journal.get(), 2, entry_key) ||
        !bind_text(journal.get(), 3, value) || sqlite3_step(journal.get()) != SQLITE_DONE)
        return rollback();
    journal.reset();
    if (!execute_sql(database.get(), "COMMIT"))
        return rollback();
    return true;
}

bool delete_english_candidate(const std::string &english_db_path, const std::string &user_db_path,
                              const std::string &entry_key, const std::string &value)
{
    return delete_dictionary_candidate(english_db_path, user_db_path, DictionaryKind::English, entry_key, value);
}

bool bump_wubi_weight(const std::string &main_db_path, const std::string &user_db_path, const std::string &key,
                      const std::string &value)
{
    if (key.empty() || value.empty())
        return false;
    // Initialize the journal before attaching it; every operation owns its connections.
    if (!ensure_user_database(user_db_path))
        return false;
    auto database = open_database(main_db_path, SQLITE_OPEN_READWRITE);
    auto attach = database ? prepare(database.get(), "ATTACH DATABASE ?1 AS candidate_journal") : Stmt{};
    if (!attach || !bind_text(attach.get(), 1, user_db_path) || sqlite3_step(attach.get()) != SQLITE_DONE)
        return false;
    attach.reset();
    if (!execute_sql(database.get(), "BEGIN IMMEDIATE"))
        return false;
    const auto rollback = [&]() {
        (void)execute_sql(database.get(), "ROLLBACK");
        return false;
    };
    // 增量语义对齐全拼侧 build_sql_for_updating_word：新权重 = 同码组当前最高权重 + 1，
    // 选中候选升到组内首位，而不是固定 +1（同码组有更高权重行时固定 +1 不会改变排序）。
    auto current = prepare(database.get(), "SELECT MAX(weight) FROM main.\"wubi86\" WHERE \"key\"=?1");
    if (!current || !bind_text(current.get(), 1, key) || sqlite3_step(current.get()) != SQLITE_ROW)
        return rollback();
    const std::int64_t new_weight = clamp_managed_weight(sqlite3_column_int64(current.get(), 0) + 1);
    current.reset();
    auto bump = prepare(database.get(), "UPDATE main.\"wubi86\" SET weight=?1 WHERE \"key\"=?2 AND \"value\"=?3");
    // changes()==0 表示目标行不存在：与 update_wubi_weight 一致，不造行。
    if (!bump || sqlite3_bind_int64(bump.get(), 1, new_weight) != SQLITE_OK || !bind_text(bump.get(), 2, key) ||
        !bind_text(bump.get(), 3, value) || sqlite3_step(bump.get()) != SQLITE_DONE ||
        sqlite3_changes(database.get()) == 0)
        return rollback();
    bump.reset();
    auto journal =
        prepare(database.get(), "INSERT INTO candidate_journal.user_dictionary_operations(dictionary,key,value,"
                                "operation,weight,display) VALUES(?1,?2,?3,'upsert',?4,'')"
                                " ON CONFLICT(dictionary,key,value) DO UPDATE SET operation='upsert',"
                                "weight=excluded.weight,display='',updated_at=unixepoch()");
    if (!journal || !bind_text(journal.get(), 1, kind_name(DictionaryKind::Wubi)) ||
        !bind_text(journal.get(), 2, key) || !bind_text(journal.get(), 3, value) ||
        sqlite3_bind_int64(journal.get(), 4, new_weight) != SQLITE_OK || sqlite3_step(journal.get()) != SQLITE_DONE)
        return rollback();
    journal.reset();
    if (!execute_sql(database.get(), "COMMIT"))
        return rollback();
    return true;
}

namespace
{
// 快捷短语组的选择计数借用 candidate_selection_state，context 按编码区分，promote / bypass
// 两个方向各数各的。value 写编码本身：状态导出要求它非空。
const std::string kQuickPhraseSelectionContext = "quick_phrase:";

bool read_quick_phrase_slot(sqlite3 *db, const std::string &schema, const std::string &code, int &slot)
{
    auto stmt = prepare(db, "SELECT slot FROM " + schema + "quick_phrase_slots WHERE code=?1");
    if (!stmt || !bind_text(stmt.get(), 1, code))
        return false;
    const int status = sqlite3_step(stmt.get());
    if (status == SQLITE_ROW)
    {
        slot = (std::max)(0, sqlite3_column_int(stmt.get(), 0));
        return true;
    }
    slot = 0;
    return status == SQLITE_DONE;
}

bool write_quick_phrase_slot(sqlite3 *db, const std::string &schema, const std::string &code, int slot)
{
    auto stmt = slot <= 0 ? prepare(db, "DELETE FROM " + schema + "quick_phrase_slots WHERE code=?1")
                          : prepare(db, "INSERT INTO " + schema +
                                            "quick_phrase_slots(code,slot) VALUES(?1,?2)"
                                            " ON CONFLICT(code) DO UPDATE SET slot=excluded.slot");
    return stmt && bind_text(stmt.get(), 1, code) &&
           (slot <= 0 || sqlite3_bind_int(stmt.get(), 2, slot) == SQLITE_OK) && sqlite3_step(stmt.get()) == SQLITE_DONE;
}

// 计一次数；到 trigger_count 时置 reached 并清零，计数不会越过状态导出允许的上限。
// context_prefix 区分快捷短语组和英文槽位，两者的计数互不干扰。
bool count_slot_selection(sqlite3 *db, const std::string &schema, const std::string &context_prefix,
                          const std::string &code, const char *direction, int trigger_count, bool &reached)
{
    reached = false;
    const std::string context = context_prefix + code;
    {
        auto counter = prepare(db, "INSERT INTO " + schema +
                                       "candidate_selection_state(context_key,entry_key,value,selection_count)"
                                       " VALUES(?1,?2,?3,1) ON CONFLICT(context_key,entry_key,value)"
                                       " DO UPDATE SET selection_count=selection_count+1 RETURNING selection_count");
        if (!counter || !bind_text(counter.get(), 1, context) || !bind_text(counter.get(), 2, direction) ||
            !bind_text(counter.get(), 3, code) || sqlite3_step(counter.get()) != SQLITE_ROW)
            return false;
        reached = sqlite3_column_int(counter.get(), 0) >= (std::max)(1, (std::min)(10, trigger_count));
    }
    if (!reached)
        return true;
    auto reset = prepare(db, "DELETE FROM " + schema +
                                 "candidate_selection_state WHERE context_key=?1 AND entry_key=?2 AND value=?3");
    return reset && bind_text(reset.get(), 1, context) && bind_text(reset.get(), 2, direction) &&
           bind_text(reset.get(), 3, code) && sqlite3_step(reset.get()) == SQLITE_DONE;
}
} // namespace

int quick_phrase_slot(const std::string &user_db_path, const std::string &code)
{
    if (code.empty())
        return 0;
    UserDatabase db(user_db_path);
    int slot = 0;
    if (!db || !read_quick_phrase_slot(db.get(), "", code, slot))
        return 0;
    return slot;
}

bool learn_quick_phrase_selection(const std::string &main_db_path, const std::string &user_db_path,
                                  const std::string &code, const std::string &value, bool first_in_group,
                                  const std::string &mode, int linear_step, int trigger_count)
{
    if (code.empty() || value.empty())
        return false;
    if (mode == "disabled")
        return true;
    // Initialize the journal before attaching it; every operation owns its connections.
    if (!ensure_user_database(user_db_path))
        return false;
    auto database = open_database(main_db_path, SQLITE_OPEN_READWRITE);
    auto attach = database ? prepare(database.get(), "ATTACH DATABASE ?1 AS candidate_journal") : Stmt{};
    if (!attach || !bind_text(attach.get(), 1, user_db_path) || sqlite3_step(attach.get()) != SQLITE_DONE)
        return false;
    attach.reset();
    if (!execute_sql(database.get(), "BEGIN IMMEDIATE"))
        return false;
    const auto rollback = [&]() {
        (void)execute_sql(database.get(), "ROLLBACK");
        return false;
    };
    const auto commit = [&]() { return execute_sql(database.get(), "COMMIT") || rollback(); };
    const std::string schema = "candidate_journal.";
    int slot = 0;
    if (!read_quick_phrase_slot(database.get(), schema, code, slot))
        return rollback();
    // 组在首位、选的又是组内第一条：这就是默认上屏的那一个，没有可学的。
    if (slot == 0 && first_in_group)
        return commit();
    bool reached = false;
    if (!count_slot_selection(database.get(), schema, kQuickPhraseSelectionContext, code, "promote", trigger_count,
                              reached))
        return rollback();
    if (!reached)
        return commit();
    if (slot > 0 && !write_quick_phrase_slot(database.get(), schema, code,
                                             static_cast<int>(ranking_target(static_cast<size_t>(slot), mode,
                                                                             (std::max)(1, linear_step), false))))
        return rollback();
    if (!first_in_group)
    {
        // 组内顺序按权重走，语义同 bump_wubi_weight：选中的那条升到同码最高 + 1。
        auto current = prepare(database.get(), "SELECT MAX(weight) FROM main.quick_parases WHERE key=?1");
        if (!current || !bind_text(current.get(), 1, code) || sqlite3_step(current.get()) != SQLITE_ROW)
            return rollback();
        const std::int64_t new_weight = clamp_managed_weight(sqlite3_column_int64(current.get(), 0) + 1);
        current.reset();
        auto bump = prepare(database.get(), "UPDATE main.quick_parases SET weight=?1 WHERE key=?2 AND value=?3");
        if (!bump || sqlite3_bind_int64(bump.get(), 1, new_weight) != SQLITE_OK || !bind_text(bump.get(), 2, code) ||
            !bind_text(bump.get(), 3, value) || sqlite3_step(bump.get()) != SQLITE_DONE ||
            sqlite3_changes(database.get()) == 0)
            return rollback();
        bump.reset();
        auto journal =
            prepare(database.get(), "INSERT INTO candidate_journal.user_dictionary_operations(dictionary,key,value,"
                                    "operation,weight,display) VALUES(?1,?2,?3,'upsert',?4,'')"
                                    " ON CONFLICT(dictionary,key,value) DO UPDATE SET operation='upsert',"
                                    "weight=excluded.weight,display='',updated_at=unixepoch()");
        if (!journal || !bind_text(journal.get(), 1, kind_name(DictionaryKind::QuickPhrase)) ||
            !bind_text(journal.get(), 2, code) || !bind_text(journal.get(), 3, value) ||
            sqlite3_bind_int64(journal.get(), 4, new_weight) != SQLITE_OK || sqlite3_step(journal.get()) != SQLITE_DONE)
            return rollback();
    }
    return commit();
}

bool learn_quick_phrase_bypass(const std::string &user_db_path, const std::string &code, int ordinary_rank,
                               const std::string &mode, int linear_step, int trigger_count, int max_slot)
{
    if (code.empty() || ordinary_rank < 0)
        return false;
    if (mode == "disabled" || max_slot <= 0)
        return true;
    UserDatabase db(user_db_path);
    if (!db || !execute_sql(db.get(), "BEGIN IMMEDIATE"))
        return false;
    const auto rollback = [&]() {
        (void)execute_sql(db.get(), "ROLLBACK");
        return false;
    };
    const auto commit = [&]() { return execute_sql(db.get(), "COMMIT") || rollback(); };
    int slot = 0;
    if (!read_quick_phrase_slot(db.get(), "", code, slot))
        return rollback();
    // 选中的候选本来就在组前面，或者组已经退到首页最后一位。
    if (ordinary_rank < slot || slot >= max_slot)
        return commit();
    // 整组算作它前面的一个位置：它的目标位置落在组的位置或更前，才算越过了这一组。
    const size_t target =
        ranking_target(static_cast<size_t>(ordinary_rank) + 1, mode, (std::max)(1, linear_step), false);
    if (target > static_cast<size_t>(slot))
        return commit();
    bool reached = false;
    if (!count_slot_selection(db.get(), "", kQuickPhraseSelectionContext, code, "bypass", trigger_count, reached))
        return rollback();
    if (reached && !write_quick_phrase_slot(db.get(), "", code, (std::min)(slot + 1, max_slot)))
        return rollback();
    return commit();
}

namespace
{
const std::string kEnglishSlotSelectionContext = "english_slot:";

bool write_english_slot(sqlite3 *db, const std::string &code, int slot)
{
    auto stmt = prepare(db, "INSERT INTO english_slots(code,slot) VALUES(?1,?2)"
                            " ON CONFLICT(code) DO UPDATE SET slot=excluded.slot");
    return stmt && bind_text(stmt.get(), 1, code) &&
           sqlite3_bind_int(stmt.get(), 2, (std::max)(0, slot)) == SQLITE_OK && sqlite3_step(stmt.get()) == SQLITE_DONE;
}

// 选择计数与写槽位放在一个事务里；write_slot 只在计数到 trigger_count 时调用。
template <typename WriteSlot>
bool count_and_write_english_slot(const std::string &user_db_path, const std::string &code, const char *direction,
                                  int trigger_count, WriteSlot write_slot)
{
    UserDatabase db(user_db_path);
    if (!db || !execute_sql(db.get(), "BEGIN IMMEDIATE"))
        return false;
    const auto rollback = [&]() {
        (void)execute_sql(db.get(), "ROLLBACK");
        return false;
    };
    bool reached = false;
    if (!count_slot_selection(db.get(), "", kEnglishSlotSelectionContext, code, direction, trigger_count, reached))
        return rollback();
    if (reached && !write_slot(db.get()))
        return rollback();
    return execute_sql(db.get(), "COMMIT") || rollback();
}
} // namespace

std::optional<int> english_slot(const std::string &user_db_path, const std::string &code)
{
    if (code.empty())
        return std::nullopt;
    UserDatabase db(user_db_path);
    auto stmt = db ? prepare(db.get(), "SELECT slot FROM english_slots WHERE code=?1") : Stmt{};
    if (!stmt || !bind_text(stmt.get(), 1, code) || sqlite3_step(stmt.get()) != SQLITE_ROW)
        return std::nullopt;
    return (std::max)(0, sqlite3_column_int(stmt.get(), 0));
}

bool set_english_slot(const std::string &user_db_path, const std::string &code, int slot)
{
    if (code.empty())
        return false;
    UserDatabase db(user_db_path);
    return db && write_english_slot(db.get(), code, slot);
}

bool learn_english_slot_selection(const std::string &user_db_path, const std::string &code, int english_index,
                                  const std::string &mode, int linear_step, int trigger_count)
{
    if (code.empty() || english_index < 0)
        return false;
    // 已经在首位就是默认上屏的那一个，没有可学的。
    if (mode == "disabled" || english_index == 0)
        return true;
    // 和中文调频同一套名次规则：按它此刻在列表里的名次算目标位置。
    const int target =
        static_cast<int>(ranking_target(static_cast<size_t>(english_index), mode, (std::max)(1, linear_step), false));
    return count_and_write_english_slot(user_db_path, code, "promote", trigger_count,
                                        [&](sqlite3 *db) { return write_english_slot(db, code, target); });
}

bool learn_english_slot_bypass(const std::string &user_db_path, const std::string &code, int english_index,
                               int selected_index, const std::string &mode, int linear_step, int trigger_count,
                               int max_slot)
{
    if (code.empty() || english_index < 0 || selected_index <= english_index)
        return false;
    if (mode == "disabled" || english_index >= max_slot)
        return true;
    // 和快捷短语组同一个判据：选中的候选按调频模式的目标位置落在英文的位置或更前，才算越过了它。
    const size_t target = ranking_target(static_cast<size_t>(selected_index), mode, (std::max)(1, linear_step), false);
    if (target > static_cast<size_t>(english_index))
        return true;
    return count_and_write_english_slot(user_db_path, code, "bypass", trigger_count, [&](sqlite3 *db) {
        return write_english_slot(db, code, (std::min)(english_index + 1, max_slot));
    });
}

namespace
{
const std::string kDateTimeSelectionContext = "date_time_order:";

// 学到的顺序合进出厂顺序：表里记过的按记下的先后排，没记过的（后来新增的格式）插回它出厂时的下标。
// 表里有、default_ids 里没有的（已经删掉的格式）直接丢掉。
bool read_date_time_order(sqlite3 *db, const std::string &category, const std::vector<std::string> &default_ids,
                          std::vector<std::string> &order)
{
    order.clear();
    auto stmt = prepare(db, "SELECT format FROM date_time_format_order WHERE category=?1 ORDER BY position");
    if (!stmt || !bind_text(stmt.get(), 1, category))
        return false;
    int status;
    while ((status = sqlite3_step(stmt.get())) == SQLITE_ROW)
    {
        const unsigned char *format = sqlite3_column_text(stmt.get(), 0);
        if (format == nullptr)
            continue;
        std::string id(reinterpret_cast<const char *>(format));
        if (std::find(default_ids.begin(), default_ids.end(), id) != default_ids.end() &&
            std::find(order.begin(), order.end(), id) == order.end())
            order.push_back(std::move(id));
    }
    if (status != SQLITE_DONE)
        return false;
    for (size_t index = 0; index < default_ids.size(); ++index)
    {
        if (std::find(order.begin(), order.end(), default_ids[index]) == order.end())
            order.insert(order.begin() + static_cast<std::ptrdiff_t>((std::min)(index, order.size())),
                         default_ids[index]);
    }
    return true;
}

bool write_date_time_order(sqlite3 *db, const std::string &category, const std::vector<std::string> &order)
{
    auto clear = prepare(db, "DELETE FROM date_time_format_order WHERE category=?1");
    if (!clear || !bind_text(clear.get(), 1, category) || sqlite3_step(clear.get()) != SQLITE_DONE)
        return false;
    auto insert = prepare(db, "INSERT INTO date_time_format_order(category,format,position) VALUES(?1,?2,?3)");
    if (!insert)
        return false;
    for (size_t index = 0; index < order.size(); ++index)
    {
        sqlite3_reset(insert.get());
        if (!bind_text(insert.get(), 1, category) || !bind_text(insert.get(), 2, order[index]) ||
            sqlite3_bind_int(insert.get(), 3, static_cast<int>(index)) != SQLITE_OK ||
            sqlite3_step(insert.get()) != SQLITE_DONE)
            return false;
    }
    return true;
}

// force_top 置顶：不计数，直接挪到首位。否则按调频模式从它此刻的名次算目标位置，计数到 trigger_count 才挪。
bool move_date_time_format(const std::string &user_db_path, const std::string &category,
                           const std::vector<std::string> &default_ids, const std::string &format_id,
                           const std::string &mode, int linear_step, int trigger_count, bool force_top)
{
    if (category.empty() || format_id.empty())
        return false;
    if (!force_top && mode == "disabled")
        return true;
    UserDatabase db(user_db_path);
    if (!db || !execute_sql(db.get(), "BEGIN IMMEDIATE"))
        return false;
    const auto rollback = [&]() {
        (void)execute_sql(db.get(), "ROLLBACK");
        return false;
    };
    const auto commit = [&]() { return execute_sql(db.get(), "COMMIT") || rollback(); };
    std::vector<std::string> order;
    if (!read_date_time_order(db.get(), category, default_ids, order))
        return rollback();
    const auto selected = std::find(order.begin(), order.end(), format_id);
    // 已经在首位就是默认上屏的那一个，没有可学的。
    if (selected == order.end() || selected == order.begin())
        return commit();
    const size_t rank = static_cast<size_t>(selected - order.begin());
    if (!force_top)
    {
        bool reached = false;
        if (!count_slot_selection(db.get(), "", kDateTimeSelectionContext, format_id, "promote", trigger_count,
                                  reached))
            return rollback();
        if (!reached)
            return commit();
    }
    const size_t target = ranking_target(rank, mode, (std::max)(1, linear_step), force_top);
    std::string moved = std::move(order[rank]);
    order.erase(order.begin() + static_cast<std::ptrdiff_t>(rank));
    order.insert(order.begin() + static_cast<std::ptrdiff_t>((std::min)(target, order.size())), std::move(moved));
    if (!write_date_time_order(db.get(), category, order))
        return rollback();
    return commit();
}
} // namespace

std::string date_time_fixed_position_context(const std::string &category)
{
    return "date_time:" + category;
}

void apply_date_time_order(const std::string &user_db_path, const std::string &category,
                           std::vector<WordItem> &candidates, bool learned_order)
{
    if (category.empty() || candidates.empty())
        return;
    UserDatabase db(user_db_path);
    if (!db)
        return;
    std::vector<std::string> default_ids;
    default_ids.reserve(candidates.size());
    for (const auto &candidate : candidates)
        default_ids.push_back(candidate.pinyin);
    std::vector<std::string> order;
    if (learned_order && read_date_time_order(db.get(), category, default_ids, order))
    {
        const auto rank = [&](const WordItem &item) {
            return std::find(order.begin(), order.end(), item.pinyin) - order.begin();
        };
        std::stable_sort(candidates.begin(), candidates.end(),
                         [&](const WordItem &left, const WordItem &right) { return rank(left) < rank(right); });
    }

    // 固定位置借用 fixed_candidate_positions，entry_key 和 value 都写格式 ID：apply_fixed_positions
    // 按文字认候选，日期文本每天都变，所以这里按格式 ID 自己摆。
    auto fixed = prepare(db.get(), "SELECT entry_key,position FROM fixed_candidate_positions WHERE context_key=?1"
                                   " ORDER BY position");
    if (!fixed || !bind_text(fixed.get(), 1, date_time_fixed_position_context(category)))
        return;
    std::vector<WordItem> rows;
    while (sqlite3_step(fixed.get()) == SQLITE_ROW)
    {
        const unsigned char *key = sqlite3_column_text(fixed.get(), 0);
        if (key == nullptr)
            continue;
        const std::string format_id(reinterpret_cast<const char *>(key));
        const auto existing = std::find_if(candidates.begin(), candidates.end(),
                                           [&](const WordItem &item) { return item.pinyin == format_id; });
        if (existing == candidates.end())
            continue;
        WordItem item = std::move(*existing);
        candidates.erase(existing);
        item.fixed_position = sqlite3_column_int(fixed.get(), 1);
        rows.push_back(std::move(item));
    }
    for (auto &row : rows)
    {
        const size_t index = (std::min)(static_cast<size_t>((std::max)(row.fixed_position, 1) - 1), candidates.size());
        candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(index), std::move(row));
    }
}

bool learn_date_time_selection(const std::string &user_db_path, const std::string &category,
                               const std::vector<std::string> &default_ids, const std::string &format_id,
                               const std::string &mode, int linear_step, int trigger_count)
{
    return move_date_time_format(user_db_path, category, default_ids, format_id, mode, linear_step, trigger_count,
                                 false);
}

bool pin_date_time_format(const std::string &user_db_path, const std::string &category,
                          const std::vector<std::string> &default_ids, const std::string &format_id)
{
    return move_date_time_format(user_db_path, category, default_ids, format_id, "pin", 1, 1, true);
}

bool learn_entered_english_word(const std::string &english_db_path, const std::string &user_db_path,
                                const std::string &display, std::int64_t weight)
{
    constexpr size_t kMaximumLearnedEnglishWordLength = 64;
    if (display.empty() || display.size() > kMaximumLearnedEnglishWordLength ||
        !std::all_of(display.begin(), display.end(),
                     [](unsigned char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }))
        return false;

    std::string word = display;
    std::transform(word.begin(), word.end(), word.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    weight = (std::max)(std::int64_t{0}, weight);
    if (!EnglishDictionary::ensure_schema(english_db_path))
        return false;

    auto database = open_database(english_db_path, SQLITE_OPEN_READWRITE);
    auto insert =
        database ? prepare(database.get(), "INSERT OR IGNORE INTO english_words(word,display,weight) VALUES(?1,?2,?3)")
                 : Stmt{};
    if (!insert || !bind_text(insert.get(), 1, word) || !bind_text(insert.get(), 2, display) ||
        sqlite3_bind_int64(insert.get(), 3, weight) != SQLITE_OK || sqlite3_step(insert.get()) != SQLITE_DONE)
        return false;
    if (sqlite3_changes(database.get()) == 0)
        return true;

    if (record_user_insert(user_db_path, DictionaryKind::English, word, display, weight, display))
        return true;

    // Do not leave an entry that cannot survive a dictionary upgrade.
    auto rollback_insert = prepare(database.get(), "DELETE FROM english_words WHERE word=?1 AND display=?2");
    if (rollback_insert && bind_text(rollback_insert.get(), 1, word) && bind_text(rollback_insert.get(), 2, display))
        (void)sqlite3_step(rollback_insert.get());
    return false;
}

bool clear_fixed_position(const std::string &user_db_path, const std::string &context_key, const std::string &entry_key,
                          const std::string &value)
{
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    auto stmt =
        prepare(db.get(), "DELETE FROM fixed_candidate_positions WHERE context_key=?1 AND entry_key=?2 AND value=?3");
    return stmt && bind_text(stmt.get(), 1, context_key) && bind_text(stmt.get(), 2, entry_key) &&
           bind_text(stmt.get(), 3, value) && sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool is_fixed(const std::string &user_db_path, const std::string &context_key, const std::string &entry_key,
              const std::string &value)
{
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    auto stmt =
        prepare(db.get(), "SELECT 1 FROM fixed_candidate_positions WHERE context_key=?1 AND entry_key=?2 AND value=?3");
    return stmt && bind_text(stmt.get(), 1, context_key) && bind_text(stmt.get(), 2, entry_key) &&
           bind_text(stmt.get(), 3, value) && sqlite3_step(stmt.get()) == SQLITE_ROW;
}

void apply_fixed_positions(
    const std::string &user_db_path, const std::string &context_key, std::vector<WordItem> &candidates,
    bool include_missing,
    const std::function<std::optional<WordItem>(const std::string &, const std::string &)> &find_candidate,
    bool keep_dynamic_candidate_positions)
{
    if (context_key.empty() || candidates.empty())
        return;
    UserDatabase user_db(user_db_path);
    if (!user_db)
        return;
    auto fixed = prepare(
        user_db.get(),
        "SELECT entry_key,value,position FROM fixed_candidate_positions WHERE context_key=?1 ORDER BY position");
    if (!fixed || !bind_text(fixed.get(), 1, context_key))
        return;
    std::vector<WordItem> dynamic_candidates;
    if (!keep_dynamic_candidate_positions)
    {
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                        [&](const WordItem &item) {
                                            if (item.source != CandidateSource::CloudSuggestion &&
                                                item.source != CandidateSource::AiSuggestion)
                                                return false;
                                            dynamic_candidates.push_back(item);
                                            return true;
                                        }),
                         candidates.end());
    }

    struct Fixed
    {
        WordItem item;
        int position;
    };
    std::vector<Fixed> rows;
    while (sqlite3_step(fixed.get()) == SQLITE_ROW)
    {
        const std::string key = reinterpret_cast<const char *>(sqlite3_column_text(fixed.get(), 0));
        const std::string value = reinterpret_cast<const char *>(sqlite3_column_text(fixed.get(), 1));
        const int position = sqlite3_column_int(fixed.get(), 2);
        const auto existing = std::find_if(candidates.begin(), candidates.end(),
                                           [&](const WordItem &item) { return item.word == value; });
        if (existing != candidates.end())
        {
            WordItem item = *existing;
            item.fixed_position = position;
            rows.push_back({std::move(item), position});
        }
        else if (include_missing && find_candidate)
        {
            if (auto found = find_candidate(key, value))
            {
                WordItem item = std::move(*found);
                item.fixed_position = position;
                rows.push_back({std::move(item), position});
            }
        }
    }
    for (const auto &row : rows)
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                        [&](const WordItem &item) { return item.word == row.item.word; }),
                         candidates.end());
    for (const auto &row : rows)
    {
        const size_t index = (std::min)(static_cast<size_t>(row.position - 1), candidates.size());
        candidates.insert(candidates.begin() + index, row.item);
    }
    for (const auto &item : dynamic_candidates)
    {
        const size_t preferred_index = item.source == CandidateSource::CloudSuggestion ? 1 : 2;
        const size_t index = (std::min)(preferred_index, candidates.size());
        candidates.insert(candidates.begin() + index, item);
    }
}

bool adjust_candidate_ranking(const std::string &main_db_path, const std::string &user_db_path,
                              const std::string &context_key, const std::vector<WordItem> &ordered_candidates,
                              const std::string &entry_key, const std::string &value, const std::string &mode,
                              int linear_step, int trigger_count, bool force_top, bool *ranking_changed,
                              DictionaryKind kind)
{
    if (ranking_changed)
        *ranking_changed = false;
    if (entry_key.empty() || value.empty() || ordered_candidates.empty() ||
        is_fixed(user_db_path, context_key, entry_key, value))
        return false;
    if (!force_top && mode == "disabled")
        return true;
    UserDatabase user_db(user_db_path);
    if (!user_db)
        return false;
    auto journal_upsert = prepare_upsert_journal(user_db.get());
    if (!journal_upsert)
        return false;
    if (!execute_sql(user_db.get(), "BEGIN IMMEDIATE"))
        return false;
    const auto rollback_user = [&]() { (void)execute_sql(user_db.get(), "ROLLBACK"); };
    trigger_count = (std::max)(1, (std::min)(10, trigger_count));
    if (!force_top)
    {
        auto counter = prepare(
            user_db.get(),
            "INSERT INTO candidate_selection_state(context_key,entry_key,value,selection_count) VALUES(?1,?2,?3,1)"
            " ON CONFLICT(context_key,entry_key,value) DO UPDATE SET selection_count=selection_count+1"
            " RETURNING selection_count");
        if (!counter || !bind_text(counter.get(), 1, context_key) || !bind_text(counter.get(), 2, entry_key) ||
            !bind_text(counter.get(), 3, value) || sqlite3_step(counter.get()) != SQLITE_ROW)
        {
            rollback_user();
            return false;
        }
        if (sqlite3_column_int(counter.get(), 0) < trigger_count)
        {
            counter.reset();
            const bool committed = execute_sql(user_db.get(), "COMMIT");
            if (!committed)
                rollback_user();
            return committed;
        }
    }

    // One list mixes several entry keys: single-letter and jianpin contexts show
    // 一/yi next to 有/you, and a re-segmentation shows 吉安 (ji'an) inside the jian list. All of them have to
    // share one scale, because the user's 调频 is "move this above what I can see".
    //
    // The midpoint math below assumes this vector is sorted by weight, descending:
    // it brackets the target position between its neighbours' weights and splits
    // the gap. The displayed order does NOT satisfy that -- an alternative
    // segmentation takes a protected slot near the top and pinned candidates are
    // hoisted to fixed positions, both regardless of weight. Ranking on display
    // order is what wrote 写 as 506 (= 西鄂's weight 6 + 500) in #400. Sorting by
    // weight fixes that at its source, so no weight basis can ever come from a row
    // sitting above its own weight.
    //
    // #400 instead restricted the set to keys with the same syllable count. That
    // also kept 西鄂 out, but it walled every candidate into its own group: 吉安
    // (ji'an, w=1) could only ever be compared against 积案/几案/急案/即按, so its
    // learnable weight was capped around 10k and it could never pass 见 (jian,
    // w=3460998) no matter how many times the user picked it. Weight order removes
    // the wall without bringing the bug back. Writes still go to entry_key rows
    // only, so a rebalance cannot land on another key's row the way it did in #36.
    std::vector<WordItem> database_candidates;
    for (const auto &item : ordered_candidates)
    {
        if (item.source != CandidateSource::Database && item.source != CandidateSource::UserDatabase)
            continue;
        database_candidates.push_back(item);
    }
    std::stable_sort(database_candidates.begin(), database_candidates.end(),
                     [](const WordItem &lhs, const WordItem &rhs) { return lhs.weight > rhs.weight; });
    std::vector<bool> owns_entry_key;
    owns_entry_key.reserve(database_candidates.size());
    for (const auto &item : database_candidates)
    {
        const std::string item_key =
            kind == DictionaryKind::Wubi ? item.pinyin : candidate_dictionary_key(item, context_key);
        owns_entry_key.push_back(item_key == entry_key);
    }
    const auto selected = std::find_if(database_candidates.begin(), database_candidates.end(),
                                       [&](const WordItem &item) { return item.word == value; });
    if (selected == database_candidates.end())
    {
        rollback_user();
        return false;
    }
    const size_t rank = static_cast<size_t>(selected - database_candidates.begin());
    if (rank == 0)
    {
        auto reset = prepare(
            user_db.get(), "DELETE FROM candidate_selection_state WHERE context_key=?1 AND entry_key=?2 AND value=?3");
        const bool reset_ok = reset && bind_text(reset.get(), 1, context_key) && bind_text(reset.get(), 2, entry_key) &&
                              bind_text(reset.get(), 3, value) && sqlite3_step(reset.get()) == SQLITE_DONE;
        reset.reset();
        const bool committed = reset_ok && execute_sql(user_db.get(), "COMMIT");
        if (!committed)
            rollback_user();
        return committed;
    }
    const size_t target = ranking_target(rank, mode, linear_step, force_top);
    // The weight arithmetic below is pure computation over the candidate list already in memory, so it runs before the
    // main database's write lock is taken. It used to sit inside the transaction, which held the lock across the whole
    // sort and midpoint search and made every concurrent read wait for work that never touched the disk.
    // Keep learned weights in the same general range as the shipped dictionary
    // (currently below 77 million). Large spacing is unnecessary and previously
    // caused local rebalances to create values around 10^12. A later bug used a
    // tiny `upper` (weight=1 rare words) as the staircase base and wrote
    // negative weights onto neighboring keys from query_series().
    std::int64_t new_weight = 0;
    const bool top_near_limit =
        target == 0 && database_candidates[0].weight > (std::numeric_limits<std::int64_t>::max)() - 1000;
    const std::int64_t upper = target == 0 && !top_near_limit ? database_candidates[0].weight + 1000
                                                              : (target == 0 ? database_candidates[0].weight
                                                                             : database_candidates[target - 1].weight);
    const std::int64_t lower = database_candidates[target].weight;
    const bool oversized = upper > kManagedWeightCeiling || lower > kManagedWeightCeiling;
    bool need_rebalance =
        top_near_limit || lower == (std::numeric_limits<std::int64_t>::max)() || upper <= lower || oversized;
    if (!need_rebalance)
    {
        if (upper == lower + 1)
        {
            // There is no integer midpoint. Move one point above the upper
            // candidate and skip a short run of occupied weights. This may
            // advance one extra position, but keeps ordering deterministic
            // without paying for a rebalance in the common case.
            new_weight = upper + 1;
            size_t conflicts = 0;
            for (size_t i = target; i-- > 0;)
            {
                const std::int64_t occupied = database_candidates[i].weight;
                if (occupied < new_weight)
                    continue;
                if (occupied > new_weight)
                    break;
                if (++conflicts >= 16 || new_weight >= kManagedWeightCeiling)
                {
                    need_rebalance = true;
                    break;
                }
                ++new_weight;
            }
        }
        else
        {
            new_weight = lower + (upper - lower) / 2;
        }
    }
    if (need_rebalance && !oversized && !top_near_limit && target != 0)
    {
        const size_t rebalance_end = (std::min)(database_candidates.size(), target + kRebalanceCount);
        const std::int64_t last_index = static_cast<std::int64_t>(rebalance_end - 1);
        const std::int64_t last_weight =
            upper - kRebalanceGap - (last_index - static_cast<std::int64_t>(target)) * kRebalanceGap;
        if (last_weight < kManagedWeightFloor)
        {
            // Equal/low weights have no room for a 16-slot descending staircase.
            // Promote only the selected row instead of writing negatives onto
            // neighbors (and onto shorter-syllable singles mixed into the UI list).
            const std::int64_t cluster = (std::max)(upper, lower);
            if (cluster > (std::numeric_limits<std::int64_t>::max)() - kRebalanceGap)
            {
                // Fall through to the ceiling compact path below.
            }
            else
            {
                new_weight = clamp_managed_weight(cluster + kRebalanceGap);
                need_rebalance = false;
            }
        }
    }
    auto main_db = open_database(main_db_path, SQLITE_OPEN_READWRITE);
    if (!main_db || !execute_sql(main_db.get(), "BEGIN IMMEDIATE"))
    {
        rollback_user();
        return false;
    }
    const auto rollback_main = [&]() { (void)execute_sql(main_db.get(), "ROLLBACK"); };
    if (need_rebalance)
    {
        const size_t rebalance_begin = target;
        const size_t rebalance_end = (std::min)(database_candidates.size(), rebalance_begin + kRebalanceCount);
        std::int64_t base = target == 0 ? kManagedWeightCeiling - kRebalanceGap : kManagedWeightCeiling;
        if (target != 0 && !top_near_limit && !oversized)
        {
            const std::int64_t requested_base =
                upper + static_cast<std::int64_t>(target) * kRebalanceGap - kRebalanceGap;
            base = (std::min)(requested_base, kManagedWeightCeiling);
        }
        bool rebalance_ok = true;
        for (size_t i = rebalance_begin; i < rebalance_end && rebalance_ok; ++i)
        {
            if (!owns_entry_key[i])
                continue;
            const std::int64_t weight = clamp_managed_weight(base - static_cast<std::int64_t>(i) * kRebalanceGap);
            rebalance_ok = update_ranked_weight(main_db.get(), journal_upsert.get(), kind, entry_key,
                                                database_candidates[i].word, weight);
        }
        if (!rebalance_ok)
        {
            rollback_main();
            rollback_user();
            return false;
        }
        new_weight = target == 0 ? base + kRebalanceGap
                                 : base - static_cast<std::int64_t>(target) * kRebalanceGap + kRebalanceGap / 2;
    }
    new_weight = clamp_managed_weight(new_weight);
    const bool ok = update_ranked_weight(main_db.get(), journal_upsert.get(), kind, entry_key, value, new_weight);
    if (!ok)
    {
        rollback_main();
        rollback_user();
        return false;
    }
    auto reset = prepare(user_db.get(),
                         "DELETE FROM candidate_selection_state WHERE context_key=?1 AND entry_key=?2 AND value=?3");
    const bool reset_ok = reset && bind_text(reset.get(), 1, context_key) && bind_text(reset.get(), 2, entry_key) &&
                          bind_text(reset.get(), 3, value) && sqlite3_step(reset.get()) == SQLITE_DONE;
    reset.reset();
    if (!reset_ok || !execute_sql(user_db.get(), "COMMIT"))
    {
        rollback_main();
        rollback_user();
        return false;
    }
    if (!execute_sql(main_db.get(), "COMMIT"))
    {
        rollback_main();
        return false;
    }
    if (ranking_changed)
        *ranking_changed = true;
    return true;
}

namespace
{
// dictionary_state 里记「日志上次回放到的出厂词库」的那一行。
constexpr const char *kReplayedDictionaryState = "replayed_dictionary_sha256";

// 出厂 msime.db 的身份：与它一起装进数据目录的 dictionary-manifest.json 记下的 SHA-256。
// msime.db 一经调频就不再等于出厂字节，所以不能拿文件本身算；清单缺失或读不懂时返回空，
// 回放按原样写回日志里的权重。
std::optional<std::string> shipped_dictionary_identity(const std::string &main_db_path)
{
    const auto manifest_path =
        metasequoia::path_from_utf8(main_db_path.c_str()).parent_path() / metasequoia::assets::dictionary_manifest;
    std::ifstream input(manifest_path, std::ios::binary);
    if (!input)
        return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    try
    {
        const auto manifest = neural::json::parse(text);
        const auto *files = manifest.find("files");
        const auto *dictionary = files != nullptr ? files->find(metasequoia::assets::main_dictionary) : nullptr;
        const auto *sha256 = dictionary != nullptr ? dictionary->find("sha256") : nullptr;
        if (sha256 == nullptr || !sha256->is_string() || sha256->string.empty())
            return std::nullopt;
        return sha256->string;
    }
    catch (const std::exception &)
    {
        return std::nullopt;
    }
}

std::optional<std::string> replayed_dictionary_identity(sqlite3 *journal)
{
    // 只读打开的旧日志可能还没有 dictionary_state 表，prepare 失败即视为没有记录。
    auto stmt = prepare(journal, "SELECT value FROM dictionary_state WHERE name=?1");
    if (!stmt || !bind_text(stmt.get(), 1, kReplayedDictionaryState) || sqlite3_step(stmt.get()) != SQLITE_ROW)
        return std::nullopt;
    const unsigned char *value = sqlite3_column_text(stmt.get(), 0);
    return value == nullptr ? std::nullopt : std::optional<std::string>(reinterpret_cast<const char *>(value));
}

// 回放开始前目标词库里这一行的权重，即新装出厂词库的权重；没有这一行时为空。
std::optional<std::int64_t> shipped_weight(sqlite3 *main_db, const std::string &kind, const std::string &key,
                                           const std::string &value)
{
    const std::string table = kind == "wubi" ? std::string("wubi86") : pinyin_table(key);
    if (table.empty())
        return std::nullopt;
    auto stmt = prepare(main_db, "SELECT MAX(weight) FROM \"" + table + "\" WHERE key=?1 AND value=?2");
    if (!stmt || !bind_text(stmt.get(), 1, key) || !bind_text(stmt.get(), 2, value) ||
        sqlite3_step(stmt.get()) != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) == SQLITE_NULL)
        return std::nullopt;
    return sqlite3_column_int64(stmt.get(), 0);
}

struct RebasedOperation
{
    std::string kind;
    std::string key;
    std::string value;
    std::int64_t weight = 0;
};

// 把改记到新词库上的调频写回日志，并记下这次回放到的词库。写不成只是下次回放再改记一遍，
// 不影响已经提交的 msime.db，所以不算回放失败。
bool save_rebased_journal(const std::string &user_db_path, const std::string &dictionary_identity,
                          const std::vector<RebasedOperation> &rebased)
{
    UserDatabase user_db(user_db_path);
    if (!user_db || !execute_sql(user_db.get(), "BEGIN IMMEDIATE"))
        return false;
    auto update =
        prepare(user_db.get(), "UPDATE user_dictionary_operations SET weight=?4 WHERE dictionary=?1 AND "
                               "key=?2 AND value=?3 AND operation='upsert' AND user_inserted=0 AND weight_edited=0");
    if (!update)
        return false;
    for (const auto &operation : rebased)
    {
        sqlite3_reset(update.get());
        if (!bind_text(update.get(), 1, operation.kind) || !bind_text(update.get(), 2, operation.key) ||
            !bind_text(update.get(), 3, operation.value) ||
            sqlite3_bind_int64(update.get(), 4, operation.weight) != SQLITE_OK ||
            sqlite3_step(update.get()) != SQLITE_DONE)
            return false;
    }
    update.reset();
    auto state = prepare(user_db.get(), "INSERT INTO dictionary_state(name,value) VALUES(?1,?2)"
                                        " ON CONFLICT(name) DO UPDATE SET value=excluded.value");
    if (!state || !bind_text(state.get(), 1, kReplayedDictionaryState) ||
        !bind_text(state.get(), 2, dictionary_identity) || sqlite3_step(state.get()) != SQLITE_DONE)
        return false;
    state.reset();
    return execute_sql(user_db.get(), "COMMIT");
}
} // namespace

ReplayResult replay(const std::string &user_db_path, const std::string &main_db_path,
                    const std::string &english_db_path)
{
    ReplayResult result;
    if (!std::filesystem::exists(metasequoia::path_from_utf8(user_db_path.c_str())))
        return result;
    auto journal = open_database(user_db_path, SQLITE_OPEN_READONLY);
    if (!journal)
    {
        result.error = "cannot open user dictionary database";
        return result;
    }
    if (!EnglishDictionary::ensure_schema(english_db_path))
    {
        result.error = "cannot migrate English dictionary database";
        return result;
    }
    auto main_db = open_database(main_db_path, SQLITE_OPEN_READWRITE);
    if (!main_db)
    {
        result.error = "cannot open target dictionary database";
        return result;
    }
    auto attach_english = prepare(main_db.get(), "ATTACH DATABASE ?1 AS replay_english");
    if (!attach_english || !bind_text(attach_english.get(), 1, english_db_path) ||
        sqlite3_step(attach_english.get()) != SQLITE_DONE)
    {
        result.error = "cannot attach English dictionary database";
        return result;
    }
    attach_english.reset();
    // 回放以只读方式打开日志，不做 schema 迁移；没有 user_inserted 列的旧日志里也不会有手动加的词。
    const bool has_user_inserted = has_column(journal.get(), "user_dictionary_operations", "user_inserted");
    const std::string learned_kana =
        has_user_inserted ? std::string("(") + kLearnedKanaPinyinCondition + ")" : std::string("0");
    // 旧日志没有 weight_edited 列，设置页改过的权重在那时与自动调频分不开，按自动调频对待。
    const std::string user_owned =
        std::string(has_user_inserted ? "user_inserted" : "0") +
        (has_column(journal.get(), "user_dictionary_operations", "weight_edited") ? " OR weight_edited" : "");
    // 日志记的是调频写下的绝对权重，按的是当时那份出厂词库的量级。出厂词库没变时原样写回，
    // 调频提上去的词和为它让位的邻居都回到原处。词库换过（或日志还没记过回放到哪份词库）时，
    // 旧量级的权重会把新词库的词频压下去：新词库里「了 le」是 5200 万，日志里还是旧词库上
    // 调出来的 650 万。这时自动调频的行只在高过新出厂权重时写回，其余保留新权重，
    // 并把日志里的这一行改记成新权重，免得下次回放又写回旧值。用户手动加的词、在设置页
    // 亲手改的权重和删除照旧。
    const auto dictionary_identity = shipped_dictionary_identity(main_db_path);
    const bool rebase =
        dictionary_identity.has_value() && replayed_dictionary_identity(journal.get()) != dictionary_identity;
    std::vector<RebasedOperation> rebased;
    auto rows = prepare(journal.get(), "SELECT dictionary,key,value,operation,weight,display," + learned_kana + ",(" +
                                           user_owned + ") FROM user_dictionary_operations ORDER BY updated_at,rowid");
    if (!rows)
    {
        result.error = "invalid user dictionary database";
        return result;
    }

    if (!execute_sql(main_db.get(), "BEGIN IMMEDIATE"))
    {
        result.error = "cannot start replay transaction";
        return result;
    }
    int row_result = SQLITE_ROW;
    while ((row_result = sqlite3_step(rows.get())) == SQLITE_ROW)
    {
        const std::string kind = reinterpret_cast<const char *>(sqlite3_column_text(rows.get(), 0));
        const std::string key = reinterpret_cast<const char *>(sqlite3_column_text(rows.get(), 1));
        const std::string value = reinterpret_cast<const char *>(sqlite3_column_text(rows.get(), 2));
        const std::string operation = reinterpret_cast<const char *>(sqlite3_column_text(rows.get(), 3));
        const std::int64_t weight = sqlite3_column_int64(rows.get(), 4);
        const std::string display = reinterpret_cast<const char *>(sqlite3_column_text(rows.get(), 5));
        const bool learned_kana_pinyin = sqlite3_column_int(rows.get(), 6) != 0;
        const bool user_owned_row = sqlite3_column_int(rows.get(), 7) != 0;
        if ((kind == "pinyin" && operation == "upsert" && weight < 1) || learned_kana_pinyin)
        {
            ++result.skipped;
            continue;
        }
        if (rebase && operation == "upsert" && !user_owned_row && (kind == "pinyin" || kind == "wubi"))
        {
            const auto shipped = shipped_weight(main_db.get(), kind, key, value);
            if (shipped && *shipped >= weight)
            {
                if (*shipped != weight)
                    rebased.push_back({kind, key, value, *shipped});
                ++result.applied;
                continue;
            }
        }
        bool ok = false;
        if (kind == "pinyin")
            ok = apply_pinyin(main_db.get(), key, value, operation, weight);
        else if (kind == "wubi")
            ok = apply_simple(main_db.get(), "wubi86", "key", "value", key, value, operation, weight);
        else if (kind == "quick")
            ok = apply_simple(main_db.get(), "quick_parases", "key", "value", key, value, operation, weight);
        else if (kind == "english")
            ok = apply_english(main_db.get(), key, value, operation, weight, display);
        ok ? ++result.applied : ++result.failed;
    }
    rows.reset();
    if (row_result != SQLITE_DONE)
    {
        result.error = execute_sql(main_db.get(), "ROLLBACK")
                           ? "cannot read the complete user dictionary journal; changes were rolled back"
                           : "cannot read the complete user dictionary journal and rollback also failed";
        return result;
    }
    if (result.failed == 0)
    {
        if (!execute_sql(main_db.get(), "COMMIT"))
        {
            result.error = execute_sql(main_db.get(), "ROLLBACK")
                               ? "cannot commit replay transaction; changes were rolled back"
                               : "cannot commit or roll back replay transaction";
        }
        else if (rebase)
        {
            journal.reset();
            if (save_rebased_journal(user_db_path, *dictionary_identity, rebased))
                result.rebased = static_cast<int>(rebased.size());
        }
    }
    else
    {
        result.error = execute_sql(main_db.get(), "ROLLBACK")
                           ? "one or more operations failed; changes were rolled back"
                           : "one or more operations failed and rollback also failed";
    }
    return result;
}
} // namespace user_dictionary

namespace metasequoia
{
namespace
{
user_dictionary::DictionaryKind journal_kind(PersonalDictionaryKind kind)
{
    switch (kind)
    {
    case PersonalDictionaryKind::Pinyin:
        return user_dictionary::DictionaryKind::Pinyin;
    case PersonalDictionaryKind::Wubi:
        return user_dictionary::DictionaryKind::Wubi;
    case PersonalDictionaryKind::QuickPhrase:
        return user_dictionary::DictionaryKind::QuickPhrase;
    case PersonalDictionaryKind::English:
        return user_dictionary::DictionaryKind::English;
    }
    return user_dictionary::DictionaryKind::Pinyin;
}
bool apply_personal_edit(sqlite3 *db, const PersonalDictionaryEntry &entry, bool remove)
{
    using namespace user_dictionary;
    const std::string operation = remove ? "delete" : "upsert";
    bool applied = false;
    switch (entry.kind)
    {
    case PersonalDictionaryKind::Pinyin:
        applied = apply_pinyin(db, entry.key, entry.value, operation, entry.weight);
        break;
    case PersonalDictionaryKind::Wubi:
        applied = apply_simple(db, "wubi86", "key", "value", entry.key, entry.value, operation, entry.weight);
        break;
    case PersonalDictionaryKind::QuickPhrase:
        applied = apply_simple(db, "quick_parases", "key", "value", entry.key, entry.value, operation, entry.weight);
        break;
    case PersonalDictionaryKind::English:
        applied = apply_english(db, entry.key, entry.value, operation, entry.weight, entry.value);
        break;
    }
    if (!applied)
        return false;
    auto journal = prepare(
        db, "INSERT INTO "
            "personal_journal.user_dictionary_operations(dictionary,key,value,operation,weight,display,user_inserted)"
            " VALUES(?1,?2,?3,?4,?5,?6,1) ON CONFLICT(dictionary,key,value) DO UPDATE SET operation=excluded.operation,"
            " weight=excluded.weight,display=excluded.display,user_inserted=1,updated_at=unixepoch()");
    return journal && bind_text(journal.get(), 1, kind_name(journal_kind(entry.kind))) &&
           bind_text(journal.get(), 2, entry.key) && bind_text(journal.get(), 3, entry.value) &&
           bind_text(journal.get(), 4, operation) &&
           sqlite3_bind_int64(journal.get(), 5, remove ? 0 : entry.weight) == SQLITE_OK &&
           bind_text(journal.get(), 6, entry.kind == PersonalDictionaryKind::English ? entry.value : "") &&
           sqlite3_step(journal.get()) == SQLITE_DONE;
}
} // namespace
PersonalDictionaryEditResult edit_personal_dictionary(const RuntimePaths &paths,
                                                      const std::optional<PersonalDictionaryEntry> &previous,
                                                      const std::optional<PersonalDictionaryEntry> &replacement,
                                                      const std::string &request_id)
{
    using namespace user_dictionary;
    if (request_id.size() > 128 || !std::all_of(request_id.begin(), request_id.end(), [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
                   ch == '_';
        }))
        return {false, "Invalid personal dictionary request ID"};
    if (!previous && !replacement)
        return {false, "An entry is required"};
    std::optional<PersonalDictionaryEntry> old_entry, new_entry;
    if (previous)
    {
        auto checked = validate_personal_dictionary_entry(*previous);
        if (!checked.entry)
            return {false, checked.error};
        old_entry = std::move(checked.entry);
    }
    if (replacement)
    {
        auto checked = validate_personal_dictionary_entry(*replacement);
        if (!checked.entry)
            return {false, checked.error};
        new_entry = std::move(checked.entry);
        // 只归一新写入的词：查询会把 xvan、lue 先改成 xuan、lve 再查库，照原样存的行永远查不到。
        // 被替换的旧词保留原键，旧版本存下的 lue、jve 行才删得掉。
        if (new_entry->kind == PersonalDictionaryKind::Pinyin)
        {
            std::string key;
            std::size_t start = 0;
            while (true)
            {
                const auto end = new_entry->key.find('\'', start);
                key += quanpin::canonical_umlaut_spelling(new_entry->key.substr(start, end - start));
                if (end == std::string::npos)
                    break;
                key += '\'';
                start = end + 1;
            }
            new_entry->key = std::move(key);
        }
    }
    try
    {
        paths.validate();
        const auto journal_path = path_to_utf8(paths.user(assets::user_journal));
        const auto english_path = path_to_utf8(paths.dictionary(assets::english_dictionary));
        if (!ensure_user_database(journal_path) || !EnglishDictionary::ensure_schema(english_path))
            return {false, "Cannot prepare personal dictionary storage"};
        auto db = open_database(path_to_utf8(paths.dictionary(assets::main_dictionary)), SQLITE_OPEN_READWRITE);
        if (!db)
            return {false, "Cannot open dictionary"};
        for (const auto &attachment :
             {std::make_pair("personal_journal", journal_path), std::make_pair("replay_english", english_path)})
        {
            auto attach = prepare(db.get(), std::string("ATTACH DATABASE ?1 AS ") + attachment.first);
            if (!attach || !bind_text(attach.get(), 1, attachment.second) || sqlite3_step(attach.get()) != SQLITE_DONE)
                return {false, "Cannot attach personal dictionary storage"};
        }
        if (!execute_sql(db.get(), "BEGIN IMMEDIATE"))
            return {false, "Cannot begin personal dictionary edit"};
        const auto fail = [&](const char *message) {
            execute_sql(db.get(), "ROLLBACK");
            return PersonalDictionaryEditResult{false, message};
        };
        std::string payload;
        const auto append_payload = [&](const std::optional<PersonalDictionaryEntry> &entry) {
            if (!entry)
            {
                payload += "none;";
                return;
            }
            payload += std::to_string(static_cast<int>(entry->kind)) + ";" + std::to_string(entry->weight) + ";";
            for (const auto &value : {entry->key, entry->value})
                payload += std::to_string(value.size()) + ":" + value;
        };
        if (!request_id.empty())
        {
            append_payload(old_entry);
            append_payload(new_entry);
            auto receipt = prepare(
                db.get(), "SELECT payload FROM personal_journal.personal_dictionary_receipts WHERE request_id=?1");
            if (!receipt || !bind_text(receipt.get(), 1, request_id))
                return fail("Cannot read edit receipt");
            const int step = sqlite3_step(receipt.get());
            if (step == SQLITE_ROW)
            {
                const auto *stored = sqlite3_column_text(receipt.get(), 0);
                const bool same = stored && payload == reinterpret_cast<const char *>(stored);
                receipt.reset();
                if (!same)
                    return fail("Request ID was already used for another edit");
                if (!execute_sql(db.get(), "COMMIT"))
                    return fail("Cannot finish edit retry");
                return {true, {}};
            }
            if (step != SQLITE_DONE)
                return fail("Cannot finish reading edit receipt");
        }
        if (old_entry)
        {
            auto match = prepare(db.get(), "SELECT 1 FROM personal_journal.user_dictionary_operations"
                                           " WHERE dictionary=?1 AND key=?2 AND value=?3 AND weight=?4 AND "
                                           "operation='upsert' AND user_inserted=1");
            if (!match || !bind_text(match.get(), 1, kind_name(journal_kind(old_entry->kind))) ||
                !bind_text(match.get(), 2, old_entry->key) || !bind_text(match.get(), 3, old_entry->value) ||
                sqlite3_bind_int64(match.get(), 4, old_entry->weight) != SQLITE_OK ||
                sqlite3_step(match.get()) != SQLITE_ROW)
                return fail("The entry changed; reload it before editing");
            match.reset();
            if (!apply_personal_edit(db.get(), *old_entry, true))
                return fail("Cannot remove previous personal entry");
        }
        if (new_entry && !apply_personal_edit(db.get(), *new_entry, false))
            return fail("Cannot save personal entry");
        if (!request_id.empty())
        {
            auto receipt =
                prepare(db.get(),
                        "INSERT INTO personal_journal.personal_dictionary_receipts(request_id,payload) VALUES(?1,?2)");
            if (!receipt || !bind_text(receipt.get(), 1, request_id) || !bind_text(receipt.get(), 2, payload) ||
                sqlite3_step(receipt.get()) != SQLITE_DONE)
                return fail("Cannot save edit receipt");
        }
        if (!execute_sql(db.get(), "COMMIT"))
            return fail("Cannot commit personal dictionary edit");
        return {true, {}};
    }
    catch (const std::exception &)
    {
        return {false, "Cannot access personal dictionary storage"};
    }
}
PersonalDictionaryPage personal_dictionary_entries(const RuntimePaths &paths, std::size_t offset, std::size_t limit)
{
    using namespace user_dictionary;
    PersonalDictionaryPage result;
    if (limit == 0 || limit > 1000 || offset > 1000000)
    {
        result.error = "Invalid personal dictionary page";
        return result;
    }
    try
    {
        paths.validate();
        const auto journal_path = paths.user(assets::user_journal);
        if (!std::filesystem::exists(journal_path))
            return result;
        auto db = open_database(path_to_utf8(journal_path), SQLITE_OPEN_READONLY);
        auto rows =
            db ? prepare(
                     db.get(),
                     "SELECT dictionary,key,value,weight FROM user_dictionary_operations"
                     " WHERE user_inserted=1 AND operation='upsert' ORDER BY dictionary,key,value LIMIT ?1 OFFSET ?2")
               : Stmt{};
        if (!rows || sqlite3_bind_int64(rows.get(), 1, static_cast<sqlite3_int64>(limit + 1)) != SQLITE_OK ||
            sqlite3_bind_int64(rows.get(), 2, static_cast<sqlite3_int64>(offset)) != SQLITE_OK)
        {
            result.error = "Cannot read personal dictionary";
            return result;
        }
        int step;
        while ((step = sqlite3_step(rows.get())) == SQLITE_ROW)
        {
            if (result.entries.size() == limit)
            {
                result.has_more = true;
                break;
            }
            auto text = [&](int index) {
                const auto *value = sqlite3_column_text(rows.get(), index);
                return value
                           ? std::string(reinterpret_cast<const char *>(value), sqlite3_column_bytes(rows.get(), index))
                           : std::string{};
            };
            const auto kind = text(0);
            PersonalDictionaryKind entry_kind;
            if (kind == "pinyin")
                entry_kind = PersonalDictionaryKind::Pinyin;
            else if (kind == "wubi")
                entry_kind = PersonalDictionaryKind::Wubi;
            else if (kind == "quick")
                entry_kind = PersonalDictionaryKind::QuickPhrase;
            else if (kind == "english")
                entry_kind = PersonalDictionaryKind::English;
            else
                continue;
            result.entries.push_back({entry_kind, text(1), text(2), sqlite3_column_int64(rows.get(), 3)});
        }
        if (step != SQLITE_DONE && !result.has_more)
        {
            result.entries.clear();
            result.error = "Cannot finish reading personal dictionary";
        }
    }
    catch (const std::exception &)
    {
        result.entries.clear();
        result.error = "Cannot access personal dictionary storage";
    }
    return result;
}
} // namespace metasequoia
