// 链接真实的 OpenCC 与 chinese_converter.cpp。转换器从本程序所在目录的 assets/opencc 读字典，
// 测试程序与 server 输出到同一个目录，读到的就是 server 打包用的那一份：
// 干净构建下漏拷字典时这里会失败（增量构建不删旧文件，测不出）。
#include "conversion/chinese_converter.h"
#include "ipc/ai_context_policy.h"
#include "tests/includes/test_framework.h"

#include <string>

TEST_CASE(chinese_converter_converts_in_both_directions)
{
    REQUIRE_EQ(ChineseConverter::ToTraditional("台湾"), std::string("臺灣"));
    REQUIRE_EQ(ChineseConverter::ToSimplified("臺灣"), std::string("台湾"));
    REQUIRE_EQ(ChineseConverter::ToSimplified("因為"), std::string("因为"));
}

TEST_CASE(ai_candidate_text_rule_with_opencc)
{
    const auto rule = [](const std::string &word) {
        return FanyImeIpc::SimplifiedAiCandidateText(word, FanyImeIpc::kTraditionalCharacterSet,
                                                     ChineseConverter::ToSimplified, ChineseConverter::ToTraditional);
    };
    REQUIRE_EQ(rule("臺灣"), std::string("台湾"));
    REQUIRE_EQ(rule("台灣"), std::string("台湾"));
    REQUIRE_EQ(rule("因為"), std::string("因为"));
    REQUIRE_EQ(rule("裡面"), std::string("里面"));
    REQUIRE_EQ(rule("發展"), std::string("发展"));
    REQUIRE_EQ(rule("萧乾"), std::string("萧乾"));
    REQUIRE_EQ(rule("乾清宫"), std::string("乾清宫"));
    REQUIRE_EQ(rule("沈阳"), std::string("沈阳"));
}
