#pragma once

#include "../quanpin/latin_words.h"
#include "shuangpin_profile.h"
#include <string>
#include <utility>
#include <vector>

// 字母词（见 quanpin::LatinWords）在一套双拼方案下的按键：字母仍是一个键，汉字的音节换成这套方案的
// 两个键（小鹤：T恤 txu、B超 bic、AA制 aavi）。双拼的贪心切分会把 tx 切成「tie」，先切分再换算成
// 全拼去查是查不到的，只能拿原始按键直接比。
class ShuangpinLatinWords
{
  public:
    void build(const quanpin::LatinWords &words, const ShuangpinProfile &profile);
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
    // 按键以 keys 开头、比它长的词（不含 exact_only），权重高的在前，至多 limit 个。
    std::vector<const quanpin::LatinWord *> completions(const std::string &keys, std::size_t limit) const;

  private:
    // 按按键升序，同按键按权重降序。一个音节在有的方案里有两种敲法（小鹤 xu 也能敲成 xv），都收。
    std::vector<std::pair<std::string, const quanpin::LatinWord *>> codes_;
    std::size_t max_code_length_ = 0;
};
