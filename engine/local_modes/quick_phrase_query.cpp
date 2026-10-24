#include "../contracts/assets/assets.h"
#include "quick_phrase_query.h"
#include "local_database.h"

#include "../core/data_path.h"

#include <sqlite3.h>

#include <algorithm>
#include <iterator>
#include <memory>

namespace metasequoia::local_modes
{
namespace
{
struct StatementCloser
{
    void operator()(sqlite3_stmt *statement) const
    {
        if (statement != nullptr)
        {
            sqlite3_finalize(statement);
        }
    }
};

using Statement = std::unique_ptr<sqlite3_stmt, StatementCloser>;

bool valid_code(const std::string &code)
{
    return !code.empty() && std::all_of(code.begin(), code.end(),
                                        [](unsigned char character) { return character >= 'a' && character <= 'z'; });
}

QuickPhraseQueryResult query_failure(const char *diagnostic)
{
    return {{}, std::string(diagnostic)};
}
// 前缀查询给 K 模式用，精确查询给全拼/双拼混排用；上界拼 0x7f 让 key 落在 [code, code+0x7f)。
QuickPhraseQueryResult query_quick_phrase_rows(const std::string &code, const std::filesystem::path &database_path,
                                               int limit, bool exact)
{
    if (!valid_code(code) || limit <= 0)
    {
        return {};
    }
    if (database_path.empty())
    {
        return query_failure("Quick phrase database is unavailable.");
    }

    const std::shared_ptr<sqlite3> database = open_local_database(database_path);
    if (!database)
    {
        return query_failure("Quick phrase database is unavailable.");
    }

    constexpr const char *kPrefixSql = "SELECT key,value,weight FROM quick_parases WHERE key>=?1 AND key<?2 "
                                       "ORDER BY weight DESC,key,value LIMIT ?3";
    constexpr const char *kExactSql = "SELECT key,value,weight FROM quick_parases WHERE key=?1 AND key=?2 "
                                      "ORDER BY weight DESC,value LIMIT ?3";
    sqlite3_stmt *raw_statement = nullptr;
    if (sqlite3_prepare_v2(database.get(), exact ? kExactSql : kPrefixSql, -1, &raw_statement, nullptr) != SQLITE_OK)
    {
        return query_failure("Quick phrase database could not be queried.");
    }
    Statement statement(raw_statement);
    std::string upper_bound = code;
    if (!exact)
    {
        upper_bound.push_back(static_cast<char>(0x7f));
    }
    if (sqlite3_bind_text(statement.get(), 1, code.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(statement.get(), 2, upper_bound.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int(statement.get(), 3, limit) != SQLITE_OK)
    {
        return query_failure("Quick phrase database could not be queried.");
    }

    QuickPhraseQueryResult result;
    int step_result = SQLITE_ROW;
    while ((step_result = sqlite3_step(statement.get())) == SQLITE_ROW)
    {
        const auto *key = reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0));
        const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 1));
        if (key != nullptr && value != nullptr)
        {
            result.candidates.emplace_back(key, value, sqlite3_column_int64(statement.get(), 2),
                                           CandidateSource::QuickPhrase);
        }
    }
    if (step_result != SQLITE_DONE)
    {
        return query_failure("Quick phrase database could not be queried.");
    }
    return result;
}
} // namespace

QuickPhraseQueryResult query_quick_phrases(const std::string &prefix, int limit)
{
    return query_quick_phrases(prefix, data_file_path(metasequoia::assets::main_dictionary), limit);
}

QuickPhraseQueryResult query_quick_phrases(const std::string &prefix, const std::filesystem::path &database_path,
                                           int limit)
{
    return query_quick_phrase_rows(prefix, database_path, limit, false);
}

QuickPhraseQueryResult query_quick_phrases_by_code(const std::string &code, int limit)
{
    return query_quick_phrases_by_code(code, data_file_path(metasequoia::assets::main_dictionary), limit);
}

QuickPhraseQueryResult query_quick_phrases_by_code(const std::string &code, const std::filesystem::path &database_path,
                                                   int limit)
{
    return query_quick_phrase_rows(code, database_path, limit, true);
}

bool counts_toward_quick_phrase_slot(const WordItem &item)
{
    if (item.fixed_position > 0 || item.variant_rank > 0)
    {
        return false;
    }
    switch (item.source)
    {
    case CandidateSource::QuickPhrase:
    case CandidateSource::CloudSuggestion:
    case CandidateSource::AiSuggestion:
    case CandidateSource::EnglishDictionary:
    case CandidateSource::Emoji:
    case CandidateSource::Kaomoji:
    case CandidateSource::Symbol:
    case CandidateSource::DateTime:
        return false;
    default:
        return true;
    }
}

void place_quick_phrases(std::vector<WordItem> &candidates, std::vector<WordItem> phrases, std::size_t slot)
{
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&](const WordItem &item) {
                                        return item.source == CandidateSource::QuickPhrase ||
                                               std::any_of(phrases.begin(), phrases.end(), [&](const WordItem &phrase) {
                                                   return phrase.word == item.word;
                                               });
                                    }),
                     candidates.end());
    if (phrases.empty())
    {
        return;
    }

    std::vector<WordItem> fixed;
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&](WordItem &item) {
                                        if (item.fixed_position <= 0)
                                            return false;
                                        fixed.push_back(std::move(item));
                                        return true;
                                    }),
                     candidates.end());

    std::size_t index = 0;
    for (std::size_t counted = 0; index < candidates.size() && counted < slot; ++index)
    {
        if (counts_toward_quick_phrase_slot(candidates[index]))
        {
            ++counted;
        }
    }
    candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(index), std::make_move_iterator(phrases.begin()),
                      std::make_move_iterator(phrases.end()));

    std::stable_sort(fixed.begin(), fixed.end(), [](const WordItem &left, const WordItem &right) {
        return left.fixed_position < right.fixed_position;
    });
    for (auto &item : fixed)
    {
        const std::size_t position = (std::min)(static_cast<std::size_t>(item.fixed_position - 1), candidates.size());
        candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(position), std::move(item));
    }
}
} // namespace metasequoia::local_modes
