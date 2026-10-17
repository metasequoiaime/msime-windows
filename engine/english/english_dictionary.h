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
    // Exact prefix results always win. Only a missing prefix of 3..24 ASCII
    // letters gets a bounded, one-edit spelling fallback; no model or per-key scan.
    std::vector<WordItem> query_candidates(const std::string &prefix, size_t limit = 5, bool mixed = false,
                                           bool reconvert = false);
    bool ready();
    static bool ensure_schema(const std::string &db_path);
    static bool upsert_gloss(const std::string &db_path, bool chinese_to_english, const std::string &key,
                             const std::string &gloss);

  private:
    bool ensure_query_statement();
    bool ensure_mixed_query_statement();
    bool ensure_correction_index();
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
    struct SpellingEntry
    {
        std::string key;
        std::string display;
        std::int64_t weight;
    };
    std::vector<SpellingEntry> spelling_index_;
    // Iterative arg-best tree: two indices per row (at most 8 MB on 64-bit).
    std::vector<size_t> spelling_best_;
    size_t better_spelling_entry(size_t left, size_t right) const;
    size_t best_spelling_entry(size_t begin, size_t end) const;
    std::vector<size_t> query_spelling_prefix(const std::string &prefix) const;
    int spelling_data_version_ = -1;
    std::unordered_map<std::string, std::string> custom_en_zh_;
    std::unordered_map<std::string, std::string> custom_zh_en_;
};
