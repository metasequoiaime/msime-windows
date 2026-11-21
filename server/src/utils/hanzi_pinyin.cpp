#include "utils/hanzi_pinyin.h"

#include <cpp-pinyin/G2pglobal.h>
#include <cpp-pinyin/Pinyin.h>

#include <windows.h>
#include <filesystem>
#include <memory>
#include <mutex>

namespace HanziPinyin
{
namespace
{
std::filesystem::path ExecutableDirectory()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    return std::filesystem::path(path).parent_path();
}
} // namespace

// cpp-pinyin 的词典由 CMake 的 POST_BUILD 拷到设置程序可执行文件旁的 dict\。现在只有设置进程
// 用它（导入词条时给纯中文词注音）：U 模式候选的读音随拆字表烘进来，Server 不再加载这份词典。
// 词典缺失或损坏时 cpp-pinyin 会抛出，这里吞掉并留空实例——初始化失败不能把设置页打崩。
Pinyin::Pinyin *Engine()
{
    static std::once_flag once;
    static std::unique_ptr<Pinyin::Pinyin> engine;
    std::call_once(once, [] {
        try
        {
            Pinyin::setDictionaryPath(ExecutableDirectory() / "dict");
            engine = std::make_unique<Pinyin::Pinyin>();
        }
        catch (...)
        {
            engine.reset();
        }
    });
    return engine.get();
}
} // namespace HanziPinyin
