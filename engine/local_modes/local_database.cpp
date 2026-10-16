#include "../contracts/assets/assets.h"
#include "local_database.h"

#include "../core/data_path.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace metasequoia::local_modes
{
namespace
{
std::shared_ptr<sqlite3> open_read_only(const std::filesystem::path &path)
{
    sqlite3 *raw = nullptr;
    // FULLMUTEX: a shared connection is used from the IME worker and from the
    // emoji/kaomoji lookup threads.
    if (sqlite3_open_v2(path_to_utf8(path).c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) !=
        SQLITE_OK)
    {
        if (raw != nullptr)
        {
            sqlite3_close(raw);
        }
        return {};
    }
    sqlite3_busy_timeout(raw, 1000);
    return std::shared_ptr<sqlite3>(raw, [](sqlite3 *database) { sqlite3_close(database); });
}

// Only the shipped dictionaries are shared: msime.db is held open by the engine
// for the whole session anyway, and others.db is replaced only by the installer
// while the Server is stopped. Keeping anything else open could block a tool or
// a test that deletes its database afterwards, which Windows refuses while a
// handle is open.
bool is_shared_dictionary(const std::filesystem::path &path)
{
    return path == data_file_path(metasequoia::assets::main_dictionary) ||
           path == data_file_path(metasequoia::assets::other_dictionary);
}

struct CachedConnection
{
    std::filesystem::path path;
    std::shared_ptr<sqlite3> connection;
};

std::mutex &cache_mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<CachedConnection> &cache()
{
    static std::vector<CachedConnection> connections;
    return connections;
}

struct StatementCloser
{
    void operator()(sqlite3_stmt *statement) const
    {
        if (statement != nullptr)
        {
            sqlite3_finalize(statement);
        }
    }
};
} // namespace

std::optional<std::vector<std::string>> query_ranked_entries(sqlite3 *database, const char *sql,
                                                             const std::vector<RankedLookup> &lookups, int limit)
{
    sqlite3_stmt *raw_statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &raw_statement, nullptr) != SQLITE_OK)
    {
        return std::nullopt;
    }
    const std::unique_ptr<sqlite3_stmt, StatementCloser> statement(raw_statement);

    struct Entry
    {
        std::string text;
        int tier;
        int sort_order;
    };
    std::vector<Entry> entries;
    for (const RankedLookup &lookup : lookups)
    {
        std::string upper_bound = lookup.code;
        upper_bound.push_back(static_cast<char>(0x7f));
        if (sqlite3_reset(statement.get()) != SQLITE_OK ||
            sqlite3_bind_text(statement.get(), 1, lookup.code.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(statement.get(), 2, upper_bound.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_int(statement.get(), 3, limit) != SQLITE_OK)
        {
            return std::nullopt;
        }

        int step_result = SQLITE_ROW;
        while ((step_result = sqlite3_step(statement.get())) == SQLITE_ROW)
        {
            const auto *text = reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0));
            if (text != nullptr)
            {
                const bool exact = lookup.exact_ranks && sqlite3_column_int(statement.get(), 2) != 0;
                entries.push_back({text, exact ? 0 : 1, sqlite3_column_int(statement.get(), 1)});
            }
        }
        if (step_result != SQLITE_DONE)
        {
            return std::nullopt;
        }
    }

    // Sorting before deduplicating keeps each text at its best rank when
    // several lookups, or several keys of one lookup, return it.
    std::stable_sort(entries.begin(), entries.end(), [](const Entry &left, const Entry &right) {
        return left.tier != right.tier ? left.tier < right.tier : left.sort_order < right.sort_order;
    });
    std::vector<std::string> texts;
    std::unordered_set<std::string> seen;
    for (Entry &entry : entries)
    {
        if (texts.size() >= static_cast<std::size_t>(limit))
        {
            break;
        }
        if (seen.insert(entry.text).second)
        {
            texts.push_back(std::move(entry.text));
        }
    }
    return texts;
}

std::shared_ptr<sqlite3> open_local_database(const std::filesystem::path &path)
{
    if (path.empty())
    {
        return {};
    }
    if (!is_shared_dictionary(path))
    {
        return open_read_only(path);
    }

    const std::lock_guard<std::mutex> guard(cache_mutex());
    auto &connections = cache();
    for (const CachedConnection &cached : connections)
    {
        if (cached.path == path)
        {
            return cached.connection;
        }
    }
    auto connection = open_read_only(path);
    if (connection)
    {
        // At most one entry per shipped dictionary: a connection cached for a
        // data directory that is no longer active is released here.
        connections.erase(
            std::remove_if(connections.begin(), connections.end(),
                           [&](const CachedConnection &cached) { return cached.path.filename() == path.filename(); }),
            connections.end());
        connections.push_back({path, connection});
    }
    return connection;
}

void close_cached_local_databases()
{
    const std::lock_guard<std::mutex> guard(cache_mutex());
    cache().clear();
}
} // namespace metasequoia::local_modes
