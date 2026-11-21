#pragma once

// 英文拼写纠错：phonex 读音分桶 + 编辑距离。
//
// 召回策略移植自 hallelujahIM（https://github.com/dongyuwei/hallelujahIM，GPL-3.0，
// 作者 Dong Yuwei）的 ConversionEngine：只有前缀补全查不到词时才纠错，同一 phonex 码
// 下编辑距离不超过 3 的词按距离排序。phonex 编码本身移植自 talisman
// （https://github.com/Yomguithereal/talisman，MIT，Copyright (c) 2016 Guillaume Plique），
// 算法出自 Lait & Randell, "An Assessment of Name Matching Algorithms"。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace metasequoia::english
{
// talisman 的 phonex。非字母字符会被忽略；没有字母时返回空串。
std::string phonex(std::string_view word);

// 普通 Levenshtein 距离（插入、删除、替换各算 1）。超过 limit 时提前结束并返回 limit + 1。
std::size_t levenshtein(std::string_view left, std::string_view right, std::size_t limit);

struct SpellingSuggestion
{
    std::string word;    // 小写键，与 english.db 的 word 列一致
    std::string display; // 词库里的写法（大小写）
    std::int64_t weight = 0;
    std::size_t distance = 0;
};

class PhonexIndex
{
  public:
    // 和 hallelujahIM 一样，太短的词读音区分度太低，不进索引，也不拿来查。
    static constexpr std::size_t kMinimumLength = 4;
    static constexpr std::size_t kMaximumDistance = 3;

    // word 必须是小写 ASCII 字母串；同一个 word 只记权重最高的写法。
    void add(std::string_view word, std::string_view display, std::int64_t weight);
    // 按编辑距离升序、权重降序返回同读音的词，不含 input 本身。input 必须是小写字母串。
    std::vector<SpellingSuggestion> suggest(std::string_view input, std::size_t limit) const;
    bool empty() const
    {
        return buckets_.empty();
    }
    std::size_t size() const
    {
        return size_;
    }

  private:
    struct Entry
    {
        std::string word;
        std::string display;
        std::int64_t weight = 0;
    };
    std::unordered_map<std::string, std::vector<Entry>> buckets_;
    std::size_t size_ = 0;
};
} // namespace metasequoia::english
