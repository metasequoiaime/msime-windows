#pragma once

#include <string>
#include <windows.h>
#include <vector>

std::string wstring_to_string(const std::wstring &);
std::wstring string_to_wstring(const std::string &);
void SendImeInputs(std::wstring words);

namespace CommonUtils
{
std::wstring get_local_appdata_path_w();
std::wstring get_ime_data_path_w();
// Directory holding config.toml. Same as the data directory unless METASEQUOIA_IME_CONFIG_DIR
// redirects it (tests only).
std::wstring get_ime_config_dir_w();
void ensure_ime_data_writable();
std::string get_local_appdata_path();
std::string get_ime_data_path();
std::string get_username();
std::vector<std::wstring> cvt_str_to_vector(std::wstring text);
} // namespace CommonUtils