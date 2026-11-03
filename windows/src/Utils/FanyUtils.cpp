#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include "Define.h"
#include "Globals.h"
#include "FanyUtils.h"
#include "Ipc.h"
#include "VimMode.h"
#include <utf8cpp/utf8.h>
#include <fmt/xchar.h>

using namespace std;

namespace FanyUtils
{
namespace
{
constexpr ULONGLONG kConfigRecheckMs = 1000;

std::string TrimAscii(const std::string &value)
{
    size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\r'))
    {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r'))
    {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string UnquoteTomlBasicString(const std::string &value)
{
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

bool ParseTomlBool(const std::string &raw, bool fallback)
{
    const std::string value = to_lower_copy(UnquoteTomlBasicString(TrimAscii(raw)));
    if (value == "true" || value == "1")
    {
        return true;
    }
    if (value == "false" || value == "0")
    {
        return false;
    }
    return fallback;
}

// Build a wide path and open it as such. A narrow std::string path would be opened through the
// ANSI code page, which cannot round-trip a non-ASCII (e.g. Chinese) user profile path on a
// non-UTF-8 system, so the TSF would read the wrong file or fail to find the config.
// GetEnvironmentVariableW rather than _wgetenv: the CRT variant is deprecated (C4996) because
// it hands out a pointer into a buffer another thread can invalidate, and this DLL runs inside
// arbitrary hosts.
std::wstring EnvironmentValue(const wchar_t *name)
{
    std::wstring value(MAX_PATH, L'\0');
    DWORD length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (length > value.size())
    {
        // The variable is longer than MAX_PATH; length is now the required size including the NUL.
        value.resize(length);
        length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    }
    if (length == 0 || length > value.size())
    {
        return {};
    }
    value.resize(length);
    return value;
}

// The data directory the user picked during setup. This DLL is also built 32-bit and loads into
// 32-bit hosts, so KEY_WOW64_64KEY is mandatory: without it the read is redirected to
// Wow6432Node, where the 64-bit setup never wrote anything.
enum class InstalledDataDirStatus
{
    Found,
    Absent,
    Retry
};

struct InstalledDataDirResult
{
    InstalledDataDirStatus status;
    std::wstring path;
};

InstalledDataDirResult ReadInstalledDataDir()
{
    HKEY key = nullptr;
    const LSTATUS openStatus = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Metasequoia\\MetasequoiaIME", 0,
                                             KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (openStatus != ERROR_SUCCESS)
    {
        return {openStatus == ERROR_FILE_NOT_FOUND || openStatus == ERROR_PATH_NOT_FOUND
                    ? InstalledDataDirStatus::Absent
                    : InstalledDataDirStatus::Retry,
                {}};
    }

    DWORD type = 0;
    DWORD bytes = 0;
    const LSTATUS queryStatus = RegQueryValueExW(key, L"DataDir", nullptr, &type, nullptr, &bytes);
    if (queryStatus != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t))
    {
        RegCloseKey(key);
        return {queryStatus == ERROR_FILE_NOT_FOUND ? InstalledDataDirStatus::Absent : InstalledDataDirStatus::Retry,
                {}};
    }

    std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
    const LSTATUS status =
        RegQueryValueExW(key, L"DataDir", nullptr, &type, reinterpret_cast<LPBYTE>(value.data()), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ)
    {
        return {InstalledDataDirStatus::Retry, {}};
    }
    // REG_SZ carries no guaranteed terminator and the stored length may or may not include one.
    value.resize(std::wcslen(value.c_str()));
    if (value.empty() || !std::filesystem::path(value).is_absolute())
    {
        return {InstalledDataDirStatus::Retry, {}};
    }
    return {InstalledDataDirStatus::Found, std::move(value)};
}

std::wstring InstalledDataDir(bool *found)
{
    // Cache only a valid installed path. Retry missing or failed reads once per second.
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::wstring *cached = nullptr;
    static ULONGLONG checkedTick = 0;
    static bool checked = false;
    AcquireSRWLockExclusive(&lock);
    std::wstring result;
    *found = false;
    try
    {
        const ULONGLONG now = GetTickCount64();
        if (cached == nullptr && (!checked || now - checkedTick >= kConfigRecheckMs))
        {
            checked = true;
            checkedTick = now;
            InstalledDataDirResult read = ReadInstalledDataDir();
            if (read.status == InstalledDataDirStatus::Found)
            {
                cached = new std::wstring(std::move(read.path));
            }
        }
        if (cached != nullptr)
        {
            result = *cached;
            *found = true;
        }
    }
    catch (...)
    {
        // Retry after an allocation or path error.
    }
    ReleaseSRWLockExclusive(&lock);
    return result;
}

// Same resolution order as the Server (server/src/utils/ime_paths.cpp) and the engine
// (engine/core/data_path.h): environment override, then the location chosen during setup, then
// the default under LocalAppData. All three must agree or the TSF reads a different config than
// the one the Server writes.
std::filesystem::path SharedDataDirectory(bool *stable)
{
    *stable = false;
    const std::wstring fromEnv = EnvironmentValue(L"METASEQUOIA_IME_DATA_DIR");
    if (!fromEnv.empty() && std::filesystem::path(fromEnv).is_absolute())
    {
        *stable = true;
        return fromEnv;
    }

    bool installedFound = false;
    const std::wstring installed = InstalledDataDir(&installedFound);
    if (installedFound)
    {
        *stable = true;
        return installed;
    }

    const std::wstring localAppData = EnvironmentValue(L"LOCALAPPDATA");
    if (localAppData.empty())
    {
        return {};
    }
    return std::filesystem::path(localAppData) / L"metasequoiaime";
}

std::filesystem::path SharedConfigPath(bool *stable = nullptr)
{
    bool pathStable = false;
    const std::filesystem::path directory = SharedDataDirectory(&pathStable);
    if (stable != nullptr)
    {
        *stable = pathStable;
    }
    if (directory.empty())
    {
        return {};
    }
    return directory / L"config.toml";
}
} // namespace

namespace
{
// A mode switch reads these values more than once on the host UI thread.
// Check the file once per second and parse it only after a change.
struct InputConfigValues
{
    BOOL chinese = TRUE;
    int punctuationLock = Global::PunctuationLock::Follow;
    BOOL japanese = FALSE;
};

struct InputConfigCache
{
    bool pathStable = false;
    bool checked = false;
    bool haveStamp = false;
    ULONGLONG checkedTick = 0;
    FILETIME lastWrite = {};
    DWORD sizeLow = 0;
    DWORD sizeHigh = 0;
    InputConfigValues values;
};

SRWLOCK g_inputConfigLock = SRWLOCK_INIT;
InputConfigCache g_inputConfig;
std::filesystem::path *g_inputConfigPath = nullptr;

bool ParseInputConfig(const std::filesystem::path &path, InputConfigValues &values)
{
    std::ifstream input(path);
    if (!input)
    {
        return false;
    }

    bool inInputSection = false;
    bool sawDefaultMode = false;
    bool sawPunctuationLock = false;
    bool sawMode = false;
    std::string line;
    while (std::getline(input, line))
    {
        const size_t comment = line.find('#');
        if (comment != std::string::npos)
        {
            line = line.substr(0, comment);
        }
        line = TrimAscii(line);
        if (line.empty())
        {
            continue;
        }
        if (line.front() == '[' && line.back() == ']')
        {
            inInputSection = (line == "[input]");
            continue;
        }
        if (!inInputSection)
        {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        const std::string key = TrimAscii(line.substr(0, eq));
        const std::string value = to_lower_copy(UnquoteTomlBasicString(TrimAscii(line.substr(eq + 1))));
        if (key == "default_ime_mode" && !sawDefaultMode)
        {
            values.chinese = value != "english";
            sawDefaultMode = true;
        }
        else if (key == "punctuation_lock" && !sawPunctuationLock)
        {
            if (value == "chinese")
            {
                values.punctuationLock = Global::PunctuationLock::AlwaysChinese;
            }
            else if (value == "english")
            {
                values.punctuationLock = Global::PunctuationLock::AlwaysEnglish;
            }
            sawPunctuationLock = true;
        }
        else if (key == "mode" && !sawMode)
        {
            values.japanese = value == "japanese";
            sawMode = true;
        }
    }
    return !input.bad();
}

InputConfigValues ReadInputConfig()
{
    AcquireSRWLockExclusive(&g_inputConfigLock);
    try
    {
        const ULONGLONG now = GetTickCount64();
        if (!g_inputConfig.checked || now - g_inputConfig.checkedTick >= kConfigRecheckMs)
        {
            g_inputConfig.checked = true;
            g_inputConfig.checkedTick = now;
            if (!g_inputConfig.pathStable)
            {
                bool stable = false;
                const std::filesystem::path path = SharedConfigPath(&stable);
                if (g_inputConfigPath == nullptr)
                {
                    g_inputConfigPath = new std::filesystem::path(path);
                }
                else if (*g_inputConfigPath != path)
                {
                    *g_inputConfigPath = path;
                    g_inputConfig.haveStamp = false;
                }
                g_inputConfig.pathStable = stable;
            }
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (g_inputConfigPath->empty() ||
                !GetFileAttributesExW(g_inputConfigPath->c_str(), GetFileExInfoStandard, &attributes))
            {
                g_inputConfig.values = InputConfigValues{};
                g_inputConfig.haveStamp = false;
            }
            else if (!g_inputConfig.haveStamp ||
                     CompareFileTime(&attributes.ftLastWriteTime, &g_inputConfig.lastWrite) != 0 ||
                     attributes.nFileSizeLow != g_inputConfig.sizeLow ||
                     attributes.nFileSizeHigh != g_inputConfig.sizeHigh)
            {
                InputConfigValues parsed;
                if (ParseInputConfig(*g_inputConfigPath, parsed))
                {
                    g_inputConfig.values = parsed;
                    g_inputConfig.lastWrite = attributes.ftLastWriteTime;
                    g_inputConfig.sizeLow = attributes.nFileSizeLow;
                    g_inputConfig.sizeHigh = attributes.nFileSizeHigh;
                    g_inputConfig.haveStamp = true;
                }
                else
                {
                    g_inputConfig.haveStamp = false;
                }
            }
        }
    }
    catch (...)
    {
        g_inputConfig.haveStamp = false;
    }
    const InputConfigValues result = g_inputConfig.values;
    ReleaseSRWLockExclusive(&g_inputConfigLock);
    return result;
}
} // namespace

BOOL ReadConfiguredDefaultImeModeChinese()
{
    return ReadInputConfig().chinese;
}

int ReadConfiguredPunctuationLock()
{
    return ReadInputConfig().punctuationLock;
}

void RefreshPunctuationLockFromConfig()
{
    Global::PunctuationLockMode.store(ReadConfiguredPunctuationLock(), std::memory_order_relaxed);
}

BOOL ReadConfiguredJapaneseInputMode()
{
    return ReadInputConfig().japanese;
}

LanguageBarVisibility ReadConfiguredLanguageBarVisibility()
{
    LanguageBarVisibility result;

    const std::filesystem::path configPath = SharedConfigPath();
    if (configPath.empty())
    {
        return result;
    }

    std::ifstream input(configPath);
    if (!input)
    {
        return result;
    }

    bool inLanguageBarSection = false;
    std::string line;
    while (std::getline(input, line))
    {
        const size_t comment = line.find('#');
        if (comment != std::string::npos)
        {
            line = line.substr(0, comment);
        }
        line = TrimAscii(line);
        if (line.empty())
        {
            continue;
        }
        if (line.front() == '[' && line.back() == ']')
        {
            inLanguageBarSection = (line == "[language_bar]");
            continue;
        }
        if (!inLanguageBarSection)
        {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        const std::string key = TrimAscii(line.substr(0, eq));
        const std::string raw = line.substr(eq + 1);
        if (key == "enabled")
        {
            result.enabled = ParseTomlBool(raw, true);
        }
        else if (key == "show_ime_mode")
        {
            result.ime_mode = ParseTomlBool(raw, true);
        }
        else if (key == "show_double_single_byte")
        {
            result.double_single_byte = ParseTomlBool(raw, true);
        }
        else if (key == "show_punctuation")
        {
            result.punctuation = ParseTomlBool(raw, true);
        }
    }

    return result;
}

namespace
{
SwitchLanguageHotkeys ParseSwitchLanguageHotkeys(const std::filesystem::path &configPath)
{
    SwitchLanguageHotkeys result;
    std::array<std::string, FanyImeInputStateShortcuts::Definitions.size()> shortcutValues;
    auto values = FanyImeInputStateShortcuts::Defaults();
    std::ifstream input(configPath);
    if (!input)
    {
        return result;
    }

    bool inKeybindings = false;
    bool sawShift = false;
    bool sawCtrl = false;
    bool sawCtrlAltSpace = false;
    std::string line;
    while (std::getline(input, line))
    {
        const size_t comment = line.find('#');
        if (comment != std::string::npos)
        {
            line = line.substr(0, comment);
        }
        line = TrimAscii(line);
        if (line.empty())
        {
            continue;
        }
        if (line.front() == '[' && line.back() == ']')
        {
            inKeybindings = (line == "[keybindings]");
            continue;
        }
        if (!inKeybindings)
        {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        const std::string key = TrimAscii(line.substr(0, eq));
        const std::string raw = line.substr(eq + 1);
        if (key == "switch_language_shift")
        {
            result.shift = ParseTomlBool(raw, true);
            sawShift = true;
        }
        else if (key == "switch_language_ctrl")
        {
            result.ctrl = ParseTomlBool(raw, false);
            sawCtrl = true;
        }
        else if (key == "switch_language_ctrl_alt_space")
        {
            result.ctrl_alt_space = ParseTomlBool(raw, true);
            sawCtrlAltSpace = true;
        }
        else if (key == "toggle_character_set_ctrl_shift_f")
        {
            result.character_set_ctrl_shift_f = ParseTomlBool(raw, true);
        }
        else if (key == "commit_pinyin_shift_enter")
        {
            result.commit_pinyin_shift_enter = ParseTomlBool(raw, true);
        }
        else if (key == "switch_language" && !sawShift && !sawCtrlAltSpace)
        {
            // Legacy array: switch_language = ["Ctrl+Space", "Shift"]
            result.shift = raw.find("Shift") != std::string::npos;
            result.ctrl_alt_space =
                raw.find("Ctrl+Alt+Space") != std::string::npos || raw.find("Ctrl+Space") != std::string::npos;
        }
        for (std::size_t i = 0; i < values.size(); ++i)
            if (key == FanyImeInputStateShortcuts::Definitions[i].key)
            {
                shortcutValues[i] = FanyImeKeyChord::TomlString(raw);
                values[i] = shortcutValues[i];
            }
    }
    result.input_state = FanyImeInputStateShortcuts::Resolve(values);
    (void)sawCtrl;
    return result;
}

// The key sink asks for these hotkeys on every key down and key up, on the host's UI thread.
// Resolve fallback paths again after one second. A failed registry read can later recover.
// Parse the file only when its timestamp or size changes. These values use constant initialization
// because this DLL avoids global constructors and uses /Zc:threadSafeInit-.
constexpr ULONGLONG kHotkeyConfigRecheckMs = 1000;

struct HotkeyConfigCache
{
    bool pathStable = false;
    bool checked = false;
    bool haveStamp = false;
    ULONGLONG checkedTick = 0;
    FILETIME lastWrite = {};
    DWORD sizeLow = 0;
    DWORD sizeHigh = 0;
    SwitchLanguageHotkeys value;
};

SRWLOCK g_hotkeyConfigLock = SRWLOCK_INIT;
HotkeyConfigCache g_hotkeyConfig;
// Heap-allocated on first use and intentionally never freed, so it needs no global destructor.
std::filesystem::path *g_hotkeyConfigPath = nullptr;
} // namespace

SwitchLanguageHotkeys ReadConfiguredSwitchLanguageHotkeys()
{
    AcquireSRWLockExclusive(&g_hotkeyConfigLock);
    SwitchLanguageHotkeys result = g_hotkeyConfig.value;
    try
    {
        const ULONGLONG now = GetTickCount64();
        const bool due = !g_hotkeyConfig.checked || now - g_hotkeyConfig.checkedTick >= kHotkeyConfigRecheckMs;
        if (due)
        {
            g_hotkeyConfig.checked = true;
            g_hotkeyConfig.checkedTick = now;
            if (!g_hotkeyConfig.pathStable)
            {
                bool stable = false;
                const std::filesystem::path path = SharedConfigPath(&stable);
                if (g_hotkeyConfigPath == nullptr)
                {
                    g_hotkeyConfigPath = new std::filesystem::path(path);
                }
                else if (*g_hotkeyConfigPath != path)
                {
                    *g_hotkeyConfigPath = path;
                    g_hotkeyConfig.haveStamp = false;
                }
                g_hotkeyConfig.pathStable = stable;
            }
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (!g_hotkeyConfigPath->empty() &&
                GetFileAttributesExW(g_hotkeyConfigPath->c_str(), GetFileExInfoStandard, &attributes))
            {
                const bool changed = !g_hotkeyConfig.haveStamp ||
                                     CompareFileTime(&attributes.ftLastWriteTime, &g_hotkeyConfig.lastWrite) != 0 ||
                                     attributes.nFileSizeLow != g_hotkeyConfig.sizeLow ||
                                     attributes.nFileSizeHigh != g_hotkeyConfig.sizeHigh;
                if (changed)
                {
                    g_hotkeyConfig.value = ParseSwitchLanguageHotkeys(*g_hotkeyConfigPath);
                    g_hotkeyConfig.lastWrite = attributes.ftLastWriteTime;
                    g_hotkeyConfig.sizeLow = attributes.nFileSizeLow;
                    g_hotkeyConfig.sizeHigh = attributes.nFileSizeHigh;
                    g_hotkeyConfig.haveStamp = true;
                }
            }
            else
            {
                // A missing config means defaults, as the uncached read returned; the stamp
                // stays unset so the file is picked up as soon as the Server writes it.
                g_hotkeyConfig.value = SwitchLanguageHotkeys{};
                g_hotkeyConfig.haveStamp = false;
            }
        }
        result = g_hotkeyConfig.value;
    }
    catch (...)
    {
        // Keep the last known hotkeys; a read failure must not take the key path down.
    }
    ReleaseSRWLockExclusive(&g_hotkeyConfigLock);
    return result;
}

// Same one-second stamp cache as the hotkeys above: the key sink asks for this on every
// translation chord while the settings page writes the file underneath it. Constant-initialized,
// so the local statics carry no guard variable and no destructor even with /Zc:threadSafeInit-.
bool ReadConfiguredCandidateTranslationsEnabled()
{
    struct Cache
    {
        ULONGLONG checkedTick = 0;
        FILETIME lastWrite{};
        DWORD sizeLow = 0;
        DWORD sizeHigh = 0;
        bool checked = false;
        bool haveStamp = false;
        bool value = false;
    };
    static Cache cache;
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    const ULONGLONG now = GetTickCount64();
    if (!cache.checked || now - cache.checkedTick >= kHotkeyConfigRecheckMs)
    {
        cache.checked = true;
        cache.checkedTick = now;
        try
        {
            const std::filesystem::path path = SharedConfigPath();
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
            {
                cache.value = false;
                cache.haveStamp = false;
            }
            else if (!cache.haveStamp || CompareFileTime(&attributes.ftLastWriteTime, &cache.lastWrite) != 0 ||
                     attributes.nFileSizeLow != cache.sizeLow || attributes.nFileSizeHigh != cache.sizeHigh)
            {
                cache.haveStamp = true;
                cache.lastWrite = attributes.ftLastWriteTime;
                cache.sizeLow = attributes.nFileSizeLow;
                cache.sizeHigh = attributes.nFileSizeHigh;
                bool parsed = false;
                std::ifstream input(path, std::ios::binary);
                if (!input)
                {
                    cache.haveStamp = false;
                }
                else
                {
                    bool inGeneral = false;
                    std::string line;
                    while (std::getline(input, line))
                    {
                        const size_t comment = line.find('#');
                        if (comment != std::string::npos)
                        {
                            line = line.substr(0, comment);
                        }
                        line = TrimAscii(line);
                        if (line.empty())
                        {
                            continue;
                        }
                        if (line.front() == '[' && line.back() == ']')
                        {
                            inGeneral = (line == "[general]");
                            continue;
                        }
                        if (!inGeneral)
                        {
                            continue;
                        }
                        const size_t eq = line.find('=');
                        if (eq == std::string::npos)
                        {
                            continue;
                        }
                        const std::string key = TrimAscii(line.substr(0, eq));
                        const std::string raw = line.substr(eq + 1);
                        if (key == "candidate_translations")
                        {
                            parsed = ParseTomlBool(raw, false);
                        }
                    }
                    cache.value = parsed;
                }
            }
        }
        catch (...)
        {
            cache.value = false;
            cache.haveStamp = false;
        }
    }
    const bool result = cache.value;
    ReleaseSRWLockExclusive(&lock);
    return result;
}

bool ReadConfiguredVimMode()
{
    struct Cache
    {
        ULONGLONG checkedTick = 0;
        FILETIME lastWrite{};
        DWORD sizeLow = 0;
        DWORD sizeHigh = 0;
        bool checked = false;
        bool haveStamp = false;
        bool enabled = false;
    };
    static Cache cache;
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    const ULONGLONG now = GetTickCount64();
    if (!cache.checked || now - cache.checkedTick >= 1000)
    {
        cache.checked = true;
        cache.checkedTick = now;
        try
        {
            const std::filesystem::path path = SharedConfigPath();
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
            {
                cache.enabled = false;
                cache.haveStamp = false;
            }
            else if (!cache.haveStamp || CompareFileTime(&attributes.ftLastWriteTime, &cache.lastWrite) != 0 ||
                     attributes.nFileSizeLow != cache.sizeLow || attributes.nFileSizeHigh != cache.sizeHigh)
            {
                cache.enabled = false;
                cache.haveStamp = true;
                cache.lastWrite = attributes.ftLastWriteTime;
                cache.sizeLow = attributes.nFileSizeLow;
                cache.sizeHigh = attributes.nFileSizeHigh;
                std::ifstream input(path, std::ios::binary);
                const std::string config((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
                if (!input.is_open() || input.bad())
                {
                    // A temporary sharing/read failure must be retried even if the file stamp is unchanged.
                    cache.haveStamp = false;
                }
                else
                {
                    cache.enabled = VimMode::EnabledForProcess(config, wstring_to_string(GetCurrentProcessName()));
                }
            }
        }
        catch (...)
        {
            cache.enabled = false;
            cache.haveStamp = false;
        }
    }
    const bool enabled = cache.enabled;
    ReleaseSRWLockExclusive(&lock);
    return enabled;
}

void SendKeys(std::wstring pinyin)
{
    for (wchar_t ch : pinyin)
    {
        INPUT in[2]{};

        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wScan = ch;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;

        in[1] = in[0];
        in[1].ki.dwFlags |= KEYEVENTF_KEYUP;

        UINT sent = SendInput(2, in, sizeof(INPUT));
        if (sent != 2)
        {
        }
    }
}

std::wstring string_to_wstring(const std::string &str)
{
    std::u16string utf16result;
    utf8::utf8to16(str.begin(), str.end(), std::back_inserter(utf16result));
    return std::wstring(utf16result.begin(), utf16result.end());
}

std::string wstring_to_string(const std::wstring &wstr)
{
    std::string result;
    utf8::utf16to8(wstr.begin(), wstr.end(), std::back_inserter(result));
    return result;
}

std::string to_lower_copy(const std::string &str)
{
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::tolower(c); });
    return result;
}

std::wstring GetCurrentProcessName()
{
    TCHAR fullPath[MAX_PATH] = {0};
    if (GetModuleFileName(NULL, fullPath, MAX_PATH) == 0)
        return L"";

    std::wstring wfullPath(fullPath);
    size_t pos = wfullPath.find_last_of(L"\\/");
    std::wstring wname = (pos != std::wstring::npos) ? wfullPath.substr(pos + 1) : wfullPath;
    return wname;
}

/**
 * @brief Count UTF-8 chars
 *
 * @param str
 * @return string::size_type
 */
string::size_type count_utf8_chars(const string &str)
{
    return utf8::distance(str.begin(), str.end());
}
} // namespace FanyUtils
