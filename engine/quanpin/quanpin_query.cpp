#include "../contracts/assets/assets.h"
#include "../contracts/dictionary/format.h"
#include "quanpin_query.h"

#include "fuzzy_pinyin.h"
#include "quanpin_utils.h"
#include "../shuangpin/shuangpin_utils.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace quanpin
{
namespace
{

std::vector<std::string> split(const std::string &text, char delimiter)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true)
    {
        const size_t pos = text.find(delimiter, start);
        if (pos == std::string::npos)
        {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
}

constexpr size_t kCorrectionPathLimit = 32;

using CorrectionAliases = std::unordered_map<std::string, std::vector<std::string>>;

const CorrectionAliases &pinyin_correction_aliases()
{
    static const CorrectionAliases kAliases = [] {
        CorrectionAliases aliases;
        const auto add_alias = [&](const std::string &typed, const std::string &canonical) {
            auto &canonical_syllables = aliases[typed];
            if (std::find(canonical_syllables.begin(), canonical_syllables.end(), canonical) ==
                canonical_syllables.end())
            {
                canonical_syllables.push_back(canonical);
            }
        };
        const auto add_suffix_aliases = [&](const std::string &canonical_suffix, const std::string &typed_suffix) {
            for (const auto &syllable : intact_pinyin_list())
            {
                if (syllable.size() < canonical_suffix.size() ||
                    syllable.compare(syllable.size() - canonical_suffix.size(), canonical_suffix.size(),
                                     canonical_suffix) != 0)
                {
                    continue;
                }

                const std::string prefix = syllable.substr(0, syllable.size() - canonical_suffix.size());
                add_alias(prefix + typed_suffix, syllable);
            }
        };

        add_suffix_aliases("iang", "aing");
        add_suffix_aliases("uang", "aung");
        add_suffix_aliases("ian", "ain");
        add_suffix_aliases("uan", "aun");
        add_suffix_aliases("iao", "aio");
        add_suffix_aliases("ing", "ihng");
        add_suffix_aliases("ang", "agn");
        add_suffix_aliases("eng", "egn");
        add_alias("egn", "eng");
        add_alias("jv", "ju");
        // Missing final g before the next syllable: zhonguo -> zhong guo.
        add_suffix_aliases("ong", "on");

        // Prefer a transposed h (ahng -> hang), while retaining the extra-h
        // interpretation as an alternative (ahng -> ang).
        add_suffix_aliases("hang", "ahng");
        add_suffix_aliases("ang", "ahng");

        // Prefer a transposed h (cehng -> cheng), while retaining the extra-h
        // interpretation as an alternative (cehng -> ceng).
        add_suffix_aliases("heng", "ehng");
        add_suffix_aliases("eng", "ehng");
        return aliases;
    }();
    return kAliases;
}

std::vector<Segments> cut_one_piece_with_corrections(const std::string &pinyin)
{
    struct RankedPath
    {
        Segments segments;
        std::vector<size_t> typed_lengths;
        std::vector<size_t> correction_ranks;
    };

    const auto &valid_pinyin = intact_pinyin_set();
    const auto &aliases = pinyin_correction_aliases();
    std::unordered_map<size_t, std::vector<RankedPath>> memo;

    const auto solve = [&](auto &&self, size_t index) -> std::vector<RankedPath> {
        if (index == pinyin.size())
        {
            return {RankedPath{}};
        }
        if (const auto found = memo.find(index); found != memo.end())
        {
            return found->second;
        }

        std::vector<RankedPath> paths;
        for (size_t end = pinyin.size(); end > index; --end)
        {
            const std::string typed = pinyin.substr(index, end - index);
            std::vector<std::string> canonical_syllables;
            if (const auto alias = aliases.find(typed); alias != aliases.end())
            {
                canonical_syllables = alias->second;
            }
            else if (valid_pinyin.find(typed) != valid_pinyin.end())
            {
                canonical_syllables.push_back(typed);
            }
            else
            {
                continue;
            }

            const auto suffix_paths = self(self, end);
            for (size_t canonical_index = 0; canonical_index < canonical_syllables.size(); ++canonical_index)
            {
                for (const auto &suffix : suffix_paths)
                {
                    RankedPath candidate;
                    candidate.segments.reserve(1 + suffix.segments.size());
                    candidate.segments.push_back(canonical_syllables[canonical_index]);
                    candidate.segments.insert(candidate.segments.end(), suffix.segments.begin(), suffix.segments.end());
                    candidate.typed_lengths.reserve(1 + suffix.typed_lengths.size());
                    candidate.typed_lengths.push_back(end - index);
                    candidate.typed_lengths.insert(candidate.typed_lengths.end(), suffix.typed_lengths.begin(),
                                                   suffix.typed_lengths.end());
                    candidate.correction_ranks.reserve(1 + suffix.correction_ranks.size());
                    candidate.correction_ranks.push_back(canonical_index);
                    candidate.correction_ranks.insert(candidate.correction_ranks.end(), suffix.correction_ranks.begin(),
                                                      suffix.correction_ranks.end());
                    paths.push_back(std::move(candidate));
                }
            }
        }

        std::stable_sort(paths.begin(), paths.end(), [](const RankedPath &lhs, const RankedPath &rhs) {
            if (lhs.segments.size() != rhs.segments.size())
            {
                return lhs.segments.size() < rhs.segments.size();
            }
            if (lhs.typed_lengths != rhs.typed_lengths)
            {
                return std::lexicographical_compare(lhs.typed_lengths.begin(), lhs.typed_lengths.end(),
                                                    rhs.typed_lengths.begin(), rhs.typed_lengths.end());
            }
            return std::lexicographical_compare(lhs.correction_ranks.begin(), lhs.correction_ranks.end(),
                                                rhs.correction_ranks.begin(), rhs.correction_ranks.end());
        });
        paths.erase(
            std::unique(paths.begin(), paths.end(),
                        [](const RankedPath &lhs, const RankedPath &rhs) { return lhs.segments == rhs.segments; }),
            paths.end());
        if (!paths.empty())
        {
            const size_t minimum_segments = paths.front().segments.size();
            paths.erase(std::find_if(paths.begin(), paths.end(),
                                     [&](const RankedPath &path) { return path.segments.size() != minimum_segments; }),
                        paths.end());
        }
        if (paths.size() > kCorrectionPathLimit)
        {
            paths.resize(kCorrectionPathLimit);
        }
        memo.emplace(index, paths);
        return paths;
    };

    const auto ranked_paths = solve(solve, 0);
    std::vector<Segments> paths;
    paths.reserve(ranked_paths.size());
    for (const auto &path : ranked_paths)
    {
        paths.push_back(path.segments);
    }
    return paths;
}

std::vector<Segments> cut_pinyin_with_corrections(const std::string &pinyin)
{
    std::vector<Segments> merged_paths = {Segments{}};
    for (const auto &part : split(pinyin, '\''))
    {
        const auto part_paths = cut_one_piece_with_corrections(part);
        if (part_paths.empty())
        {
            return {};
        }

        std::vector<Segments> combined;
        for (const auto &merged : merged_paths)
        {
            for (const auto &part_path : part_paths)
            {
                Segments candidate = merged;
                candidate.insert(candidate.end(), part_path.begin(), part_path.end());
                combined.push_back(std::move(candidate));
                if (combined.size() == kCorrectionPathLimit)
                {
                    break;
                }
            }
            if (combined.size() == kCorrectionPathLimit)
            {
                break;
            }
        }
        merged_paths = std::move(combined);
    }
    return merged_paths;
}

std::string build_table_name_impl(const Segments &segments)
{
    if (segments.empty() || segments.front().empty())
    {
        return "";
    }
    return metasequoia::dictionary_format::quanpin_table(segments.size(), segments.front().front());
}

std::string segments_to_jianpin_impl(const Segments &segments)
{
    std::string jp;
    for (const auto &segment : segments)
    {
        if (!segment.empty())
        {
            jp.push_back(segment.front());
        }
    }
    return jp;
}

std::string build_key_like_pattern(const Segments &segments)
{
    Segments parts;
    for (size_t i = 0; i < segments.size(); ++i)
    {
        const bool is_last = (i + 1 == segments.size());
        if (is_last || segments[i].size() == 1)
        {
            parts.push_back(segments[i] + "%");
        }
        else
        {
            parts.push_back(segments[i]);
        }
    }
    return join_segments(parts);
}

std::string build_key_prefix_upper_bound(const std::string &prefix)
{
    return prefix + "{";
}

bool is_pure_jianpin(const Segments &segments)
{
    return std::all_of(segments.begin(), segments.end(),
                       [](const std::string &segment) { return segment.size() == 1; });
}

bool is_shuangpin_initial_token(const std::string &segment)
{
    return segment == "zh" || segment == "ch" || segment == "sh";
}

bool needs_mixed_jianpin_query(const Segments &segments, QuerySource source)
{
    if (segments.size() <= 1)
    {
        return false;
    }

    return std::any_of(segments.begin(), segments.end() - 1, [source](const std::string &segment) {
        if (segment.size() == 1)
        {
            return true;
        }
        return source == QuerySource::Shuangpin && is_shuangpin_initial_token(segment);
    });
}

std::string extract_initial_token(const std::string &segment)
{
    if (segment.size() >= 2)
    {
        const auto prefix = segment.substr(0, 2);
        if (prefix == "zh" || prefix == "ch" || prefix == "sh")
        {
            return prefix;
        }
    }
    return segment.empty() ? "" : segment.substr(0, 1);
}

bool matches_mixed_segments(const std::string &key, const Segments &segments, QuerySource source)
{
    const auto key_segments = split(key, '\'');
    if (key_segments.size() != segments.size())
    {
        return false;
    }

    for (size_t i = 0; i < segments.size(); ++i)
    {
        const auto &expected = segments[i];
        const auto &actual = key_segments[i];

        if (expected.empty() || actual.empty())
        {
            return false;
        }

        const bool is_strict_shuangpin_initial =
            source == QuerySource::Shuangpin && is_shuangpin_initial_token(expected);
        if (expected.size() == 1 || is_strict_shuangpin_initial)
        {
            if (source == QuerySource::Shuangpin)
            {
                if (extract_initial_token(actual) != expected)
                {
                    return false;
                }
            }
            else if (actual.front() != expected.front())
            {
                return false;
            }
            continue;
        }

        if (actual != expected)
        {
            return false;
        }
    }

    return true;
}

int build_mixed_jianpin_scan_limit(int limit)
{
    if (limit > INT_MAX / 16)
    {
        return INT_MAX;
    }
    return std::max(limit * 16, 128);
}

bool can_match_exact_key(const Segments &segments)
{
    if (segments.empty())
    {
        return false;
    }

    const auto &valid_pinyin = intact_pinyin_set();
    return std::all_of(segments.begin(), segments.end(),
                       [&](const std::string &segment) { return valid_pinyin.find(segment) != valid_pinyin.end(); });
}

class SqliteDb
{
  public:
    explicit SqliteDb(const std::string &db_path)
    {
        if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK)
        {
            const std::string message = db_ != nullptr ? sqlite3_errmsg(db_) : "sqlite open failed";
            if (db_ != nullptr)
            {
                sqlite3_close(db_);
                db_ = nullptr;
            }
            throw std::runtime_error(message);
        }
        // See QuanpinDictionary's constructor: a query must wait out a commit, not fail.
        sqlite3_busy_timeout(db_, kDictionaryBusyTimeoutMs);
    }

    ~SqliteDb()
    {
        if (db_ != nullptr)
        {
            sqlite3_close(db_);
        }
    }

    sqlite3 *get() const
    {
        return db_;
    }

  private:
    sqlite3 *db_ = nullptr;
};

std::vector<QueryItem> run_query(sqlite3 *db, const std::string &sql, const std::string &value, int limit)
{
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
    {
        return {};
    }

    sqlite3_bind_text(stmt, 1, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    std::vector<QueryItem> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        const std::int64_t weight = sqlite3_column_int64(stmt, 1);
        rows.emplace_back(text == nullptr ? "" : reinterpret_cast<const char *>(text), weight);
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<QueryItem> run_query(sqlite3 *db, std::unordered_map<std::string, sqlite3_stmt *> &statement_cache,
                                 const std::string &sql, const std::string &value, int limit)
{
    sqlite3_stmt *stmt = nullptr;
    const auto found = statement_cache.find(sql);
    if (found != statement_cache.end())
    {
        stmt = found->second;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    else
    {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            return {};
        }
        statement_cache.emplace(sql, stmt);
    }

    sqlite3_bind_text(stmt, 1, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    std::vector<QueryItem> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        const std::int64_t weight = sqlite3_column_int64(stmt, 1);
        rows.emplace_back(text == nullptr ? "" : reinterpret_cast<const char *>(text), weight);
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return rows;
}

std::vector<QueryItem> run_query(sqlite3 *db, const std::string &sql, const std::string &lower_bound,
                                 const std::string &upper_bound, int limit)
{
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
    {
        return {};
    }

    sqlite3_bind_text(stmt, 1, lower_bound.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, upper_bound.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, limit);

    std::vector<QueryItem> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        const std::int64_t weight = sqlite3_column_int64(stmt, 1);
        rows.emplace_back(text == nullptr ? "" : reinterpret_cast<const char *>(text), weight);
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<QueryItem> run_query(sqlite3 *db, std::unordered_map<std::string, sqlite3_stmt *> &statement_cache,
                                 const std::string &sql, const std::string &lower_bound, const std::string &upper_bound,
                                 int limit)
{
    sqlite3_stmt *stmt = nullptr;
    const auto found = statement_cache.find(sql);
    if (found != statement_cache.end())
    {
        stmt = found->second;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    else
    {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            return {};
        }
        statement_cache.emplace(sql, stmt);
    }

    sqlite3_bind_text(stmt, 1, lower_bound.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, upper_bound.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, limit);

    std::vector<QueryItem> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        const std::int64_t weight = sqlite3_column_int64(stmt, 1);
        rows.emplace_back(text == nullptr ? "" : reinterpret_cast<const char *>(text), weight);
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return rows;
}

std::vector<KeyedQueryItem> run_keyed_query(sqlite3 *db, const std::string &sql, const std::string &value, int limit)
{
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
    {
        return {};
    }

    sqlite3_bind_text(stmt, 1, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    std::vector<KeyedQueryItem> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const unsigned char *key = sqlite3_column_text(stmt, 0);
        const unsigned char *value_text = sqlite3_column_text(stmt, 1);
        const std::int64_t weight = sqlite3_column_int64(stmt, 2);
        rows.push_back(KeyedQueryItem{
            key == nullptr ? "" : reinterpret_cast<const char *>(key),
            value_text == nullptr ? "" : reinterpret_cast<const char *>(value_text),
            weight,
        });
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<KeyedQueryItem> run_keyed_query(sqlite3 *db,
                                            std::unordered_map<std::string, sqlite3_stmt *> &statement_cache,
                                            const std::string &sql, const std::string &value, int limit)
{
    sqlite3_stmt *stmt = nullptr;
    const auto found = statement_cache.find(sql);
    if (found != statement_cache.end())
    {
        stmt = found->second;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    else
    {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            return {};
        }
        statement_cache.emplace(sql, stmt);
    }

    sqlite3_bind_text(stmt, 1, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    std::vector<KeyedQueryItem> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const unsigned char *key = sqlite3_column_text(stmt, 0);
        const unsigned char *value_text = sqlite3_column_text(stmt, 1);
        const std::int64_t weight = sqlite3_column_int64(stmt, 2);
        rows.push_back(KeyedQueryItem{
            key == nullptr ? "" : reinterpret_cast<const char *>(key),
            value_text == nullptr ? "" : reinterpret_cast<const char *>(value_text),
            weight,
        });
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return rows;
}

std::vector<KeyedQueryItem> run_keyed_query(sqlite3 *db, const std::string &sql, const std::string &lower_bound,
                                            const std::string &upper_bound, int limit)
{
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
    {
        return {};
    }

    sqlite3_bind_text(stmt, 1, lower_bound.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, upper_bound.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, limit);

    std::vector<KeyedQueryItem> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const unsigned char *key = sqlite3_column_text(stmt, 0);
        const unsigned char *value_text = sqlite3_column_text(stmt, 1);
        rows.push_back(KeyedQueryItem{
            key == nullptr ? "" : reinterpret_cast<const char *>(key),
            value_text == nullptr ? "" : reinterpret_cast<const char *>(value_text),
            sqlite3_column_int64(stmt, 2),
        });
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<KeyedQueryItem> run_keyed_query(sqlite3 *db,
                                            std::unordered_map<std::string, sqlite3_stmt *> &statement_cache,
                                            const std::string &sql, const std::string &lower_bound,
                                            const std::string &upper_bound, int limit)
{
    sqlite3_stmt *stmt = nullptr;
    const auto found = statement_cache.find(sql);
    if (found != statement_cache.end())
    {
        stmt = found->second;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    else
    {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            return {};
        }
        statement_cache.emplace(sql, stmt);
    }

    sqlite3_bind_text(stmt, 1, lower_bound.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, upper_bound.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, limit);

    std::vector<KeyedQueryItem> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const unsigned char *key = sqlite3_column_text(stmt, 0);
        const unsigned char *value_text = sqlite3_column_text(stmt, 1);
        rows.push_back(KeyedQueryItem{
            key == nullptr ? "" : reinterpret_cast<const char *>(key),
            value_text == nullptr ? "" : reinterpret_cast<const char *>(value_text),
            sqlite3_column_int64(stmt, 2),
        });
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return rows;
}

std::vector<KeyedQueryItem> run_keyed_batch_query(sqlite3 *db,
                                                  std::unordered_map<std::string, sqlite3_stmt *> &statement_cache,
                                                  const std::string &table, const std::vector<std::string> &keys,
                                                  int limit)
{
    if (db == nullptr || table.empty() || keys.empty() || limit <= 0)
    {
        return {};
    }

    std::string placeholders;
    for (size_t index = 0; index < keys.size(); ++index)
    {
        if (index > 0)
        {
            placeholders += ',';
        }
        placeholders += '?';
    }
    const std::string sql = "SELECT \"key\", \"value\", \"weight\" FROM \"" + table + "\" WHERE \"key\" IN (" +
                            placeholders + ") ORDER BY \"weight\" DESC LIMIT ?";

    sqlite3_stmt *stmt = nullptr;
    const auto found = statement_cache.find(sql);
    if (found != statement_cache.end())
    {
        stmt = found->second;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    else
    {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            return {};
        }
        statement_cache.emplace(sql, stmt);
    }

    for (size_t index = 0; index < keys.size(); ++index)
    {
        sqlite3_bind_text(stmt, static_cast<int>(index + 1), keys[index].c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, static_cast<int>(keys.size() + 1), limit);

    std::vector<KeyedQueryItem> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const unsigned char *key = sqlite3_column_text(stmt, 0);
        const unsigned char *value_text = sqlite3_column_text(stmt, 1);
        rows.push_back(KeyedQueryItem{
            key == nullptr ? "" : reinterpret_cast<const char *>(key),
            value_text == nullptr ? "" : reinterpret_cast<const char *>(value_text),
            sqlite3_column_int64(stmt, 2),
        });
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return rows;
}

std::vector<KeyedQueryItem> filter_mixed_jianpin_rows(const std::vector<KeyedQueryItem> &rows, const Segments &segments,
                                                      int limit, QuerySource source)
{
    std::vector<KeyedQueryItem> matched;
    matched.reserve(static_cast<size_t>(std::min(limit, static_cast<int>(rows.size()))));
    for (const auto &row : rows)
    {
        if (!matches_mixed_segments(row.key, segments, source))
        {
            continue;
        }

        matched.push_back(row);
        if (static_cast<int>(matched.size()) >= limit)
        {
            break;
        }
    }
    return matched;
}

std::vector<QueryItem> without_keys(const std::vector<KeyedQueryItem> &rows)
{
    std::vector<QueryItem> result;
    result.reserve(rows.size());
    for (const auto &row : rows)
    {
        result.emplace_back(row.value, row.weight);
    }
    return result;
}

void deduplicate_keyed_items_by_value(std::vector<KeyedQueryItem> &items)
{
    std::unordered_set<std::string> seen;
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&](const KeyedQueryItem &item) { return !seen.insert(item.value).second; }),
                items.end());
}

std::vector<KeyedQueryItem> query_single_cut_keyed(sqlite3 *db, const Segments &segments, int limit, QuerySource source)
{
    const auto table = build_table_name_impl(segments);
    if (table.empty())
    {
        return {};
    }

    const auto key = join_segments(segments);
    const auto jp = segments_to_jianpin_impl(segments);
    const auto mixed_query_limit = build_mixed_jianpin_scan_limit(limit);
    const auto needs_mixed_query = needs_mixed_jianpin_query(segments, source);
    const auto key_prefix_pattern = build_key_like_pattern(segments);
    const auto key_prefix = key_prefix_pattern.substr(0, key_prefix_pattern.size() - 1);
    const auto key_prefix_upper_bound = build_key_prefix_upper_bound(key_prefix);

    const auto exact_sql = "SELECT \"key\", \"value\", \"weight\" FROM \"" + table +
                           "\" WHERE \"key\" = ? ORDER BY \"weight\" DESC LIMIT ?";
    std::vector<KeyedQueryItem> rows;
    if (can_match_exact_key(segments))
    {
        rows = run_keyed_query(db, exact_sql, key, limit);
        if (!rows.empty())
        {
            return rows;
        }
    }

    const auto prefix_sql = "SELECT \"key\", \"value\", \"weight\" FROM \"" + table +
                            "\" WHERE \"key\" >= ? AND \"key\" < ? ORDER BY \"weight\" DESC LIMIT ?";
    rows = run_keyed_query(db, prefix_sql, key_prefix, key_prefix_upper_bound, limit);
    if (!rows.empty())
    {
        return rows;
    }

    if (needs_mixed_query)
    {
        const auto mixed_sql = "SELECT \"key\", \"value\", \"weight\" FROM \"" + table +
                               "\" WHERE \"jp\" = ? ORDER BY \"weight\" DESC LIMIT ?";
        rows =
            filter_mixed_jianpin_rows(run_keyed_query(db, mixed_sql, jp, mixed_query_limit), segments, limit, source);
        if (!rows.empty())
        {
            return rows;
        }
    }

    if (!is_pure_jianpin(segments))
    {
        return {};
    }

    const auto jp_sql = "SELECT \"key\", \"value\", \"weight\" FROM \"" + table +
                        "\" WHERE \"jp\" = ? ORDER BY \"weight\" DESC LIMIT ?";
    return run_keyed_query(db, jp_sql, jp, limit);
}

std::vector<KeyedQueryItem> query_single_cut_keyed(sqlite3 *db,
                                                   std::unordered_map<std::string, sqlite3_stmt *> &statement_cache,
                                                   const Segments &segments, int limit, QuerySource source)
{
    const auto table = build_table_name_impl(segments);
    if (table.empty())
    {
        return {};
    }

    const auto key = join_segments(segments);
    const auto jp = segments_to_jianpin_impl(segments);
    const auto mixed_query_limit = build_mixed_jianpin_scan_limit(limit);
    const auto needs_mixed_query = needs_mixed_jianpin_query(segments, source);
    const auto key_prefix_pattern = build_key_like_pattern(segments);
    const auto key_prefix = key_prefix_pattern.substr(0, key_prefix_pattern.size() - 1);
    const auto key_prefix_upper_bound = build_key_prefix_upper_bound(key_prefix);

    const auto exact_sql = "SELECT \"key\", \"value\", \"weight\" FROM \"" + table +
                           "\" WHERE \"key\" = ? ORDER BY \"weight\" DESC LIMIT ?";
    std::vector<KeyedQueryItem> rows;
    if (can_match_exact_key(segments))
    {
        rows = run_keyed_query(db, statement_cache, exact_sql, key, limit);
        if (!rows.empty())
        {
            return rows;
        }
    }

    const auto prefix_sql = "SELECT \"key\", \"value\", \"weight\" FROM \"" + table +
                            "\" WHERE \"key\" >= ? AND \"key\" < ? ORDER BY \"weight\" DESC LIMIT ?";
    rows = run_keyed_query(db, statement_cache, prefix_sql, key_prefix, key_prefix_upper_bound, limit);
    if (!rows.empty())
    {
        return rows;
    }

    if (needs_mixed_query)
    {
        const auto mixed_sql = "SELECT \"key\", \"value\", \"weight\" FROM \"" + table +
                               "\" WHERE \"jp\" = ? ORDER BY \"weight\" DESC LIMIT ?";
        rows = filter_mixed_jianpin_rows(run_keyed_query(db, statement_cache, mixed_sql, jp, mixed_query_limit),
                                         segments, limit, source);
        if (!rows.empty())
        {
            return rows;
        }
    }

    if (!is_pure_jianpin(segments))
    {
        return {};
    }

    const auto jp_sql = "SELECT \"key\", \"value\", \"weight\" FROM \"" + table +
                        "\" WHERE \"jp\" = ? ORDER BY \"weight\" DESC LIMIT ?";
    return run_keyed_query(db, statement_cache, jp_sql, jp, limit);
}

std::vector<QueryItem> query_single_cut(sqlite3 *db, const Segments &segments, int limit, QuerySource source)
{
    return without_keys(query_single_cut_keyed(db, segments, limit, source));
}

std::vector<QueryItem> query_single_cut(sqlite3 *db, std::unordered_map<std::string, sqlite3_stmt *> &statement_cache,
                                        const Segments &segments, int limit, QuerySource source)
{
    return without_keys(query_single_cut_keyed(db, statement_cache, segments, limit, source));
}

} // namespace

Segments cut_pinyin_greedy(const std::string &pinyin, bool intact_only)
{
    if (pinyin.empty())
    {
        return {};
    }

    if (pinyin.find('\'') == std::string::npos)
    {
        return cut_one_piece_min_segments(pinyin, intact_only);
    }

    Segments merged;
    for (const auto &part : split(pinyin, '\''))
    {
        auto cut = cut_one_piece_min_segments(part, intact_only);
        if (cut.empty())
        {
            if (intact_only && !part.empty())
            {
                return {};
            }
            if (!part.empty())
            {
                merged.push_back(part);
            }
            continue;
        }
        merged.insert(merged.end(), cut.begin(), cut.end());
    }
    return merged;
}

std::vector<Segments> cut_pinyin_by_mode(const std::string &pinyin, const std::string &mode)
{
    if (mode != "greedy" && mode != "correction")
    {
        throw std::invalid_argument("mode must be one of: greedy, correction");
    }

    if (mode == "greedy")
    {
        const auto greedy = cut_pinyin_greedy(pinyin, false);
        if (!greedy.empty())
        {
            return {greedy};
        }
        return {};
    }

    const auto corrected_paths = cut_pinyin_with_corrections(pinyin);
    if (!corrected_paths.empty())
    {
        return corrected_paths;
    }

    const auto greedy = cut_pinyin_greedy(pinyin, false);
    if (!greedy.empty())
    {
        return {greedy};
    }

    return {};
}

Segments split_segments(const std::string &segmentation)
{
    if (segmentation.empty())
    {
        return {};
    }

    return split(segmentation, '\'');
}

std::string join_segments(const Segments &segments, const std::string &delimiter)
{
    std::string joined;
    for (size_t i = 0; i < segments.size(); ++i)
    {
        if (i > 0)
        {
            joined += delimiter;
        }
        joined += segments[i];
    }
    return joined;
}

std::string build_table_name(const Segments &segments)
{
    return build_table_name_impl(segments);
}

std::string segments_to_jianpin(const Segments &segments)
{
    return segments_to_jianpin_impl(segments);
}

std::string get_default_db_path()
{
    return metasequoia::path_to_utf8(shuangpin::get_data_file_path(metasequoia::assets::main_dictionary));
}

void warm_up(sqlite3 *db, std::unordered_map<std::string, sqlite3_stmt *> &statement_cache)
{
    (void)intact_pinyin_set();
    (void)prefix_pinyin_set();

    if (db == nullptr)
    {
        return;
    }

    // Warm the most common single-syllable prefixes to hide first-query setup costs.
    (void)query_single_cut(db, statement_cache, Segments{"n"}, 1, QuerySource::Quanpin);
    (void)query_single_cut(db, statement_cache, Segments{"ni"}, 1, QuerySource::Quanpin);
}

std::vector<KeyedQueryItem> query_initial(sqlite3 *db, const std::string &prefix, int limit)
{
    if (db == nullptr || prefix.empty() || prefix.front() < 'a' || prefix.front() > 'z' || limit <= 0)
    {
        return {};
    }

    const std::string table = "tbl_1_" + std::string(1, prefix.front());
    const std::string sql = "SELECT \"key\", \"value\", \"weight\" FROM \"" + table +
                            "\" WHERE \"key\" >= ?1 AND \"key\" < ?2 ORDER BY \"weight\" DESC LIMIT ?3";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
    {
        return {};
    }

    const std::string upper_bound = prefix + "{";
    sqlite3_bind_text(stmt, 1, prefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, upper_bound.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, limit);

    std::vector<KeyedQueryItem> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const auto *key = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
        const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        rows.push_back(
            KeyedQueryItem{key == nullptr ? "" : key, value == nullptr ? "" : value, sqlite3_column_int64(stmt, 2)});
    }
    sqlite3_finalize(stmt);
    return rows;
}

QueryResult query_words(const std::string &pinyin, const std::string &db_path, const std::string &mode, int limit)
{
    const auto cuts = cut_pinyin_by_mode(pinyin, mode);
    QueryResult result{pinyin, mode, {}};
    if (cuts.empty())
    {
        return result;
    }

    SqliteDb db(db_path);
    for (const auto &segments : cuts)
    {
        result.results.push_back(QueryResultEntry{
            segments,
            join_segments(segments),
            build_table_name(segments),
            query_single_cut(db.get(), segments, limit, QuerySource::Quanpin),
        });
    }
    return result;
}

QueryResult query_segments(const Segments &segments, const std::string &db_path, int limit, QuerySource source)
{
    QueryResult result{join_segments(segments), "precut", {}};
    if (segments.empty())
    {
        return result;
    }

    SqliteDb db(db_path);
    result.results.push_back(QueryResultEntry{
        segments,
        join_segments(segments),
        build_table_name(segments),
        query_single_cut(db.get(), segments, limit, source),
    });
    return result;
}

std::vector<QueryItem> query_words_flat(const std::string &pinyin, const std::string &db_path, const std::string &mode,
                                        int limit)
{
    const auto result = query_words(pinyin, db_path, mode, limit);
    std::vector<QueryItem> items;
    std::unordered_set<std::string> seen;
    for (const auto &entry : result.results)
    {
        for (const auto &item : entry.items)
        {
            if (seen.insert(item.first).second)
            {
                items.push_back(item);
            }
        }
    }

    std::sort(items.begin(), items.end(),
              [](const QueryItem &lhs, const QueryItem &rhs) { return lhs.second > rhs.second; });

    if (static_cast<int>(items.size()) > limit)
    {
        items.resize(static_cast<size_t>(limit));
    }
    return items;
}

std::vector<QueryItem> query_segments_flat(const Segments &segments, const std::string &db_path, int limit,
                                           QuerySource source)
{
    const auto result = query_segments(segments, db_path, limit, source);
    std::vector<QueryItem> items;
    std::unordered_set<std::string> seen;
    for (const auto &entry : result.results)
    {
        for (const auto &item : entry.items)
        {
            if (seen.insert(item.first).second)
            {
                items.push_back(item);
            }
        }
    }

    std::sort(items.begin(), items.end(),
              [](const QueryItem &lhs, const QueryItem &rhs) { return lhs.second > rhs.second; });

    if (static_cast<int>(items.size()) > limit)
    {
        items.resize(static_cast<size_t>(limit));
    }
    return items;
}

std::vector<QueryItem> query_segments_flat(const Segments &segments, sqlite3 *db,
                                           std::unordered_map<std::string, sqlite3_stmt *> &statement_cache, int limit,
                                           QuerySource source)
{
    if (db == nullptr || segments.empty())
    {
        return {};
    }

    std::vector<QueryItem> items;
    std::unordered_set<std::string> seen;
    for (const auto &item : query_single_cut(db, statement_cache, segments, limit, source))
    {
        if (seen.insert(item.first).second)
        {
            items.push_back(item);
        }
    }

    std::sort(items.begin(), items.end(),
              [](const QueryItem &lhs, const QueryItem &rhs) { return lhs.second > rhs.second; });

    if (static_cast<int>(items.size()) > limit)
    {
        items.resize(static_cast<size_t>(limit));
    }
    return items;
}

std::vector<KeyedQueryItem> query_segments_keyed_flat(const Segments &segments, const std::string &db_path, int limit,
                                                      QuerySource source)
{
    if (segments.empty())
    {
        return {};
    }

    SqliteDb db(db_path);
    if (db.get() == nullptr)
    {
        return {};
    }

    auto items = query_single_cut_keyed(db.get(), segments, limit, source);
    deduplicate_keyed_items_by_value(items);
    std::sort(items.begin(), items.end(),
              [](const KeyedQueryItem &lhs, const KeyedQueryItem &rhs) { return lhs.weight > rhs.weight; });
    if (static_cast<int>(items.size()) > limit)
    {
        items.resize(static_cast<size_t>(limit));
    }
    return items;
}

std::vector<KeyedQueryItem> query_segments_keyed_flat(const Segments &segments, sqlite3 *db,
                                                      std::unordered_map<std::string, sqlite3_stmt *> &statement_cache,
                                                      int limit, QuerySource source)
{
    if (db == nullptr || segments.empty())
    {
        return {};
    }

    auto items = query_single_cut_keyed(db, statement_cache, segments, limit, source);
    deduplicate_keyed_items_by_value(items);
    std::sort(items.begin(), items.end(),
              [](const KeyedQueryItem &lhs, const KeyedQueryItem &rhs) { return lhs.weight > rhs.weight; });
    if (static_cast<int>(items.size()) > limit)
    {
        items.resize(static_cast<size_t>(limit));
    }
    return items;
}

std::vector<KeyedQueryItem> query_exact_segmentations_keyed_flat(
    const std::vector<Segments> &segmentations, sqlite3 *db,
    std::unordered_map<std::string, sqlite3_stmt *> &statement_cache, int limit)
{
    if (db == nullptr || segmentations.empty() || limit <= 0)
    {
        return {};
    }

    std::map<std::string, std::vector<std::string>> keys_by_table;
    std::unordered_set<std::string> seen;
    for (const auto &segments : segmentations)
    {
        if (!has_only_complete_pinyin_segments(segments))
        {
            continue;
        }
        const std::string table = build_table_name_impl(segments);
        const std::string key = join_segments(segments);
        if (table.empty() || key.empty() || !seen.insert(table + '\n' + key).second)
        {
            continue;
        }
        keys_by_table[table].push_back(key);
    }

    std::vector<KeyedQueryItem> result;
    for (const auto &[table, keys] : keys_by_table)
    {
        auto rows = run_keyed_batch_query(db, statement_cache, table, keys, limit);
        result.insert(result.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const KeyedQueryItem &lhs, const KeyedQueryItem &rhs) { return lhs.weight > rhs.weight; });
    return result;
}

// ü 的两种写法归一到词库存的那一种。intact_pinyin_list() 两种都认，可词库只存
// 一种：j/q/x/y 后面写 u（ju、jue），l/n 后面写 v（lv、lve）。切出来的 jv、lue
// 这类音节因此一行都查不到，精确键查询查空，整个词格就断在这里——wo'men'lue'de
// 解不出任何路径，长句联想直接消失。这不是模糊音降级，lue 和 lve 本来就是同一个
// 读音的两种拼写，归一化之后仍然是精确匹配。
const std::string &canonical_syllable(const std::string &syllable)
{
    static const std::unordered_map<std::string, std::string> kSpellings = {
        {"jv", "ju"},   {"qv", "qu"},   {"xv", "xu"},   {"yv", "yu"},   {"jve", "jue"},
        {"qve", "que"}, {"xve", "xue"}, {"yve", "yue"}, {"lue", "lve"}, {"nue", "nve"},
    };
    const auto found = kSpellings.find(syllable);
    return found == kSpellings.end() ? syllable : found->second;
}

WordLatticeLookup make_lattice_db_lookup(sqlite3 *db, std::unordered_map<std::string, sqlite3_stmt *> &statement_cache,
                                         int span_limit, metasequoia::FuzzyPinyinOptions fuzzy)
{
    return [db, &statement_cache, span_limit, fuzzy](const Segments &span) {
        Segments normalized;
        normalized.reserve(span.size());
        for (const auto &syllable : span)
            normalized.push_back(canonical_syllable(syllable));

        // 词格只收「键与跨度完全相等」的行。query_segments_keyed_flat 是带降级的
        // 查询：精确键查不到就退到前缀区间扫描（key >= "gun'qi" AND key < …），
        // gun'qi 于是捞出 gun'qiu 的「滚球」——拿这种行去造句，联想出来的短语读音
        // 根本对不上用户敲的拼音。简拼那两级同理。走批量精确键查询既堵住这个口子，
        // 也省掉了白扫的前缀区间。
        std::vector<Segments> keys{normalized};
        for (auto &variant : fuzzy_segmentations(normalized, fuzzy, kFuzzyLatticeVariantLimit))
        {
            for (auto &syllable : variant)
                syllable = canonical_syllable(syllable);
            keys.push_back(std::move(variant));
        }
        // 模糊变体与敲出的读音同批查（同表一条 SQL）。LIMIT 按表计，放宽到每个键一份，
        // 免得高频的模糊读音把敲出读音的行挤出去；折算之后再截回 span_limit。
        const int limit = span_limit * static_cast<int>(keys.size());
        const auto rows = query_exact_segmentations_keyed_flat(keys, db, statement_cache, limit);
        std::vector<LatticeLexeme> lexemes;
        lexemes.reserve(rows.size());
        for (const auto &row : rows)
        {
            LatticeLexeme lexeme{row.key, row.value, row.weight};
            if (keys.size() > 1)
            {
                // 每个与敲出读音不同的音节扣一份分：一个词里错两个音，比只错一个更不可能。
                const auto syllables = split_segments(row.key);
                for (size_t i = 0; i < syllables.size() && i < normalized.size(); ++i)
                    lexeme.penalty += syllables[i] == normalized[i] ? 0.0 : kFuzzyLatticePenaltyLog;
            }
            lexemes.push_back(std::move(lexeme));
        }
        if (keys.size() > 1)
        {
            // 按折后权重排序再截断：与 build_graph 一样，参与解码的只是排在前面的 span_limit 行。
            std::stable_sort(lexemes.begin(), lexemes.end(), [](const LatticeLexeme &a, const LatticeLexeme &b) {
                return static_cast<double>(a.weight) * std::pow(10.0, -a.penalty) >
                       static_cast<double>(b.weight) * std::pow(10.0, -b.penalty);
            });
            // 同一个字两种读音都收（多音字），只留排在前面的那行。
            std::unordered_set<std::string> seen;
            lexemes.erase(
                std::remove_if(lexemes.begin(), lexemes.end(),
                               [&](const LatticeLexeme &lexeme) { return !seen.insert(lexeme.value).second; }),
                lexemes.end());
            if (lexemes.size() > static_cast<size_t>(std::max(0, span_limit)))
                lexemes.resize(static_cast<size_t>(std::max(0, span_limit)));
        }
        return lexemes;
    };
}

} // namespace quanpin
