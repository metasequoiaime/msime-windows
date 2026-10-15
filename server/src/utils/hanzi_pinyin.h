#pragma once

#include <string>

// 汉字 → 注音（复用 Server 已链接的 cpp-pinyin），带声调（jīng）。U 模式候选用它反显读音；
// 多音字取 cpp-pinyin 的默认读音。只覆盖 CJK 基本区，扩展区或无法识别的字返回空串
// （前端按「无注音」渲染）。
namespace HanziPinyin
{
std::string Annotate(const std::string &hanzi);
}
