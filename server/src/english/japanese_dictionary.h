#pragma once

#include <string>

struct sqlite3;
struct sqlite3_stmt;

struct JapaneseGloss
{
    std::string gloss;
    std::string reading;
};

// Optional Chinese-to-Japanese dictionary; accessed by the EnglishIme worker.
class JapaneseDictionary
{
  public:
    static constexpr const char *kFileName = "japanese.db";
    explicit JapaneseDictionary(const std::string &path);
    ~JapaneseDictionary();
    JapaneseDictionary(const JapaneseDictionary &) = delete;
    JapaneseDictionary &operator=(const JapaneseDictionary &) = delete;

    bool available() const;
    JapaneseGloss lookup(const std::string &chinese);

  private:
    void close();
    sqlite3 *db_ = nullptr;
    sqlite3_stmt *query_ = nullptr;
};
