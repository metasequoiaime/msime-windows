#include "../contracts/assets/assets.h"
#include "symbol_query.h"
#include "local_database.h"

#include "../core/data_path.h"

#include <cstdint>
#include <memory>
#include <unordered_set>

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

// ?1 = 编码，?2 = 前缀上界，?3 = limit。返回 symbol 一列，同一符号只取排位最好的那行。
LocalQueryResult run_query(const std::string &code, const std::filesystem::path &database_path, const char *sql,
                           int limit)
{
    const std::string lower = normalize_code(code);
    if (lower.empty() || limit <= 0)
        return {};
    if (database_path.empty())
        return query_failure("Symbol database is unavailable.");
    const std::shared_ptr<sqlite3> database = open_local_database(database_path);
    if (!database)
        return query_failure("Symbol database is unavailable.");

    sqlite3_stmt *raw_statement = nullptr;
    if (sqlite3_prepare_v2(database.get(), sql, -1, &raw_statement, nullptr) != SQLITE_OK)
        return query_failure("Symbol database could not be queried.");
    const std::unique_ptr<sqlite3_stmt, StatementCloser> statement(raw_statement);
    std::string upper_bound = lower;
    upper_bound.push_back(static_cast<char>(0x7f));
    // 前缀查询的同一符号可能挂在好几个编码下（a 与 ao、codex 的 alpha 与 Alpha），多取一些再去重。
    const int fetch_limit = limit <= (1 << 20) ? limit * 4 : limit;
    if (sqlite3_bind_text(statement.get(), 1, lower.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(statement.get(), 2, upper_bound.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int(statement.get(), 3, fetch_limit) != SQLITE_OK)
        return query_failure("Symbol database could not be queried.");

    LocalQueryResult result;
    std::unordered_set<std::string> seen;
    int step_result = SQLITE_ROW;
    while ((step_result = sqlite3_step(statement.get())) == SQLITE_ROW &&
           result.candidates.size() < static_cast<std::size_t>(limit))
    {
        const auto *symbol = reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0));
        if (symbol == nullptr || !seen.insert(symbol).second)
            continue;
        result.candidates.emplace_back(lower, symbol, 0, CandidateSource::Symbol);
    }
    if (step_result != SQLITE_ROW && step_result != SQLITE_DONE)
        return query_failure("Symbol database could not be queried.");
    const std::size_t count = result.candidates.size();
    for (std::size_t index = 0; index < count; ++index)
        result.candidates[index].weight = static_cast<std::int64_t>(count - index);
    return result;
}
} // namespace

LocalQueryResult query_symbols(const std::string &code, int limit)
{
    return query_symbols(code, data_file_path(metasequoia::assets::other_dictionary), limit);
}

LocalQueryResult query_symbols(const std::string &code, const std::filesystem::path &database_path, int limit)
{
    // 整串命中的编码排最前；其余前缀命中按编码长短，短的通常是这一类的总表（x 先出 xl 再出 xlc）。
    constexpr const char *kSql = "SELECT symbol FROM symbol_code WHERE code>=?1 AND code<?2 "
                                 "ORDER BY code IS NOT ?1,length(code),sort_order LIMIT ?3";
    return run_query(code, database_path, kSql, limit);
}

LocalQueryResult query_mixed_symbols(const std::string &code, const std::filesystem::path &database_path, int limit)
{
    constexpr const char *kSql = "SELECT symbol FROM symbol_code WHERE code=?1 AND code<?2 AND mixed=1 "
                                 "ORDER BY sort_order LIMIT ?3";
    return run_query(code, database_path, kSql, limit);
}

LocalQueryResult query_symbols_by_keyword(const std::string &keyword, int limit)
{
    return query_symbols_by_keyword(keyword, data_file_path(metasequoia::assets::other_dictionary), limit);
}

LocalQueryResult query_symbols_by_keyword(const std::string &keyword, const std::filesystem::path &database_path,
                                          int limit)
{
    if (keyword.empty() || limit <= 0)
    {
        return {};
    }
    if (database_path.empty())
    {
        return query_failure("Symbol database is unavailable.");
    }
    const std::shared_ptr<sqlite3> database = open_local_database(database_path);
    if (!database)
    {
        return query_failure("Symbol database is unavailable.");
    }

    // 与 query_kaomoji_by_keyword 同构：symbol_catalog.keywords 以空格分隔（建库脚本保证），做子串匹配——
    // 只要某个关键词含有查询词就命中。整词命中排在子串命中之前，避免精确行被 LIMIT 截掉；整词判据仍是
    // 两侧补空格。转义 LIKE 通配符，中文词不含这些字符，但英文词映射接入后调用侧可能传入含通配符的串。
    std::string escaped;
    escaped.reserve(keyword.size() + 8);
    for (const char ch : keyword)
    {
        if (ch == '%' || ch == '_' || ch == '\\')
        {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }

    // ?1 是转义后的查询词，在 SQL 里拼成两种 pattern：子串匹配用于 WHERE，整词匹配用于 ORDER BY 的
    // NOT LIKE——整词行取 0 排前、子串行取 1 排后，各自再按目录顺序（sort_order）。symbol_catalog 主键是
    // (symbol, category)，同一符号可能挂在多个类别下，按符号去重，只保留排位最好的那行。
    constexpr const char *kKeywordSql = "SELECT symbol FROM symbol_catalog WHERE keywords LIKE '%'||?1||'%' "
                                        "ESCAPE '\\' ORDER BY (' '||keywords||' ') NOT LIKE '% '||?1||' %' "
                                        "ESCAPE '\\', sort_order LIMIT ?2";
    sqlite3_stmt *raw_statement = nullptr;
    if (sqlite3_prepare_v2(database.get(), kKeywordSql, -1, &raw_statement, nullptr) != SQLITE_OK)
    {
        return query_failure("Symbol database could not be queried.");
    }
    const std::unique_ptr<sqlite3_stmt, StatementCloser> statement(raw_statement);
    LocalQueryResult result;
    std::unordered_set<std::string> seen;
    sqlite3_bind_text(statement.get(), 1, escaped.data(), static_cast<int>(escaped.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(statement.get(), 2, limit);
    while (sqlite3_step(statement.get()) == SQLITE_ROW)
    {
        const unsigned char *text = sqlite3_column_text(statement.get(), 0);
        if (text == nullptr)
        {
            continue;
        }
        // 顺序即目录顺序（sort_order），整词命中已排在前面。weight 不被面板读取，面板按返回顺序平铺
        // 编号，这里留 0。
        const std::string symbol(reinterpret_cast<const char *>(text));
        if (!seen.insert(symbol).second)
        {
            continue;
        }
        result.candidates.emplace_back(keyword, symbol, 0, CandidateSource::Symbol);
    }
    return result;
}
} // namespace metasequoia::local_modes
