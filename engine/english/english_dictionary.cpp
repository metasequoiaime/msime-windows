#include "../contracts/assets/assets.h"
#include "english_dictionary.h"
#include "../core/data_path.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <tuple>
#include <spdlog/spdlog.h>
#include <utility>

namespace
{
bool IsLowerAsciiWord(const std::string &value)
{
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; });
}

std::vector<WordItem> QueryPrefix(sqlite3_stmt *statement, const std::string &prefix, size_t limit)
{
    const std::string upper_bound = prefix + "{";
    const int sqlite_limit =
        static_cast<int>((std::min)(limit, static_cast<size_t>((std::numeric_limits<int>::max)())));

    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    if (sqlite3_bind_text(statement, 1, prefix.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(statement, 2, upper_bound.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int(statement, 3, sqlite_limit) != SQLITE_OK)
    {
        sqlite3_reset(statement);
        return {};
    }

    std::vector<WordItem> candidates;
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW)
    {
        const auto *word = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
        const auto *display = reinterpret_cast<const char *>(sqlite3_column_text(statement, 1));
        if (word == nullptr || display == nullptr)
        {
            continue;
        }
        candidates.emplace_back(word, display, sqlite3_column_int64(statement, 2), CandidateSource::EnglishDictionary);
    }

    sqlite3_reset(statement);
    if (result != SQLITE_DONE)
    {
        (void)0;
        return {};
    }
    return candidates;
}
} // namespace

EnglishDictionary::EnglishDictionary(std::string db_path, bool initialize_schema, std::string translations_path)
    : translations_path_(std::move(translations_path)), db_path_(std::move(db_path))
{
    if (initialize_schema)
    {
        (void)ensure_schema(db_path_);
    }
    load_custom_translations();
}

EnglishDictionary::~EnglishDictionary()
{
    close_database();
}

std::vector<WordItem> EnglishDictionary::query_prefix(const std::string &prefix, size_t limit)
{
    if (!IsLowerAsciiWord(prefix) || limit == 0 || !ensure_query_statement())
        return {};
    return QueryPrefix(query_statement_, prefix, limit);
}

std::vector<WordItem> EnglishDictionary::query_mixed_prefix(const std::string &prefix, size_t limit)
{
    if (!IsLowerAsciiWord(prefix) || limit == 0 || !ensure_mixed_query_statement())
        return {};
    return QueryPrefix(mixed_query_statement_, prefix, (std::min)(limit, kMixedCandidateLimit));
}

std::vector<WordItem> EnglishDictionary::query_candidates(const std::string &input, size_t limit, bool mixed,
                                                          bool reconvert)
{
    std::string prefix = input;
    std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                   [](unsigned char ch) { return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch); });
    auto exact = mixed ? query_mixed_prefix(prefix, limit) : query_prefix(prefix, limit);
    if ((!exact.empty() && !reconvert) || limit == 0 || prefix.size() < 3 || prefix.size() > 24 ||
        !IsLowerAsciiWord(prefix))
        return exact;
    std::vector<WordItem> result;
    if (reconvert)
        result.emplace_back("", input, 0, CandidateSource::Generated);

    // Like upstream mobile correction (#8237), preserve deliberate abbreviations
    // and camelCase rather than offering a spelling replacement for them.
    if (std::any_of(input.begin() + 1, input.end(), [](unsigned char ch) { return ch >= 'A' && ch <= 'Z'; }))
        return result;
    if (!ensure_correction_index())
        return result;
    // The sorted index is built only on the first typo, and refreshed only after
    // a dictionary write. Binary prefix probes avoid hundreds of SQL calls per
    // typo. Input bounds cap probes at 1321; each spelling takes three ranked
    // rows using O(log N) range queries, without scanning a broad prefix.
    std::map<std::string, int> variants;
    const auto add = [&](std::string value, int cost) {
        if (value != prefix && value.size() >= 2)
        {
            const auto found = variants.find(value);
            if (found == variants.end() || cost < found->second)
                variants.insert_or_assign(std::move(value), cost);
        }
    };
    for (size_t i = 0; i < prefix.size(); ++i)
    {
        auto value = prefix;
        value.erase(i, 1);
        add(std::move(value), 1);
        if (i + 1 < prefix.size())
        {
            value = prefix;
            std::swap(value[i], value[i + 1]);
            add(std::move(value), 0);
        }
        for (char ch = 'a'; ch <= 'z'; ++ch)
        {
            value = prefix;
            value[i] = ch;
            add(std::move(value), 2);
        }
    }
    for (size_t i = 0; i <= prefix.size(); ++i)
        for (char ch = 'a'; ch <= 'z'; ++ch)
        {
            auto value = prefix;
            value.insert(value.begin() + static_cast<std::ptrdiff_t>(i), ch);
            add(std::move(value), 1);
        }

    struct Match
    {
        WordItem item;
        bool whole_word;
        int edit_cost;
    };
    std::map<std::string, Match> unique;
    for (const auto &[variant, cost] : variants)
    {
        for (const auto index : query_spelling_prefix(variant))
        {
            const auto *entry = &spelling_index_[index];
            if (entry->key == prefix)
                continue;
            WordItem item(entry->key, entry->display, entry->weight, CandidateSource::EnglishDictionary);
            const bool whole_word = item.pinyin == variant;
            if (input.front() >= 'A' && input.front() <= 'Z' && !item.word.empty() && item.word.front() >= 'a' &&
                item.word.front() <= 'z')
                item.word.front() -= 'a' - 'A';
            auto found = unique.find(item.word);
            if (found == unique.end() || (whole_word && !found->second.whole_word) ||
                (whole_word == found->second.whole_word && cost < found->second.edit_cost))
            {
                item.corrected_from = input;
                const std::string display = item.word;
                unique.insert_or_assign(display, Match{std::move(item), whole_word, cost});
            }
        }
    }
    std::vector<Match> matches;
    for (auto &[word, match] : unique)
        matches.push_back(std::move(match));
    std::sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) {
        if (a.whole_word != b.whole_word)
            return a.whole_word;
        if (a.edit_cost != b.edit_cost)
            return a.edit_cost < b.edit_cost;
        if (a.item.weight != b.item.weight)
            return a.item.weight > b.item.weight;
        if (a.item.word.size() != b.item.word.size())
            return a.item.word.size() < b.item.word.size();
        return a.item.word < b.item.word;
    });
    for (auto &match : matches)
    {
        if (result.size() >= (std::min)(limit, kMixedCandidateLimit))
            break;
        result.push_back(std::move(match.item));
    }
    return result;
}

size_t EnglishDictionary::better_spelling_entry(size_t left, size_t right) const
{
    const size_t size = spelling_index_.size();
    if (left == size)
        return right;
    if (right == size)
        return left;
    const auto &a = spelling_index_[left];
    const auto &b = spelling_index_[right];
    if (a.weight != b.weight)
        return a.weight > b.weight ? left : right;
    if (a.key.size() != b.key.size())
        return a.key.size() < b.key.size() ? left : right;
    return std::tie(a.key, a.display) < std::tie(b.key, b.display) ? left : right;
}

size_t EnglishDictionary::best_spelling_entry(size_t begin, size_t end) const
{
    const size_t size = spelling_index_.size();
    size_t best = size;
    for (begin += size, end += size; begin < end; begin /= 2, end /= 2)
    {
        if (begin % 2 != 0)
            best = better_spelling_entry(best, spelling_best_[begin++]);
        if (end % 2 != 0)
            best = better_spelling_entry(best, spelling_best_[--end]);
    }
    return best;
}

std::vector<size_t> EnglishDictionary::query_spelling_prefix(const std::string &prefix) const
{
    const auto lower = [](const SpellingEntry &entry, const std::string &key) { return entry.key < key; };
    const auto begin = std::lower_bound(spelling_index_.begin(), spelling_index_.end(), prefix, lower);
    const auto exact_end =
        std::upper_bound(begin, spelling_index_.end(), prefix,
                         [](const std::string &key, const SpellingEntry &entry) { return key < entry.key; });
    const auto end = std::lower_bound(exact_end, spelling_index_.end(), prefix + "{", lower);
    std::vector<size_t> best;
    const auto append = [&](size_t first, size_t last) {
        std::vector<std::pair<size_t, size_t>> ranges;
        if (first < last)
            ranges.emplace_back(first, last);
        while (best.size() < 3 && !ranges.empty())
        {
            size_t selected = spelling_index_.size();
            size_t selected_range = 0;
            for (size_t i = 0; i < ranges.size(); ++i)
            {
                const size_t candidate = best_spelling_entry(ranges[i].first, ranges[i].second);
                if (better_spelling_entry(candidate, selected) == candidate)
                {
                    selected = candidate;
                    selected_range = i;
                }
            }
            const auto [left, right] = ranges[selected_range];
            ranges[selected_range] = ranges.back();
            ranges.pop_back();
            if (left < selected)
                ranges.emplace_back(left, selected);
            if (selected + 1 < right)
                ranges.emplace_back(selected + 1, right);
            best.push_back(selected);
        }
    };
    append(static_cast<size_t>(begin - spelling_index_.begin()),
           static_cast<size_t>(exact_end - spelling_index_.begin()));
    append(static_cast<size_t>(exact_end - spelling_index_.begin()),
           static_cast<size_t>(end - spelling_index_.begin()));
    return best;
}

bool EnglishDictionary::ensure_correction_index()
{
    if (!ensure_query_statement())
        return false;
    sqlite3_stmt *version = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA data_version", -1, &version, nullptr) != SQLITE_OK)
        return false;
    const int current_version = sqlite3_step(version) == SQLITE_ROW ? sqlite3_column_int(version, 0) : -1;
    sqlite3_finalize(version);
    if (current_version < 0)
        return false;
    if (spelling_data_version_ == current_version)
        return true;
    sqlite3_stmt *query = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT word,display,weight FROM english_words ORDER BY word LIMIT 500001", -1, &query,
                           nullptr) != SQLITE_OK)
        return false;
    std::vector<SpellingEntry> entries;
    int status = SQLITE_ROW;
    while ((status = sqlite3_step(query)) == SQLITE_ROW)
    {
        const auto *key = reinterpret_cast<const char *>(sqlite3_column_text(query, 0));
        const auto *display = reinterpret_cast<const char *>(sqlite3_column_text(query, 1));
        if (!key || !display)
            continue;
        const std::string word(key);
        if (word.size() > 64 || !IsLowerAsciiWord(word))
            continue;
        if (entries.size() >= 500000)
        {
            sqlite3_finalize(query);
            spelling_index_.clear();
            spelling_best_.clear();
            spelling_data_version_ = current_version;
            return true; // Do not repeat an oversized index build on every key.
        }
        entries.push_back({word, display, sqlite3_column_int64(query, 2)});
    }
    sqlite3_finalize(query);
    if (status != SQLITE_DONE)
        return false;
    spelling_index_ = std::move(entries);
    const size_t size = spelling_index_.size();
    spelling_best_.resize(2 * size);
    for (size_t i = 0; i < size; ++i)
        spelling_best_[size + i] = i;
    for (size_t i = size; i > 1; --i)
    {
        const size_t parent = i - 1;
        spelling_best_[parent] = better_spelling_entry(spelling_best_[2 * parent], spelling_best_[2 * parent + 1]);
    }
    spelling_data_version_ = current_version;
    return true;
}

bool EnglishDictionary::ready()
{
    return ensure_query_statement();
}

namespace
{
std::string QueryGloss(sqlite3_stmt *statement, const std::string &key)
{
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    if (sqlite3_bind_text(statement, 1, key.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
        return {};
    std::string result;
    if (sqlite3_step(statement) == SQLITE_ROW)
    {
        const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
        if (value != nullptr)
            result = value;
    }
    sqlite3_reset(statement);
    return result;
}
} // namespace

std::string EnglishDictionary::query_chinese_gloss(const std::string &english)
{
    const auto custom = custom_en_zh_.find(english);
    if (custom != custom_en_zh_.end())
        return custom->second;
    return english.empty() || !ensure_gloss_statements() ? std::string{} : QueryGloss(en_zh_statement_, english);
}

std::string EnglishDictionary::query_english_gloss(const std::string &chinese)
{
    const auto custom = custom_zh_en_.find(chinese);
    if (custom != custom_zh_en_.end())
        return custom->second;
    return chinese.empty() || !ensure_gloss_statements() ? std::string{} : QueryGloss(zh_en_statement_, chinese);
}

bool EnglishDictionary::upsert_gloss(const std::string &db_path, bool chinese_to_english, const std::string &key,
                                     const std::string &gloss)
{
    if (db_path.empty() || key.empty() || gloss.empty() || !ensure_schema(db_path))
        return false;

    sqlite3 *database = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK)
    {
        if (database != nullptr)
            sqlite3_close(database);
        return false;
    }
    sqlite3_busy_timeout(database, 250);
    sqlite3_stmt *statement = nullptr;
    const char *sql = chinese_to_english ? "INSERT OR REPLACE INTO zh_en_glosses(chinese,english_gloss) VALUES(?1,?2)"
                                         : "INSERT OR REPLACE INTO en_zh_glosses(english,chinese_gloss) VALUES(?1,?2)";
    bool ok = sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK &&
              sqlite3_bind_text(statement, 1, key.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
              sqlite3_bind_text(statement, 2, gloss.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
              sqlite3_step(statement) == SQLITE_DONE;
    if (statement != nullptr)
        sqlite3_finalize(statement);
    sqlite3_close(database);
    return ok;
}

void EnglishDictionary::load_custom_translations()
{
    custom_en_zh_.clear();
    custom_zh_en_.clear();
    if (db_path_.empty())
        return;

    const auto sidecar = translations_path_.empty() ? metasequoia::path_from_utf8(db_path_.c_str()).parent_path() /
                                                          metasequoia::assets::translations
                                                    : metasequoia::path_from_utf8(translations_path_.c_str());
    std::ifstream input(sidecar, std::ios::binary);
    if (!input)
        return;

    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
    {
        text.erase(0, 3);
    }

    size_t begin = 0;
    while (begin < text.size())
    {
        size_t end = text.find_first_of("\r\n", begin);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(begin, end - begin);
        if (end < text.size() && text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n')
            begin = end + 2;
        else
            begin = end == text.size() ? text.size() : end + 1;

        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
            line.erase(line.begin());
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        if (line.empty() || line.front() == '#')
            continue;

        const auto tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size())
            continue;
        std::string source = line.substr(0, tab);
        std::string gloss = line.substr(tab + 1);
        while (!source.empty() && std::isspace(static_cast<unsigned char>(source.back())))
            source.pop_back();
        while (!gloss.empty() && std::isspace(static_cast<unsigned char>(gloss.front())))
            gloss.erase(gloss.begin());
        while (!gloss.empty() && std::isspace(static_cast<unsigned char>(gloss.back())))
            gloss.pop_back();
        if (source.empty() || gloss.empty())
            continue;

        const bool chinese_source =
            std::any_of(source.begin(), source.end(), [](unsigned char ch) { return ch >= 0x80; });
        if (chinese_source)
            custom_zh_en_[std::move(source)] = std::move(gloss);
        else
            custom_en_zh_[std::move(source)] = std::move(gloss);
    }
}

bool EnglishDictionary::ensure_schema(const std::string &db_path)
{
    sqlite3 *database = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
    {
        if (database != nullptr)
            sqlite3_close(database);
        return false;
    }

    bool has_weight = false;
    bool composite_primary_key = false;
    bool has_table = false;
    sqlite3_stmt *columns = nullptr;
    if (sqlite3_prepare_v2(database, "PRAGMA table_info(english_words)", -1, &columns, nullptr) == SQLITE_OK)
    {
        int primary_key_columns = 0;
        while (sqlite3_step(columns) == SQLITE_ROW)
        {
            has_table = true;
            const auto *name = reinterpret_cast<const char *>(sqlite3_column_text(columns, 1));
            has_weight = has_weight || (name != nullptr && std::string(name) == "weight");
            if (sqlite3_column_int(columns, 5) > 0)
                ++primary_key_columns;
        }
        composite_primary_key = primary_key_columns == 2;
    }
    if (columns != nullptr)
        sqlite3_finalize(columns);

    bool english_words_ok = false;
    if (!has_table)
    {
        english_words_ok = sqlite3_exec(database,
                                        "CREATE TABLE english_words("
                                        "word TEXT COLLATE BINARY NOT NULL,display TEXT NOT NULL,"
                                        "weight INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(word,display)) WITHOUT ROWID;",
                                        nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    else if (has_weight && composite_primary_key)
    {
        english_words_ok = true;
    }
    else
    {
        const char *copy_sql = has_weight ? "INSERT OR IGNORE INTO english_words_new(word,display,weight) "
                                            "SELECT word,display,weight FROM english_words;"
                                          : "INSERT OR IGNORE INTO english_words_new(word,display,weight) "
                                            "SELECT word,display,0 FROM english_words;";
        english_words_ok = sqlite3_exec(database, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK &&
                           sqlite3_exec(database,
                                        "CREATE TABLE english_words_new("
                                        "word TEXT COLLATE BINARY NOT NULL,display TEXT NOT NULL,"
                                        "weight INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(word,display)) WITHOUT ROWID",
                                        nullptr, nullptr, nullptr) == SQLITE_OK &&
                           sqlite3_exec(database, copy_sql, nullptr, nullptr, nullptr) == SQLITE_OK &&
                           sqlite3_exec(database, "DROP TABLE english_words", nullptr, nullptr, nullptr) == SQLITE_OK &&
                           sqlite3_exec(database, "ALTER TABLE english_words_new RENAME TO english_words", nullptr,
                                        nullptr, nullptr) == SQLITE_OK;
        sqlite3_exec(database, english_words_ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);
    }

    const bool gloss_tables_ok =
        english_words_ok &&
        sqlite3_exec(database,
                     "CREATE TABLE IF NOT EXISTS en_zh_glosses("
                     "english TEXT COLLATE BINARY PRIMARY KEY,chinese_gloss TEXT NOT NULL) WITHOUT ROWID;"
                     "CREATE TABLE IF NOT EXISTS zh_en_glosses("
                     "chinese TEXT COLLATE BINARY PRIMARY KEY,english_gloss TEXT NOT NULL) WITHOUT ROWID;"
                     "PRAGMA user_version=3;",
                     nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(database);
    return gloss_tables_ok;
}

bool EnglishDictionary::ensure_query_statement()
{
    if (query_statement_ != nullptr)
    {
        return true;
    }

    if (db_ == nullptr &&
        sqlite3_open_v2(db_path_.c_str(), &db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK)
    {
        (void)0;
        close_database();
        return false;
    }

    constexpr const char *query_sql =
        "SELECT word,display,weight FROM english_words "
        "WHERE word >= ?1 AND word < ?2 "
        "ORDER BY CASE WHEN word = ?1 THEN 0 ELSE 1 END, weight DESC, length(word), word, display "
        "LIMIT ?3";
    if (sqlite3_prepare_v2(db_, query_sql, -1, &query_statement_, nullptr) != SQLITE_OK)
    {
        (void)0;
        close_database();
        return false;
    }
    return true;
}

bool EnglishDictionary::ensure_mixed_query_statement()
{
    if (mixed_query_statement_ != nullptr)
        return true;
    if (!ensure_query_statement())
        return false;

    // Budget different words first, then union the legacy first five displays:
    // casing variants must neither hide inflected forms nor disappear when a
    // user has already chosen or fixed one of those previously visible rows.
    constexpr const char *query_sql =
        "WITH distinct_words AS ("
        "SELECT candidate.word,candidate.display,candidate.weight FROM english_words AS candidate "
        "WHERE candidate.word >= ?1 AND candidate.word < ?2 AND NOT EXISTS ("
        "SELECT 1 FROM english_words AS better WHERE better.word = candidate.word AND "
        "(better.weight > candidate.weight OR "
        "(better.weight = candidate.weight AND better.display < candidate.display))) "
        "ORDER BY CASE WHEN candidate.word = ?1 THEN 0 ELSE 1 END, candidate.weight DESC, "
        "length(candidate.word), candidate.word, candidate.display "
        "LIMIT ?3), legacy_displays AS ("
        "SELECT word,display,weight FROM english_words WHERE word >= ?1 AND word < ?2 "
        "ORDER BY CASE WHEN word = ?1 THEN 0 ELSE 1 END, weight DESC, length(word), word, display "
        "LIMIT min(?3,5)) "
        "SELECT word,display,weight FROM ("
        "SELECT word,display,weight FROM distinct_words UNION "
        "SELECT word,display,weight FROM legacy_displays) "
        "ORDER BY CASE WHEN word = ?1 THEN 0 ELSE 1 END, weight DESC, length(word), word, display";
    if (sqlite3_prepare_v2(db_, query_sql, -1, &mixed_query_statement_, nullptr) != SQLITE_OK)
    {
        close_database();
        return false;
    }
    return true;
}

bool EnglishDictionary::ensure_gloss_statements()
{
    if (en_zh_statement_ != nullptr && zh_en_statement_ != nullptr)
        return true;
    if (!ensure_query_statement())
        return false;
    if (sqlite3_prepare_v2(db_, "SELECT chinese_gloss FROM en_zh_glosses WHERE english=?1", -1, &en_zh_statement_,
                           nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db_, "SELECT english_gloss FROM zh_en_glosses WHERE chinese=?1", -1, &zh_en_statement_,
                           nullptr) != SQLITE_OK)
    {
        close_database();
        return false;
    }
    return true;
}

void EnglishDictionary::close_database()
{
    spelling_index_.clear();
    spelling_best_.clear();
    spelling_data_version_ = -1;
    if (mixed_query_statement_ != nullptr)
    {
        sqlite3_finalize(mixed_query_statement_);
        mixed_query_statement_ = nullptr;
    }
    if (en_zh_statement_ != nullptr)
    {
        sqlite3_finalize(en_zh_statement_);
        en_zh_statement_ = nullptr;
    }
    if (zh_en_statement_ != nullptr)
    {
        sqlite3_finalize(zh_en_statement_);
        zh_en_statement_ = nullptr;
    }
    if (query_statement_ != nullptr)
    {
        sqlite3_finalize(query_statement_);
        query_statement_ = nullptr;
    }
    if (db_ != nullptr)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}
