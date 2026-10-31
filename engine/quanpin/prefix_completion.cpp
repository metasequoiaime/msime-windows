#include "prefix_completion.h"

#include "../contracts/dictionary/format.h"
#include "quanpin_query.h"
#include "quanpin_utils.h"

#include <utf8.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <optional>
#include <unordered_set>

namespace quanpin
{
namespace
{
// 出厂词库里五个音节以上的词几乎都是这个权重以下（万象没给词频的词按 1 入库，再经调频落到个位数）。
// 高过它的行要么带着真实词频，要么被用户选过。
constexpr std::int64_t kShippedFlatWeight = 10;
// 语言模型认为补出来的那几个字跟在前缀后面的概率至少要有这么大（log10）。低于它的补全多半是
// 「北京大学星」「计算机科学中」这类切词残片，不显示。由 eval_prefix_completion 对照定。
constexpr double kMinimumTailLog10 = -3.0;
// 语言模型的词表里最长的词按这么多个字找（贪心最长匹配）。
constexpr std::size_t kLanguageModelLongestToken = 8;
// 缓存超过这么多个前缀就整体清掉，免得长时间输入把内存撑大。
constexpr std::size_t kCacheLimit = 512;

bool is_han(char32_t ch)
{
    return (ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0x3400 && ch <= 0x4DBF) || (ch >= 0xF900 && ch <= 0xFAFF) ||
           (ch >= 0x20000 && ch <= 0x3134F) || ch == 0x3007;
}

// 全是汉字时返回码点序列，混进别的字符（字母、数字、符号）或不是合法 UTF-8 时返回空。
std::u32string han_codepoints(const std::string &text)
{
    std::u32string result;
    try
    {
        auto it = text.begin();
        while (it != text.end())
        {
            const char32_t ch = static_cast<char32_t>(utf8::next(it, text.end()));
            if (!is_han(ch))
                return {};
            result.push_back(ch);
        }
    }
    catch (const utf8::exception &)
    {
        return {};
    }
    return result;
}

std::string to_utf8(const std::u32string &text)
{
    std::string result;
    utf8::utf32to8(text.begin(), text.end(), std::back_inserter(result));
    return result;
}

std::string normalized_code(const std::string &code)
{
    std::string result;
    result.reserve(code.size());
    for (const char ch : code)
    {
        if (ch == '\'')
            continue;
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

std::size_t syllable_count(const std::string &key)
{
    return key.empty() ? 0 : static_cast<std::size_t>(std::count(key.begin(), key.end(), '\'')) + 1;
}

struct LanguageModelReading
{
    double log10 = 0.0;
    std::vector<std::size_t> token_lengths; // 按字数
};

// 一段文字在语言模型下的 log10 概率：从左往右按模型词表做最长匹配切词，逐词累加。单字总在词表里，
// 不在时按未登录词计（模型自己加罚分）。
LanguageModelReading read_with_language_model(const ngram::LanguageModel &model, const std::u32string &text)
{
    LanguageModelReading reading;
    ngram::State state = model.null_state();
    ngram::State next;
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t length = (std::min)(kLanguageModelLongestToken, text.size() - start);
        for (; length > 1; --length)
        {
            if (!model.is_unknown(model.index(to_utf8(text.substr(start, length)))))
                break;
        }
        reading.log10 += model.score(state, to_utf8(text.substr(start, length)), next);
        reading.token_lengths.push_back(length);
        state = next;
        start += length;
    }
    return reading;
}

// 补全是不是切词残片：前面已经是个完整的词，后面只跟了一个常见字（人工智能能、社会主义好、改革开放后），
// 或补完一个词又多拖了一个字（计算机科学中）。这类行在万象词库里不少，语言模型看它们是通顺的短句，
// 分数并不低，只能按形状挡。
bool looks_like_fragment(std::size_t prefix_chars, std::size_t value_chars, bool prefix_is_word,
                         const LanguageModelReading *reading)
{
    if (prefix_is_word && value_chars == prefix_chars + 1)
        return true;
    if (reading == nullptr || reading->token_lengths.size() < 2 || reading->token_lengths.back() != 1)
        return false;
    // 末尾是单独一个字，且它前面那些字已经越过了前缀：前缀本身就补成了一个词，这个字是多出来的。
    return value_chars - 1 > prefix_chars;
}

bool is_sentence_guess(const WordItem &item)
{
    return item.source == CandidateSource::Generated || item.source == CandidateSource::Fallback ||
           item.source == CandidateSource::NeuralDesktop || item.source == CandidateSource::NeuralKeyboard ||
           item.source == CandidateSource::Collocation;
}
} // namespace

std::vector<PrefixCompletionRow> query_prefix_completion_rows(
    sqlite3 *db, std::unordered_map<std::string, sqlite3_stmt *> &statement_cache, const std::string &prefix_key,
    const std::string &prefix_word, int per_table_limit)
{
    std::vector<PrefixCompletionRow> rows;
    const std::size_t prefix_syllables = syllable_count(prefix_key);
    if (db == nullptr || prefix_syllables == 0)
        return rows;
    // 分隔符 ' 是 0x27，( 是 0x28：[key', key( ) 正好是「key 后面还有音节」的全部键，不会捞到
    // zhong'hua'ren'ming 这种最后一个音节只是前缀的键。
    const std::string lower = prefix_key + "'";
    const std::string upper = prefix_key + "(";
    std::unordered_set<std::string> tables;
    std::unordered_map<std::string, std::size_t> by_value;
    for (std::size_t syllables = prefix_syllables + 1;
         syllables <= metasequoia::dictionary_format::maximum_numbered_syllables + 1; ++syllables)
    {
        // 表名只经格式契约拼（≥ 8 音节都落在 tbl_others_*，见 engine/AGENTS.md）。
        const std::string table = metasequoia::dictionary_format::quanpin_table(syllables, prefix_key.front());
        if (table.empty() || !tables.insert(table).second)
            continue;
        for (auto &row : query_key_range_keyed(db, statement_cache, table, lower, upper, per_table_limit))
        {
            if (!prefix_word.empty() &&
                (row.value.size() <= prefix_word.size() || row.value.compare(0, prefix_word.size(), prefix_word) != 0))
                continue;
            if (han_codepoints(row.value).size() != syllable_count(row.key))
                continue;
            const auto found = by_value.find(row.value);
            if (found != by_value.end())
            {
                if (row.weight > rows[found->second].weight)
                {
                    rows[found->second].key = std::move(row.key);
                    rows[found->second].weight = row.weight;
                }
                continue;
            }
            by_value.emplace(row.value, rows.size());
            PrefixCompletionRow completion;
            completion.key = std::move(row.key);
            completion.value = std::move(row.value);
            completion.weight = row.weight;
            rows.push_back(std::move(completion));
        }
    }
    return rows;
}

void score_prefix_completions(std::vector<PrefixCompletionRow> &rows, std::size_t prefix_chars, bool prefix_is_word,
                              const ngram::LanguageModel *language_model)
{
    const bool has_model = language_model != nullptr && language_model->valid();
    // 以前缀开头的只有这一行：没得挑，就是它。「海内存知」只补得出「海内存知己」，不能因为它只多一个字、
    // 权重又在出厂平线上就挡掉；「人工智能能」那类残片出在以它开头的行有几十条的前缀上。
    const bool only_choice = rows.size() == 1;
    for (auto &row : rows)
    {
        const std::u32string value = han_codepoints(row.value);
        std::optional<LanguageModelReading> reading;
        double prefix_log10 = 0.0;
        if (has_model && value.size() > prefix_chars)
        {
            reading = read_with_language_model(*language_model, value);
            // 条件概率的分母取这一行自己的前几个字：按拼音补的行，前缀的字和首选（整句猜的）未必一样。
            prefix_log10 = read_with_language_model(*language_model, value.substr(0, prefix_chars)).log10;
        }
        row.prior = std::log10(static_cast<double>((std::max<std::int64_t>)(row.weight, 1)));
        row.tail = reading ? reading->log10 - prefix_log10 : 0.0;
        row.score = row.prior + row.tail;
        const bool trusted_weight = row.weight > kShippedFlatWeight;
        const bool fragment =
            looks_like_fragment(prefix_chars, value.size(), prefix_is_word, reading ? &*reading : nullptr);
        if (has_model)
        {
            // 不像切词残片才显示；前缀是词库词时还要模型认得它。前缀不是词库词时，以这串读音开头的行
            // 本来就少而准，模型只管排序：诗句的条件概率常常很低（春眠不觉晓 的「晓」），卡门槛会把它挡掉。
            // 用户选过或带着真实词频的词照样显示。
            row.confident = trusted_weight || (!fragment && (!prefix_is_word || row.tail >= kMinimumTailLog10));
        }
        else
        {
            // 没有模型时，前缀是词库词的（人工智能）只补权重高过出厂平线的：以它开头的平线行多是
            // 「人工智能能」这类残片，挑出来的只会是字典序第一个。前缀不是词库词的（床前明月、马克思主）
            // 以这串读音开头的行通常只有一两条，就是用户正在打的那个长词或诗句，平线也照补。
            row.confident = trusted_weight || !prefix_is_word;
        }
        row.confident = row.confident || only_choice;
    }
    std::stable_sort(rows.begin(), rows.end(), [](const PrefixCompletionRow &left, const PrefixCompletionRow &right) {
        if (left.score != right.score)
            return left.score > right.score;
        // 同分时短的在前：补得越少越可能就是用户要的。
        return left.value.size() < right.value.size();
    });
}

const std::vector<PrefixCompletionRow> *PrefixCompletionCache::find(const std::string &key) const
{
    const auto found = entries_.find(key);
    return found == entries_.end() ? nullptr : &found->second;
}

void PrefixCompletionCache::insert(const std::string &key, std::vector<PrefixCompletionRow> rows)
{
    if (entries_.size() >= kCacheLimit)
        entries_.clear();
    entries_[key] = std::move(rows);
}

std::string prefix_completion_head_key(const std::vector<WordItem> &result, const std::string &full_code,
                                       const PrefixCompletionOptions &options)
{
    if (!options.enabled || result.empty() || full_code.empty())
        return {};
    const WordItem &head = result.front();
    if (head.source != CandidateSource::Database && head.source != CandidateSource::UserDatabase &&
        !is_sentence_guess(head))
        return {};
    if (head.latin_word || head.canonical_pinyin.empty() || normalized_code(head.pinyin) != normalized_code(full_code))
        return {};
    const auto segments = split_segments(head.canonical_pinyin);
    if (!has_only_complete_pinyin_segments(segments))
        return {};
    const std::size_t chars = han_codepoints(head.word).size();
    if (chars < options.minimum_chars || chars != segments.size())
        return {};
    return head.canonical_pinyin;
}

void insert_prefix_completion(std::vector<WordItem> &result, const std::string &full_code,
                              const PrefixCompletionOptions &options, sqlite3 *db,
                              std::unordered_map<std::string, sqlite3_stmt *> &statement_cache,
                              PrefixCompletionCache &cache, const ngram::LanguageModel *language_model)
{
    const std::string prefix_key = prefix_completion_head_key(result, full_code, options);
    if (prefix_key.empty())
        return;
    const WordItem &head = result.front();
    // 首选是词库里的行，前缀本身就是一个词；整句猜出来的首选（马克思主、计算机科）往往只是半个词。
    const bool prefix_is_word =
        head.source == CandidateSource::Database || head.source == CandidateSource::UserDatabase;
    // 同一个读音可能对应不同的首选（打了辅助码、换了整句来源），缓存键带上字和它是不是词库词。
    const std::string cache_key = prefix_key + '\t' + head.word + (prefix_is_word ? "\tw" : "\ts");
    const std::vector<PrefixCompletionRow> *rows = cache.find(cache_key);
    if (rows == nullptr)
    {
        // 首选是词库词时按它的字补；是整句猜的时只按读音补：猜出来的字常常不对（chuangqianmingyue 猜成
        // 「窗前明月」），按字补就补不出「床前明月光」。
        auto queried =
            query_prefix_completion_rows(db, statement_cache, prefix_key, prefix_is_word ? head.word : std::string());
        score_prefix_completions(queried, syllable_count(prefix_key), prefix_is_word, language_model);
        cache.insert(cache_key, std::move(queried));
        rows = cache.find(cache_key);
    }
    const auto best =
        std::find_if(rows->begin(), rows->end(), [](const PrefixCompletionRow &row) { return row.confident; });
    if (best == rows->end())
        return;
    // 已经在候选里（整句恰好猜出了这个词，或排在后面几页）就把那一行挪到第二位，不再插一份。
    const auto existing =
        std::find_if(result.begin() + 1, result.end(), [&](const WordItem &item) { return item.word == best->value; });
    if (existing != result.end())
    {
        std::rotate(result.begin() + 1, existing, existing + 1);
        return;
    }
    WordItem item(head.pinyin, best->value, best->weight, CandidateSource::Database, best->key);
    item.scheme = head.scheme;
    item.prefix_completion = true;
    result.insert(result.begin() + 1, std::move(item));
}
} // namespace quanpin
