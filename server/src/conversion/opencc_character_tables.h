#pragma once

#include <string>
#include <vector>

// OpenCC 两张单字字表的原始查询。字表与 chinese_converter.cpp 里的配置共用同一套延迟载入与 Preload，
// 所以定义在那里；候选的繁体写法要经 TraditionalSpellingIndex 取，不直接调这里的函数。
namespace ChineseConverter
{
/**
 * 单字 character 在简转繁字表（STCharacters）里的全部写法，按字表顺序，不做任何过滤。
 * 不在表里、不是单字或字表载入失败时返回空列表，不抛异常。
 */
std::vector<std::string> CharacterTableSpellings(const std::string &character);

/**
 * 单字 character 在繁转简字表（TSCharacters）里的第一个值，即 OpenCC 繁转简取的那个字。
 * 不在表里、不是单字或字表载入失败时返回空串，不抛异常。
 */
std::string TraditionalToSimplifiedCharacter(const std::string &character);

/**
 * character 是不是独立的简体字：繁转简字表里有它的条目，且第一个值是它自己。
 * 不在表里、不是单字或字表载入失败时为假，不抛异常。
 */
bool IsIndependentSimplifiedCharacter(const std::string &character);
} // namespace ChineseConverter
