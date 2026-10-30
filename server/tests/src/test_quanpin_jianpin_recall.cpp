#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"
#include "engine/quanpin/quanpin_query.h"

#include <sqlite3.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using quanpin::KeyedQueryItem;
using quanpin::QuerySource;
using quanpin::Segments;

// 自包含的词库夹具：全拼前缀区间、jp 简拼同层合并、全拼双字母声母、双拼单字母声母
// 四条规则都只需要这几行就能区分开，不依赖真实词库，也不需要 DictionaryAvailable 判断。
std::filesystem::path CreateRecallDatabase()
{
    const auto path = test::TempRoot() / "msime-quanpin-jianpin-recall.db";
    std::filesystem::remove(path);

    sqlite3 *db = nullptr;
    if (sqlite3_open(test::Utf8(path).c_str(), &db) != SQLITE_OK)
    {
        if (db != nullptr)
        {
            sqlite3_close(db);
        }
        throw std::runtime_error("建临时全拼简拼召回库失败。");
    }

    const char *sql = "PRAGMA journal_mode=WAL;"
                      "CREATE TABLE tbl_2_e(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "CREATE TABLE tbl_2_z(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "CREATE TABLE tbl_3_z(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "INSERT INTO tbl_2_e VALUES('e''qing','xq','恶情',500);"
                      "INSERT INTO tbl_2_e VALUES('e''qi','eq','恶气',223);"
                      "INSERT INTO tbl_2_e VALUES('er''qie','eq','而且',7201659);"
                      "INSERT INTO tbl_2_e VALUES('er''qu','eq','而去',10538);"
                      "INSERT INTO tbl_2_z VALUES('zhe''ge','zg','这个',16033015);"
                      "INSERT INTO tbl_2_z VALUES('zhong''guo','zg','中国',9609626);"
                      "INSERT INTO tbl_2_z VALUES('zhang''hao','zh','张浩',8000);"
                      "INSERT INTO tbl_2_z VALUES('zui''hou','zh','最后',6000);"
                      "INSERT INTO tbl_3_z VALUES('zhi''li''yu','zly','致力于',14190);"
                      "INSERT INTO tbl_3_z VALUES('zi''lai''ye','zly','自来也',2358);"
                      "INSERT INTO tbl_3_z VALUES('zhang''liang''ying','zly','张靓颖',1206);";
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    if (result != SQLITE_OK)
    {
        std::filesystem::remove(path);
        throw std::runtime_error("初始化临时全拼简拼召回库失败。");
    }
    return path;
}

std::vector<KeyedQueryItem> QueryRecall(const std::filesystem::path &path, const Segments &segments, QuerySource source)
{
    return quanpin::query_segments_keyed_flat(segments, test::Utf8(path), 50, source);
}

bool Contains(const std::vector<KeyedQueryItem> &items, const std::string &value)
{
    for (const auto &item : items)
    {
        if (item.value == value)
        {
            return true;
        }
    }
    return false;
}

// 不存在时返回 -1，便于直接把两个候选的序号拿来比先后。
int IndexOf(const std::vector<KeyedQueryItem> &items, const std::string &value)
{
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (items[i].value == value)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}
} // namespace

// 前缀区间必须用真实键前缀 "e'q" 做区间比较。曾经这里沿用给 LIKE 准备的 "e'q%"，把通配符
// 带进字符串区间后范围就成空集。这行的 jp 故意写成不匹配的值，模拟只有前缀区间能捞到的行，
// 因为真实词库里 jp 与 key 一致时，简拼层会替前缀层兜住同一个词，断言就看不出区别。
TEST_CASE(quanpin_prefix_range_uses_a_real_key_prefix)
{
    const auto db_path = CreateRecallDatabase();
    const auto items = QueryRecall(db_path, Segments{"e", "q"}, QuerySource::Quanpin);
    if (!Contains(items, "恶情"))
    {
        throw std::runtime_error("前缀区间按 e'q 扫描必须命中 恶情，否则说明区间又被 LIKE 通配符算空了。");
    }
    std::filesystem::remove(db_path);
}

// 全拼的简拼召回与前缀区间同层：按权重合并排序，前缀命中不再天然压住整条简拼候选。
TEST_CASE(quanpin_jianpin_and_prefix_share_one_tier)
{
    const auto db_path = CreateRecallDatabase();
    const auto items = QueryRecall(db_path, Segments{"e", "q"}, QuerySource::Quanpin);
    REQUIRE(!items.empty());
    REQUIRE_EQ(items.front().value, std::string("而且"));

    const int qu_le = IndexOf(items, "而去");
    const int e_qi = IndexOf(items, "恶气");
    REQUIRE(qu_le >= 0);
    REQUIRE(e_qi >= 0);
    if (qu_le >= e_qi)
    {
        throw std::runtime_error("合并后必须按权重排：而去 权重 10538 高于 恶气 223，序号应该更小。");
    }
    std::filesystem::remove(db_path);
}

// 全拼把 zh/ch/sh 当声母段参与简拼：zhg 要认 zhang/zhe/zhong，zly 只认 zhi/zhang 开头的音节。
TEST_CASE(quanpin_supports_two_letter_initials)
{
    const auto db_path = CreateRecallDatabase();

    const auto zhg = QueryRecall(db_path, Segments{"zh", "g"}, QuerySource::Quanpin);
    if (!Contains(zhg, "中国") || !Contains(zhg, "这个"))
    {
        throw std::runtime_error("zhg 必须同时命中 中国 与 这个，说明全拼没有按 zh 声母段比较音节。");
    }

    const auto zly = QueryRecall(db_path, Segments{"zh", "l", "y"}, QuerySource::Quanpin);
    if (!Contains(zly, "张靓颖"))
    {
        throw std::runtime_error("zly 必须命中 张靓颖，否则全拼的两字母声母段没有生效。");
    }
    if (Contains(zly, "自来也"))
    {
        throw std::runtime_error("zly 不能命中 自来也：它首音节是 zi，不是 zh，两字母声母不能退化成单字母。");
    }
    std::filesystem::remove(db_path);
}

// 双拼的声母规则独立于全拼：单字母 z 只认初始码为 z 的音节，zhang 的初始码是 zh，
// 所以同一份数据、同一组片段，双拼不召回 张浩，全拼召回。
TEST_CASE(shuangpin_keeps_its_own_initial_rule)
{
    const auto db_path = CreateRecallDatabase();

    const auto shuangpin = QueryRecall(db_path, Segments{"z", "h"}, QuerySource::Shuangpin);
    if (Contains(shuangpin, "张浩"))
    {
        throw std::runtime_error("双拼查询 z'h 不能召回 张浩，说明双拼被全拼的 zh/ch/sh 声母规则改掉了。");
    }

    const auto quanpin_items = QueryRecall(db_path, Segments{"z", "h"}, QuerySource::Quanpin);
    if (!Contains(quanpin_items, "张浩"))
    {
        throw std::runtime_error("全拼查询 z'h 必须召回 张浩，因为单字母 z 在全拼里匹配 zhang。");
    }
    std::filesystem::remove(db_path);
}
