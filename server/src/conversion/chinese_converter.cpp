#include "chinese_converter.h"

#include <Windows.h>
#include <filesystem>
#include <memory>
#include <SimpleConverter.hpp>
#include <vector>

namespace ChineseConverter
{
namespace
{
std::filesystem::path ResourceDirectory()
{
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
        return {};
    executable.resize(length);
    return std::filesystem::path(executable).parent_path() / L"assets" / L"opencc";
}

// Returns nullptr when the config or one of its dictionaries cannot be read.
std::unique_ptr<opencc::SimpleConverter> LoadConverter(const char *config_file)
{
    try
    {
        const auto resources = ResourceDirectory();
        if (!resources.empty())
            return std::make_unique<opencc::SimpleConverter>(config_file,
                                                             std::vector<std::string>{resources.u8string()});
    }
    catch (...)
    {
    }
    return nullptr;
}

// Each converter is loaded once, on first use, and kept for the process lifetime.
opencc::SimpleConverter *SimplifiedToTraditional()
{
    static const std::unique_ptr<opencc::SimpleConverter> converter = LoadConverter("s2t.json");
    return converter.get();
}

opencc::SimpleConverter *TraditionalToSimplified()
{
    static const std::unique_ptr<opencc::SimpleConverter> converter = LoadConverter("t2s.json");
    return converter.get();
}

std::string ConvertOrKeep(opencc::SimpleConverter *(*converter)(), const std::string &text)
{
    if (text.empty())
        return text;
    try
    {
        if (auto *loaded = converter())
            return loaded->Convert(text);
    }
    catch (...)
    {
    }
    return text;
}
} // namespace

std::string ToTraditional(const std::string &text)
{
    return ConvertOrKeep(SimplifiedToTraditional, text);
}

std::string ToSimplified(const std::string &text)
{
    return ConvertOrKeep(TraditionalToSimplified, text);
}
} // namespace ChineseConverter
