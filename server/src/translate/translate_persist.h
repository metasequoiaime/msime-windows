#pragma once

#include "translate/translate_types.h"
#include <string>

namespace Translate
{
// 句子判定与落库。词条与整句的长度门不同：整句译文常常是词条的两三倍。
bool HasSentencePunctuation(const std::string &text);
bool IsSentenceLike(const std::string &key);
bool TooLong(const std::string &key);
// 译文是否该落库：词条与整句走不同的长度门。校对写回前也用它，
// 避免内存里是校对版、词库里还是旧版。
bool ShouldPersist(const Query &query, const std::string &gloss);

// 查得到就入本地库：词条进词表，整句也进——同一句话再打一次就不必再问云端。
void PersistGloss(const Query &query, const std::string &gloss, const std::string &target_language);

// 批量落库：一轮十几条，逐条写会多出十几次 open 与 fsync。
struct GlossWrite
{
    Query query;
    std::string gloss;
};
void PersistGlosses(const std::vector<GlossWrite> &writes, const std::string &target_language);
} // namespace Translate
