#pragma once
#include <boost/json.hpp>
#include <sqlite3.h>
#include <algorithm>
#include <cstdint>
#include <string>

namespace SettingsDictionary::Paging
{
inline int Integer(const boost::json::object &request, const char *key, int fallback, int maximum)
{
    const auto *value = request.if_contains(key);
    if (!value || !value->is_int64())
        return fallback;
    return static_cast<int>((std::max)(int64_t{0}, (std::min)(value->as_int64(), int64_t{maximum})));
}
inline int Limit(const boost::json::object &request)
{
    return (std::max)(1, Integer(request, "limit", 200, 200));
}
inline std::string Sql(const boost::json::object &request)
{
    return " LIMIT " + std::to_string(Limit(request) + 1) + " OFFSET " +
           std::to_string(Integer(request, "offset", 0, 2147483647));
}
inline boost::json::object Read(sqlite3_stmt *statement, const boost::json::object &request, bool english = false)
{
    namespace json = boost::json;
    json::array rows;
    int64_t total = 0;
    int status;
    while ((status = sqlite3_step(statement)) == SQLITE_ROW)
    {
        const auto text = [statement](int column) {
            const auto *value = sqlite3_column_text(statement, column);
            return value ? reinterpret_cast<const char *>(value) : "";
        };
        rows.push_back({{english ? "word" : "code", text(0)},
                        {english ? "display" : "word", text(1)},
                        {"weight", sqlite3_column_int(statement, 2)}});
        total = sqlite3_column_int64(statement, 3);
    }
    if (status != SQLITE_DONE)
        return {{"ok", false}, {"message", "查询失败，请重试"}, {"rows", json::array{}}};
    const bool more = rows.size() > static_cast<size_t>(Limit(request));
    if (more)
        rows.pop_back();
    return {{"ok", true},      {"message", rows.empty() ? "没有找到词条" : "查询成功"}, {"rows", std::move(rows)},
            {"hasMore", more}, {"offset", Integer(request, "offset", 0, 2147483647)},   {"total", total}};
}
} // namespace SettingsDictionary::Paging
