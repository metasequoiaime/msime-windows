#include "../contracts/assets/assets.h"
#include "kaomoji_query.h"
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
} // namespace

LocalQueryResult query_kaomoji(const std::string &code, SchemeType scheme, int limit, const ShuangpinProfile &profile)
{
    return query_kaomoji(code, scheme, data_file_path(metasequoia::assets::other_dictionary), limit, profile);
}

LocalQueryResult query_kaomoji(const std::string &code, SchemeType scheme, const std::filesystem::path &database_path,
                               int limit, const ShuangpinProfile &profile)
{
    if (!valid_code(code) || limit <= 0)
    {
        return {};
    }
    if (database_path.empty())
    {
        return query_failure("Kaomoji database is unavailable.");
    }

    const std::string lower = lower_ascii(code);
    std::vector<RankedLookup> lookups{{lower, true}};
    if (scheme == SchemeType::Shuangpin)
    {
        const std::string quanpin = shuangpin::normalize_input(lower, profile);
        if (!quanpin.empty() && quanpin != lower)
        {
            // 同 emoji：双拼下精确命中只认展开后的全拼，原始码只按目录顺序参与前缀命中。
            lookups.front().exact_ranks = false;
            lookups.push_back({quanpin, true});
        }
    }

    const std::shared_ptr<sqlite3> database = open_local_database(database_path);
    if (!database)
    {
        return query_failure("Kaomoji database is unavailable.");
    }

    // 同 emoji：全拼精确命中排在更长键的前缀命中之前——bai 的精确命中不能被 baifo 顶掉。
    // 简拼精确命中不提级：单音节条目的简拼就是一个字母，打 k 时所有 ku 的条目都会精确命中 jianpin。
    constexpr const char *kSql = "SELECT kaomoji,sort_order,pinyin IS ?1 FROM kaomoji "
                                 "WHERE (pinyin>=?1 AND pinyin<?2) OR (jianpin>=?1 AND jianpin<?2) "
                                 "ORDER BY pinyin IS NOT ?1,sort_order LIMIT ?3";
    const auto entries = query_ranked_entries(database.get(), kSql, lookups, limit);
    if (!entries)
    {
        return query_failure("Kaomoji database could not be queried.");
    }
    const std::size_t count = entries->size();
    LocalQueryResult result;
    result.candidates.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        result.candidates.emplace_back(lower, (*entries)[index], static_cast<std::int64_t>(count - index),
                                       CandidateSource::Kaomoji);
    }
    return result;
}

LocalQueryResult query_kaomoji_by_keyword(const std::string &keyword, int limit)
{
    return query_kaomoji_by_keyword(keyword, data_file_path(metasequoia::assets::other_dictionary), limit);
}

LocalQueryResult query_kaomoji_by_keyword(const std::string &keyword, const std::filesystem::path &database_path,
                                          int limit)
{
    if (keyword.empty() || limit <= 0)
    {
        return {};
    }
    if (database_path.empty())
    {
        return query_failure("Kaomoji database is unavailable.");
    }
    const std::shared_ptr<sqlite3> database = open_local_database(database_path);
    if (!database)
    {
        return query_failure("Kaomoji database is unavailable.");
    }

    // 与 query_emoji_by_keyword 同构：kaomoji_catalog.keywords 以空格分隔（建库脚本保证），做子串匹配——
    // 只要某个关键词含有查询词就命中（「右」既命中把「右」当独立关键词的行，也命中只在「往右瞄」这类长
    // 关键词里带「右」的行）。整词命中排在子串命中之前，避免精确行被 LIMIT 截掉；整词判据仍是两侧补空格。
    // 转义 LIKE 通配符，中文词不含这些字符，但英文词映射接入后调用侧可能传入含通配符的串。
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
    // NOT LIKE——整词行取 0 排前、子串行取 1 排后，各自再按目录顺序（sort_order）。
    constexpr const char *kKeywordSql = "SELECT kaomoji FROM kaomoji_catalog WHERE keywords LIKE '%'||?1||'%' "
                                        "ESCAPE '\\' ORDER BY (' '||keywords||' ') NOT LIKE '% '||?1||' %' "
                                        "ESCAPE '\\', sort_order LIMIT ?2";
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(database.get(), kKeywordSql, -1, &statement, nullptr) != SQLITE_OK)
    {
        return query_failure("Kaomoji database could not be queried.");
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
        // kaomoji_catalog 以 kaomoji 为主键，天然无重复；顺序即目录顺序（sort_order）。weight 不被面板
        // 读取，面板按返回顺序平铺编号，这里留 0。
        result.candidates.emplace_back(keyword, reinterpret_cast<const char *>(text), 0, CandidateSource::Kaomoji);
    }
    sqlite3_finalize(statement);
    return result;
}
} // namespace metasequoia::local_modes
