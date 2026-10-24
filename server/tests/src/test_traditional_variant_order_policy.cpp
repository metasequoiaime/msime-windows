#include "ipc/traditional_variant_order_policy.h"
#include "tests/includes/test_framework.h"

#include <string>
#include <vector>

// 入参与期望照施工单〈实跑数据〉表（OpenCC 8cf737a4 用 opencc.exe 实测）逐行抄写：
// s2t、t2tw(s2t)、t2hk(s2t)、STCharacters 字表行（多字词与不在表里的字传空）。

using FanyImeIpc::OrderTraditionalVariants;
using FanyImeIpc::TraditionalRegion;
using Spellings = std::vector<std::string>;

namespace
{
struct Row
{
    std::string standard;
    std::string taiwan;
    std::string hong_kong;
    Spellings character_table;
    Spellings expected_taiwan;
    Spellings expected_hong_kong;
};

void RequireRow(const Row &row)
{
    const auto taiwan = OrderTraditionalVariants(row.standard, row.taiwan, row.hong_kong, row.character_table,
                                                 TraditionalRegion::Taiwan);
    const auto hong_kong = OrderTraditionalVariants(row.standard, row.taiwan, row.hong_kong, row.character_table,
                                                    TraditionalRegion::HongKong);
    REQUIRE_EQ(taiwan, row.expected_taiwan);
    REQUIRE_EQ(hong_kong, row.expected_hong_kong);
}
} // namespace

TEST_CASE(traditional_variant_order_qunzhong_keeps_three_distinct_spellings)
{
    RequireRow({"羣衆", "群眾", "羣眾", {}, {"群眾", "羣眾", "羣衆"}, {"羣眾", "群眾", "羣衆"}});
}
