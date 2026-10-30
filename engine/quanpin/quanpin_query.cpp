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

// 非纠错的拼写归一：ü 归一（jv -> ju）。它不属于手误纠正，永远生效——关掉纠错开关
// 也要把 jv 当 ju 查库（jv 本身不是合法音节，靠这条归一才能切出来）。n/l 系的
// nue/lue -> nve/lve 由 normalize_umlaut_aliases 在段级完成，不进这张表。
const CorrectionAliases &pinyin_normalization_aliases()
{
    static const CorrectionAliases kAliases = [] {
        CorrectionAliases aliases;
        aliases["jv"].push_back("ju");
        return aliases;
    }();
    return kAliases;
}

// 手误别名：换位形状（aing/uang/ian/uan/iao/agn/egn/on）、多一个 h、漏一个 g，外加
// 上面那条 ü 归一。属纠错，按掩码门控（开关关掉时不参与切分）。
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
        // ü 归一也留在这张表里，纠错开着时一张表就够；关掉时只查 pinyin_normalization_aliases。
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

std::vector<Segments> cut_one_piece_with_corrections(const std::string &pinyin, bool include_correction_aliases)
{
    struct RankedPath
    {
        Segments segments;
        std::vector<size_t> typed_lengths;
        std::vector<size_t> correction_ranks;
    };

    const auto &valid_pinyin = intact_pinyin_set();
    // include=false keeps only the ü normalization (jv -> ju); the hand-mistake
    // aliases are part of autocorrection and follow its switch.
    const auto &aliases = include_correction_aliases ? pinyin_correction_aliases() : pinyin_normalization_aliases();
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

std::vector<Segments> cut_pinyin_with_corrections(const std::string &pinyin, bool include_correction_aliases)
{
    std::vector<Segments> merged_paths = {Segments{}};
    for (const auto &part : split(pinyin, '\''))
    {
        const auto part_paths = cut_one_piece_with_corrections(part, include_correction_aliases);
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

bool is_two_letter_initial(const std::string &segment)
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
        return source == QuerySource::Shuangpin && is_two_letter_initial(segment);
    });
}

// 全拼的简拼形态：至少两段，且非末尾段里有单字母段或 zh/ch/sh 声母段（zhly = 张靓颖）。
// 末尾段是待补全的音节，由前缀区间负责，不算简拼意图。
bool quanpin_jianpin_shaped(const Segments &segments)
{
    if (segments.size() <= 1)
    {
        return false;
    }

    return std::any_of(segments.begin(), segments.end() - 1, [](const std::string &segment) {
        return segment.size() == 1 || is_two_letter_initial(segment);
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

        const bool is_strict_shuangpin_initial = source == QuerySource::Shuangpin && is_two_letter_initial(expected);
        // 全拼把 zh/ch/sh 当声母段时按声母比，而不是整段相等：zhg 的 zh 要认 zhang。
        const bool is_quanpin_two_letter_initial = source == QuerySource::Quanpin && is_two_letter_initial(expected);
        if (expected.size() == 1 || is_strict_shuangpin_initial || is_quanpin_two_letter_initial)
        {
            if (source == QuerySource::Shuangpin || is_quanpin_two_letter_initial)
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

// 全拼简拼与前缀同层合并时最多扫多少行 jp 命中。jp 是索引列，等值命中量由词库决定，
// 常用两键输入在一两百到一千多行；2048 只给异常词库兜底，正常词库不会截掉候选，
// 同时避免按键路径上无上限地扫一张异常大的表。
constexpr int kQuanpinJianpinScanCeiling = 2048;

int quanpin_jianpin_scan_limit(int limit)
{
    return std::min(build_mixed_jianpin_scan_limit(limit), kQuanpinJianpinScanCeiling);
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

// 全拼的简拼召回与全拼前缀同层：按词去重保留最大权重，再按权重降序。平局用稳定排序，
// 前缀行排在前面，因为它按用户真正敲到的音节切分，比整串首字母更贴字面。搜狗把全拼与
// 简拼候选放在同一层按词频排，这里对齐它，不再让前缀命中压掉整条简拼候选。
std::vector<KeyedQueryItem> merge_jianpin_into_prefix(std::vector<KeyedQueryItem> prefix_rows,
                                                      std::vector<KeyedQueryItem> jianpin_rows, int limit)
{
    std::unordered_map<std::string, std::size_t> position_by_value;
    position_by_value.reserve(prefix_rows.size() + jianpin_rows.size());
    std::vector<KeyedQueryItem> merged;
    merged.reserve(prefix_rows.size() + jianpin_rows.size());
    const auto append = [&](std::vector<KeyedQueryItem> &rows) {
        for (auto &row : rows)
        {
            const auto inserted = position_by_value.emplace(row.value, merged.size());
            if (inserted.second)
            {
                merged.push_back(std::move(row));
            }
            else if (row.weight > merged[inserted.first->second].weight)
            {
                merged[inserted.first->second] = std::move(row);
            }
        }
    };
    append(prefix_rows);
    append(jianpin_rows);
    std::stable_sort(merged.begin(), merged.end(),
                     [](const KeyedQueryItem &lhs, const KeyedQueryItem &rhs) { return lhs.weight > rhs.weight; });
    if (limit > 0 && static_cast<int>(merged.size()) > limit)
    {
        merged.resize(static_cast<std::size_t>(limit));
    }
    return merged;
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
    const auto jp_sql = "SELECT \"key\", \"value\", \"weight\" FROM \"" + table +
                        "\" WHERE \"jp\" = ? ORDER BY \"weight\" DESC LIMIT ?";

    if (source == QuerySource::Quanpin)
    {
        // 全拼：按真正的键前缀做区间扫描。build_key_like_pattern 里的 % 是给 LIKE 通配符
        // 准备的，用在这个字符串区间比较上会把区间算空，所以全拼不经过它。
        auto prefix_rows = run_keyed_query(db, prefix_sql, key, build_key_prefix_upper_bound(key), limit);
        if (!quanpin_jianpin_shaped(segments))
        {
            return prefix_rows;
        }
        auto jianpin_rows = filter_mixed_jianpin_rows(
            run_keyed_query(db, jp_sql, jp, quanpin_jianpin_scan_limit(limit)), segments, limit, source);
        return merge_jianpin_into_prefix(std::move(prefix_rows), std::move(jianpin_rows), limit);
    }

    // 双拼保持原有分层：前缀命中即返回，简拼只在空手时兜底。
    rows = run_keyed_query(db, prefix_sql, key_prefix, key_prefix_upper_bound, limit);
    if (!rows.empty())
    {
        return rows;
    }

    if (needs_mixed_query)
    {
        rows = filter_mixed_jianpin_rows(run_keyed_query(db, jp_sql, jp, mixed_query_limit), segments, limit, source);
        if (!rows.empty())
        {
            return rows;
        }
    }

    if (!is_pure_jianpin(segments))
    {
        return {};
    }

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
    const auto jp_sql = "SELECT \"key\", \"value\", \"weight\" FROM \"" + table +
                        "\" WHERE \"jp\" = ? ORDER BY \"weight\" DESC LIMIT ?";

    if (source == QuerySource::Quanpin)
    {
        // 与不带 statement cache 的重载保持同一套全拼语义，改动要两边同步。
        auto prefix_rows =
            run_keyed_query(db, statement_cache, prefix_sql, key, build_key_prefix_upper_bound(key), limit);
        if (!quanpin_jianpin_shaped(segments))
        {
            return prefix_rows;
        }
        auto jianpin_rows = filter_mixed_jianpin_rows(
            run_keyed_query(db, statement_cache, jp_sql, jp, quanpin_jianpin_scan_limit(limit)), segments, limit,
            source);
        return merge_jianpin_into_prefix(std::move(prefix_rows), std::move(jianpin_rows), limit);
    }

    // 双拼保持原有分层：前缀命中即返回，简拼只在空手时兜底。
    rows = run_keyed_query(db, statement_cache, prefix_sql, key_prefix, key_prefix_upper_bound, limit);
    if (!rows.empty())
    {
        return rows;
    }

    if (needs_mixed_query)
    {
        rows = filter_mixed_jianpin_rows(run_keyed_query(db, statement_cache, jp_sql, jp, mixed_query_limit), segments,
                                         limit, source);
        if (!rows.empty())
        {
            return rows;
        }
    }

    if (!is_pure_jianpin(segments))
    {
        return {};
    }

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

std::vector<Segments> cut_pinyin_by_mode(const std::string &pinyin, const std::string &mode,
                                         bool include_correction_aliases)
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

    const auto corrected_paths = cut_pinyin_with_corrections(pinyin, include_correction_aliases);
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
    // 离线查询工具，不挂在任何会话的纠错开关上。
    const auto cuts = cut_pinyin_by_mode(pinyin, mode, true);
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

std::vector<KeyedQueryItem> query_key_range_keyed(sqlite3 *db,
                                                  std::unordered_map<std::string, sqlite3_stmt *> &statement_cache,
                                                  const std::string &table, const std::string &lower,
                                                  const std::string &upper, const int limit)
{
    if (db == nullptr || table.empty() || lower.empty() || lower >= upper || limit <= 0)
    {
        return {};
    }
    const std::string sql = "SELECT \"key\", \"value\", \"weight\" FROM \"" + table +
                            "\" WHERE \"key\" >= ?1 AND \"key\" < ?2 ORDER BY \"weight\" DESC LIMIT ?3";
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
    sqlite3_bind_text(stmt, 1, lower.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, upper.c_str(), -1, SQLITE_TRANSIENT);
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

std::shared_ptr<const CharReadingTotals> load_char_reading_totals(sqlite3 *db)
{
    auto totals = std::make_shared<CharReadingTotals>();
    if (db == nullptr)
        return totals;

    std::vector<std::string> tables;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(
            db, "SELECT name FROM sqlite_master WHERE type = 'table' AND name LIKE 'tbl\\_1\\_%' ESCAPE '\\'", -1,
            &statement, nullptr) != SQLITE_OK)
        return totals;
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        const auto *name = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
        if (name != nullptr)
            tables.emplace_back(name);
    }
    sqlite3_finalize(statement);

    // 先按字累计权重和读音数，再只留多音字：单读音的字占比恒为 1，存着只占内存。
    std::unordered_map<std::string, std::pair<std::int64_t, int>> sums;
    for (const auto &table : tables)
    {
        const std::string sql = "SELECT value, weight FROM " + table;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
            continue;
        while (sqlite3_step(statement) == SQLITE_ROW)
        {
            const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
            if (value == nullptr)
                continue;
            auto &[sum, readings] = sums[value];
            sum += (std::max)(sqlite3_column_int64(statement, 1), sqlite3_int64{0});
            ++readings;
        }
        sqlite3_finalize(statement);
    }
    for (const auto &[value, sum] : sums)
    {
        if (sum.second > 1)
            totals->emplace(value, sum.first);
    }
    return totals;
}

namespace
{

// P(读音|字) 的免罚线与扣分上限（log10）。
//
// 已有的 reading_prior 看的是 P(字|拼音)：这一行占该拼音全部行的权重比。它分得出权重为 0 的
// 「卷」读 gun，却分不出「大」读 tai——大 的 tai 行有 4719，占 tai 的 0.47%，只扣 0.33，翻不过
// 语言模型对「大赚」的偏好（差 1.6 左右），tai'zhuan'qian'le 于是出「大赚钱了」。同一行换个
// 角度看，大 的 da 行有 327 万，读 tai 的只占 0.14%：这才是「这个字几乎不这么读」。语言模型
// 只认汉字，这一维只能由读音补上（噪声信道里的 P(拼音|字)）。
//
// 免罚线定在 10%：长 chang 42%、重 chong 20%、行 hang 15% 这类常见的第二读音一分不扣，
// 觉 jiao、还 huan（8%~9%）只扣零点零几，着 zhao/zhuo、地 di（3%~4%）扣 0.4~0.5。真正的
// 生僻读音——大 tai/dai、会 kuai、都 du、给 ji——都在 0.1%~0.3%，扣 1.5~2。了 liao（0.47%）
// 扣 1.3，「受不了」「去不了」仍靠语言模型的上下文赢下 料/聊。5% 那一档还差一点翻不过
// 「大赚钱了」，20% 在评测句上与 10% 结果相同，取保守的一边。上限只管权重为 0 的读音，那些
// reading_prior 已经扣过一大截。
//
// 只罚单字：多音字的少见读音落在词里（会计、成都、大夫）时整词就是读音的证据。没有同音
// 竞争的字（得 只有它一个读 dei）各条路径扣得一样多，排序不受影响。
constexpr double kRareCharReadingShare = 0.1;
constexpr double kRareCharReadingFloor = 2.5;

double rare_char_reading_penalty(const LatticeLexeme &lexeme, const CharReadingTotals *totals)
{
    if (totals == nullptr)
        return 0.0;
    const auto found = totals->find(lexeme.value);
    if (found == totals->end() || found->second <= 0)
        return 0.0;
    const double weight = lexeme.weight > 0 ? static_cast<double>(lexeme.weight) : 0.0;
    const double share = (weight + 1.0) / (static_cast<double>(found->second) + 1.0);
    if (share >= kRareCharReadingShare)
        return 0.0;
    return (std::min)(-std::log10(share / kRareCharReadingShare), kRareCharReadingFloor);
}

} // namespace

WordLatticeLookup make_lattice_db_lookup(sqlite3 *db, std::unordered_map<std::string, sqlite3_stmt *> &statement_cache,
                                         int span_limit, metasequoia::FuzzyPinyinOptions fuzzy,
                                         std::shared_ptr<const CharReadingTotals> char_totals)
{
    return [db, &statement_cache, span_limit, fuzzy, char_totals = std::move(char_totals)](const Segments &span) {
        // ü 的两种写法归一到词库存的那一种。intact_pinyin_list() 两种都认，可词库只存
        // 一种：切出来的 jv、lue 这类音节一行都查不到，精确键查询查空，整个词格就断在
        // 这里——wo'men'lue'de 解不出任何路径，长句联想直接消失。这不是模糊音降级，lue
        // 和 lve 本来就是同一个读音的两种拼写，归一化之后仍然是精确匹配。
        Segments normalized;
        normalized.reserve(span.size());
        for (const auto &syllable : span)
            normalized.push_back(canonical_umlaut_spelling(syllable));

        // 词格只收「键与跨度完全相等」的行。query_segments_keyed_flat 是带降级的
        // 查询：精确键查不到就退到前缀区间扫描（key >= "gun'qi" AND key < …），
        // gun'qi 于是捞出 gun'qiu 的「滚球」——拿这种行去造句，联想出来的短语读音
        // 根本对不上用户敲的拼音。简拼那两级同理。走批量精确键查询既堵住这个口子，
        // 也省掉了白扫的前缀区间。
        std::vector<Segments> keys{normalized};
        for (auto &variant : fuzzy_segmentations(normalized, fuzzy, kFuzzyLatticeVariantLimit))
        {
            for (auto &syllable : variant)
                syllable = canonical_umlaut_spelling(std::move(syllable));
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
            if (span.size() == 1)
                lexeme.penalty += rare_char_reading_penalty(lexeme, char_totals.get());
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
