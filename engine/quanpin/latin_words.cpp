#include "latin_words.h"
#include "quanpin_query.h"

#include <utf8.h>

#include <algorithm>
#include <cctype>
#include <iterator>

namespace quanpin
{
namespace
{
// 别的连接提交过就会变的计数。读不到时返回 -1：每次都当作变了，宁可多读几遍也不漏掉新加的词。
// 每次按键要查好几回，语句放进词典的语句缓存反复用（现编一次约 70 µs，复用只要零头）；读完立刻
// reset，不让这条语句一直占着读事务。
sqlite3_int64 data_version(sqlite3 *db, std::unordered_map<std::string, sqlite3_stmt *> &statement_cache)
{
    if (db == nullptr)
        return -1;
    static const std::string kSql = "PRAGMA data_version";
    sqlite3_stmt *statement = nullptr;
    if (const auto found = statement_cache.find(kSql); found != statement_cache.end())
    {
        statement = found->second;
        sqlite3_reset(statement);
    }
    else
    {
        if (sqlite3_prepare_v2(db, kSql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
        {
            sqlite3_finalize(statement);
            return -1;
        }
        statement_cache.emplace(kSql, statement);
    }
    const sqlite3_int64 version = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
    sqlite3_reset(statement);
    return version;
}
} // namespace

std::shared_ptr<const LatinWords> refresh_latin_words(std::shared_ptr<const LatinWords> current, bool &stale,
                                                      sqlite3_int64 &loaded_version, sqlite3 *db,
                                                      std::unordered_map<std::string, sqlite3_stmt *> &statement_cache)
{
    // 词典自己的缓存只在命中时才查 data_version，没命中的那次查询不会清缓存；字母词常驻内存，
    // 自己再看一眼，不管走哪条查询路径，设置页加完词的下一次按键都能用上。
    const sqlite3_int64 version = data_version(db, statement_cache);
    if (current && !stale && version >= 0 && version == loaded_version)
        return current;
    stale = false;
    loaded_version = version;
    auto fresh = std::make_shared<LatinWords>();
    fresh->load(db);
    if (current && current->all() == fresh->all())
        return current;
    return fresh;
}

void LatinWords::load(sqlite3 *db)
{
    words_.clear();
    max_code_length_ = 0;
    if (db == nullptr)
        return;
    sqlite3_stmt *statement = nullptr;
    // 没有 exact_only 列的表是车牌还没收进来时建的，全按普通字母词读。表不存在时两条都 prepare 失败：
    // 旧词库，没有字母词。
    bool has_exact_only = true;
    if (sqlite3_prepare_v2(db, "SELECT \"key\", \"value\", \"weight\", \"exact_only\" FROM latin_words", -1, &statement,
                           nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(statement);
        statement = nullptr;
        has_exact_only = false;
        if (sqlite3_prepare_v2(db, "SELECT \"key\", \"value\", \"weight\" FROM latin_words", -1, &statement, nullptr) !=
            SQLITE_OK)
        {
            sqlite3_finalize(statement);
            return;
        }
    }
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        const auto *key = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
        const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement, 1));
        if (key == nullptr || value == nullptr)
            continue;
        LatinWord word;
        word.key = key;
        word.value = value;
        word.weight = sqlite3_column_int64(statement, 2);
        word.exact_only = has_exact_only && sqlite3_column_int(statement, 3) != 0;
        word.code = latin_code(split_segments(word.key));
        if (word.code.empty() || word.value.empty())
            continue;
        max_code_length_ = (std::max)(max_code_length_, word.code.size());
        words_.push_back(std::move(word));
    }
    sqlite3_finalize(statement);
    std::sort(words_.begin(), words_.end(), [](const LatinWord &left, const LatinWord &right) {
        return left.code != right.code ? left.code < right.code : left.weight > right.weight;
    });
}

std::vector<const LatinWord *> LatinWords::exact(const std::string &code) const
{
    std::vector<const LatinWord *> result;
    if (code.empty())
        return result;
    auto it = std::lower_bound(words_.begin(), words_.end(), code,
                               [](const LatinWord &word, const std::string &target) { return word.code < target; });
    for (; it != words_.end() && it->code == code; ++it)
        result.push_back(&*it);
    return result;
}

std::vector<const LatinWord *> LatinWords::exact_in_sentence(const std::string &code) const
{
    auto result = exact(code);
    result.erase(std::remove_if(result.begin(), result.end(), [](const LatinWord *word) { return word->exact_only; }),
                 result.end());
    return result;
}

std::vector<const LatinWord *> LatinWords::completions(const std::string &code, std::size_t limit) const
{
    std::vector<const LatinWord *> result;
    if (code.empty() || limit == 0)
        return result;
    auto it = std::lower_bound(words_.begin(), words_.end(), code,
                               [](const LatinWord &word, const std::string &target) { return word.code < target; });
    for (; it != words_.end() && it->code.compare(0, code.size(), code) == 0; ++it)
    {
        if (it->code.size() > code.size() && !it->exact_only)
            result.push_back(&*it);
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const LatinWord *left, const LatinWord *right) { return left->weight > right->weight; });
    if (result.size() > limit)
        result.resize(limit);
    return result;
}

bool LatinWords::covers_incomplete_segments(const Segments &segments) const
{
    if (words_.empty())
        return false;
    const auto &complete = intact_pinyin_set();
    std::vector<bool> covered(segments.size(), false);
    bool any_incomplete = false;
    for (std::size_t start = 0; start < segments.size(); ++start)
    {
        any_incomplete = any_incomplete || complete.find(segments[start]) == complete.end();
        std::string code;
        for (std::size_t end = start + 1; end <= segments.size(); ++end)
        {
            code += segments[end - 1];
            if (code.size() > max_code_length_)
                break;
            if (exact_in_sentence(code).empty())
                continue;
            for (std::size_t i = start; i < end; ++i)
                covered[i] = true;
        }
    }
    if (!any_incomplete)
        return false;
    for (std::size_t i = 0; i < segments.size(); ++i)
    {
        if (!covered[i] && complete.find(segments[i]) == complete.end())
            return false;
    }
    return true;
}

void insert_latin_candidates(std::vector<WordItem> &result, const std::vector<const LatinWord *> &exact,
                             const std::vector<const LatinWord *> &completions, const std::string &selection_code,
                             const Segments &typed_syllables)
{
    if (exact.empty() && completions.empty())
        return;
    const auto is_sentence_guess = [](const WordItem &item) {
        return item.source == CandidateSource::Generated || item.source == CandidateSource::Fallback ||
               item.source == CandidateSource::NeuralDesktop || item.source == CandidateSource::NeuralKeyboard ||
               item.source == CandidateSource::Collocation;
    };
    // 先把重复的整句都去掉，再算插入位置，免得下标在插入途中错位。
    for (const auto *words : {&exact, &completions})
    {
        for (const auto *word : *words)
        {
            result.erase(std::remove_if(
                             result.begin(), result.end(),
                             [&](const WordItem &item) { return item.word == word->value && is_sentence_guess(item); }),
                         result.end());
        }
    }
    const auto place = [&](const LatinWord *word, std::size_t at) {
        if (std::any_of(result.begin(), result.end(), [&](const WordItem &item) { return item.word == word->value; }))
            return at;
        WordItem item(selection_code, word->value, word->weight, CandidateSource::Database, word->key);
        item.latin_word = true;
        at = (std::min)(at, result.size());
        result.insert(result.begin() + static_cast<std::ptrdiff_t>(at), std::move(item));
        return at + 1;
    };
    std::size_t at = generated_sentence_insert_position(result, typed_syllables);
    for (const auto *word : exact)
        at = place(word, at);
    while (at < result.size() && is_sentence_guess(result[at]))
        ++at;
    for (const auto *word : completions)
        at = place(word, at);
}

void prepend_latin_lexemes(const LatinWords &words, const Segments &span, const ngram::LanguageModel *model,
                           std::vector<LatticeLexeme> &rows)
{
    // sc.lm 只认得 T恤、U盘、卡拉OK 等几十个字母词，其余按未登录词罚分（-7.78）。罚掉的大半还回去：
    // t、k 这类只有字母词覆盖得了的段不受影响（每条整句都要经过那个词），这一截只在 AA制、A股 这种
    // 全由 a/e/o 和完整音节组成、能和纯汉字路径比分的词上起作用——全还会让它们挤掉正常整句，
    // 一点不还它们又总输给「啊啊制」。留下的 2.0 由 eval_quanpin_sentence 对照定。
    constexpr double kUnknownResidual = 2.0;
    const auto matches = words.exact_in_sentence(latin_code(span));
    if (matches.empty())
        return;
    std::vector<LatticeLexeme> latin;
    latin.reserve(matches.size());
    for (const auto *word : matches)
    {
        LatticeLexeme lexeme{word->key, word->value, word->weight};
        if (model != nullptr && model->valid() && model->is_unknown(model->index(word->value)))
            lexeme.penalty = static_cast<double>(model->unknown_penalty()) + kUnknownResidual;
        latin.push_back(std::move(lexeme));
    }
    rows.insert(rows.begin(), std::make_move_iterator(latin.begin()), std::make_move_iterator(latin.end()));
}

std::string latin_code(const Segments &segments)
{
    std::string code;
    for (const auto &segment : segments)
    {
        for (const char ch : segment)
        {
            if (ch < 'a' || ch > 'z')
                return {};
            code.push_back(ch);
        }
    }
    return code;
}

std::string latin_code(const std::string &raw_input)
{
    std::string code;
    code.reserve(raw_input.size());
    for (const char ch : raw_input)
    {
        if (ch == '\'')
            continue;
        if (ch >= 'A' && ch <= 'Z')
            code.push_back(static_cast<char>(ch - 'A' + 'a'));
        else if (ch >= 'a' && ch <= 'z')
            code.push_back(ch);
        else
            return {};
    }
    return code;
}

bool contains_latin_letter(const std::string &text)
{
    return std::any_of(text.begin(), text.end(),
                       [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); });
}

namespace
{
// 一条字母词至多这么多个字。出厂最长的不过十来个字，留足余量，只为挡住把整段话当成一个词导进来。
constexpr std::size_t kLatinWordMaxChars = 32;
// 最长的音节是 zhuang、chuang、shuang。
constexpr std::size_t kMaxSyllableLength = 6;

bool is_ascii_letter(char32_t ch)
{
    return (ch >= U'a' && ch <= U'z') || (ch >= U'A' && ch <= U'Z');
}

char ascii_lower(char32_t ch)
{
    return static_cast<char>(ch >= U'A' && ch <= U'Z' ? ch - U'A' + U'a' : ch);
}

// 能出现在字母词里的汉字：基本区、扩展 A、兼容表意字、2/3 平面的扩展 B 及以后。
bool is_han(char32_t ch)
{
    return (ch >= 0x3400 && ch <= 0x4DBF) || (ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0xF900 && ch <= 0xFAFF) ||
           (ch >= 0x20000 && ch <= 0x3FFFF);
}

// 解不了的 UTF-8 返回空。
std::optional<std::vector<char32_t>> decode(const std::string &text)
{
    if (!utf8::is_valid(text.begin(), text.end()))
        return std::nullopt;
    std::vector<char32_t> result;
    utf8::unchecked::utf8to32(text.begin(), text.end(), std::back_inserter(result));
    return result;
}

// 带声调的拼音字母还原成键盘上的字母，ü 写成 v（与建库脚本 to_normal 后的写法一致）。认不出返回 0。
char toneless_letter(char32_t ch)
{
    switch (ch)
    {
    case 0x0101:
    case 0x00E1:
    case 0x01CE:
    case 0x00E0:
    case 0x0100:
    case 0x00C1:
    case 0x01CD:
    case 0x00C0:
        return 'a';
    case 0x0113:
    case 0x00E9:
    case 0x011B:
    case 0x00E8:
    case 0x00EA:
    case 0x0112:
    case 0x00C9:
    case 0x011A:
    case 0x00C8:
    case 0x00CA:
        return 'e';
    case 0x012B:
    case 0x00ED:
    case 0x01D0:
    case 0x00EC:
    case 0x012A:
    case 0x00CD:
    case 0x01CF:
    case 0x00CC:
        return 'i';
    case 0x014D:
    case 0x00F3:
    case 0x01D2:
    case 0x00F2:
    case 0x014C:
    case 0x00D3:
    case 0x01D1:
    case 0x00D2:
        return 'o';
    case 0x016B:
    case 0x00FA:
    case 0x01D4:
    case 0x00F9:
    case 0x016A:
    case 0x00DA:
    case 0x01D3:
    case 0x00D9:
        return 'u';
    case 0x01D6:
    case 0x01D8:
    case 0x01DA:
    case 0x01DC:
    case 0x00FC:
    case 0x01D5:
    case 0x01D7:
    case 0x01D9:
    case 0x01DB:
    case 0x00DC:
        return 'v';
    case 0x0144:
    case 0x0148:
    case 0x01F9:
    case 0x0143:
    case 0x0147:
    case 0x01F8:
        return 'n';
    case 0x1E3F:
    case 0x1E3E:
        return 'm';
    default:
        return 0;
    }
}

// 零声母的 n、ng、m 在拼音表里写成 en、eng、me，与建库脚本的 _ZERO_INITIAL 一致；ü 的两种写法归到
// 词库存的那一种（lve、ju），与设置页全拼导入一致。不是完整音节时返回空。
std::string normalized_syllable(const std::string &syllable)
{
    std::string result = syllable == "n" ? "en" : syllable == "ng" ? "eng" : syllable == "m" ? "me" : syllable;
    if (intact_pinyin_set().count(result) == 0)
        return {};
    return canonical_umlaut_spelling(std::move(result));
}

struct Unit
{
    bool letter = false;
    char lower = 0; // 字母的小写
};

std::optional<std::vector<Unit>> latin_units(const std::string &word, std::string &error)
{
    error = latin_word_problem(word);
    if (!error.empty())
        return std::nullopt;
    // 先存下来再遍历：直接对 decode 返回的临时 optional 解引用做 range-for，临时对象在循环开始前就析构了。
    const auto chars = decode(word);
    std::vector<Unit> units;
    for (const char32_t ch : *chars)
        units.push_back(is_ascii_letter(ch) ? Unit{true, ascii_lower(ch)} : Unit{false, 0});
    return units;
}

std::string join_key(const std::vector<Unit> &units, const std::vector<std::string> &han_syllables)
{
    std::string key;
    std::size_t next = 0;
    for (const auto &unit : units)
    {
        if (!key.empty())
            key.push_back('\'');
        if (unit.letter)
            key.push_back(unit.lower);
        else
            key += han_syllables[next++];
    }
    return key;
}
} // namespace

std::string latin_word_problem(const std::string &word)
{
    if (word.empty())
        return "词条不能为空";
    const auto chars = decode(word);
    if (!chars)
        return "词条不是有效的文字";
    if (chars->size() > kLatinWordMaxChars)
        return "字母词不能超过 " + std::to_string(kLatinWordMaxChars) + " 个字";
    bool letter = false;
    bool han = false;
    for (const char32_t ch : *chars)
    {
        if (is_ascii_letter(ch))
            letter = true;
        else if (is_han(ch))
            han = true;
        else
            return "字母词只能由英文字母和汉字组成（不支持数字、空格和符号）";
    }
    if (!letter || !han)
        return "字母词要同时含有英文字母和汉字";
    return {};
}

bool is_license_plate(const std::string &word)
{
    static const std::u32string kProvinces = U"京津沪渝冀豫云辽黑湘皖鲁新苏浙赣鄂桂甘晋蒙陕吉闽贵粤青藏川宁琼";
    const auto chars = decode(word);
    return chars && chars->size() == 2 && kProvinces.find((*chars)[0]) != std::u32string::npos && (*chars)[1] >= U'A' &&
           (*chars)[1] <= U'Z';
}

std::optional<LatinEntry> parse_latin_entry(const std::string &word, const std::string &reading, std::string &error)
{
    const auto units = latin_units(word, error);
    if (!units)
        return std::nullopt;
    const auto chars = decode(reading);
    if (!chars)
    {
        error = "读音不是有效的文字";
        return std::nullopt;
    }

    // 读音压成一串小写字母，记下哪些位置前面原本有分隔符：分隔符只能落在两段之间，不能切开一个音节。
    std::string letters;
    std::vector<bool> separated{false};
    for (std::size_t i = 0; i < chars->size(); ++i)
    {
        const char32_t ch = (*chars)[i];
        if (ch == U'\'' || ch == U' ' || ch == U'\t' || ch == 0x2019 || ch == 0x2018)
        {
            separated.back() = !letters.empty();
            continue;
        }
        if (ch >= U'0' && ch <= U'5')
            continue; // 数字调
        if (ch == 0x0308 && !letters.empty() && letters.back() == 'u')
        {
            letters.back() = 'v'; // u 加组合分音符
            continue;
        }
        if (ch >= 0x0300 && ch <= 0x036F)
            continue; // 组合声调符
        const char plain = is_ascii_letter(ch) ? ascii_lower(ch) : toneless_letter(ch);
        if (plain == 0)
        {
            error = "读音只能包含拼音、英文字母、空格和 '";
            return std::nullopt;
        }
        letters.push_back(plain);
        separated.push_back(false);
    }
    if (letters.empty())
    {
        error = "读音不能为空";
        return std::nullopt;
    }
    // separated[p]：letters[p] 前面有分隔符。末尾的分隔符没有意义。
    separated.resize(letters.size() + 1, false);

    // ways[i * stride + p]：units[i..] 对上 letters[p..] 的切法数，只数到 2（够判断唯一）。
    const std::size_t n = units->size();
    const std::size_t m = letters.size();
    const std::size_t stride = m + 1;
    std::vector<int> ways((n + 1) * stride, 0);
    ways[n * stride + m] = 1;
    const auto crosses_separator = [&](std::size_t from, std::size_t length) {
        for (std::size_t q = from + 1; q < from + length; ++q)
        {
            if (separated[q])
                return true;
        }
        return false;
    };
    const auto piece_lengths = [&](std::size_t i, std::size_t p) {
        std::vector<std::size_t> lengths;
        if ((*units)[i].letter)
        {
            if (p < m && letters[p] == (*units)[i].lower)
                lengths.push_back(1);
            return lengths;
        }
        for (std::size_t length = 1; length <= kMaxSyllableLength && p + length <= m; ++length)
        {
            if (!crosses_separator(p, length) && !normalized_syllable(letters.substr(p, length)).empty())
                lengths.push_back(length);
        }
        return lengths;
    };
    for (std::size_t i = n; i-- > 0;)
    {
        for (std::size_t p = 0; p <= m; ++p)
        {
            int total = 0;
            for (const std::size_t length : piece_lengths(i, p))
                total = (std::min)(2, total + ways[(i + 1) * stride + p + length]);
            ways[i * stride + p] = total;
        }
    }
    if (ways[0] == 0)
    {
        error = "读音对不上：词里的每个字母要对读音里的同一个字母，每个汉字对一个完整音节";
        return std::nullopt;
    }
    if (ways[0] > 1)
    {
        error = "读音有不止一种切法，请用 ' 隔开每个音节（例如 fang'an）";
        return std::nullopt;
    }

    std::vector<std::string> han_syllables;
    for (std::size_t i = 0, p = 0; i < n; ++i)
    {
        for (const std::size_t length : piece_lengths(i, p))
        {
            if (ways[(i + 1) * stride + p + length] == 0)
                continue;
            if (!(*units)[i].letter)
                han_syllables.push_back(normalized_syllable(letters.substr(p, length)));
            p += length;
            break;
        }
    }
    LatinEntry entry;
    entry.key = join_key(*units, han_syllables);
    entry.value = word;
    entry.exact_only = is_license_plate(word);
    return entry;
}

std::optional<LatinEntry> make_latin_entry(const std::string &word, const std::vector<std::string> &han_syllables,
                                           std::string &error)
{
    const auto units = latin_units(word, error);
    if (!units)
        return std::nullopt;
    const auto han_count =
        static_cast<std::size_t>(std::count_if(units->begin(), units->end(), [](const Unit &u) { return !u.letter; }));
    if (han_count != han_syllables.size())
    {
        error = "注音结果和汉字数量对不上";
        return std::nullopt;
    }
    std::vector<std::string> normalized;
    for (const auto &syllable : han_syllables)
    {
        std::string lower = syllable;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        auto canonical = normalized_syllable(lower);
        if (canonical.empty())
        {
            error = "注音得到的「" + syllable + "」不是完整的拼音音节";
            return std::nullopt;
        }
        normalized.push_back(std::move(canonical));
    }
    LatinEntry entry;
    entry.key = join_key(*units, normalized);
    entry.value = word;
    entry.exact_only = is_license_plate(word);
    return entry;
}

std::vector<std::string> latin_word_han_runs(const std::string &word)
{
    std::vector<std::string> runs;
    const auto chars = decode(word);
    if (!chars)
        return runs;
    std::string current;
    for (const char32_t ch : *chars)
    {
        if (is_han(ch))
        {
            utf8::unchecked::append(static_cast<std::uint32_t>(ch), std::back_inserter(current));
            continue;
        }
        if (!current.empty())
            runs.push_back(std::move(current));
        current.clear();
    }
    if (!current.empty())
        runs.push_back(std::move(current));
    return runs;
}
} // namespace quanpin
