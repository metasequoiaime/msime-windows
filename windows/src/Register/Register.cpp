#ifndef UNICODE
#define UNICODE
#endif // !UNICODE

#ifndef _UNICODE
#define _UNICODE
#endif // !UNICODE

#include "Private.h"
#include "Globals.h"
#include "RegKey.h"
#include "GameCompat.h"
#include <string>

static const WCHAR RegInfo_Prefix_CLSID[] = L"CLSID\\";
static const WCHAR RegInfo_Key_InProSvr32[] = L"InProcServer32";
static const WCHAR RegInfo_Key_ThreadModel[] = L"ThreadingModel";

// IME text service description, will be displayed in the language menu when switching IME
static const WCHAR TEXTSERVICE_DESC[] = L"Metasequoia \u6c34\u6749\u8f93\u5165\u6cd5";

// Valve game compatibility: CS2 / Dota 2 only handle IME messages for input methods whose registered profile name is
// on a whitelist inside the game, and otherwise let Windows pop its default composition window, which throws an
// exclusive-fullscreen game out of fullscreen. Registration has to honor the name the user chose in the settings
// page: RegisterProfile overwrites the name on every install or upgrade, and unregistering first deletes the CTF key,
// so the current Description cannot tell us what the user chose.

static const GUID SupportCategories[] = {
    GUID_TFCAT_TIP_KEYBOARD,
    GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
    GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
    GUID_TFCAT_TIPCAP_SECUREMODE,
    GUID_TFCAT_TIPCAP_COMLESS,
    GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT,
    GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
    GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
};

// Copy of the profile icon (msime.ico) that the installer keeps at a fixed path, one level above
// the DLL's directory. The installer puts the DLL into a fresh msime_v* directory on every upgrade,
// so an icon path pointing at the DLL itself changes with each install; if the taskbar re-reads the
// icon while that path is in flux, it falls back to a text label for the language and keeps showing
// that until explorer restarts.
static const WCHAR TEXTSERVICE_STABLE_ICON_FILE[] = L"MetasequoiaImeTip.ico";

//+---------------------------------------------------------------------------
//
//  GetStableIconFile
//
//  Builds <parent of the DLL's directory>\MetasequoiaImeTip.ico and reports whether that file
//  exists. Development registrations from a build directory have no such file and keep using the
//  icon resource embedded in the DLL.
//
//----------------------------------------------------------------------------

static BOOL GetStableIconFile(_In_ LPCWSTR modulePath, _Out_writes_(cchIconFile) LPWSTR iconFile, size_t cchIconFile)
{
    if (StringCchCopy(iconFile, cchIconFile, modulePath) != S_OK)
    {
        return FALSE;
    }

    // Strip the file name, then the version directory.
    for (int i = 0; i < 2; ++i)
    {
        WCHAR *separator = wcsrchr(iconFile, L'\\');
        if (!separator)
        {
            return FALSE;
        }
        *separator = L'\0';
    }

    if (StringCchCat(iconFile, cchIconFile, L"\\") != S_OK ||
        StringCchCat(iconFile, cchIconFile, TEXTSERVICE_STABLE_ICON_FILE) != S_OK)
    {
        return FALSE;
    }

    DWORD attributes = GetFileAttributes(iconFile);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

//+---------------------------------------------------------------------------
//
//  RegisterProfiles
//
//----------------------------------------------------------------------------

BOOL RegisterProfiles()
{
    HRESULT hr = S_FALSE;

    ITfInputProcessorProfileMgr *pITfInputProcessorProfileMgr = nullptr;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfileMgr,
                          (void **)&pITfInputProcessorProfileMgr);
    if (FAILED(hr))
    {
        return FALSE;
    }

    WCHAR achIconFile[MAX_PATH] = {'\0'};
    DWORD cchA = 0;
    cchA = GetModuleFileName(Global::dllInstanceHandle, achIconFile, MAX_PATH);
    cchA = cchA >= MAX_PATH ? (MAX_PATH - 1) : cchA;
    achIconFile[cchA] = '\0';
    UINT iconIndex = (UINT)TEXTSERVICE_ICON_INDEX;

    // A truncated module path would yield a wrong parent directory, so only look for the stable
    // icon when GetModuleFileName returned the full path.
    WCHAR stableIconFile[MAX_PATH] = {'\0'};
    if (cchA > 0 && cchA < MAX_PATH - 1 && GetStableIconFile(achIconFile, stableIconFile, ARRAYSIZE(stableIconFile)))
    {
        size_t lenOfStableIconFile = 0;
        if (StringCchLength(stableIconFile, ARRAYSIZE(stableIconFile), &lenOfStableIconFile) == S_OK &&
            StringCchCopy(achIconFile, ARRAYSIZE(achIconFile), stableIconFile) == S_OK)
        {
            cchA = static_cast<DWORD>(lenOfStableIconFile);
            iconIndex = 0; // a .ico file holds one icon (in several sizes)
        }
    }

    {
        const std::wstring compatName = Global::ReadGameCompatProfileName();
        const WCHAR *profileName = compatName.empty() ? TEXTSERVICE_DESC : compatName.c_str();
        size_t lenOfDesc = 0;
        hr = StringCchLength(profileName, STRSAFE_MAX_CCH, &lenOfDesc);
        if (hr != S_OK)
        {
            goto Exit;
        }
        hr = pITfInputProcessorProfileMgr->RegisterProfile(
            Global::MetasequoiaIMECLSID, TEXTSERVICE_LANGID, Global::MetasequoiaIMEGuidProfile, profileName,
            static_cast<ULONG>(lenOfDesc), achIconFile, cchA, iconIndex, NULL, 0, TRUE, 0);
    }

    if (FAILED(hr))
    {
        goto Exit;
    }

Exit:
    if (pITfInputProcessorProfileMgr)
    {
        pITfInputProcessorProfileMgr->Release();
    }

    return (hr == S_OK);
}

//+---------------------------------------------------------------------------
//
//  UnregisterProfiles
//
//----------------------------------------------------------------------------

void UnregisterProfiles()
{
    HRESULT hr = S_OK;

    ITfInputProcessorProfileMgr *pITfInputProcessorProfileMgr = nullptr;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfileMgr,
                          (void **)&pITfInputProcessorProfileMgr);
    if (FAILED(hr))
    {
        goto Exit;
    }

    hr = pITfInputProcessorProfileMgr->UnregisterProfile(Global::MetasequoiaIMECLSID, TEXTSERVICE_LANGID,
                                                         Global::MetasequoiaIMEGuidProfile, 0);
    if (FAILED(hr))
    {
        goto Exit;
    }

Exit:
    if (pITfInputProcessorProfileMgr)
    {
        pITfInputProcessorProfileMgr->Release();
    }

    return;
}

//+---------------------------------------------------------------------------
//
//  RegisterCategories
//
//----------------------------------------------------------------------------

BOOL RegisterCategories()
{
    ITfCategoryMgr *pCategoryMgr = nullptr;
    HRESULT hr = S_OK;

    hr = CoCreateInstance(CLSID_TF_CategoryMgr, NULL, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr, (void **)&pCategoryMgr);
    if (FAILED(hr))
    {
        return FALSE;
    }

    // for each (GUID guid in SupportCategories)
    for (const auto &guid : SupportCategories)
    {
        hr = pCategoryMgr->RegisterCategory(Global::MetasequoiaIMECLSID, guid, Global::MetasequoiaIMECLSID);
    }

    pCategoryMgr->Release();

    return (hr == S_OK);
}

//+---------------------------------------------------------------------------
//
//  UnregisterCategories
//
//----------------------------------------------------------------------------

void UnregisterCategories()
{
    ITfCategoryMgr *pCategoryMgr = S_OK;
    HRESULT hr = S_OK;

    hr = CoCreateInstance(CLSID_TF_CategoryMgr, NULL, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr, (void **)&pCategoryMgr);
    if (FAILED(hr))
    {
        return;
    }

    // for each (GUID guid in SupportCategories)
    for (const auto &guid : SupportCategories)
    {
        pCategoryMgr->UnregisterCategory(Global::MetasequoiaIMECLSID, guid, Global::MetasequoiaIMECLSID);
    }

    pCategoryMgr->Release();

    return;
}

//+---------------------------------------------------------------------------
//
// RecurseDeleteKey
//
// RecurseDeleteKey is necessary because on NT RegDeleteKey doesn't work if the
// specified key has subkeys
//----------------------------------------------------------------------------

LONG RecurseDeleteKey(_In_ HKEY hParentKey, _In_ LPCTSTR lpszKey)
{
    HKEY regKeyHandle = nullptr;
    LONG res = 0;
    FILETIME time;
    WCHAR stringBuffer[256] = {'\0'};
    DWORD size = ARRAYSIZE(stringBuffer);

    if (RegOpenKey(hParentKey, lpszKey, &regKeyHandle) != ERROR_SUCCESS)
    {
        return ERROR_SUCCESS;
    }

    res = ERROR_SUCCESS;
    while (RegEnumKeyEx(regKeyHandle, 0, stringBuffer, &size, NULL, NULL, NULL, &time) == ERROR_SUCCESS)
    {
        stringBuffer[ARRAYSIZE(stringBuffer) - 1] = '\0';
        res = RecurseDeleteKey(regKeyHandle, stringBuffer);
        if (res != ERROR_SUCCESS)
        {
            break;
        }
        size = ARRAYSIZE(stringBuffer);
    }
    RegCloseKey(regKeyHandle);

    return res == ERROR_SUCCESS ? RegDeleteKey(hParentKey, lpszKey) : res;
}

//+---------------------------------------------------------------------------
//
//  RegisterServer
//
//----------------------------------------------------------------------------

BOOL RegisterServer()
{
    DWORD copiedStringLen = 0;
    HKEY regKeyHandle = nullptr;
    HKEY regSubkeyHandle = nullptr;
    BOOL ret = FALSE;
    WCHAR achIMEKey[ARRAYSIZE(RegInfo_Prefix_CLSID) + CLSID_STRLEN] = {'\0'};
    WCHAR achFileName[MAX_PATH] = {'\0'};

    if (!CLSIDToString(Global::MetasequoiaIMECLSID, achIMEKey + ARRAYSIZE(RegInfo_Prefix_CLSID) - 1))
    {
        return FALSE;
    }

    memcpy(achIMEKey, RegInfo_Prefix_CLSID, sizeof(RegInfo_Prefix_CLSID) - sizeof(WCHAR));

    if (RegCreateKeyEx(HKEY_CLASSES_ROOT, achIMEKey, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &regKeyHandle,
                       &copiedStringLen) == ERROR_SUCCESS)
    {
        if (RegSetValueEx(regKeyHandle, NULL, 0, REG_SZ, (const BYTE *)TEXTSERVICE_DESC,
                          (_countof(TEXTSERVICE_DESC)) * sizeof(WCHAR)) != ERROR_SUCCESS)
        {
            goto Exit;
        }

        if (RegCreateKeyEx(regKeyHandle, RegInfo_Key_InProSvr32, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL,
                           &regSubkeyHandle, &copiedStringLen) == ERROR_SUCCESS)
        {
            copiedStringLen = GetModuleFileNameW(Global::dllInstanceHandle, achFileName, ARRAYSIZE(achFileName));
            copiedStringLen = (copiedStringLen >= (MAX_PATH - 1)) ? MAX_PATH : (++copiedStringLen);
            if (RegSetValueEx(regSubkeyHandle, NULL, 0, REG_SZ, (const BYTE *)achFileName,
                              (copiedStringLen) * sizeof(WCHAR)) != ERROR_SUCCESS)
            {
                goto Exit;
            }
            if (RegSetValueEx(regSubkeyHandle, RegInfo_Key_ThreadModel, 0, REG_SZ, (const BYTE *)TEXTSERVICE_MODEL,
                              (_countof(TEXTSERVICE_MODEL)) * sizeof(WCHAR)) != ERROR_SUCCESS)
            {
                goto Exit;
            }

            ret = TRUE;
        }
    }

Exit:
    if (regSubkeyHandle)
    {
        RegCloseKey(regSubkeyHandle);
        regSubkeyHandle = nullptr;
    }
    if (regKeyHandle)
    {
        RegCloseKey(regKeyHandle);
        regKeyHandle = nullptr;
    }

    return ret;
}

//+---------------------------------------------------------------------------
//
//  UnregisterServer
//
//----------------------------------------------------------------------------

void UnregisterServer()
{
    WCHAR achIMEKey[ARRAYSIZE(RegInfo_Prefix_CLSID) + CLSID_STRLEN] = {'\0'};

    if (!CLSIDToString(Global::MetasequoiaIMECLSID, achIMEKey + ARRAYSIZE(RegInfo_Prefix_CLSID) - 1))
    {
        return;
    }

    memcpy(achIMEKey, RegInfo_Prefix_CLSID, sizeof(RegInfo_Prefix_CLSID) - sizeof(WCHAR));

    RecurseDeleteKey(HKEY_CLASSES_ROOT, achIMEKey);
}
