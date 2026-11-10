#include "tests/includes/test_framework.h"
#include "engine/local_modes/symbol_query.h"
#include "utils/common_utils.h"

#include <filesystem>
#include <string>
#include <vector>

namespace
{
// 展开面板按词查符号用的锚点词与期望命中（字节转义，避免源文件编码歧义）。
// 「箭头」是若干箭头符号的关键词（以「箭头符号」这类整词或更长关键词出现）；目录序最靠前的
// ⬅（U+2B05）用于 Contains 断言。
const std::string kWordArrow = "\xE7\xAE\xAD\xE5\xA4\xB4"; // 箭头
const std::string kLeftArrow = "\xE2\xAC\x85";             // ⬅

bool SymbolDatabaseAvailable()
{
    return std::filesystem::exists(CommonUtils::get_ime_data_path() + "\\others.db");
}

bool Contains(const std::vector<WordItem> &items, const std::string &word)
{
    for (const auto &item : items)
        if (item.word == word)
            return true;
    return false;
}
} // namespace

TEST_CASE(symbol_query_by_keyword_matches_chinese_token)
{
    if (!SymbolDatabaseAvailable())
        return;
    // 展开面板按高亮词查符号：「箭头」是若干箭头符号的关键词，应命中且来源标为 Symbol。
    const auto results = metasequoia::local_modes::query_symbols_by_keyword(kWordArrow, 10).candidates;
    REQUIRE(!results.empty());
    for (const auto &item : results)
        REQUIRE(item.source == CandidateSource::Symbol);
    REQUIRE(Contains(results, kLeftArrow));
}

TEST_CASE(symbol_query_by_keyword_dedupes_across_categories)
{
    if (!SymbolDatabaseAvailable())
        return;
    // symbol_catalog 主键是 (symbol, category)，同一符号可能挂在多个类别下；结果必须按符号去重。
    const auto results = metasequoia::local_modes::query_symbols_by_keyword(kWordArrow, 50).candidates;
    std::vector<std::string> seen;
    for (const auto &item : results)
    {
        for (const auto &prior : seen)
            REQUIRE(prior != item.word);
        seen.push_back(item.word);
    }
}

TEST_CASE(symbol_query_by_keyword_empty_and_unknown_return_empty)
{
    if (!SymbolDatabaseAvailable())
        return;
    // 空词、不存在的词、limit<=0 仍返回空。
    REQUIRE(metasequoia::local_modes::query_symbols_by_keyword("", 10).candidates.empty());
    REQUIRE(metasequoia::local_modes::query_symbols_by_keyword("zzqqxx", 10).candidates.empty());
    REQUIRE(metasequoia::local_modes::query_symbols_by_keyword(kWordArrow, 0).candidates.empty());
}
