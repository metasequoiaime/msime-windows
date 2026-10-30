#pragma once

#include "conversion/traditional_forms.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ChineseConverter
{
/**
 * 繁体写法要查的全部数据。可以从多个线程同时调用；数据载入失败时各查询按各自写明的方式降级，不抛异常。
 */
class TraditionalSpellingIndex
{
  public:
    virtual ~TraditionalSpellingIndex() = default;

    /** simplified 的标准、台湾、香港三种繁体写法，不做过滤。 */
    virtual TraditionalForms FormsOf(const std::string &simplified) const = 0;

    /**
     * 单字 character 的字表写法，按字表顺序。字表里不等于 character、本身又是独立简体字的写法不在其中。
     * 不在表里、不是单字时返回空列表。
     */
    virtual std::vector<std::string> CharacterTableSpellingsOf(const std::string &character) const = 0;

    /** text 是不是简体写法：每个字都不会被繁转简转成别的字时为真。空串为真。 */
    virtual bool IsSimplifiedSpelling(const std::string &text) const = 0;

    /**
     * 单字写法 spelling 的首选底词：繁转简时它转成的那个字，不转换时是它自己。不是单字时返回空串。
     */
    virtual std::string PreferredWordOf(const std::string &spelling) const = 0;
};

inline bool IsSingleUtf8CodePoint(const std::string &text)
{
    if (text.empty())
        return false;
    const auto lead = static_cast<unsigned char>(text.front());
    const size_t length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : 4;
    return text.size() == length;
}

/**
 * character 的字表写法 table 里，去掉不等于 character、且 is_independent_simplified_character 为真的写法，
 * 其余按原顺序返回。
 */
template <typename IsIndependentSimplifiedCharacter>
std::vector<std::string> WithoutOtherIndependentSimplifiedCharacters(
    const std::string &character, std::vector<std::string> table,
    IsIndependentSimplifiedCharacter &&is_independent_simplified_character)
{
    std::vector<std::string> kept;
    kept.reserve(table.size());
    for (auto &spelling : table)
    {
        if (spelling != character && is_independent_simplified_character(static_cast<const std::string &>(spelling)))
            continue;
        kept.push_back(std::move(spelling));
    }
    return kept;
}

/**
 * 读 OpenCC 字典的索引，进程内一份。字典第一次使用时载入，载入失败的那一份不再重试：标准繁体转换配置
 * 载入失败时标准写法是原文，台湾、香港写法在原文上转；台湾或香港转换配置载入失败时，那一种写法是标准写法；
 * 简转繁字表载入失败时没有字表写法；繁转简字表载入失败时字表写法不做过滤、每段文字都算简体写法、
 * 单字的首选底词是它自己。
 */
const TraditionalSpellingIndex &OpenCcTraditionalSpellingIndex();
} // namespace ChineseConverter
