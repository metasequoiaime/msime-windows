#include "../contracts/assets/assets.h"
#include "english_dictionary.h"
#include "../core/data_path.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
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

std::string EnglishDictionary::query_target_gloss(const std::string &chinese, const std::string &language)
{
    if (language.empty() || language == "en")
        return query_english_gloss(chinese);
    // custom_zh_en_ 是人工维护的英语释义，不能当真它别的语种。
    if (chinese.empty() || !ensure_target_gloss_statement())
        return {};
    sqlite3_reset(zh_target_statement_);
    sqlite3_clear_bindings(zh_target_statement_);
    if (sqlite3_bind_text(zh_target_statement_, 1, chinese.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(zh_target_statement_, 2, language.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
        return {};
    std::string result;
    if (sqlite3_step(zh_target_statement_) == SQLITE_ROW)
    {
        const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(zh_target_statement_, 0));
        if (value != nullptr)
            result = value;
    }
    sqlite3_reset(zh_target_statement_);
    return result;
}

bool EnglishDictionary::upsert_gloss(const std::string &db_path, bool chinese_to_english, const std::string &key,
                                     const std::string &gloss, const std::string &language)
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
    // 非英语目标语种落在 zh_glosses(chinese, lang, gloss)，与英语目标同等的“本地优先”。
    const bool target_language = chinese_to_english && !language.empty() && language != "en";
    const char *sql = target_language
                          ? "INSERT OR REPLACE INTO zh_glosses(chinese,lang,gloss) VALUES(?1,?2,?3)"
                          : (chinese_to_english ? "INSERT OR REPLACE INTO zh_en_glosses(chinese,english_gloss) "
                                                  "VALUES(?1,?2)"
                                                : "INSERT OR REPLACE INTO en_zh_glosses(english,chinese_gloss) "
                                                  "VALUES(?1,?2)");
    bool ok = sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK &&
              sqlite3_bind_text(statement, 1, key.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
    if (ok && target_language)
        ok = sqlite3_bind_text(statement, 2, language.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
             sqlite3_bind_text(statement, 3, gloss.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
    else if (ok)
        ok = sqlite3_bind_text(statement, 2, gloss.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
    ok = ok && sqlite3_step(statement) == SQLITE_DONE;
    if (statement != nullptr)
        sqlite3_finalize(statement);
    sqlite3_close(database);
    return ok;
}

bool EnglishDictionary::upsert_glosses(const std::string &db_path, const std::vector<GlossEntry> &entries)
{
    if (db_path.empty() || entries.empty() || !ensure_schema(db_path))
        return false;

    sqlite3 *database = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK)
    {
        if (database != nullptr)
            sqlite3_close(database);
        return false;
    }
    sqlite3_busy_timeout(database, 250);
    // 一次开库、一个事务、三条预备语句：不再每条译文都 fsync 一次。
    const bool transaction = sqlite3_exec(database, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK;

    sqlite3_stmt *zh_target = nullptr;
    sqlite3_stmt *zh_en = nullptr;
    sqlite3_stmt *en_zh = nullptr;
    bool ok = transaction;
    for (const auto &entry : entries)
    {
        if (!ok || entry.key.empty() || entry.gloss.empty())
            continue;
        const bool target_language = entry.chinese_to_english && !entry.language.empty() && entry.language != "en";
        sqlite3_stmt **slot = target_language ? &zh_target : (entry.chinese_to_english ? &zh_en : &en_zh);
        if (*slot == nullptr)
        {
            const char *sql = target_language
                                  ? "INSERT OR REPLACE INTO zh_glosses(chinese,lang,gloss) VALUES(?1,?2,?3)"
                                  : (entry.chinese_to_english
                                         ? "INSERT OR REPLACE INTO zh_en_glosses(chinese,english_gloss) VALUES(?1,?2)"
                                         : "INSERT OR REPLACE INTO en_zh_glosses(english,chinese_gloss) VALUES(?1,?2)");
            if (sqlite3_prepare_v2(database, sql, -1, slot, nullptr) != SQLITE_OK)
            {
                ok = false;
                break;
            }
        }
        sqlite3_stmt *stmt = *slot;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        bool bound = sqlite3_bind_text(stmt, 1, entry.key.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
        if (bound && target_language)
            bound = sqlite3_bind_text(stmt, 2, entry.language.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                    sqlite3_bind_text(stmt, 3, entry.gloss.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
        else if (bound)
            bound = sqlite3_bind_text(stmt, 2, entry.gloss.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
        if (!bound || sqlite3_step(stmt) != SQLITE_DONE)
        {
            ok = false;
            break;
        }
    }

    if (zh_target != nullptr)
        sqlite3_finalize(zh_target);
    if (zh_en != nullptr)
        sqlite3_finalize(zh_en);
    if (en_zh != nullptr)
        sqlite3_finalize(en_zh);
    if (transaction)
        sqlite3_exec(database, ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);
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
                     "CREATE TABLE IF NOT EXISTS zh_glosses("
                     "chinese TEXT COLLATE BINARY NOT NULL,lang TEXT COLLATE BINARY NOT NULL,"
                     "gloss TEXT NOT NULL,PRIMARY KEY(chinese,lang)) WITHOUT ROWID;"
                     "PRAGMA user_version=4;",
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

bool EnglishDictionary::ensure_target_gloss_statement()
{
    if (zh_target_statement_ != nullptr)
        return true;
    // 旧词库没有 zh_glosses：记下来，别每次候选刷新都重试一次 prepare。
    if (zh_target_unavailable_ || !ensure_query_statement())
        return false;
    if (sqlite3_prepare_v2(db_, "SELECT gloss FROM zh_glosses WHERE chinese=?1 AND lang=?2", -1, &zh_target_statement_,
                           nullptr) != SQLITE_OK)
    {
        zh_target_statement_ = nullptr;
        zh_target_unavailable_ = true;
        return false;
    }
    return true;
}

void EnglishDictionary::close_database()
{
    if (zh_target_statement_ != nullptr)
    {
        sqlite3_finalize(zh_target_statement_);
        zh_target_statement_ = nullptr;
    }
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
