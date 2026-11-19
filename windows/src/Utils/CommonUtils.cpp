#include "CommonUtils.h"
#include <Windows.h>
#include <Psapi.h>

std::wstring GetCurrentProcessName()
{
    DWORD pid = GetCurrentProcessId();
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess)
        return L"";

    wchar_t processName[MAX_PATH] = L"<unknown>";
    if (GetModuleBaseNameW(hProcess, nullptr, processName, MAX_PATH))
    {
        CloseHandle(hProcess);
        return std::wstring(processName);
    }

    CloseHandle(hProcess);
    return L"";
}

DWORD ReadHkcuDword(LPCWSTR subKey, LPCWSTR valueName, DWORD fallback)
{
    DWORD value = fallback;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, subKey, valueName, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
    {
        return fallback;
    }
    return value;
}
