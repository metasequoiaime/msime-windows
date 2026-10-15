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

// cpp-pinyin 的词典由 CMake 的 POST_BUILD 拷到可执行文件旁的 dict\。Server 与设置进程各自
// 初始化一次：设置页的导入注音也用它，两边共用同一份词典拷贝，只是进程内各持一个实例。
// 词典缺失或损坏时 cpp-pinyin 会抛出，这里吞掉并留空实例——候选组装在按键路径上，绝不能
// 让第三方初始化失败把输入法打崩。
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

std::string Annotate(const std::string &hanzi)
{
    if (hanzi.empty())
    {
        return {};
    }
    Pinyin::Pinyin *annotator = Engine();
    if (annotator == nullptr)
    {
        return {};
    }
    try
    {
        // 带调号（jīng）比不带调（jing）更能说清生僻字怎么读；多音字取 cpp-pinyin 的默认读音。
        const auto results =
            annotator->hanziToPinyin(hanzi, Pinyin::ManTone::Style::TONE, Pinyin::Error::Default, false, false, false);
        if (results.empty())
        {
            return {};
        }
        std::string joined;
        for (const auto &item : results)
        {
            if (item.error)
            {
                return {};
            }
            if (!joined.empty())
            {
                joined.push_back(' ');
            }
            joined += item.pinyin;
        }
        return joined;
    }
    catch (...)
    {
        return {};
    }
}
} // namespace HanziPinyin
