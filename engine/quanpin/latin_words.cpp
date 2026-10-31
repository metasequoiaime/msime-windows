#include "latin_words.h"
#include "quanpin_query.h"

#include <algorithm>
#include <iterator>

namespace quanpin
{
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
} // namespace quanpin
