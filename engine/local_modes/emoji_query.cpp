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
} // namespace

LocalQueryResult query_emoji(const std::string &code, SchemeType scheme, int limit, const ShuangpinProfile &profile)
{
    return query_emoji(code, scheme, data_file_path(metasequoia::assets::other_dictionary), limit, profile);
}

LocalQueryResult query_emoji(const std::string &code, SchemeType scheme, const std::filesystem::path &database_path,
                             int limit, const ShuangpinProfile &profile)
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
            // 不能压过用户想打的全拼前缀。
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
    constexpr const char *kSql = "SELECT emoji,sort_order,key IS ?1 FROM emoji_pinyin WHERE key>=?1 AND key<?2 "
                                 "ORDER BY key IS NOT ?1,sort_order LIMIT ?3";
    const auto entries = query_ranked_entries(database.get(), kSql, lookups, limit);
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
} // namespace metasequoia::local_modes
