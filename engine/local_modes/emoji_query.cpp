#include "../contracts/assets/assets.h"
#include "emoji_query.h"
#include "local_database.h"

#include "../core/data_path.h"
#include "../shuangpin/shuangpin_query.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <vector>

namespace metasequoia::local_modes
{
namespace
{
bool valid_code(const std::string &code)
{
    return !code.empty() && std::all_of(code.begin(), code.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '\'';
    });
}

std::string lower_ascii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
}

LocalQueryResult query_failure(const char *diagnostic)
{
    return {{}, std::string(diagnostic)};
}

LocalQueryResult run_emoji_query(const std::string &code, SchemeType scheme, const std::filesystem::path &database_path,
                                 int limit, const ShuangpinProfile &profile, bool exact_only)
{
    if (!valid_code(code) || limit <= 0)
    {
        return {};
    }
    if (database_path.empty())
    {
        return query_failure("Emoji database is unavailable.");
    }

    const std::string lower = lower_ascii(code);
    std::vector<RankedLookup> lookups{{lower, true}};
    if (scheme == SchemeType::Shuangpin)
    {
        const std::string quanpin = shuangpin::normalize_input(lower, profile);
        if (!quanpin.empty() && quanpin != lower)
        {
            // 双拼下精确命中只认展开后的全拼：原始码恰好等于某个简拼或英文键（如 xl → 😀）时，
            // 不能压过用户想打的全拼前缀；只要精确命中时原始码干脆不查。
            if (exact_only)
                lookups.clear();
            else
                lookups.front().exact_ranks = false;
            lookups.push_back({quanpin, true});
        }
    }

    const std::shared_ptr<sqlite3> database = open_local_database(database_path);
    if (!database)
    {
        return query_failure("Emoji database is unavailable.");
    }

    // 精确命中的键排在更长的键之前。只按目录顺序（sort_order）排会把短码挤下去——
    // ku 会先出 kuxiao（苦笑）→😅，而 哭→😢 的目录位置更靠后；精确行排在 LIMIT 之前，不会被前缀行截掉。
    constexpr const char *kPrefixSql = "SELECT emoji,sort_order,key IS ?1 FROM emoji_pinyin WHERE key>=?1 AND key<?2 "
                                       "ORDER BY key IS NOT ?1,sort_order LIMIT ?3";
    constexpr const char *kExactSql = "SELECT emoji,sort_order,1 FROM emoji_pinyin WHERE key=?1 AND key<?2 "
                                      "ORDER BY sort_order LIMIT ?3";
    const auto entries = query_ranked_entries(database.get(), exact_only ? kExactSql : kPrefixSql, lookups, limit);
    if (!entries)
    {
        return query_failure("Emoji database could not be queried.");
    }
    const std::size_t count = entries->size();
    LocalQueryResult result;
    result.candidates.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        result.candidates.emplace_back(lower, (*entries)[index], static_cast<std::int64_t>(count - index),
                                       CandidateSource::Emoji);
    }
    return result;
}
} // namespace

LocalQueryResult query_emoji(const std::string &code, SchemeType scheme, int limit, const ShuangpinProfile &profile)
{
    return query_emoji(code, scheme, data_file_path(metasequoia::assets::other_dictionary), limit, profile);
}

LocalQueryResult query_emoji(const std::string &code, SchemeType scheme, const std::filesystem::path &database_path,
                             int limit, const ShuangpinProfile &profile)
{
    return run_emoji_query(code, scheme, database_path, limit, profile, false);
}

LocalQueryResult query_mixed_emoji(const std::string &code, SchemeType scheme,
                                   const std::filesystem::path &database_path, int limit,
                                   const ShuangpinProfile &profile)
{
    return run_emoji_query(code, scheme, database_path, limit, profile, true);
}

LocalQueryResult query_emoji_by_keyword(const std::string &keyword, int limit)
{
    return query_emoji_by_keyword(keyword, data_file_path(metasequoia::assets::other_dictionary), limit);
}

LocalQueryResult query_emoji_by_keyword(const std::string &keyword, const std::filesystem::path &database_path,
                                        int limit)
{
    if (keyword.empty() || limit <= 0)
    {
        return {};
    }
    if (database_path.empty())
    {
        return query_failure("Emoji database is unavailable.");
    }
    const std::shared_ptr<sqlite3> database = open_local_database(database_path);
    if (!database)
    {
        return query_failure("Emoji database is unavailable.");
    }

    // 模糊匹配：keywords 以空格分隔（建库脚本保证），只要某个关键词含有查询词就命中——「右」既命中把
    // 「右」当独立关键词的 ➡️，也命中只在「向右指」「右边」这类长关键词里带「右」的 👉。但整词命中排在
    // 子串命中之前：纯按目录顺序排会把 ➡️ 这种精确行挤到 LIMIT 之外。整词判据仍是两侧补空格，与旧的精确
    // 语义一致。转义 LIKE 通配符，中文词不含这些字符，但英文词映射接入后调用侧可能传入含通配符的串。
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

    // ?1 是转义后的查询词，在 SQL 里拼成两种 pattern：子串匹配用于 WHERE（取所有含它的行），整词匹配
    // 用于 ORDER BY 的 NOT LIKE——整词行取 0 排前、子串行取 1 排后，各自再按目录顺序（sort_order）。
    constexpr const char *kKeywordSql =
        "SELECT emoji FROM emoji WHERE keywords LIKE '%'||?1||'%' ESCAPE '\\' "
        "ORDER BY (' '||keywords||' ') NOT LIKE '% '||?1||' %' ESCAPE '\\', sort_order LIMIT ?2";
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(database.get(), kKeywordSql, -1, &statement, nullptr) != SQLITE_OK)
    {
        return query_failure("Emoji database could not be queried.");
    }
    LocalQueryResult result;
    sqlite3_bind_text(statement, 1, escaped.data(), static_cast<int>(escaped.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, limit);
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        const unsigned char *text = sqlite3_column_text(statement, 0);
        if (text == nullptr)
        {
            continue;
        }
        // emoji 表以 emoji 为主键，天然无重复；顺序即目录顺序（sort_order）。weight 不被面板读取，
        // 面板按返回顺序平铺编号，这里留 0。
        result.candidates.emplace_back(keyword, reinterpret_cast<const char *>(text), 0, CandidateSource::Emoji);
    }
    sqlite3_finalize(statement);
    return result;
}
} // namespace metasequoia::local_modes
