#pragma once

#include "../core/fuzzy_pinyin_options.h"
#include "word_lattice.h"

#include <sqlite3.h>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <cstdint>

namespace quanpin
{

using Segments = std::vector<std::string>;
using QueryItem = std::pair<std::string, std::int64_t>;

struct KeyedQueryItem
{
    std::string key;
    std::string value;
    std::int64_t weight = 0;
};

enum class QuerySource
{
    Quanpin,
    Shuangpin,
};

struct QueryResultEntry
{
    Segments segments;
    std::string key;
    std::string table;
    std::vector<QueryItem> items;
};

struct QueryResult
{
    std::string pinyin;
    std::string mode;
    std::vector<QueryResultEntry> results;
};

Segments cut_pinyin_greedy(const std::string &pinyin, bool intact_only = false);
// include_correction_aliases gates the hand-mistake alias layer (transposition
// shapes, extra/missing letters) on the autocorrect switch; false keeps only the
// ü normalization (jv -> ju). It is deliberately required: every caller must
// decide whether its cut follows the switch (quanpin callers derive it with
// correction_aliases_enabled), otherwise its keys drift from the query's cut.
// Greedy mode ignores it.
std::vector<Segments> cut_pinyin_by_mode(const std::string &pinyin, const std::string &mode,
                                         bool include_correction_aliases);
Segments split_segments(const std::string &segmentation);
std::string join_segments(const Segments &segments, const std::string &delimiter = "'");
std::string build_table_name(const Segments &segments);
std::string segments_to_jianpin(const Segments &segments);
std::string get_default_db_path();
// Busy timeout for every read connection to the pinyin dictionary. Writes hold
// the lock only for a commit, so a short wait is enough; failing instead would
// return an empty candidate page.
constexpr int kDictionaryBusyTimeoutMs = 250;
void warm_up(sqlite3 *db, std::unordered_map<std::string, sqlite3_stmt *> &statement_cache);
std::vector<KeyedQueryItem> query_initial(sqlite3 *db, const std::string &prefix, int limit);
QueryResult query_words(const std::string &pinyin, const std::string &db_path, const std::string &mode = "greedy",
                        int limit = 8);
QueryResult query_segments(const Segments &segments, const std::string &db_path, int limit = 8,
                           QuerySource source = QuerySource::Quanpin);
std::vector<QueryItem> query_words_flat(const std::string &pinyin, const std::string &db_path,
                                        const std::string &mode = "greedy", int limit = 8);
std::vector<QueryItem> query_segments_flat(const Segments &segments, const std::string &db_path, int limit = 8,
                                           QuerySource source = QuerySource::Quanpin);
std::vector<QueryItem> query_segments_flat(const Segments &segments, sqlite3 *db,
                                           std::unordered_map<std::string, sqlite3_stmt *> &statement_cache,
                                           int limit = 8, QuerySource source = QuerySource::Quanpin);
std::vector<KeyedQueryItem> query_segments_keyed_flat(const Segments &segments, const std::string &db_path,
                                                      int limit = 8, QuerySource source = QuerySource::Quanpin);
std::vector<KeyedQueryItem> query_segments_keyed_flat(const Segments &segments, sqlite3 *db,
                                                      std::unordered_map<std::string, sqlite3_stmt *> &statement_cache,
                                                      int limit = 8, QuerySource source = QuerySource::Quanpin);
std::vector<KeyedQueryItem> query_exact_segmentations_keyed_flat(
    const std::vector<Segments> &segmentations, sqlite3 *db,
    std::unordered_map<std::string, sqlite3_stmt *> &statement_cache, int limit = 128);

// 词格的跨度查询：只返回键与跨度完全相等的行，没有前缀区间与简拼降级——整句
// 联想出来的每个词，读音都必须精确对上用户敲的那段拼音。因此也不需要 QuerySource：
// 区分全拼/双拼的只有简拼那几级。
// 开着模糊音时同时收跨度的模糊变体：行的 key 是词库里的真实读音，每个与敲出读音不同的音节
// 在 penalty 上记一份 kFuzzyLatticePenaltyLog。整句的 canonical_pinyin 因此落在真实读音上，
// 选中后造词不会把错音写进词库。
WordLatticeLookup make_lattice_db_lookup(sqlite3 *db, std::unordered_map<std::string, sqlite3_stmt *> &statement_cache,
                                         int span_limit, metasequoia::FuzzyPinyinOptions fuzzy = {});

} // namespace quanpin
