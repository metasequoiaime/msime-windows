#pragma once

namespace Pinyin
{
class Pinyin;
}

// 汉字 → 拼音的 cpp-pinyin 实例。只有设置进程的导入注音用它：U 模式候选的读音已经烘进
// 拆字表（见 engine/radical/NOTICE.md），Server 不再加载这份词典。
namespace HanziPinyin
{
// 进程内唯一的 cpp-pinyin 实例，词典取可执行文件旁的 dict\。词典缺失或损坏时返回 nullptr，
// 调用方按「注音不可用」降级。首次调用要加载词典，按键路径之外可以先调一次预热。
Pinyin::Pinyin *Engine();
} // namespace HanziPinyin
