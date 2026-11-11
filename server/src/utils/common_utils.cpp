#include "common_utils.h"
#include <boost/locale.hpp>
#include <memory>

using namespace std;

std::string wstring_to_string(const std::wstring &wstr)
{
    return boost::locale::conv::utf_to_utf<std::string::value_type>(wstr);
}

std::wstring string_to_wstring(const std::string &str)
{
    return boost::locale::conv::utf_to_utf<std::wstring::value_type>(str);
}

void SendUnicode(const wchar_t data)
{
    INPUT input[4];

    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = 0;
    input[0].ki.wScan = data;
    input[0].ki.dwFlags = KEYEVENTF_UNICODE;
    input[0].ki.time = 0;
    input[0].ki.dwExtraInfo = GetMessageExtraInfo();
    SendInput(1, &input[0], sizeof(INPUT));

    input[1].type = INPUT_KEYBOARD;
    input[1].ki.wVk = 0;
    input[1].ki.wScan = data;
    input[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    input[1].ki.time = 0;
    input[1].ki.dwExtraInfo = GetMessageExtraInfo();
    SendInput(1, &input[1], sizeof(INPUT));
}

//
// In telegram, SendUnicode when words are en chars will trigger tsf, so we need to send char directly
//
void SendCharToForeground(wchar_t ch)
{
    HWND hwnd = GetForegroundWindow();
    PostMessage(hwnd, WM_CHAR, ch, 0);
}

void SendImeInputs(std::wstring words)
{
    for (wchar_t ch : words)
    {
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z'))
        {
            SendCharToForeground(ch);
        }
        else
        {
            SendUnicode(ch);
        }
    }
}

namespace CommonUtils
{
string get_local_appdata_path()
{
    return wstring_to_string(get_local_appdata_path_w());
}

string get_ime_data_path()
{
    return wstring_to_string(get_ime_data_path_w());
}

string get_username()
{
    char *localAppDataDir = nullptr;
    std::string localAppDataDirStr;
    errno_t err = _dupenv_s(&localAppDataDir, nullptr, "USERNAME");
    if (err == 0 && localAppDataDir != nullptr)
    {
        localAppDataDirStr = std::string(localAppDataDir);
    }
    std::unique_ptr<char, decltype(&free)> dirPtr(localAppDataDir, free);
    return localAppDataDirStr.empty() ? "" : localAppDataDirStr;
}

std::vector<std::wstring> cvt_str_to_vector(std::wstring text)
{
    std::vector<std::wstring> lines;
    size_t start = 0;
    size_t end = text.find(L',');
    while (end != std::wstring::npos)
    {
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
        end = text.find(L',', start);
    }
    lines.push_back(text.substr(start));
    return lines;
}
} // namespace CommonUtils