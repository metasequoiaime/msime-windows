#pragma once

#include "../core/word_item.h"
#include <cstddef>
#include <sqlite3.h>
#include <string>
#include <unordered_map>
#include <vector>

class EnglishDictionary
{
  public:
    static constexpr size_t kMixedCandidateLimit = 32;

    explicit EnglishDictionary(std::string db_path, bool initialize_schema = true, std::string translations_path = {});
    ~EnglishDictionary();

    EnglishDictionary(const EnglishDictionary &) = delete;
    EnglishDictionary &operator=(const EnglishDictionary &) = delete;

    std::vector<WordItem> query_prefix(const std::string &prefix, size_t limit = 5);
    // The mixed budget counts distinct words. Also retain the first five legacy
    // display rows so existing casing choices and their fixed positions survive.
    // At most kMixedCandidateLimit + 5 real dictionary rows are returned.
    std::vector<WordItem> query_mixed_prefix(const std::string &prefix, size_t limit = kMixedCandidateLimit);
    // 词库里每个 word 一行，写法取权重最高的那一种。建拼写纠错索引用，整表扫描，不要在按键路径上调。
    std::vector<WordItem> query_distinct_words();
    std::string query_chinese_gloss(const std::string &english);
    std::string query_english_gloss(const std::string &chinese);
    // en_ipa 表里的音标（不带斜线或方括号）。旧词库没有这张表时返回空串，不影响其他查询。
    std::string query_ipa(const std::string &english);
    // english_phrases 表里键以 prefix 开头的短语，键等于 prefix 的排最前，其余按权重。来源记 Generated、
    // english_phrase 置位。旧词库没有这张表时返回空。
    std::vector<WordItem> query_phrase_prefix(const std::string &prefix, size_t limit);
    bool ready();
    static bool ensure_schema(const std::string &db_path);
    static bool upsert_gloss(const std::string &db_path, bool chinese_to_english, const std::string &key,
                             const std::string &gloss);

  private:
    bool ensure_query_statement();
    bool ensure_mixed_query_statement();
    bool ensure_gloss_statements();
    void load_custom_translations();
    std::string translations_path_;
    void close_database();

  private:
    std::string db_path_;
    sqlite3 *db_ = nullptr;
    sqlite3_stmt *query_statement_ = nullptr;
    sqlite3_stmt *mixed_query_statement_ = nullptr;
    sqlite3_stmt *en_zh_statement_ = nullptr;
    sqlite3_stmt *zh_en_statement_ = nullptr;
    sqlite3_stmt *ipa_statement_ = nullptr;
    bool ipa_unavailable_ = false;
    sqlite3_stmt *phrase_statement_ = nullptr;
    bool phrases_unavailable_ = false;
    std::unordered_map<std::string, std::string> custom_en_zh_;
    std::unordered_map<std::string, std::string> custom_zh_en_;
};
