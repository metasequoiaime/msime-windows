#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <utf8.h>
#include <spdlog/spdlog.h>
#include <vector>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include "shuangpin_utils.h"
#include "../core/data_path.h"
#include "../quanpin/quanpin_utils.h"

using namespace std;

const string ShuangpinUtil::app_name = "metasequoiaime";

string ShuangpinUtil::get_local_appdata_path()
{
    return metasequoia::path_to_utf8(metasequoia::data_directory().parent_path());
}

namespace
{
std::string find_source_by_code(const std::unordered_map<std::string, std::string> &mapping, const std::string &code)
{
    for (const auto &[source, mapped_code] : mapping)
    {
        if (mapped_code == code)
        {
            return source;
        }
    }
    return {};
}

const std::unordered_set<std::string> &shuangpin_pinyin_set()
{
    static const std::unordered_set<std::string> pinyin_set = [] {
        auto result = quanpin::intact_pinyin_set();

        // Preserve the historical exclusions that affect ambiguous shuangpin
        // segmentation, but accept "yo" as the complete syllable for "哟".
        result.insert("eng");
        for (const char *pinyin : {"chua", "den", "fiao", "lo", "lue", "nou", "nue", "nun", "zhei"})
        {
            result.erase(pinyin);
        }
        // j/q/x/y 的 v 别名（jve、xvan……）解码器从不产出：cvt_single_sp_to_pinyin 把它们的 v 读成 u。
        for (auto it = result.begin(); it != result.end();)
        {
            it = quanpin::is_jqxy_v_alias(*it) ? result.erase(it) : std::next(it);
        }
        return result;
    }();
    return pinyin_set;
}
} // namespace

/**
 * @brief Convert one shuangpin syllable to quanpin using the selected profile
 *
 * Currently support 402 pinyin
 *
 * @param sp_str Shuangpin string
 * @return string Quanpin string
 */
string ShuangpinUtil::cvt_single_sp_to_pinyin(string sp_str, const ShuangpinProfile &profile)
{
    const std::string zero_initial = find_source_by_code(profile.zero_initials, sp_str);
    if (!zero_initial.empty())
    {
        return zero_initial;
    }
    if (sp_str.size() != 2)
        return "";
    string res = "";
    string sm;
    vector<string> ym_list;

    sm = find_source_by_code(profile.initials, sp_str.substr(0, 1));
    if (sm.empty())
        sm = sp_str.substr(0, 1);

    for (const auto &pair : profile.finals)
    {
        if (pair.second == sp_str.substr(1, 1))
        {
            ym_list.push_back(pair.first);
        }
    }
    if (sm == "" || ym_list.size() == 0)
    {
        return "";
    }
    for (const auto &ym : ym_list)
    {
        std::string normalized_ym = ym;
        if (ym == "v" && (sm == "j" || sm == "q" || sm == "x" || sm == "y"))
        {
            normalized_ym = "u";
        }
        if (shuangpin_pinyin_set().count(sm + normalized_ym) > 0)
        {
            res = sm + normalized_ym;
        }
    }
    return res;
}

bool ShuangpinUtil::is_accepted_syllable_code(const std::string &sp_str, const ShuangpinProfile &profile)
{
    return shuangpin_pinyin_set().count(cvt_single_sp_to_pinyin(sp_str, profile)) > 0;
}

const std::unordered_set<std::string> &ShuangpinUtil::decodable_syllables()
{
    return shuangpin_pinyin_set();
}

/**
 * @brief Split shuangpin, using ' as delimiter, using forward greedy segmentation
 *
 * @param sp_str Shuangpin string
 * @return string Segmented string with '
 */
string ShuangpinUtil::pinyin_segmentation(string sp_str, const ShuangpinProfile &profile)
{
    if (sp_str.size() == 1)
    {
        return sp_str;
    }
    string res("");
    string::size_type range_start = 0;
    while (range_start < sp_str.size())
    {
        if ((range_start + 2) <= sp_str.size())
        {
            // Try to cut two chars to test
            string cur_sp = sp_str.substr(range_start, 2);
            if (is_accepted_syllable_code(boost::algorithm::to_lower_copy(cur_sp), profile))
            {
                res = res + "'" + cur_sp;
                range_start += 2;
            }
            else
            {
                res = res + "'" + cur_sp.substr(0, 1);
                range_start += 1;
            }
        }
        else
        {
            res = res + "'" + sp_str.substr(sp_str.size() - 1, 1);
            range_start += 1;
        }
    }
    while (!res.empty() && res[0] == '\'')
    {
        res.erase(0, 1);
    }
    while (!res.empty() && res[res.size()] == '\'')
    {
        res.erase(res.size() - 1, 1);
    }
    return res;
}

/**
 * @brief Get the first han char
 *
 * @param words
 * @return std::string
 */
/**
 * @brief Get first UTF-8 char size
 *
 * @param words UTF-8 string
 * @return string::size_type Char size
 */
string::size_type ShuangpinUtil::get_first_char_size(string words)
{
    size_t cplen = 1;
    if ((words[0] & 0xf8) == 0xf0)
        cplen = 4;
    else if ((words[0] & 0xf0) == 0xe0)
        cplen = 3;
    else if ((words[0] & 0xe0) == 0xc0)
        cplen = 2;
    if (cplen > words.length())
        cplen = 1;
    return cplen;
}

/**
 * @brief Count UTF-8 chars
 *
 * @param str
 * @return string::size_type
 */
string::size_type ShuangpinUtil::count_utf8_chars(const string &str)
{
    return utf8::distance(str.begin(), str.end());
}

/**
 * @brief Extract preview without helpcodes
 *
 * @param candidate UTF-8 string
 * @return string Pure hanzi string
 */
string ShuangpinUtil::extract_preview(string candidate)
{
    size_t start_pos = candidate.find('(');
    if (start_pos != string::npos)
    {
        return candidate.substr(0, start_pos);
    }
    return candidate;
}

/**
 * @brief Check if all pinyin is quanpin
 *
 * @param pure_pinyin Pure shuangpin
 * @param seg_pinyin Segmented shuangpin
 * @return true If all pinyin is quanpin
 * @return false Otherwise
 */
bool ShuangpinUtil::is_all_complete_pinyin(string pure_pinyin, string seg_pinyin)
{
    if (pure_pinyin.size() % 2)
        return false;
    auto pinyin_size = seg_pinyin.size();
    size_t index = 0;
    while (index < pinyin_size)
    {
        if (seg_pinyin[index] == '\'' || seg_pinyin[index + 1] == '\'')
            return false;
        index += 3;
    }
    return true;
}

/**
 * @brief Convert segmented shuangpin to segmented complete pinyin
 *
 * @param seg_shangpin Segmented shuangpin
 * @return string Segmented quanpin
 */
string ShuangpinUtil::convert_seg_shuangpin_to_seg_complete_pinyin(string seg_shangpin, const ShuangpinProfile &profile)
{
    vector<string> splitted_shuangpin;
    boost::split(splitted_shuangpin, seg_shangpin, boost::is_any_of("'"));
    string res = "";
    for (auto each : splitted_shuangpin)
    {
        if (each.size() == 1)
        {
            const std::string initial = find_source_by_code(profile.initials, each);
            res += (initial.empty() ? each : initial) + "'";
        }
        else if (each.size() == 2)
        {
            res += cvt_single_sp_to_pinyin(each, profile) + "'";
        }
    }
    return res.substr(0, res.size() - 1);
}

/**
 * @brief 判断是否是全码辅助
 *
 * @param pinyin
 * @return true 长度为偶数，且末尾两个辅助码中至少一个是大写字母，且前面都是完整的双拼
 * @return false
 */
bool ShuangpinUtil::IsFullHelpMode(std::string pinyin, const ShuangpinProfile &profile)
{
    auto len = pinyin.size();
    if (len == 0 || len == 2)
        return false;
    if (len % 2 != 0)
        return false;
    auto pure_pinyin = pinyin.substr(0, len - 2);
    if (is_all_complete_pinyin(pure_pinyin, pinyin_segmentation(pure_pinyin, profile)))
    {
        const bool first_helpcode_is_upper = pinyin[len - 2] >= 'A' && pinyin[len - 2] <= 'Z';
        const bool second_helpcode_is_upper = pinyin[len - 1] >= 'A' && pinyin[len - 1] <= 'Z';
        if (first_helpcode_is_upper || second_helpcode_is_upper)
        {
            return true;
        }
    }
    return false;
}

std::string ShuangpinUtil::GetFullHelpCodes(std::string pinyin)
{
    if (pinyin.size() < 2)
    {
        return "";
    }

    const bool reverse = pinyin[pinyin.size() - 2] >= 'A' && pinyin[pinyin.size() - 2] <= 'Z';
    std::string help_codes = pinyin.substr(pinyin.size() - 2, 2);
    std::transform(help_codes.begin(), help_codes.end(), help_codes.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (reverse)
    {
        std::swap(help_codes[0], help_codes[1]);
    }
    return help_codes;
}

namespace shuangpin
{

std::string get_local_appdata_path()
{
    return ShuangpinUtil::get_local_appdata_path();
}

std::string get_app_name()
{
    return ShuangpinUtil::app_name;
}

std::filesystem::path get_data_file_path(const std::filesystem::path &relative_path)
{
    return metasequoia::data_file_path(relative_path);
}

} // namespace shuangpin
