// Builds a ShuangpinProfile from a user-described layout and proves it unambiguous by
// round-tripping every syllable through the same decoder the input session uses.
#include "shuangpin_profile.h"
#include "shuangpin_utils.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_set>

namespace
{
// Finals every layout has to place. "ue" and "ve" stand in for each other when only one is given.
constexpr std::string_view kFinals[] = {"a",   "o",  "e",   "i",   "u",    "v",    "ai", "ei", "ui",  "ao",  "ou",
                                        "iu",  "ie", "ue",  "ve",  "an",   "en",   "in", "un", "ang", "eng", "ing",
                                        "ong", "ia", "iao", "ian", "iang", "iong", "ua", "uo", "uai", "uan", "uang"};
constexpr std::string_view kZeroInitialFinals[] = {"a",  "ai", "an",  "ang", "ao", "e",
                                                   "ei", "en", "eng", "er",  "o",  "ou"};
constexpr std::string_view kInitials[] = {"b", "p", "m", "f", "d", "t", "n", "l", "g",  "k",  "h", "j",
                                          "q", "x", "r", "z", "c", "s", "y", "w", "zh", "ch", "sh"};
constexpr std::string_view kRetroflexInitials[] = {"zh", "ch", "sh"};
constexpr size_t kMaxReportedConflicts = 8;

template <size_t N> bool contains(const std::string_view (&names)[N], std::string_view name)
{
    return std::find(std::begin(names), std::end(names), name) != std::end(names);
}

std::string lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

bool is_letter(char ch)
{
    return ch >= 'a' && ch <= 'z';
}

bool is_final_key(const std::string &key)
{
    return key.size() == 1 && (is_letter(key[0]) || key[0] == ';');
}

std::string zero_initial_code(ShuangpinZeroInitialRule rule, const std::string &final, const std::string &key)
{
    switch (rule)
    {
    case ShuangpinZeroInitialRule::LetterO:
        return "o" + key;
    case ShuangpinZeroInitialRule::LetterA:
        return "a" + key;
    case ShuangpinZeroInitialRule::FullPinyin:
        if (final.size() == 2)
            return final;
        [[fallthrough]];
    case ShuangpinZeroInitialRule::FirstLetter:
        break;
    }
    return final.substr(0, 1) + key;
}

// Splits a syllable the way a layout types it; empty initial means a zero-initial syllable.
std::pair<std::string, std::string> split_syllable(const std::string &syllable)
{
    if (syllable.size() > 2 && syllable[1] == 'h' && std::string_view("zcs").find(syllable[0]) != std::string::npos)
        return {syllable.substr(0, 2), syllable.substr(2)};
    if (std::string_view("aeo").find(syllable[0]) != std::string::npos)
        return {{}, syllable};
    return {syllable.substr(0, 1), syllable.substr(1)};
}

void check_round_trip(const ShuangpinProfile &profile, std::vector<std::string> &errors)
{
    std::vector<std::string> sorted(ShuangpinUtil::decodable_syllables().begin(),
                                    ShuangpinUtil::decodable_syllables().end());
    std::sort(sorted.begin(), sorted.end());
    size_t conflicts = 0;
    for (const auto &syllable : sorted)
    {
        std::string code;
        const auto [initial, final] = split_syllable(syllable);
        if (initial.empty())
        {
            const auto zero = profile.zero_initials.find(syllable);
            if (zero == profile.zero_initials.end())
                continue;
            code = zero->second;
        }
        else
        {
            const auto final_key = profile.finals.find(final);
            if (final.empty() || final_key == profile.finals.end())
                continue;
            const auto initial_key = profile.initials.find(initial);
            code = (initial_key == profile.initials.end() ? initial : initial_key->second) + final_key->second;
        }
        const std::string decoded = ShuangpinUtil::cvt_single_sp_to_pinyin(code, profile);
        if (decoded == syllable)
            continue;
        if (++conflicts <= kMaxReportedConflicts)
        {
            errors.push_back(decoded.empty() ? "音节 " + syllable + " 按 " + code + " 输入时无法识别"
                                             : "音节 " + syllable + " 按 " + code + " 输入时会被读成 " + decoded);
        }
    }
    if (conflicts > kMaxReportedConflicts)
        errors.push_back("另有 " + std::to_string(conflicts - kMaxReportedConflicts) + " 个音节存在同样的冲突");
}
} // namespace

std::optional<ShuangpinZeroInitialRule> ParseShuangpinZeroInitialRule(std::string_view name)
{
    if (name == "o")
        return ShuangpinZeroInitialRule::LetterO;
    if (name == "a")
        return ShuangpinZeroInitialRule::LetterA;
    if (name == "first_letter")
        return ShuangpinZeroInitialRule::FirstLetter;
    if (name == "full_pinyin")
        return ShuangpinZeroInitialRule::FullPinyin;
    return std::nullopt;
}

std::vector<std::string> BuildCustomShuangpinProfile(const CustomShuangpinLayout &layout, ShuangpinProfile &profile)
{
    std::vector<std::string> errors;
    ShuangpinProfile result;
    result.name = layout.name;

    std::unordered_map<std::string, std::string> initial_owner; // key -> initial typed with it
    for (const auto &initial : kInitials)
    {
        if (initial.size() == 1)
            initial_owner.emplace(std::string(initial), std::string(initial));
    }
    for (const auto &[raw_initial, raw_key] : layout.initials)
    {
        const std::string initial = lowercase(raw_initial);
        const std::string key = lowercase(raw_key);
        if (!contains(kInitials, initial))
        {
            errors.push_back("未知的声母「" + raw_initial + "」");
            continue;
        }
        if (key.size() != 1 || !is_letter(key[0]))
        {
            errors.push_back("声母 " + initial + " 的按键必须是一个英文字母，现在是「" + raw_key + "」");
            continue;
        }
        result.initials[initial] = key;
    }
    for (const auto &initial : kRetroflexInitials)
    {
        if (result.initials.count(std::string(initial)) == 0 && errors.empty())
            errors.push_back("缺少声母 " + std::string(initial) + " 的按键");
    }
    if (errors.empty())
    {
        // A remapped initial takes its key away from whichever initial sat there by default.
        for (const auto &[initial, key] : result.initials)
        {
            if (initial.size() == 1)
                initial_owner.erase(initial);
        }
        for (const auto &[initial, key] : result.initials)
        {
            const auto [owner, inserted] = initial_owner.emplace(key, initial);
            if (!inserted && owner->second != initial)
                errors.push_back("声母 " + owner->second + " 和 " + initial + " 都在 " + key + " 键上");
        }
    }

    std::string er_key;
    for (const auto &[raw_final, raw_key] : layout.finals)
    {
        const std::string final = lowercase(raw_final);
        const std::string key = lowercase(raw_key);
        if (final != "er" && !contains(kFinals, final))
        {
            errors.push_back("未知的韵母「" + raw_final + "」");
            continue;
        }
        if (!is_final_key(key))
        {
            errors.push_back("韵母 " + final + " 的按键必须是一个英文字母或分号，现在是「" + raw_key + "」");
            continue;
        }
        if (final == "er")
            er_key = key;
        else
            result.finals[final] = key;
    }
    if (result.finals.count("ue") == 0 && result.finals.count("ve") > 0)
        result.finals["ue"] = result.finals["ve"];
    if (result.finals.count("ve") == 0 && result.finals.count("ue") > 0)
        result.finals["ve"] = result.finals["ue"];
    std::string missing;
    for (const auto &final : kFinals)
    {
        if (result.finals.count(std::string(final)) == 0)
            missing += (missing.empty() ? "" : "、") + std::string(final);
    }
    if (!missing.empty())
        errors.push_back("缺少韵母的按键：" + missing);
    if (!errors.empty())
        return errors;

    // Most layouts put "er" on R, which is also where this falls back to when the file is silent.
    if (er_key.empty())
        er_key = "r";
    for (const auto &final_view : kZeroInitialFinals)
    {
        const std::string final(final_view);
        const std::string key = final == "er" ? er_key : result.finals.at(final);
        result.zero_initials[final] = zero_initial_code(layout.zero_initial_rule, final, key);
    }
    for (const auto &[raw_final, raw_code] : layout.zero_initial_overrides)
    {
        const std::string final = lowercase(raw_final);
        const std::string code = lowercase(raw_code);
        if (!contains(kZeroInitialFinals, final))
        {
            errors.push_back("「" + raw_final +
                             "」不是零声母音节，可以单独指定的只有 a、ai、an、ang、ao、e、ei、en、"
                             "eng、er、o、ou");
            continue;
        }
        if (code.size() != 2 || !is_letter(code[0]) || !is_final_key(code.substr(1)))
        {
            errors.push_back("零声母 " + final + " 的编码必须是两个键（第二个可以是分号），现在是「" + raw_code + "」");
            continue;
        }
        result.zero_initials[final] = code;
    }
    std::unordered_map<std::string, std::string> zero_owner;
    for (const auto &final : kZeroInitialFinals)
    {
        const std::string &code = result.zero_initials.at(std::string(final));
        const auto [owner, inserted] = zero_owner.emplace(code, std::string(final));
        if (!inserted)
            errors.push_back("零声母 " + owner->second + " 和 " + std::string(final) + " 的编码都是 " + code);
    }
    if (!errors.empty())
        return errors;

    check_round_trip(result, errors);
    if (errors.empty())
        profile = std::move(result);
    return errors;
}
