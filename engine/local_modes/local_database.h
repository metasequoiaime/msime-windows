#pragma once

#include <sqlite3.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace metasequoia::local_modes
{
// One code looked up by query_ranked_entries(). Only lookups with exact_ranks
// set lift a whole-key match above longer keys: under Shuangpin the raw typed
// code is looked up too, and a raw code that happens to equal some jianpin or
// English key must not outrank the full pinyin the user meant.
struct RankedLookup
{
    std::string code;
    bool exact_ranks;
};

// Prefix lookup shared by the emoji and kaomoji modes. `sql` is prepared once
// and run per lookup with ?1 = code, ?2 = code + 0x7f (prefix upper bound) and
// ?3 = limit; it must return (text, sort_order, is_exact) with exact rows
// first, so that the per-lookup LIMIT never cuts an exact hit. The result holds
// exact hits of ranking lookups first, then every other hit, each tier in
// sort_order, deduplicated by text and capped at `limit`. Returns nullopt when
// the statement cannot be prepared, bound or stepped.
std::optional<std::vector<std::string>> query_ranked_entries(sqlite3 *database, const char *sql,
                                                             const std::vector<RankedLookup> &lookups, int limit);

// Opens a read-only connection for a local-mode query (jianpin, quick phrases,
// emoji, kaomoji). Those queries run on every keystroke of their mode, and a
// fresh connection re-parses the dictionary schema -- a couple of hundred tables
// in msime.db -- on its first prepare. For the shipped dictionaries of the
// active data directory the connection is therefore opened once and shared.
// Any other path (tests, tools) gets its own connection that closes with the
// last reference, so the file is never held open behind a caller's back.
// Returns null when the database cannot be opened.
std::shared_ptr<sqlite3> open_local_database(const std::filesystem::path &path);

// Drops the shared connections so the dictionary files can be deleted or
// replaced; each closes once the last query using it returns. Called by
// user_dictionary::close_default_user_database(), which is what callers use to
// let go of the data directory.
void close_cached_local_databases();
} // namespace metasequoia::local_modes
