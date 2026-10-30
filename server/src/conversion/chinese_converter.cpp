#include "chinese_converter.h"

#include "opencc_character_tables.h"

#include <Windows.h>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
// OpenCC 的 Dict.hpp 里有未使用参数的默认实现，服务端按 /W4 编译时会报 C4100。
#pragma warning(push)
#pragma warning(disable : 4100)
#include <MarisaDict.hpp>
#pragma warning(pop)
#include <SimpleConverter.hpp>
#include <UTF8Util.hpp>
#include <algorithm>
#include <string_view>
#include <thread>
#include <utility>
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

/** 一份 OpenCC 配置，第一次使用时载入；载入失败后保持为空，不再重试。 */
struct LazyConverter
{
    const char *config_file;
    std::once_flag once;
    std::unique_ptr<opencc::SimpleConverter> converter;
};

LazyConverter g_standard{"s2t.json"};
LazyConverter g_taiwan{"t2tw.json"};
LazyConverter g_hong_kong{"t2hk.json"};
LazyConverter g_to_simplified{"t2s.json"};

opencc::SimpleConverter *Loaded(LazyConverter &lazy)
{
    std::call_once(lazy.once, [&lazy] {
        try
        {
            const auto resources = ResourceDirectory();
            if (!resources.empty())
                lazy.converter = std::make_unique<opencc::SimpleConverter>(
                    lazy.config_file, std::vector<std::string>{resources.u8string()});
        }
        catch (...)
        {
            lazy.converter.reset();
        }
    });
    return lazy.converter.get();
}

std::string ConvertOrKeep(LazyConverter &lazy, const std::string &text)
{
    if (text.empty())
        return text;
    try
    {
        if (auto *converter = Loaded(lazy))
            return converter->Convert(text);
    }
    catch (...)
    {
    }
    return text;
}

/** 一份单字字表，第一次使用时载入；载入失败后保持为空，不再重试。 */
struct LazyCharacterTable
{
    const wchar_t *file_name;
    std::once_flag once;
    std::shared_ptr<opencc::MarisaDict> table;
};

LazyCharacterTable g_simplified_to_traditional{L"STCharacters.ocd2"};
LazyCharacterTable g_traditional_to_simplified{L"TSCharacters.ocd2"};

const opencc::MarisaDict *Loaded(LazyCharacterTable &lazy)
{
    std::call_once(lazy.once, [&lazy] {
        try
        {
            const auto resources = ResourceDirectory();
            if (!resources.empty())
                lazy.table =
                    opencc::SerializableDict::NewFromFile<opencc::MarisaDict>((resources / lazy.file_name).u8string());
        }
        catch (...)
        {
            lazy.table.reset();
        }
    });
    return lazy.table.get();
}

// 一个字在繁转简字表里转成别的字。条目的第一个值是 OpenCC 繁转简取的那一个。
bool ConvertsToAnotherCharacter(const opencc::MarisaDict &table, std::string_view character)
{
    const auto entry = table.Match(character.data(), character.size());
    return !entry.IsNull() && entry.Get()->GetDefault() != character;
}

std::string NarrowAscii(const wchar_t *text)
{
    std::string narrow;
    for (; *text != L'\0'; ++text)
        narrow.push_back(*text < 0x80 ? static_cast<char>(*text) : '?');
    return narrow;
}

std::string UserLocation()
{
    // GetUserDefaultGeoName 从 Windows 10 1709 起才有；静态导入会让更早的系统载入 exe 时失败。
    using GetUserDefaultGeoNameFn = int(WINAPI *)(LPWSTR geo_name, int geo_name_count);
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32 == nullptr)
        return {};
    const auto get_user_default_geo_name =
        reinterpret_cast<GetUserDefaultGeoNameFn>(GetProcAddress(kernel32, "GetUserDefaultGeoName"));
    if (get_user_default_geo_name == nullptr)
        return {};
    wchar_t geo_name[16]{};
    if (get_user_default_geo_name(geo_name, static_cast<int>(std::size(geo_name))) <= 0)
        return {};
    return NarrowAscii(geo_name);
}

std::string UserLocale()
{
    wchar_t locale_name[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(locale_name, LOCALE_NAME_MAX_LENGTH) <= 0)
        return {};
    return NarrowAscii(locale_name);
}

CachedTraditionalRegion g_user_region{SystemRegionSettingReaders()};

// 定义在转换器、字表与地区缓存之后，进程退出时先于它们析构；析构时等线程结束，
// 免得它还在给那些对象赋值时它们已被析构。
BackgroundTask g_background_preload([] {
    Preload();
    (void)UserTraditionalRegion();
});
} // namespace

std::string ToTraditional(const std::string &text)
{
    return ConvertOrKeep(g_standard, text);
}

TraditionalForms ToTraditionalForms(const std::string &simplified)
{
    TraditionalForms forms;
    forms.standard = ConvertOrKeep(g_standard, simplified);
    forms.taiwan = ConvertOrKeep(g_taiwan, forms.standard);
    forms.hong_kong = ConvertOrKeep(g_hong_kong, forms.standard);
    return forms;
}

std::vector<std::string> CharacterTableSpellings(const std::string &character)
{
    if (character.empty())
        return {};
    try
    {
        if (const auto *table = Loaded(g_simplified_to_traditional))
        {
            const auto entry = table->Match(character.c_str(), character.size());
            if (!entry.IsNull())
                return entry.Get()->Values();
        }
    }
    catch (...)
    {
    }
    return {};
}

bool IsSimplifiedSpelling(const std::string &text)
{
    try
    {
        const auto *table = Loaded(g_traditional_to_simplified);
        if (table == nullptr)
            return true;
        for (size_t offset = 0; offset < text.size();)
        {
            // 不合法的首字节按一个字节算，截断在末尾的字按剩下的字节算，保证前进且不越界。
            const size_t length =
                std::min(std::max<size_t>(opencc::UTF8Util::NextCharLengthNoException(text.c_str() + offset), 1),
                         text.size() - offset);
            if (ConvertsToAnotherCharacter(*table, std::string_view(text).substr(offset, length)))
                return false;
            offset += length;
        }
    }
    catch (...)
    {
    }
    return true;
}

std::string TraditionalToSimplifiedCharacter(const std::string &character)
{
    if (character.empty())
        return {};
    try
    {
        if (const auto *table = Loaded(g_traditional_to_simplified))
        {
            const auto entry = table->Match(character.c_str(), character.size());
            if (!entry.IsNull())
                return entry.Get()->GetDefault();
        }
    }
    catch (...)
    {
    }
    return {};
}

bool IsIndependentSimplifiedCharacter(const std::string &character)
{
    return !character.empty() && TraditionalToSimplifiedCharacter(character) == character;
}

void Preload()
{
    Loaded(g_standard);
    Loaded(g_taiwan);
    Loaded(g_hong_kong);
    Loaded(g_simplified_to_traditional);
    Loaded(g_traditional_to_simplified);
}

BackgroundTask::BackgroundTask(std::function<void()> work) : work_(std::move(work))
{
}

BackgroundTask::~BackgroundTask()
{
    Join();
}

void BackgroundTask::Start()
{
    std::lock_guard lock(mutex_);
    if (started_)
        return;
    started_ = true;
    thread_ = std::thread(work_);
}

void BackgroundTask::Join()
{
    std::lock_guard lock(mutex_);
    if (thread_.joinable())
        thread_.join();
}

void PreloadInBackground()
{
    g_background_preload.Start();
}

void JoinBackgroundPreload()
{
    g_background_preload.Join();
}

RegionSettingReaders SystemRegionSettingReaders()
{
    RegionSettingReaders readers;
    readers.location = UserLocation;
    readers.locale = UserLocale;
    return readers;
}

CachedTraditionalRegion::CachedTraditionalRegion(RegionSettingReaders readers) : readers_(std::move(readers))
{
}

FanyImeIpc::TraditionalRegion CachedTraditionalRegion::Get()
{
    std::call_once(once_, [this] {
        const auto read = [](const std::function<std::string()> &reader) { return reader ? reader() : std::string(); };
        const std::string location = read(readers_.location);
        const std::string locale = read(readers_.locale);
        region_ = FanyImeIpc::ResolveTraditionalRegion(location, locale);
    });
    return region_;
}

FanyImeIpc::TraditionalRegion UserTraditionalRegion()
{
    return g_user_region.Get();
}

std::string ToSimplified(const std::string &text)
{
    return ConvertOrKeep(g_to_simplified, text);
}
} // namespace ChineseConverter
