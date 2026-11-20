#pragma once
#include <boost/json.hpp>
#include <sqlite3.h>
#include <algorithm>
#include <cstdint>
#include <optional>
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
// 总数只在新搜索的第一页算一次：面板把第一页返回的 total 缓存下来，
// 翻页请求不再重复统计，数据查询也就不会被逼着物化全部匹配行。
inline bool WantsTotal(const boost::json::object &request)
{
    return Integer(request, "offset", 0, 2147483647) == 0;
}
inline std::string Sql(const boost::json::object &request)
{
    return " LIMIT " + std::to_string(Limit(request) + 1) + " OFFSET " +
           std::to_string(Integer(request, "offset", 0, 2147483647));
}
inline boost::json::object Read(sqlite3_stmt *statement, const boost::json::object &request, bool english = false,
                                std::optional<int64_t> total = std::nullopt)
{
    namespace json = boost::json;
    json::array rows;
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
    }
    if (status != SQLITE_DONE)
        return {{"ok", false}, {"message", "查询失败，请重试"}, {"rows", json::array{}}};
    const bool more = rows.size() > static_cast<size_t>(Limit(request));
    if (more)
        rows.pop_back();
    json::object result = {{"ok", true},
                           {"message", rows.empty() ? "没有找到词条" : "查询成功"},
                           {"rows", std::move(rows)},
                           {"hasMore", more},
                           {"offset", Integer(request, "offset", 0, 2147483647)}};
    if (total)
        result["total"] = *total;
    return result;
}
} // namespace SettingsDictionary::Paging
