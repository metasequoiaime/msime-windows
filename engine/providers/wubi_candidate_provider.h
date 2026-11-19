#pragma once

#include "candidate_provider.h"
#include "../core/runtime_paths.h"
#include "../core/wubi_profile.h"
#include <cstddef>
#include <sqlite3.h>
#include <string>
#include <vector>

class WubiCandidateProvider : public ICandidateProvider
{
  public:
    // paths 供调频/删词写 user journal 用；db_path 是测试注入口，生产路径由 ProviderRegistry
    // 按 paths 解析后传入，两者指向同一份主库。
    explicit WubiCandidateProvider(std::string db_path = {},
                                   metasequoia::RuntimePaths paths = metasequoia::RuntimePaths::legacy());
    ~WubiCandidateProvider() override;

    WubiCandidateProvider(const WubiCandidateProvider &) = delete;
    WubiCandidateProvider &operator=(const WubiCandidateProvider &) = delete;

    std::vector<WordItem> query(const QueryRequest &request) override;
    std::optional<WordItem> find_candidate(SchemeType, const std::string &, const std::string &) override
    {
        return std::nullopt;
    }
    void reset_cache() override;
    // 换一版码表（86、98、新世纪）。查询语句绑着表名，换了就关掉连接，下次查询按新表重新准备。
    void set_profile(metasequoia::WubiProfile profile);
    metasequoia::WubiProfile profile() const
    {
        return profile_;
    }
    int create_word(SchemeType scheme, std::string code, std::string word) override;
    int update_weight_by_pinyin_and_word(SchemeType scheme, std::string code, std::string word) override;
    int delete_by_pinyin_and_word(SchemeType scheme, std::string code, std::string word) override;
    int cache_dynamic_candidate(SchemeType scheme, const std::string &code, const std::string &word,
                                CandidateSource source) override;
    int cache_dynamic_candidate_for_request(const QueryRequest &request, const std::string &word,
                                            CandidateSource source) override;

  private:
    struct Row
    {
        std::string key;
        std::string value;
        long long weight = 0;
    };

    bool ensure_query_statement();
    // 一个码在码表里的行，按权重降序；limit 截断。出错时返回 false。
    bool code_rows(const std::string &code, std::size_t limit, std::vector<Row> &rows);
    // 走完 statement 取出一列码（提示码或通配匹配到的码）。出错时返回 false。
    bool collect_keys(sqlite3_stmt *statement, std::vector<std::string> &keys);
    // 先放 leading（精确码的行），再按名次轮流取 keys 里每个码的行：所有码的首选、再所有码的次选……
    // 直到凑满上限；按既有去重规则收敛候选。
    std::vector<WordItem> assemble(std::vector<Row> leading, const std::vector<std::string> &keys);
    void close_database();
    std::string journal_db_path() const;

    std::string db_path_;
    metasequoia::RuntimePaths paths_;
    metasequoia::WubiProfile profile_ = metasequoia::WubiProfile::Wubi86;
    sqlite3 *db_ = nullptr;
    // 一个码的全部行（精确码行，以及每个提示码的行）。
    sqlite3_stmt *code_statement_ = nullptr;
    // 以输入为前缀、比输入长的码（逐码提示），短码在前。
    sqlite3_stmt *hint_key_statement_ = nullptr;
    // 通配码（含 z）专用：WHERE 是 GLOB 而非范围，单独 prepared。
    sqlite3_stmt *wildcard_key_statement_ = nullptr;
};
