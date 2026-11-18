#pragma once

#include "candidate_provider.h"
#include "../core/runtime_paths.h"
#include "../core/wubi_profile.h"
#include <sqlite3.h>
#include <string>

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
    bool ensure_query_statement();
    // 走完 statement 并按既有去重规则收敛候选；调用前由 query() 负责 reset 与绑定。
    std::vector<WordItem> collect_rows(sqlite3_stmt *statement);
    void close_database();
    std::string journal_db_path() const;

    std::string db_path_;
    metasequoia::RuntimePaths paths_;
    metasequoia::WubiProfile profile_ = metasequoia::WubiProfile::Wubi86;
    sqlite3 *db_ = nullptr;
    sqlite3_stmt *query_statement_ = nullptr;
    // 通配码（含 z）专用：WHERE 是 GLOB 而非范围，语句与参数个数都不同，单独 prepared。
    sqlite3_stmt *wildcard_statement_ = nullptr;
};
