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
    std::string query_chinese_gloss(const std::string &english);
    std::string query_english_gloss(const std::string &chinese);
    // Gloss for a Chinese candidate in the configured target language. "en" and
    // an empty language keep the legacy zh_en_glosses table; every other
    // language reads the zh_glosses(chinese, lang, gloss) table. An empty
    // string means the word is not in the local pack for that language, which
    // is what sends it to the cloud provider.
    std::string query_target_gloss(const std::string &chinese, const std::string &language);
    bool ready();
    static bool ensure_schema(const std::string &db_path);
    static bool upsert_gloss(const std::string &db_path, bool chinese_to_english, const std::string &key,
                             const std::string &gloss, const std::string &language = {});
    // 一条待落库的译文。词条与整句共用。
    struct GlossEntry
    {
        bool chinese_to_english = true;
        std::string key;
        std::string gloss;
        std::string language;
    };
    // 批量写回：一轮翻译十几条，逐条 open/fsync/close 会让候选窗明显卡顿。
    static bool upsert_glosses(const std::string &db_path, const std::vector<GlossEntry> &entries);

  private:
    bool ensure_query_statement();
    bool ensure_mixed_query_statement();
    bool ensure_gloss_statements();
    bool ensure_target_gloss_statement();
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
    sqlite3_stmt *zh_target_statement_ = nullptr;
    bool zh_target_unavailable_ = false;
    std::unordered_map<std::string, std::string> custom_en_zh_;
    std::unordered_map<std::string, std::string> custom_zh_en_;
};
