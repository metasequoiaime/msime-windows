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
} // namespace metasequoia::local_modes
