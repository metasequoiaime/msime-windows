#pragma once

#include "../quanpin/latin_words.h"
#include "shuangpin_profile.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

// 字母词（见 quanpin::LatinWords）在一套双拼方案下的按键：字母仍是一个键，汉字的音节换成这套方案的
// 两个键（小鹤：T恤 txu、B超 bic、AA制 aavi）。双拼的贪心切分会把 tx 切成「tie」，先切分再换算成
// 全拼去查是查不到的，只能拿原始按键直接比。
class ShuangpinLatinWords
{
  public:
    // 按键表存着指向 words 里词条的指针，所以一并握住 words。
    void build(std::shared_ptr<const quanpin::LatinWords> words, const ShuangpinProfile &profile);
    // 建表用的那一份字母词：词典据此判断字母词重读之后按键表要不要重建。
    const quanpin::LatinWords *source() const
    {
        return words_.get();
    }
    bool empty() const
    {
        return codes_.empty();
    }
    std::size_t max_code_length() const
    {
        return max_code_length_;
    }
    // 按键与 keys 完全相同的词，权重高的在前。
    std::vector<const quanpin::LatinWord *> exact(const std::string &keys) const;
    // 按键以 keys 开头、比它长、且 keys 打全了词里第一段字母的词（不含 exact_only），权重高的在前，至多 limit 个。
    std::vector<const quanpin::LatinWord *> completions(const std::string &keys, std::size_t limit) const;

  private:
    struct Code
    {
        std::string keys;
        const quanpin::LatinWord *word = nullptr;
        // 补全门槛：keys 里到词中第一段连续拉丁字母为止的键数（字母一个键，汉字两个键）。
        std::size_t completion_keys = 0;

        bool operator==(const Code &other) const
        {
            return keys == other.keys && word == other.word;
        }
    };
    std::shared_ptr<const quanpin::LatinWords> words_;
    // 按按键升序，同按键按权重降序。一个音节在有的方案里有两种敲法（小鹤 xu 也能敲成 xv），都收。
    std::vector<Code> codes_;
    std::size_t max_code_length_ = 0;
};
