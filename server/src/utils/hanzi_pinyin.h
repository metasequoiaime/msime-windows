#pragma once

#include <string>

namespace Pinyin
{
class Pinyin;
}

// 汉字 → 注音（复用 Server 已链接的 cpp-pinyin），带声调（jīng）。U 模式候选用它反显读音；
// 多音字取 cpp-pinyin 的默认读音。只覆盖 CJK 基本区，扩展区或无法识别的字返回空串
// （前端按「无注音」渲染）。
namespace HanziPinyin
{
// 进程内唯一的 cpp-pinyin 实例，词典取可执行文件旁的 dict\。Server 与设置进程各自持有一个。
// 词典缺失或损坏时返回 nullptr，调用方按「注音不可用」降级。首次调用要加载词典，按键路径
// 之外可以先调一次预热。
Pinyin::Pinyin *Engine();
std::string Annotate(const std::string &hanzi);
} // namespace HanziPinyin
