#include "japanese_dictionary.h"

#include <sqlite3.h>

namespace
{
std::string ColumnText(sqlite3_stmt *statement, int column)
{
    const auto *text = reinterpret_cast<const char *>(sqlite3_column_text(statement, column));
    return text ? text : "";
}
} // namespace

JapaneseDictionary::JapaneseDictionary(const std::string &path)
{
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK)
    {
        close();
        return;
    }
    constexpr const char *sql = "SELECT gloss, reading FROM zh_glosses WHERE chinese=?1";
    if (sqlite3_prepare_v2(db_, sql, -1, &query_, nullptr) != SQLITE_OK)
        close();
}

JapaneseDictionary::~JapaneseDictionary()
{
    close();
}

void JapaneseDictionary::close()
{
    sqlite3_finalize(query_);
    query_ = nullptr;
    sqlite3_close(db_);
    db_ = nullptr;
}

bool JapaneseDictionary::available() const
{
    return query_ != nullptr;
}

JapaneseGloss JapaneseDictionary::lookup(const std::string &chinese)
{
    if (!available())
        return {};
    sqlite3_reset(query_);
    sqlite3_clear_bindings(query_);
    JapaneseGloss result;
    if (sqlite3_bind_text(query_, 1, chinese.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_step(query_) == SQLITE_ROW)
        result = {ColumnText(query_, 0), ColumnText(query_, 1)};
    sqlite3_reset(query_);
    return result;
}
