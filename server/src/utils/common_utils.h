#pragma once

#include <string>
#include <windows.h>
#include <vector>

void ShowErrorMessage(HWND, const std::wstring &);

std::string wstring_to_string(const std::wstring &);
std::wstring string_to_wstring(const std::string &);
void SendImeInputs(std::wstring words);

namespace CommonUtils
{
// Evergreen Bootstrapper 的官方直链，点开即下载安装器，用户不用在文档页里自己找。
inline constexpr wchar_t kWebView2RuntimeDownloadUrl[] = L"https://go.microsoft.com/fwlink/p/?LinkId=2124703";
std::wstring get_local_appdata_path_w();
std::wstring get_ime_data_path_w();
// Directory holding config.toml. Same as the data directory unless METASEQUOIA_IME_CONFIG_DIR
// redirects it (tests only).
std::wstring get_ime_config_dir_w();
void ensure_ime_data_writable();
// WebView2 cache lives under ProgramData so Medium-IL Edge children can write
// it whether the host is a normal user or elevated. Falls back to LocalAppData.
std::wstring get_webview2_user_data_path(const std::wstring &folder_name);
std::string get_local_appdata_path();
std::string get_ime_data_path();
std::string get_username();
std::vector<std::wstring> cvt_str_to_vector(std::wstring text);
} // namespace CommonUtils