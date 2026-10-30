#include "quanpin_utils.h"
#include "quanpin_query.h"

#include "autocorrect_table.h"
#include "../common/helpcode_utils.h"
#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace quanpin
{
unsigned literal_mixed_pinyin_match(const std::string &raw, const std::string &code, const std::string &canonical)
{
    if (raw.find('\'') != std::string::npos || canonical.empty())
        return 0;
    size_t consumed = 0;
    for (const char ch : code)
    {
        if (ch != '\'')
        {
            if (consumed == raw.size() || raw[consumed++] != ch)
                return 0;
        }
    }
    const auto tail = raw.substr(consumed);
    if (!tail.empty() && (tail.size() > 2 || !prefix_pinyin_set().count(tail) || intact_pinyin_set().count(tail)))
        return 0;
    const auto typed = split_segments(code);
    const auto reading = split_segments(canonical);
    if (typed.size() != reading.size())
        return 0;
    bool full = false;
    bool initial = false;
    for (size_t i = 0; i < typed.size(); ++i)
    {
        if (!intact_pinyin_set().count(reading[i]))
            return 0;
        if (typed[i] == reading[i])
            full = true;
        else if (typed[i].size() == 1 && !reading[i].empty() && typed[i][0] == reading[i][0] &&
                 !intact_pinyin_set().count(typed[i]))
            initial = true;
        else
            return 0;
    }
    return full && initial ? (tail.empty() ? 1u : 2u) : 0u;
}

bool literal_jianpin_pinyin_match(const std::string &raw, const std::string &code, const std::string &canonical)
{
    if (raw.find('\'') != std::string::npos || canonical.empty())
        return false;
    const auto typed = split_segments(code);
    const auto reading = split_segments(canonical);
    if (typed.size() < 2 || typed.size() != reading.size())
        return false;
    std::string letters;
    for (size_t i = 0; i < typed.size(); ++i)
    {
        if (typed[i].size() != 1 || !prefix_pinyin_set().count(typed[i]) || intact_pinyin_set().count(typed[i]) ||
            !intact_pinyin_set().count(reading[i]) || reading[i][0] != typed[i][0])
            return false;
        letters += typed[i];
    }
    return letters == raw;
}

namespace
{
struct SparsePinyinFallbackEntry
{
    Segments replacement;
    bool append_suffix = true;
};

struct SparsePinyinFallbackRule
{
    const char *full;
    std::vector<SparsePinyinFallbackEntry> replacements;
};

std::vector<std::string> split(const std::string &text, char delimiter)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true)
    {
        const size_t pos = text.find(delimiter, start);
        if (pos == std::string::npos)
        {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
}

bool is_complete_pinyin_part(const std::string &part)
{
    if (part.empty())
    {
        return false;
    }

    // 要能回溯：贪心在 zaijinuta 上取了 jin 就剩下切不动的 uta，把合法的 zai'ji'nu'ta
    // 判成手误，预编辑和选词随之去走纠错。与查询侧的切分同一个算法。
    return !cut_one_piece_min_segments(part, true).empty();
}

Segments append_rest(const Segments &head, const Segments &segments)
{
    Segments combined = head;
    if (segments.size() > 1)
    {
        combined.insert(combined.end(), segments.begin() + 1, segments.end());
    }
    return combined;
}

const std::vector<SparsePinyinFallbackRule> &sparse_pinyin_fallback_rules()
{
    static const std::vector<SparsePinyinFallbackRule> kRules = {
        {"dia", {{Segments{"di", "a"}, true}, {Segments{"di"}, false}}},
        {"biang", {{Segments{"bi", "ang"}, true}, {Segments{"bi"}, false}}},
        {"gei", {{Segments{"ge"}, false}}},
        {"yo", {{Segments{"y"}, false}}},
    };
    return kRules;
}

} // namespace

const std::vector<std::string> &intact_pinyin_list()
{
    static const std::vector<std::string> kList = {
        "a",     "ai",    "an",    "ang",    "ao",    "ba",   "bai",  "ban",   "bang",  "bao",    "bei",   "ben",
        "beng",  "bi",    "bian",  "biang",  "biao",  "bie",  "bin",  "bing",  "bo",    "bu",     "ca",    "cai",
        "can",   "cang",  "cao",   "ce",     "cen",   "ceng", "cha",  "chai",  "chan",  "chang",  "chao",  "che",
        "chen",  "cheng", "chi",   "chong",  "chou",  "chu",  "chua", "chuai", "chuan", "chuang", "chui",  "chun",
        "chuo",  "ci",    "cong",  "cou",    "cu",    "cuan", "cui",  "cun",   "cuo",   "da",     "dai",   "dan",
        "dang",  "dao",   "de",    "dei",    "den",   "deng", "di",   "dia",   "dian",  "diao",   "die",   "ding",
        "diu",   "dong",  "dou",   "du",     "duan",  "dui",  "dun",  "duo",   "e",     "ei",     "en",    "er",
        "fa",    "fan",   "fang",  "fei",    "fen",   "feng", "fiao", "fo",    "fou",   "fu",     "ga",    "gai",
        "gan",   "gang",  "gao",   "ge",     "gei",   "gen",  "geng", "gong",  "gou",   "gu",     "gua",   "guai",
        "guan",  "guang", "gui",   "gun",    "guo",   "ha",   "hai",  "han",   "hang",  "hao",    "he",    "hei",
        "hen",   "heng",  "hong",  "hou",    "hu",    "hua",  "huai", "huan",  "huang", "hui",    "hun",   "huo",
        "ji",    "jia",   "jian",  "jiang",  "jiao",  "jie",  "jin",  "jing",  "jiong", "jiu",    "ju",    "juan",
        "jue",   "jun",   "jv",    "jvan",   "jve",   "jvn",  "ka",   "kai",   "kan",   "kang",   "kao",   "ke",
        "kei",   "ken",   "keng",  "kong",   "kou",   "ku",   "kua",  "kuai",  "kuan",  "kuang",  "kui",   "kun",
        "kuo",   "la",    "lai",   "lan",    "lang",  "lao",  "le",   "lei",   "leng",  "li",     "lia",   "lian",
        "liang", "liao",  "lie",   "lin",    "ling",  "liu",  "lo",   "long",  "lou",   "lu",     "luan",  "lue",
        "lun",   "luo",   "lv",    "lve",    "ma",    "mai",  "man",  "mang",  "mao",   "me",     "mei",   "men",
        "meng",  "mi",    "mian",  "miao",   "mie",   "min",  "ming", "miu",   "mo",    "mou",    "mu",    "na",
        "nai",   "nan",   "nang",  "nao",    "ne",    "nei",  "nen",  "neng",  "ni",    "nian",   "niang", "niao",
        "nie",   "nin",   "ning",  "niu",    "nong",  "nou",  "nu",   "nuan",  "nue",   "nun",    "nuo",   "nv",
        "nve",   "o",     "ou",    "pa",     "pai",   "pan",  "pang", "pao",   "pei",   "pen",    "peng",  "pi",
        "pian",  "piao",  "pie",   "pin",    "ping",  "po",   "pou",  "pu",    "qi",    "qia",    "qian",  "qiang",
        "qiao",  "qie",   "qin",   "qing",   "qiong", "qiu",  "qu",   "quan",  "que",   "qun",    "qv",    "qvan",
        "qve",   "qvn",   "ran",   "rang",   "rao",   "re",   "ren",  "reng",  "ri",    "rong",   "rou",   "ru",
        "ruan",  "rui",   "run",   "ruo",    "sa",    "sai",  "san",  "sang",  "sao",   "se",     "sen",   "seng",
        "sha",   "shai",  "shan",  "shang",  "shao",  "she",  "shei", "shen",  "sheng", "shi",    "shou",  "shu",
        "shua",  "shuai", "shuan", "shuang", "shui",  "shun", "shuo", "si",    "song",  "sou",    "su",    "suan",
        "sui",   "sun",   "suo",   "ta",     "tai",   "tan",  "tang", "tao",   "te",    "teng",   "ti",    "tian",
        "tiao",  "tie",   "ting",  "tong",   "tou",   "tu",   "tuan", "tui",   "tun",   "tuo",    "wa",    "wai",
        "wan",   "wang",  "wei",   "wen",    "weng",  "wo",   "wu",   "xi",    "xia",   "xian",   "xiang", "xiao",
        "xie",   "xin",   "xing",  "xiong",  "xiu",   "xu",   "xuan", "xue",   "xun",   "xv",     "xvan",  "xve",
        "xvn",   "ya",    "yan",   "yang",   "yao",   "ye",   "yi",   "yin",   "ying",  "yo",     "yong",  "you",
        "yu",    "yuan",  "yue",   "yun",    "yv",    "yvan", "yve",  "yvn",   "za",    "zai",    "zan",   "zang",
        "zao",   "ze",    "zei",   "zen",    "zeng",  "zha",  "zhai", "zhan",  "zhang", "zhao",   "zhe",   "zhei",
        "zhen",  "zheng", "zhi",   "zhong",  "zhou",  "zhu",  "zhua", "zhuai", "zhuan", "zhuang", "zhui",  "zhun",
        "zhuo",  "zi",    "zong",  "zou",    "zu",    "zuan", "zui",  "zun",   "zuo"};
    return kList;
}

const std::unordered_set<std::string> &intact_pinyin_set()
{
    static const std::unordered_set<std::string> kSet(intact_pinyin_list().begin(), intact_pinyin_list().end());
    return kSet;
}

const std::unordered_set<std::string> &prefix_pinyin_set()
{
    static const std::unordered_set<std::string> kSet = [] {
        std::unordered_set<std::string> result;
        for (const auto &item : intact_pinyin_list())
        {
            for (size_t i = 1; i <= item.size(); ++i)
            {
                result.insert(item.substr(0, i));
            }
        }
        return result;
    }();
    return kSet;
}

bool has_only_complete_pinyin_segments(const Segments &segments)
{
    if (segments.empty())
    {
        return false;
    }

    const auto &valid_pinyin = intact_pinyin_set();
    return std::all_of(segments.begin(), segments.end(),
                       [&](const std::string &segment) { return valid_pinyin.find(segment) != valid_pinyin.end(); });
}

bool looks_like_jianpin_spelling(const std::string &pinyin)
{
    // Manual delimiters express user-intent boundaries and never enter the
    // correction path, so there is nothing for this guard to protect.
    if (pinyin.empty() || pinyin.find('\'') != std::string::npos)
    {
        return false;
    }

    const auto &valid_pinyin = intact_pinyin_set();
    const std::string initial = pinyin.substr(0, 1);
    // A leading initial followed by complete syllables is mixed jianpin, not an
    // extra letter to delete ("ywen" = y + wen, "sshang" = s + shang)
    if (prefix_pinyin_set().count(initial) != 0 && valid_pinyin.count(initial) == 0 &&
        is_complete_pinyin_input(pinyin.substr(1)))
    {
        return true;
    }
    // The input must reduce to one or more legal syllables plus at most one
    // trailing letter ("zheg" = zhe + g) to count as jianpin intent. The cut
    // backtracks like the query-side segmentation ("zaijinut" = zai'ji'nu + t).
    // All-consonant strings match zero syllables and return false on purpose:
    // shape alone cannot tell a pure jianpin ("nbn" = 能不能) from a dropped
    // vowel ("bng" -> bang), so both still enter correction. The dictionary
    // layer then puts a literal pure-jianpin whole word ahead of the corrected
    // readings (literal_jianpin_pinyin_match), keeping the correction behind it.
    if (is_complete_pinyin_input(pinyin))
    {
        // Fully reduced to legal syllables: a legitimate spelling, not a typo.
        return true;
    }
    if (pinyin.size() < 2 || !is_complete_pinyin_input(pinyin.substr(0, pinyin.size() - 1)))
    {
        return false;
    }
    // Exactly one trailing letter. It reads as jianpin intent only when it is a
    // plausible jianpin initial -- an initial consonant, as in "zheg" = zhe + g.
    // A bare vowel can never begin a jianpin syllable, so a complete syllable
    // followed by a lone vowel ("gau" = ga + u) is far more likely a
    // transposition typo (gua) than jianpin intent; let it reach correction.
    const char tail = pinyin.back();
    const bool tail_is_vowel = tail == 'a' || tail == 'e' || tail == 'i' || tail == 'o' || tail == 'u' || tail == 'v';
    return !tail_is_vowel;
}

SyllableGraph build_syllable_graph(const std::string &pinyin)
{
    SyllableGraph graph;
    graph.input_length = pinyin.size();
    graph.edges.resize(pinyin.size() + 1);
    if (pinyin.empty() || pinyin.find('\'') != std::string::npos)
    {
        return graph;
    }

    const auto &valid_pinyin = intact_pinyin_set();
    static const size_t kMaxSyllableLength =
        std::max_element(intact_pinyin_list().begin(), intact_pinyin_list().end(),
                         [](const std::string &lhs, const std::string &rhs) { return lhs.size() < rhs.size(); })
            ->size();

    for (size_t start = 0; start < pinyin.size(); ++start)
    {
        const size_t last_end = std::min(pinyin.size(), start + kMaxSyllableLength);
        for (size_t end = last_end; end > start; --end)
        {
            const std::string syllable = pinyin.substr(start, end - start);
            if (valid_pinyin.find(syllable) != valid_pinyin.end())
            {
                graph.edges[start].push_back(SyllableEdge{end, syllable});
            }
        }
    }

    std::vector<bool> reaches_end(pinyin.size() + 1, false);
    reaches_end[pinyin.size()] = true;
    for (size_t start = pinyin.size(); start-- > 0;)
    {
        auto &edges = graph.edges[start];
        edges.erase(std::remove_if(
                        edges.begin(), edges.end(),
                        [&](const SyllableEdge &edge) { return edge.end > pinyin.size() || !reaches_end[edge.end]; }),
                    edges.end());
        reaches_end[start] = !edges.empty();
    }
    return graph;
}

std::vector<Segments> enumerate_complete_segmentations(const SyllableGraph &graph, size_t path_limit)
{
    std::vector<Segments> result;
    if (path_limit == 0 || graph.input_length == 0 || graph.edges.size() != graph.input_length + 1)
    {
        return result;
    }

    Segments current;
    const auto enumerate = [&](auto &&self, size_t position) -> void {
        if (result.size() >= path_limit)
        {
            return;
        }
        if (position == graph.input_length)
        {
            result.push_back(current);
            return;
        }
        if (position >= graph.edges.size())
        {
            return;
        }

        for (const auto &edge : graph.edges[position])
        {
            current.push_back(edge.syllable);
            self(self, edge.end);
            current.pop_back();
            if (result.size() >= path_limit)
            {
                return;
            }
        }
    };
    enumerate(enumerate, 0);
    return result;
}

std::vector<std::string> cut_one_piece_greedy(const std::string &pinyin, bool intact_only)
{
    const auto &pinyin_set = intact_only ? intact_pinyin_set() : prefix_pinyin_set();
    std::vector<std::string> result;
    size_t index = 0;
    while (index < pinyin.size())
    {
        std::string matched;
        for (size_t end = pinyin.size(); end > index; --end)
        {
            const auto piece = pinyin.substr(index, end - index);
            if (pinyin_set.find(piece) != pinyin_set.end())
            {
                matched = piece;
                break;
            }
        }
        if (matched.empty())
        {
            return {};
        }
        result.push_back(matched);
        index += matched.size();
    }
    return result;
}

std::vector<std::string> cut_one_piece_min_segments(const std::string &pinyin, bool intact_only)
{
    const auto &pinyin_set = intact_only ? intact_pinyin_set() : prefix_pinyin_set();
    std::unordered_map<size_t, std::vector<std::string>> memo;
    std::unordered_set<size_t> visiting;

    const auto solve = [&](auto &&self, size_t index) -> std::vector<std::string> {
        if (index == pinyin.size())
        {
            return {};
        }

        if (const auto found = memo.find(index); found != memo.end())
        {
            return found->second;
        }

        if (!visiting.insert(index).second)
        {
            return {};
        }

        std::vector<std::string> best;
        bool has_best = false;

        for (size_t end = pinyin.size(); end > index; --end)
        {
            const auto piece = pinyin.substr(index, end - index);
            if (pinyin_set.find(piece) == pinyin_set.end())
            {
                continue;
            }

            std::vector<std::string> suffix;
            if (end < pinyin.size())
            {
                suffix = self(self, end);
                if (suffix.empty())
                {
                    continue;
                }
            }

            std::vector<std::string> candidate;
            candidate.reserve(1 + suffix.size());
            candidate.push_back(piece);
            candidate.insert(candidate.end(), suffix.begin(), suffix.end());

            if (!has_best || candidate.size() < best.size())
            {
                best = std::move(candidate);
                has_best = true;
                continue;
            }

            if (candidate.size() == best.size() && !candidate.empty() && !best.empty() &&
                candidate.front().size() < best.front().size())
            {
                best = std::move(candidate);
            }
        }

        visiting.erase(index);
        memo.emplace(index, has_best ? best : std::vector<std::string>{});
        return has_best ? best : std::vector<std::string>{};
    };

    return solve(solve, 0);
}

std::vector<Segments> enumerate_min_segmentations(const std::string &pinyin, const size_t path_limit)
{
    std::vector<Segments> result;
    if (pinyin.empty() || path_limit == 0)
    {
        return result;
    }
    const auto &pinyin_set = intact_pinyin_set();
    constexpr size_t kMaxSyllableLength = 6;
    constexpr size_t kUnreachable = static_cast<size_t>(-1);
    const size_t length = pinyin.size();
    // min_count[i]：从 i 切到末尾最少几个音节。只沿「这一刀之后仍是最少音节」的边往下走，
    // 枚举出来的就只有等音节数的切分，组合数只随歧义边界的个数增长。
    std::vector<size_t> min_count(length + 1, kUnreachable);
    min_count[length] = 0;
    for (size_t start = length; start-- > 0;)
    {
        for (size_t len = 1; len <= kMaxSyllableLength && start + len <= length; ++len)
        {
            if (min_count[start + len] != kUnreachable &&
                pinyin_set.find(pinyin.substr(start, len)) != pinyin_set.end())
            {
                min_count[start] = (std::min)(min_count[start], min_count[start + len] + 1);
            }
        }
    }
    if (min_count[0] == kUnreachable)
    {
        return result;
    }

    Segments current;
    const auto walk = [&](auto &&self, const size_t start) -> void {
        if (result.size() >= path_limit)
        {
            return;
        }
        if (start == length)
        {
            result.push_back(current);
            return;
        }
        for (size_t len = 1; len <= kMaxSyllableLength && start + len <= length; ++len)
        {
            if (min_count[start + len] == kUnreachable || min_count[start + len] + 1 != min_count[start])
            {
                continue;
            }
            std::string piece = pinyin.substr(start, len);
            if (pinyin_set.find(piece) == pinyin_set.end())
            {
                continue;
            }
            current.push_back(std::move(piece));
            self(self, start + len);
            current.pop_back();
        }
    };
    walk(walk, 0);
    return result;
}

bool is_complete_pinyin_input(const std::string &pinyin)
{
    if (pinyin.empty())
    {
        return false;
    }

    if (pinyin.find('\'') == std::string::npos)
    {
        return is_complete_pinyin_part(pinyin);
    }

    for (const auto &part : split(pinyin, '\''))
    {
        if (!is_complete_pinyin_part(part))
        {
            return false;
        }
    }

    return true;
}

bool is_jqxy_v_alias(const std::string &syllable)
{
    return syllable.size() >= 2 && syllable[1] == 'v' &&
           (syllable[0] == 'j' || syllable[0] == 'q' || syllable[0] == 'x' || syllable[0] == 'y');
}

std::string canonical_umlaut_spelling(std::string syllable)
{
    if (is_jqxy_v_alias(syllable))
    {
        syllable[1] = 'u';
    }
    else if (syllable == "nue" || syllable == "lue")
    {
        syllable[1] = 'v';
    }
    return syllable;
}

std::string to_google_spelling(const std::string &segmentation)
{
    // canonical_umlaut_spelling 把 ü 的两种写法归到词库存的那一种
    // （l/n 后面写 v：lve、nve）。Google 的两个解码器要的正好是另一种：本地
    // googlepinyinime-rev 的音节表来自 rawdict_utf16_65105_freq.txt，里面只有
    // nue/lue 和 nv/lv，没有 nve/lve，splparser 也不做 v→ü 转写；inputtools 云输入
    // 表现一致。把 nve'dai'dong'wu 原样送过去，nve 会被拆成 nv + e，「虐待动物」
    // 于是变成「女蛾黛动物」。j/q/x/y 后面它们同样只认 u 写法（ju、jue）。
    //
    // 按音节整体替换，认不出的原样透传：手打分隔符切出来的块可能不止一个音节，
    // 那种块交给解码器自己再切。
    const auto &valid_pinyin = intact_pinyin_set();

    std::string result;
    result.reserve(segmentation.size());
    size_t chunk_start = 0;
    while (true)
    {
        const size_t separator = segmentation.find('\'', chunk_start);
        const size_t chunk_end = separator == std::string::npos ? segmentation.size() : separator;
        std::string chunk = segmentation.substr(chunk_start, chunk_end - chunk_start);
        if (valid_pinyin.count(chunk))
        {
            chunk = canonical_umlaut_spelling(std::move(chunk));
            if (chunk == "nve" || chunk == "lve")
            {
                chunk[1] = 'u';
            }
        }
        result += chunk;
        if (separator == std::string::npos)
        {
            break;
        }
        result += '\'';
        chunk_start = separator + 1;
    }
    return result;
}

size_t detect_active_helpcode_length(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    const auto &input_with_cases = raw_input_with_cases.empty() ? raw_input : raw_input_with_cases;
    if (HelpcodeUtils::is_quanpin_double_help_mode(input_with_cases) && raw_input.size() >= 2 &&
        is_complete_pinyin_input(raw_input.substr(0, raw_input.size() - 2)))
    {
        return 2;
    }

    if (HelpcodeUtils::is_quanpin_single_help_mode(input_with_cases) && !raw_input.empty() &&
        is_complete_pinyin_input(raw_input.substr(0, raw_input.size() - 1)))
    {
        return 1;
    }

    return 0;
}

std::string strip_active_helpcodes(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    const size_t helpcode_length = detect_active_helpcode_length(raw_input, raw_input_with_cases);
    if (helpcode_length == 0 || raw_input.size() < helpcode_length)
    {
        return raw_input;
    }
    return raw_input.substr(0, raw_input.size() - helpcode_length);
}

std::string strip_active_helpcodes_with_cases(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    const auto &input_with_cases = raw_input_with_cases.empty() ? raw_input : raw_input_with_cases;
    const size_t helpcode_length = detect_active_helpcode_length(raw_input, raw_input_with_cases);
    if (helpcode_length == 0 || input_with_cases.size() < helpcode_length)
    {
        return input_with_cases;
    }
    return input_with_cases.substr(0, input_with_cases.size() - helpcode_length);
}

std::vector<Segments> sparse_pinyin_fallback_segments(const Segments &segments)
{
    if (segments.empty())
    {
        return {};
    }

    const auto &first = segments.front();
    for (const auto &rule : sparse_pinyin_fallback_rules())
    {
        if (first != rule.full)
        {
            continue;
        }

        std::vector<Segments> fallbacks;
        fallbacks.reserve(rule.replacements.size());
        for (const auto &replacement : rule.replacements)
        {
            fallbacks.push_back(replacement.append_suffix ? append_rest(replacement.replacement, segments)
                                                          : replacement.replacement);
        }
        return fallbacks;
    }

    return {};
}

namespace
{
constexpr size_t kMaxAutocorrectEdges = 3;
constexpr size_t kMaxAutocorrectInputLength = 64;
constexpr size_t kNoPredecessor = static_cast<size_t>(-1);

// Keys point at string literals in the generated tables (static storage), so views
// stay valid forever; Entry::correct is a 16-bit index into the generated
// kCorrectSyllables table, dereferenced once here so everything downstream
// (ranking, merging, queries) keeps plain string views. One wrong key maps to
// every correction target the tables offer: ambiguous variants are kept on purpose so the query layer can settle
// them with word frequency (CN 101133411 B M3). Tables are merged in weight
// order (transposition, deletion, insertion, neighbor), so target lists end up
// weight-sorted with array order breaking ties -- the deterministic "table
// order" of the ranking key.
struct CorrectionTarget
{
    std::string_view syllable;
    // OR of every correction type whose table offers this (wrong -> syllable)
    // pair. The tie-break weight is derived from the ENABLED subset at query
    // time (see min_enabled_correction_weight), never from a switched-off table.
    unsigned type_bit = 0;
    // Out-of-table generated pair (non-neighbor substitution): flat expensive-tier
    // weight (kAutocorrectGeneratedShapeWeight) instead of the per-bit derivation;
    // switch gating still uses type_bit.
    bool generated = false;
};

// Canonical per-type tie-break cost. Each correction type carries one fixed
// weight (header constants); this maps a single type bit back to it.
constexpr int correction_type_weight(const unsigned type_bit)
{
    switch (type_bit)
    {
    case kAutocorrectTransposition:
        return kAutocorrectTranspositionWeight;
    case kAutocorrectDeletion:
        return kAutocorrectDeletionWeight;
    case kAutocorrectInsertion:
        return kAutocorrectInsertionWeight;
    case kAutocorrectNeighbor:
        return kAutocorrectNeighborWeight;
    default:
        return 0;
    }
}

// Cheapest explanation among the correction types that are BOTH offered by a
// target and enabled by the caller. A pair shared by two tables is ranked by
// the min weight of the enabled ones only, so disabling a cheaper table lets
// the surviving (dearer) table set the cost instead of leaking the off table's.
int min_enabled_correction_weight(const unsigned enabled_bits)
{
    int best = std::numeric_limits<int>::max();
    for (const unsigned bit :
         {kAutocorrectTransposition, kAutocorrectDeletion, kAutocorrectInsertion, kAutocorrectNeighbor})
    {
        if (enabled_bits & bit)
        {
            best = std::min(best, correction_type_weight(bit));
        }
    }
    return best;
}

// 表外“生成式”形状（非邻键替换）已由生成器静态化进
// autocorrect_table.h 的 kGeneratedShapeEntries，运行期不再枚举：这里原来那份
// kQwertyNeighbors + is_qwerty_neighbor + build_generated_pairs 与生成器的邻键表
// 是两份必须逐字节一致的数据，静态化后一并删除，漂移面随之消失。

const std::unordered_map<std::string_view, std::vector<CorrectionTarget>> &correction_index()
{
    static const std::unordered_map<std::string_view, std::vector<CorrectionTarget>> kIndex = [] {
        std::unordered_map<std::string_view, std::vector<CorrectionTarget>> index;
        index.reserve((autocorrect::kTranspositionCount + autocorrect::kNeighborCount + autocorrect::kDeletionCount +
                       autocorrect::kInsertionCount + autocorrect::kGeneratedShapeCount) *
                      4 / 3);
        const auto add_table = [&](const autocorrect::Entry *entries, const std::size_t count, const unsigned type_bit,
                                   const bool generated = false) {
            for (std::size_t i = 0; i < count; ++i)
            {
                const std::string_view correct = autocorrect::kCorrectSyllables[entries[i].correct];
                auto &targets = index[entries[i].wrong];
                // The generator guarantees (wrong, correct) pairs are unique
                // across tables; the merge below only defends against drift.
                // Only the type bits accumulate -- the tie-break weight is
                // recomputed from the enabled subset at query time, so a shared
                // pair never inherits a switched-off table's cheaper cost.
                const auto duplicate =
                    std::find_if(targets.begin(), targets.end(),
                                 [&](const CorrectionTarget &target) { return target.syllable == correct; });
                if (duplicate != targets.end())
                {
                    duplicate->type_bit |= type_bit;
                }
                else
                {
                    targets.push_back(CorrectionTarget{correct, type_bit, generated});
                }
            }
        };
        // 静态表按权重升序合并（换位、漏字、多字、邻键），目标列表因此天然
        // 权重有序，同权重由数组序打破平局——这就是排序键里的 "table order"。
        add_table(autocorrect::kTranspositionEntries, autocorrect::kTranspositionCount, kAutocorrectTransposition);
        add_table(autocorrect::kDeletionEntries, autocorrect::kDeletionCount, kAutocorrectDeletion);
        add_table(autocorrect::kInsertionEntries, autocorrect::kInsertionCount, kAutocorrectInsertion);
        add_table(autocorrect::kNeighborEntries, autocorrect::kNeighborCount, kAutocorrectNeighbor);
        // 生成式（非邻键替换）在生成期就已剔除与静态表重复的 (wrong, correct) 对，
        // 这里只需追加：共享的原串仍然保留静态表的便宜目标在前。
        add_table(autocorrect::kGeneratedShapeEntries, autocorrect::kGeneratedShapeCount, kAutocorrectNeighbor, true);
        return index;
    }();
    return kIndex;
}

// Edge taken to arrive at a hypothesis position. raw_length is the number of
// input letters consumed: equal to syllable.size() for legal syllables and
// same-length corrections (transposition / neighbor), one less for deletion
// corrections ("zhng" -> zhang), one more for insertion corrections
// ("sghang" -> shang).
struct AutocorrectEdge
{
    size_t raw_length = 0;
    std::string_view syllable;
    bool corrected = false;
    // Out-of-table generated pair (non-neighbor substitution). Generated-
    // containing hypotheses rank behind ALL static
    // hypotheses (see the finalize comparator) so in-table inputs keep their
    // baseline cut set.
    bool generated = false;
    // 这条边自己的纠正权重（未纠正为 0），随切分交给 AutocorrectCutSegment::weight。
    int weight = 0;
};

// One propagated cut hypothesis, ranked by (edge_count, weight, arrival):
// fewest corrections first (the least-intrusive contract), then the summed
// correction weights (only within the same edge count), then the deterministic
// generation order -- positions ascending, pieces longest-first, table order.
// prev_index points back into the predecessor position's final hypothesis
// list, which is frozen once finalized, so indices stay stable.
struct SearchHypothesis
{
    size_t edge_count = 0;
    size_t prev_index = kNoPredecessor;
    size_t arrival = 0;
    int weight = 0;
    // True once any generated edge participates in this path. Such hypotheses
    // rank behind every static hypothesis (see the finalize comparator) so
    // in-table inputs keep their baseline cut set.
    bool has_generated = false;
    AutocorrectEdge edge;
    // Order-sensitive rolling hash of the joined syllable sequence. Two
    // hypotheses reaching the same position share a sequence iff their hashes
    // match; dedup keys on this instead of materializing the full string per
    // node (which was O(length^2 * k) allocation on the keystroke path). A
    // 64-bit collision across the few thousand live hypotheses is ~1e-11, and
    // its only effect would be dropping one alternative reading -- the same
    // class of outcome the dedup itself produces.
    size_t seq_hash = 0;
};

// Fold one more syllable into a sequence hash (boost-style hash_combine over
// the predecessor hash so order and separators are encoded).
inline size_t extend_sequence_hash(const size_t seed, const std::string_view syllable)
{
    const size_t piece = std::hash<std::string_view>{}(syllable);
    return seed ^ (piece + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
}
} // namespace

std::vector<AutocorrectCut> autocorrect_cut_kbest(const std::string &pinyin, const unsigned autocorrect_types,
                                                  const std::size_t k)
{
    // Contract (see header): no type enabled, manual delimiters, overlong
    // input, or k == 0 yield nothing. Every returned cut contains at least one
    // corrected edge; legal-only hypotheses at the final position are dropped
    // on arrival, so a fully legal input without any correction reading
    // returns {} -- the caller already owns the plain segmentation.
    if (k == 0 || autocorrect_types == 0 || pinyin.empty() || pinyin.size() > kMaxAutocorrectInputLength ||
        pinyin.find('\'') != std::string::npos)
    {
        return {};
    }

    const auto &valid_pinyin = intact_pinyin_set();
    const auto &index = correction_index();
    const size_t length = pinyin.size();

    // Top-k hypotheses per input position. All edges consume at least one raw
    // letter, so a single left-to-right sweep finalizes each position before
    // relaxing its out-edges -- a label-correcting pass without a queue.
    std::vector<std::vector<SearchHypothesis>> best(length + 1);
    std::vector<bool> finalized(length + 1, false);
    size_t arrival = 0;
    best[0].push_back(SearchHypothesis{}); // seed: zero edges, no predecessor

    const auto finalize = [&](const size_t position) {
        if (finalized[position])
        {
            return;
        }
        finalized[position] = true;
        auto &list = best[position];
        if (list.size() < 2)
        {
            return;
        }
        std::sort(list.begin(), list.end(), [](const SearchHypothesis &lhs, const SearchHypothesis &rhs) {
            // Generated-containing hypotheses rank behind ALL static ones
            // regardless of edge count: out-of-table shapes only step in when
            // the static space has no better explanation, and in-table inputs
            // must keep their baseline cut set (zero-regression by design).
            if (lhs.has_generated != rhs.has_generated)
            {
                return lhs.has_generated < rhs.has_generated;
            }
            if (lhs.edge_count != rhs.edge_count)
            {
                return lhs.edge_count < rhs.edge_count;
            }
            if (lhs.weight != rhs.weight)
            {
                return lhs.weight < rhs.weight;
            }
            return lhs.arrival < rhs.arrival;
        });
        // Same syllable sequence reached twice (different raw spans): keep the
        // best-ranked hypothesis, drop the rest before truncating to k. The list
        // is sorted by (edge_count, weight, arrival), NOT by sequence, so equal
        // sequences are not adjacent -- std::unique would miss them. Scan front
        // to back (best first) and drop any sequence hash already seen.
        std::unordered_set<size_t> seen_sequences;
        seen_sequences.reserve(list.size());
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const SearchHypothesis &hypothesis) {
                                      return !seen_sequences.insert(hypothesis.seq_hash).second;
                                  }),
                   list.end());
        if (list.size() > k)
        {
            list.resize(k);
        }
    };

    const auto extend = [&](const size_t end, const SearchHypothesis &parent, const std::size_t parent_index,
                            const AutocorrectEdge &edge, const int edge_weight) {
        SearchHypothesis child;
        child.edge_count = parent.edge_count + (edge.corrected ? 1 : 0);
        if (child.edge_count > kMaxAutocorrectEdges)
        {
            return;
        }
        // Legal-only hypotheses at the final position can never win a result
        // slot nor propagate further; dropping them here keeps k == 1
        // consistent with larger k (same first cut either way).
        if (end == length && child.edge_count == 0)
        {
            return;
        }
        child.prev_index = parent_index;
        child.arrival = arrival++;
        child.weight = parent.weight + edge_weight;
        child.edge = edge;
        child.edge.weight = edge_weight;
        child.has_generated = parent.has_generated || edge.generated;
        child.seq_hash = extend_sequence_hash(parent.seq_hash, edge.syllable);
        best[end].push_back(std::move(child));
    };

    for (size_t start = 0; start < length; ++start)
    {
        finalize(start);
        const auto &hypotheses = best[start];
        if (hypotheses.empty())
        {
            continue;
        }
        const size_t remaining = length - start;
        const size_t max_len = std::min(remaining, kAutocorrectMaxKeyLength);
        for (size_t len = max_len; len >= 1; --len)
        {
            const std::string_view piece(pinyin.data() + start, len);
            AutocorrectEdge edge;
            edge.raw_length = len;
            if (valid_pinyin.find(std::string(piece)) != valid_pinyin.end())
            {
                edge.syllable = piece;
                edge.corrected = false;
                for (size_t i = 0; i < hypotheses.size(); ++i)
                {
                    extend(start + len, hypotheses[i], i, edge, 0);
                }
                continue;
            }
            const auto found = index.find(piece);
            if (found == index.end())
            {
                continue;
            }
            // Every ambiguous target is a parallel edge; the enabled type
            // bits gate which tables may answer.
            for (const auto &target : found->second)
            {
                const unsigned enabled_bits = autocorrect_types & target.type_bit;
                if (enabled_bits == 0)
                {
                    continue;
                }
                edge.syllable = target.syllable;
                edge.corrected = true;
                edge.generated = target.generated;
                const int weight =
                    target.generated ? kAutocorrectGeneratedShapeWeight : min_enabled_correction_weight(enabled_bits);
                for (size_t i = 0; i < hypotheses.size(); ++i)
                {
                    extend(start + len, hypotheses[i], i, edge, weight);
                }
            }
        }
    }
    finalize(length);
    // Static-priority zero-regression (see the header contract): when the best
    // cut is pure static, drop every generated cut so every caller -- the
    // primary resolver, the head+tail composition retry, the k == 1 preedit
    // projection -- consumes exactly the baseline cut set; generated readings
    // surface only when no static cut explains the input at all. Living here
    // (not in each caller) is the point: a future consumer cannot forget it.
    if (!best[length].empty() && !best[length].front().has_generated)
    {
        best[length].erase(std::remove_if(best[length].begin(), best[length].end(),
                                          [](const SearchHypothesis &hypothesis) { return hypothesis.has_generated; }),
                           best[length].end());
    }

    // Rebuild each surviving end hypothesis by walking the predecessor chain.
    // raw_length (not syllable.size()) gives the raw span: deletion edges
    // consume one letter less, insertion edges one letter more, than the
    // syllable they produce.
    std::vector<AutocorrectCut> results;
    for (const auto &hypothesis : best[length])
    {
        AutocorrectCut cut;
        cut.edge_count = hypothesis.edge_count;
        cut.weight = hypothesis.weight;
        cut.has_generated = hypothesis.has_generated;
        size_t position = length;
        const SearchHypothesis *current = &hypothesis;
        while (current->prev_index != kNoPredecessor)
        {
            const size_t segment_start = position - current->edge.raw_length;
            cut.segments.push_back(AutocorrectCutSegment{std::string(current->edge.syllable),
                                                         pinyin.substr(segment_start, current->edge.raw_length),
                                                         segment_start, current->edge.corrected, current->edge.weight});
            position = segment_start;
            current = &best[position][current->prev_index];
        }
        std::reverse(cut.segments.begin(), cut.segments.end());
        results.push_back(std::move(cut));
    }
    return results;
}

// ---- 长句容错槽位模型 ----

namespace
{
// 按切段文本分类槽位：intact 集合内为 Determining，单字母为 SingleInitial，其余 Fault。
FaultSlotKind fault_slot_kind(const std::string &piece)
{
    if (intact_pinyin_set().count(piece) != 0)
    {
        return FaultSlotKind::Determining;
    }
    return piece.size() == 1 ? FaultSlotKind::SingleInitial : FaultSlotKind::Fault;
}
} // namespace

std::vector<FaultSlot> build_fault_slots(const std::string &pinyin)
{
    if (pinyin.empty() || pinyin.find('\'') != std::string::npos)
    {
        return {};
    }
    // 输入可能带大小写（大写段是触发键的一部分）：切分与槽位分类都按小写字母走，
    // 与 autocorrect_cut_kbest 对 fold 后字母工作同一个约定。
    std::string letters;
    letters.reserve(pinyin.size());
    for (const char ch : pinyin)
    {
        letters.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        if (letters.back() < 'a' || letters.back() > 'z')
        {
            return {};
        }
    }
    // 与查询层同一个切分器：残余字母按简拼尾解释，任何字母串都切得出。用前缀集
    // （intact_only=false）切，r、c 这类单字母声母才能成段；槽位分类再按 intact 集
    // 区分合法音节与简拼/非法段。
    const Segments segments = cut_one_piece_min_segments(letters, false);
    // 切分必须逐字母盖住输入（该切分器保证），用拼接而非 join_segments 比较：
    // 后者插入分隔符，对多段输入恒不等。
    std::string joined;
    for (const auto &segment : segments)
    {
        joined += segment;
    }
    if (segments.empty() || joined != letters)
    {
        return {};
    }
    std::vector<FaultSlot> slots;
    slots.reserve(segments.size());
    for (const auto &segment : segments)
    {
        FaultSlot slot;
        slot.raw_text = segment;
        slot.base_syllable = segment;
        slot.kind = fault_slot_kind(segment);
        // Fault 槽没有基础读法：多字母非法串不能当简拼查词。
        if (slot.kind == FaultSlotKind::Fault)
        {
            slot.base_syllable.clear();
        }
        slots.push_back(std::move(slot));
    }
    return slots;
}

std::vector<std::pair<std::string, int>> fault_slot_correction_targets(const std::string &raw_text,
                                                                       const unsigned autocorrect_types)
{
    if (autocorrect_types == 0 || raw_text.empty() || intact_pinyin_set().count(raw_text) != 0)
    {
        return {};
    }
    std::vector<std::pair<std::string, int>> targets;
    const auto found = correction_index().find(raw_text);
    if (found == correction_index().end())
    {
        return targets;
    }
    for (const auto &target : found->second)
    {
        // 掩码门控与 autocorrect_cut_kbest 同一条规则；权重按启用位折算（同
        // min_enabled_correction_weight），生成式目标走昂贵档常数。
        const unsigned enabled_bits = autocorrect_types & target.type_bit;
        if (enabled_bits == 0)
        {
            continue;
        }
        const int weight =
            target.generated ? kAutocorrectGeneratedShapeWeight : min_enabled_correction_weight(enabled_bits);
        const auto duplicate = std::find_if(targets.begin(), targets.end(),
                                            [&](const auto &entry) { return entry.first == target.syllable; });
        if (duplicate == targets.end())
        {
            targets.emplace_back(std::string(target.syllable), weight);
        }
        else if (weight < duplicate->second)
        {
            duplicate->second = weight;
        }
    }
    return targets;
}

bool fault_slots_eligible(const std::vector<FaultSlot> &slots, const unsigned autocorrect_types)
{
    if (autocorrect_types == 0 || slots.empty() || slots.size() > kMaxFaultSlotCount)
    {
        return false;
    }
    std::size_t non_determining = 0;
    std::size_t single_initial = 0;
    for (const auto &slot : slots)
    {
        if (slot.kind == FaultSlotKind::SingleInitial)
        {
            ++single_initial;
        }
        if (slot.kind != FaultSlotKind::Determining)
        {
            ++non_determining;
        }
    }
    // 必须有至少两个单字母槽：这是长句里有意混入的简拼（shuishanshurFayonghu... 的
    // r、c、l），k-best 的 3 边预算装不下它们，才轮到容错仲裁。只有一个单字母槽的串
    // （tiananmenguangchang 的后两个字母被抓成单字母槽）是字面仲裁的势力范围，拉进容错
    // 搜索只会让 kenlm 在展开读法里挑一个分数更高的错切分（天安门广场 -> 天南门广场）。
    // 短串同样走旧 autocorrect_cut_kbest 快速路径，行为零变。
    return single_initial >= 2 && non_determining > 0 && slots.size() > kMaxShortAutocorrectSyllables;
}

bool fault_winner_is_pure_jianpin(const std::vector<FaultSlot> &slots, const Segments &winner)
{
    if (slots.size() != winner.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < slots.size(); ++i)
    {
        switch (slots[i].kind)
        {
        case FaultSlotKind::Determining:
            // 合法音节被改读就是纠错，哪怕字母数没变（lia -> lai）。
            if (canonical_umlaut_spelling(winner[i]) != canonical_umlaut_spelling(slots[i].base_syllable))
            {
                return false;
            }
            break;
        case FaultSlotKind::SingleInitial:
            // 单字母段只能是同首字母的展开，或者原样保留。
            if (winner[i].empty() || slots[i].raw_text.empty() || winner[i].front() != slots[i].raw_text.front())
            {
                return false;
            }
            break;
        case FaultSlotKind::Fault:
            return false;
        }
    }
    return true;
}

namespace
{
// 罕见合法音节 -> 它的换位常用音节。人工收录、不由生成器派生：两侧都常用的换位对
// （hou/huo、bei/bie、dou/duo）放开后每个按键都要多解码几条读法，先量过再说。
constexpr std::pair<std::string_view, std::string_view> kRareLegalTranspositions[] = {
    {"lia", "lai"},
    {"dia", "dai"},
};

// 合法输入上一条读法最多带几处纠正。再多就几乎都是巧合拼出来的读法。
constexpr size_t kMaxLegalInputCorrections = 2;
// 合法输入上认的纠错类型：换位、多字、漏字。邻键替换不认——合法输入里几乎每个字母
// 都有邻键能换成另一个合法读法，那是噪声面最大的一类。
constexpr unsigned kLegalInputCorrectionTypes =
    kAutocorrectTransposition | kAutocorrectDeletion | kAutocorrectInsertion;

bool is_zero_initial_syllable(const std::string &syllable)
{
    return !syllable.empty() && (syllable[0] == 'a' || syllable[0] == 'o' || syllable[0] == 'e');
}

bool same_syllables(const AutocorrectCut &lhs, const AutocorrectCut &rhs)
{
    return std::equal(lhs.segments.begin(), lhs.segments.end(), rhs.segments.begin(), rhs.segments.end(),
                      [](const auto &a, const auto &b) { return a.syllable == b.syllable; });
}
} // namespace

std::vector<AutocorrectCut> legal_input_correction_cuts(const std::string &pinyin, const Segments &segments,
                                                        const unsigned autocorrect_types, const std::size_t k)
{
    const unsigned types = autocorrect_types & kLegalInputCorrectionTypes;
    if (k == 0 || types == 0 || segments.empty() || pinyin.find('\'') != std::string::npos ||
        !has_only_complete_pinyin_segments(segments))
    {
        return {};
    }
    // segments 必须与 pinyin 逐字母一致：ü 别名归一（jv -> ju）会改写字母，那时
    // 下面的原始区间对不上，整条路径不出读法。
    std::vector<std::pair<size_t, size_t>> zero_initial_spans;
    std::string letters;
    for (size_t i = 0; i < segments.size(); ++i)
    {
        if (i > 0 && is_zero_initial_syllable(segments[i]))
        {
            zero_initial_spans.emplace_back(letters.size(), letters.size() + segments[i].size());
        }
        letters += segments[i];
    }
    if (letters != pinyin)
    {
        return {};
    }

    std::vector<AutocorrectCut> cuts;
    const auto add_cut = [&cuts](AutocorrectCut cut) {
        const bool seen = std::any_of(cuts.begin(), cuts.end(),
                                      [&](const AutocorrectCut &existing) { return same_syllables(existing, cut); });
        if (!seen)
        {
            cuts.push_back(std::move(cut));
        }
    };

    // 合法输入上的手误几乎都在字面切分里留下一个非首位零声母音节：换位挤出元音
    // （zi'a、ji'o'a、ni'a），多打一个元音（jiu'zhe'e'yang），漏打声母（jiu'zhe'ang）。
    // 纠正的那段必须盖住这样一个音节：不然 k-best 会跨音节边界重切任何普通输入，
    // 道路 dao'lu 读成 da + olu->lou「大楼」、会的 hui'de 读成 hu + ide->die「蝴蝶」——
    // 词库扫描里第 2 位的噪声几乎全是这种。生成式空间（静态表之外的远键替换）
    // 也不认：它是给切不成合法音节的输入兜底的，放到合法输入上只会拼出巧合。
    // 先多搜几条再过滤：长句里前几名常被别处更便宜、却不盖零声母音节的纠正占满
    // （jintianjiuzheeyangba 取 3 条时一条都留不下）。与查询层同一个 k。
    for (auto &cut : zero_initial_spans.empty()
                         ? std::vector<AutocorrectCut>{}
                         : autocorrect_cut_kbest(pinyin, types, std::max(k, kAutocorrectAlignKBest)))
    {
        const bool covers_zero_initial =
            std::any_of(cut.segments.begin(), cut.segments.end(), [&](const AutocorrectCutSegment &segment) {
                const size_t end = segment.start + segment.raw_text.size();
                return segment.corrected &&
                       std::any_of(zero_initial_spans.begin(), zero_initial_spans.end(),
                                   [&](const auto &span) { return segment.start < span.second && span.first < end; });
            });
        // 末尾音节是按漏字读出来的（nia -> nian）：这多半是还没打完，不是手误。
        const auto &tail = cut.segments.back();
        const bool unfinished_tail = tail.corrected && tail.raw_text.size() < tail.syllable.size();
        if (covers_zero_initial && !unfinished_tail && !cut.has_generated &&
            cut.edge_count <= kMaxLegalInputCorrections)
        {
            add_cut(std::move(cut));
        }
    }

    // 罕见音节本身合法，k-best 只给非法片段找纠正，走不到这里。单音节输入没有上下文，
    // 打的就是这个字的可能性太大（lia 单打多半就是要「俩」），不替换。
    if ((types & kAutocorrectTransposition) != 0 && segments.size() >= 2)
    {
        AutocorrectCut replaced;
        size_t start = 0;
        for (const auto &syllable : segments)
        {
            AutocorrectCutSegment segment{syllable, syllable, start, false};
            for (const auto &[rare, common] : kRareLegalTranspositions)
            {
                if (syllable == rare)
                {
                    segment.syllable = std::string(common);
                    segment.corrected = true;
                    segment.weight = kAutocorrectTranspositionWeight;
                    ++replaced.edge_count;
                    replaced.weight += kAutocorrectTranspositionWeight;
                    break;
                }
            }
            start += syllable.size();
            replaced.segments.push_back(std::move(segment));
        }
        if (replaced.edge_count > 0 && replaced.edge_count <= kMaxLegalInputCorrections)
        {
            add_cut(std::move(replaced));
        }
    }

    if (cuts.size() > k)
    {
        cuts.resize(k);
    }
    return cuts;
}

std::optional<AutocorrectCut> correction_cut_for_reading(const std::string &raw_letters, const Segments &reading,
                                                         const unsigned autocorrect_types)
{
    if (reading.empty())
    {
        return std::nullopt;
    }
    // 与查询层同一个 k：读音可能来自主切之外的备选切分。
    for (auto &cut : autocorrect_cut_kbest(raw_letters, autocorrect_types, kAutocorrectAlignKBest))
    {
        if (cut.segments.size() >= reading.size() &&
            std::equal(reading.begin(), reading.end(), cut.segments.begin(),
                       [](const std::string &syllable, const AutocorrectCutSegment &segment) {
                           return syllable == segment.syllable;
                       }))
        {
            return std::move(cut);
        }
    }
    return std::nullopt;
}

std::optional<size_t> corrected_reading_raw_length(const std::string &raw_letters, const Segments &reading,
                                                   const unsigned autocorrect_types)
{
    std::string reading_letters;
    for (const auto &syllable : reading)
    {
        reading_letters += syllable;
    }
    // 读音是原始字母的前缀，且整串本身是合法拼音（可带一个末尾声母，与查询侧不进纠错的
    // 判据一致）：没有纠错在起作用，按读音长度即可。合法输入上的纠错读法另有原读法领衔或
    // 第 2 位兜底，选字面候选不该被它带偏（zaijinuta 选「再」曾按 zaij -> zai 吞掉 j）。
    // 只看前缀不够：buuhui 里单选「不」，bu 碰巧也是原始前缀，可它实际盖住的是 buu。
    if (reading.empty() || (raw_letters.compare(0, reading_letters.size(), reading_letters) == 0 &&
                            (is_complete_pinyin_input(raw_letters) || looks_like_jianpin_spelling(raw_letters))))
    {
        return std::nullopt;
    }
    const auto cut = correction_cut_for_reading(raw_letters, reading, autocorrect_types);
    if (!cut.has_value())
    {
        return std::nullopt;
    }
    const auto &last = cut->segments[reading.size() - 1];
    return last.start + last.raw_text.size();
}

bool is_deletion_variant(const std::string &raw_text, const std::string &reading)
{
    if (reading.size() + 1 != raw_text.size())
    {
        return false;
    }
    size_t r = 0;
    size_t skipped = 0;
    for (const char ch : raw_text)
    {
        if (skipped == 0 && (r >= reading.size() || ch != reading[r]))
        {
            skipped = 1;
            continue;
        }
        if (r < reading.size() && reading[r] == ch)
        {
            ++r;
            continue;
        }
        return false;
    }
    return skipped == 1 && r == reading.size();
}

std::optional<size_t> fault_reading_raw_length(const std::string &raw_letters, const Segments &reading)
{
    const auto slots = build_fault_slots(raw_letters);
    if (slots.empty() || reading.size() > slots.size() || reading.empty())
    {
        return std::nullopt;
    }
    const auto &intact = intact_pinyin_set();
    size_t consumed = 0;
    for (size_t i = 0; i < reading.size(); ++i)
    {
        const auto &slot = slots[i];
        const auto &syllable = reading[i];
        // 槽盖住的原始字母必须被读音逐字母消耗（允许 ü 归一）。
        const bool matches_slot = syllable == slot.base_syllable ||
                                  (syllable.size() == slot.raw_text.size() &&
                                   canonical_umlaut_spelling(slot.raw_text) == canonical_umlaut_spelling(syllable));
        switch (slot.kind)
        {
        case FaultSlotKind::Determining:
            // loose scan 产出的漏字变体（yang -> yan）也是这个槽的合法读法，与
            // arbitrate_fault_slots / QuanpinDisplayAtWinningSlots 同一条形态判定。
            if (!matches_slot && !is_deletion_variant(slot.raw_text, syllable))
            {
                return std::nullopt;
            }
            break;
        case FaultSlotKind::SingleInitial:
            // 简拼读法（r）或同首字母的展开音节（ru）都消耗这个槽；别的音节对不上。
            if (!matches_slot && (syllable.empty() || syllable.front() != slot.raw_text.front()))
            {
                return std::nullopt;
            }
            break;
        case FaultSlotKind::Fault:
            // Fault 槽只能读成合法音节；任意音节都消耗这段字母。
            if (syllable.empty() || intact.count(syllable) == 0)
            {
                return std::nullopt;
            }
            break;
        }
        consumed += slot.raw_text.size();
    }
    return consumed;
}

AutocorrectCut autocorrect_cut_detail(const std::string &pinyin, const unsigned autocorrect_types)
{
    // Single-hypothesis projection of the k-best search: same gating, same
    // ranking, cheapest path for the per-keystroke preedit consumer.
    const auto kbest = autocorrect_cut_kbest(pinyin, autocorrect_types, 1);
    return kbest.empty() ? AutocorrectCut{} : kbest.front();
}

Segments autocorrect_cut(const std::string &pinyin, const unsigned autocorrect_types)
{
    // Projection wrapper: existing callers and tests only need the corrected
    // syllable sequence, so keep the phase-1 signature working unchanged.
    const auto detail = autocorrect_cut_detail(pinyin, autocorrect_types);
    Segments result;
    result.reserve(detail.segments.size());
    for (const auto &segment : detail.segments)
    {
        result.push_back(segment.syllable);
    }
    return result;
}

} // namespace quanpin
