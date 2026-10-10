#include "default_input_method.h"

#include <shellapi.h>

#include <vector>

namespace default_input_method
{
namespace
{

// 系统用来标识 TSF 输入法的「语言:{CLSID}{Profile GUID}」串。
// 与 windows/src/Global/Globals.cpp 的 MetasequoiaIMECLSID、MetasequoiaIMEGuidProfile 保持一致。
constexpr wchar_t kProfileId[] = L"0804:{E3062E9A-D834-4637-8958-ED8CFA427D01}{4D59B1B4-D503-44AE-9259-BAD9BB2778AB}";

constexpr wchar_t kUserProfileKey[] = L"Control Panel\\International\\User Profile";

using InstallLayoutOrTipFn = BOOL(WINAPI *)(LPCWSTR, DWORD);
using SetDefaultLayoutOrTipFn = BOOL(WINAPI *)(LPCWSTR, DWORD);
// bcp47langs.dll 的导出，系统设置的「替代默认输入法」与 Set-WinDefaultInputMethodOverride
// 都经由它写 InputMethodOverride。签名取自 Microsoft.InternationalSettings.Commands 的 P/Invoke 声明。
using SetInputMethodOverrideFn = HRESULT(WINAPI *)(LPCWSTR);

bool IsOurProfile(const wchar_t *id)
{
    return CompareStringOrdinal(id, -1, kProfileId, -1, TRUE) == CSTR_EQUAL;
}

std::wstring ReadString(const wchar_t *subkey, const wchar_t *value, DWORD type_flags)
{
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, subkey, value, type_flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t))
        return {};
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, subkey, value, type_flags, nullptr, buffer.data(), &bytes) != ERROR_SUCCESS)
        return {};
    return std::wstring(buffer.data());
}

// 某个语言下的输入法：值名是输入法串，DWORD 数据是在该语言里的排序（从 1 开始）。
struct LanguageInputs
{
    bool contains_ours = false;
    std::wstring first;
};

LanguageInputs ReadLanguageInputs(const std::wstring &language)
{
    LanguageInputs result;
    const std::wstring subkey = std::wstring(kUserProfileKey) + L"\\" + language;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return result;

    DWORD best_order = MAXDWORD;
    for (DWORD index = 0;; ++index)
    {
        wchar_t name[256];
        DWORD name_length = ARRAYSIZE(name);
        DWORD type = 0;
        DWORD order = 0;
        DWORD order_bytes = sizeof(order);
        const LSTATUS status = RegEnumValueW(key, index, name, &name_length, nullptr, &type,
                                             reinterpret_cast<BYTE *>(&order), &order_bytes);
        if (status == ERROR_NO_MORE_ITEMS)
            break;
        // 其他值（CachedLanguageName 等）不是 DWORD 或比 DWORD 大，读失败后跳过即可。
        if (status != ERROR_SUCCESS || type != REG_DWORD || wcschr(name, L':') == nullptr)
            continue;
        if (IsOurProfile(name))
            result.contains_ours = true;
        if (order < best_order)
        {
            best_order = order;
            result.first = name;
        }
    }
    RegCloseKey(key);
    return result;
}

std::vector<std::wstring> ReadLanguages()
{
    std::vector<std::wstring> languages;
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kUserProfileKey, L"Languages", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &bytes) !=
        ERROR_SUCCESS)
        return languages;
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 2, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kUserProfileKey, L"Languages", RRF_RT_REG_MULTI_SZ, nullptr, buffer.data(),
                     &bytes) != ERROR_SUCCESS)
        return languages;
    for (const wchar_t *item = buffer.data(); *item != L'\0'; item += wcslen(item) + 1)
        languages.emplace_back(item);
    return languages;
}

} // namespace

Status QueryStatus()
{
    Status status;
    const std::vector<std::wstring> languages = ReadLanguages();
    std::wstring first_of_first_language;
    for (size_t i = 0; i < languages.size(); ++i)
    {
        const LanguageInputs inputs = ReadLanguageInputs(languages[i]);
        status.enabled = status.enabled || inputs.contains_ours;
        if (i == 0)
            first_of_first_language = inputs.first;
    }

    // 用户在「替代默认输入法」里选过就以那一项为准；没选过（默认值「使用语言列表」）时，
    // 默认输入法是语言列表首个语言里排第一的输入法。
    const std::wstring override_id = ReadString(kUserProfileKey, L"InputMethodOverride", RRF_RT_REG_SZ);
    const std::wstring &effective = override_id.empty() ? first_of_first_language : override_id;
    status.is_default = status.enabled && !effective.empty() && IsOurProfile(effective.c_str());
    return status;
}

bool SetAsDefault(std::string &error)
{
    HMODULE input = LoadLibraryExW(L"input.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE languages = LoadLibraryExW(L"bcp47langs.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto install =
        input ? reinterpret_cast<InstallLayoutOrTipFn>(GetProcAddress(input, "InstallLayoutOrTip")) : nullptr;
    const auto set_default =
        input ? reinterpret_cast<SetDefaultLayoutOrTipFn>(GetProcAddress(input, "SetDefaultLayoutOrTip")) : nullptr;
    const auto set_override =
        languages ? reinterpret_cast<SetInputMethodOverrideFn>(GetProcAddress(languages, "SetInputMethodOverride"))
                  : nullptr;

    bool ok = false;
    if (!install || (!set_override && !set_default))
    {
        error = "当前系统不支持自动设置默认输入法，请在系统设置中手动设置。";
    }
    else if (!QueryStatus().enabled && !install(kProfileId, 0))
    {
        error = "无法把水杉输入法加入键盘列表，请确认已安装中文（简体）语言。";
    }
    else if (set_override ? FAILED(set_override(kProfileId)) : !set_default(kProfileId, 0))
    {
        error = "系统拒绝了设置默认输入法的请求，请在系统设置中手动设置。";
    }
    else if (!QueryStatus().is_default)
    {
        error = "系统没有应用这项设置，请在系统设置中手动选择水杉输入法。";
    }
    else
    {
        ok = true;
    }
    if (languages)
        FreeLibrary(languages);
    if (input)
        FreeLibrary(input);
    return ok;
}

void OpenSystemSettings(HWND owner)
{
    ShellExecuteW(owner, L"open", L"ms-settings:keyboard", nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace default_input_method
