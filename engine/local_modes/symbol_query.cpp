#include "../contracts/assets/assets.h"
#include "symbol_query.h"
#include "local_database.h"

#include "../core/data_path.h"
#include "../shuangpin/shuangpin_query.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

namespace metasequoia::local_modes
{
namespace
{
struct StatementCloser
{
    void operator()(sqlite3_stmt *statement) const
    {
        sqlite3_finalize(statement);
    }
};

// 编码只有字母；统一转小写，非字母返回空串（查不到任何符号）。
std::string normalize_code(const std::string &code)
{
    std::string lower;
    lower.reserve(code.size());
    for (const unsigned char character : code)
    {
        if (character >= 'a' && character <= 'z')
            lower.push_back(static_cast<char>(character));
        else if (character >= 'A' && character <= 'Z')
            lower.push_back(static_cast<char>(character - 'A' + 'a'));
        else
            return {};
    }
    return lower;
}

LocalQueryResult query_failure(const char *diagnostic)
{
    return {{}, std::string(diagnostic)};
}

// 一个查询码查到的符号，按 SQL 的排位，整串等于编码的（exact）与仅前缀命中的分开。
struct LookupRows
{
    std::vector<std::string> exact;
    std::vector<std::string> prefix;
};

// ?1 = 编码，?2 = 前缀上界，?3 = limit。SQL 返回 symbol 与「整串等于编码」两列。
bool fetch_rows(sqlite3 *database, const char *sql, const std::string &code, int fetch_limit, LookupRows &rows)
{
    sqlite3_stmt *raw_statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &raw_statement, nullptr) != SQLITE_OK)
        return false;
    const std::unique_ptr<sqlite3_stmt, StatementCloser> statement(raw_statement);
    std::string upper_bound = code;
    upper_bound.push_back(static_cast<char>(0x7f));
    if (sqlite3_bind_text(statement.get(), 1, code.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(statement.get(), 2, upper_bound.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int(statement.get(), 3, fetch_limit) != SQLITE_OK)
        return false;
    int step_result = SQLITE_ROW;
    while ((step_result = sqlite3_step(statement.get())) == SQLITE_ROW)
    {
        const auto *symbol = reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0));
        if (symbol == nullptr)
            continue;
        (sqlite3_column_int(statement.get(), 1) != 0 ? rows.exact : rows.prefix).emplace_back(symbol);
    }
    return step_result == SQLITE_DONE;
}

// lookups 依次是这次输入的几种读法（原始编码在前，双拼读出的全拼在后）。整串命中排在仅前缀命中之前，
// 各层里按读法的先后；同一符号只取排位最好的那次。候选的 pinyin 记用户敲的 display_code。
LocalQueryResult run_query(const std::vector<std::string> &lookups, const std::string &display_code,
                           const std::filesystem::path &database_path, const char *sql, int limit)
{
    if (lookups.empty() || limit <= 0)
        return {};
    if (database_path.empty())
        return query_failure("Symbol database is unavailable.");
    const std::shared_ptr<sqlite3> database = open_local_database(database_path);
    if (!database)
        return query_failure("Symbol database is unavailable.");

    // 前缀查询的同一符号可能挂在好几个编码下（a 与 ao、codex 的 alpha 与 Alpha），多取一些再去重。
    const int fetch_limit = limit <= (1 << 20) ? limit * 4 : limit;
    std::vector<LookupRows> rows(lookups.size());
    for (std::size_t index = 0; index < lookups.size(); ++index)
    {
        if (!fetch_rows(database.get(), sql, lookups[index], fetch_limit, rows[index]))
            return query_failure("Symbol database could not be queried.");
    }

    LocalQueryResult result;
    std::unordered_set<std::string> seen;
    const auto take = [&](const std::vector<std::string> &symbols) {
        for (const std::string &symbol : symbols)
        {
            if (result.candidates.size() >= static_cast<std::size_t>(limit))
                return;
            if (seen.insert(symbol).second)
                result.candidates.emplace_back(display_code, symbol, 0, CandidateSource::Symbol);
        }
    };
    for (const LookupRows &lookup : rows)
        take(lookup.exact);
    for (const LookupRows &lookup : rows)
        take(lookup.prefix);
    const std::size_t count = result.candidates.size();
    for (std::size_t index = 0; index < count; ++index)
        result.candidates[index].weight = static_cast<std::int64_t>(count - index);
    return result;
}

// 整串命中的编码排最前；其余前缀命中按编码长短，短的通常是这一类的总表（x 先出 xl 再出 xlc）。
constexpr const char *kPrefixSql = "SELECT symbol,code IS ?1 FROM symbol_code WHERE code>=?1 AND code<?2 "
                                   "ORDER BY code IS NOT ?1,length(code),sort_order LIMIT ?3";
} // namespace

LocalQueryResult query_symbols(const std::string &code, int limit)
{
    return query_symbols(code, data_file_path(metasequoia::assets::other_dictionary), limit);
}

LocalQueryResult query_symbols(const std::string &code, const std::filesystem::path &database_path, int limit)
{
    const std::string lower = normalize_code(code);
    if (lower.empty())
        return {};
    return run_query({lower}, lower, database_path, kPrefixSql, limit);
}

LocalQueryResult query_symbols(const std::string &code, SchemeType scheme, const ShuangpinProfile &profile, int limit)
{
    return query_symbols(code, scheme, profile, data_file_path(metasequoia::assets::other_dictionary), limit);
}

LocalQueryResult query_symbols(const std::string &code, SchemeType scheme, const ShuangpinProfile &profile,
                               const std::filesystem::path &database_path, int limit)
{
    if (scheme != SchemeType::Shuangpin)
        return query_symbols(code, database_path, limit);
    // 双拼下编码先原样查（分组码 xl、jt 和 typst 名 alpha 照旧），再按双拼读成全拼查（小鹤 sj → san 的 ③）。
    // 原始编码恰好也是分组码时（sj 是时间符号组）它排前面，读出来的全拼翻一页可见。; 只在用它当 ing 韵母
    // 的方案里是编码键，只出现在双拼读法里。
    const bool semicolon_final = ShuangpinProfileUsesSemicolonFinal(profile);
    std::string display;
    display.reserve(code.size());
    for (const unsigned char character : code)
    {
        if (character >= 'A' && character <= 'Z')
            display.push_back(static_cast<char>(character - 'A' + 'a'));
        else if ((character >= 'a' && character <= 'z') || (semicolon_final && character == ';'))
            display.push_back(static_cast<char>(character));
        else
            return {};
    }
    if (display.empty())
        return {};
    std::vector<std::string> lookups;
    if (display.find(';') == std::string::npos)
        lookups.push_back(display);
    for (std::string &quanpin : shuangpin::decode_syllable_pairs(display, profile))
    {
        if (std::find(lookups.begin(), lookups.end(), quanpin) == lookups.end())
            lookups.push_back(std::move(quanpin));
    }
    return run_query(lookups, display, database_path, kPrefixSql, limit);
}

LocalQueryResult query_mixed_symbols(const std::string &code, const std::filesystem::path &database_path, int limit)
{
    constexpr const char *kSql = "SELECT symbol,1 FROM symbol_code WHERE code=?1 AND code<?2 AND mixed=1 "
                                 "ORDER BY sort_order LIMIT ?3";
    const std::string lower = normalize_code(code);
    if (lower.empty())
        return {};
    return run_query({lower}, lower, database_path, kSql, limit);
}
} // namespace metasequoia::local_modes
