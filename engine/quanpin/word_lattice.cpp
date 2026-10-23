#include "word_lattice.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace quanpin
{
namespace
{

constexpr double kNegInf = -std::numeric_limits<double>::infinity();

std::string join_span(const Segments &span)
{
    std::string key;
    for (size_t i = 0; i < span.size(); ++i)
    {
        if (i)
            key.push_back('\'');
        key += span[i];
    }
    return key;
}

size_t syllable_count_from_key(const std::string &key)
{
    if (key.empty())
        return 0;
    size_t n = 1;
    for (char c : key)
    {
        if (c == '\'')
            ++n;
    }
    return n;
}

// UTF-8 单字切分。词库值都是合法 UTF-8，不做容错。
std::vector<std::string> split_utf8_chars(const std::string &text)
{
    std::vector<std::string> chars;
    for (size_t i = 0; i < text.size();)
    {
        const auto lead = static_cast<unsigned char>(text[i]);
        const size_t len = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
        chars.push_back(text.substr(i, len));
        i += len;
    }
    return chars;
}

// 词的字数必须等于跨度的音节数，被钉住的那几个字都要被各自的约束认可。
bool lexeme_satisfies(const std::string &value, size_t syllables,
                      const std::vector<std::pair<size_t, const LatticeCharConstraint *>> &pins)
{
    const auto chars = split_utf8_chars(value);
    if (chars.size() != syllables)
        return false;
    return std::all_of(pins.begin(), pins.end(), [&](const auto &pin) { return pin.second->accept(chars[pin.first]); });
}

struct LatticeEdge
{
    size_t end = 0;
    std::string word;
    std::string key;
    std::int64_t weight = 0;
    // Context-free part of the edge score: the heuristic when there is no
    // model, the dictionary tiebreak when there is one.
    double base_score = 0;
    // Vocabulary index of `word`, resolved once instead of per hypothesis.
    ngram::WordIndex index = 0;
};

// Fallback scorer, used only when no language model is available. Kept on the
// natural-log scale it was written on: nothing mixes it with model scores,
// because the choice is made once per decode, not per edge.
double heuristic_log_prob(std::int64_t weight, size_t syllables, const WordLatticeOptions &options)
{
    const double w = weight > 0 ? static_cast<double>(weight) : 1.0;
    const double z = options.unigram_z > 1.0 ? options.unigram_z : 1e6;
    const double lp = std::log(w);
    // Single-character rows in msime.db are raw corpus counts (often 1e6+);
    // multi-syllable rows are phrase weights on a much smaller scale.
    // libpinyin stores comparable log-probabilities; we approximate that by
    // down-projecting unigrams and giving dictionary phrases a length bonus.
    if (syllables <= 1)
        return lp - std::log(z);
    return lp + options.phrase_length_bonus * static_cast<double>(syllables);
}

// log10 of the dictionary weight, scaled down to a tiebreak. See
// WordLatticeOptions::dictionary_tiebreak for why it has to stay this small.
double dictionary_tiebreak(std::int64_t weight, const WordLatticeOptions &options)
{
    const double w = weight > 0 ? static_cast<double>(weight) : 1.0;
    return options.dictionary_tiebreak * std::log10(w);
}

// log10 P(词|拼音)，用跨度内的权重占比近似。语言模型只认汉字，「卷」「而」这种
// 字它见得多，可它根本不该被 gun / neng 召回——词库里这些多音行权重是 0，占比
// 一算就掉到底，先验替模型补上「这个读音对不对」这一维。
double reading_prior(std::int64_t weight, std::int64_t span_total, const WordLatticeOptions &options)
{
    if (options.reading_prior <= 0 || span_total <= 0)
        return 0;
    const double w = weight > 0 ? static_cast<double>(weight) : 0.0;
    const double share = (w + 1.0) / (static_cast<double>(span_total) + 1.0);
    const double threshold = options.reading_prior_share > 0 ? options.reading_prior_share : 1.0;
    if (share >= threshold)
        return 0;
    const double floor = options.reading_prior_floor > 0 ? -options.reading_prior_floor : kNegInf;
    return options.reading_prior * (std::max)(std::log10(share / threshold), floor);
}

struct Hyp
{
    double score = kNegInf;
    int prev_pos = -1;
    int prev_idx = -1;
    std::string word;
    std::string key;
    // Trailing n-gram context of this path. Unused (and left zeroed) when
    // decoding with the heuristic.
    ngram::State state;
    // 路径已累计词文本的尾部（至多 kCollocationTailCodepoints 个码点），供字级
    // 搭配打分用。关闭搭配项时保持为空，不为每个假设白付一次字符串拼接。
    std::string collocation_tail;
};

void keep_beam(std::vector<Hyp> &column, int beam)
{
    if (static_cast<int>(column.size()) <= beam)
        return;
    std::partial_sort(column.begin(), column.begin() + beam, column.end(),
                      [](const Hyp &a, const Hyp &b) { return a.score > b.score; });
    column.resize(static_cast<size_t>(beam));
}

// The model actually usable for this decode, or null to run the heuristic.
const ngram::LanguageModel *active_model(const WordLatticeOptions &options)
{
    if (options.language_model && options.language_model->valid())
        return options.language_model;
    return nullptr;
}

std::vector<std::vector<LatticeEdge>> build_graph(const Segments &syllables, const WordLatticeLookup &lookup,
                                                  const WordLatticeOptions &options)
{
    const ngram::LanguageModel *model = active_model(options);
    const size_t n = syllables.size();
    std::vector<std::vector<LatticeEdge>> graph(n);
    const size_t max_len = static_cast<size_t>(std::max(1, options.max_phrase_syllables));
    std::unordered_map<std::string, std::vector<LatticeLexeme>> span_cache;
    for (size_t start = 0; start < n; ++start)
    {
        const size_t max_end = (std::min)(n, start + max_len);
        for (size_t end = start + 1; end <= max_end; ++end)
        {
            Segments span(syllables.begin() + static_cast<std::ptrdiff_t>(start),
                          syllables.begin() + static_cast<std::ptrdiff_t>(end));
            const std::string span_key = join_span(span);
            std::vector<std::pair<size_t, const LatticeCharConstraint *>> pins;
            for (const auto &constraint : options.char_constraints)
            {
                if (constraint.syllable >= start && constraint.syllable < end && constraint.accept)
                    pins.emplace_back(constraint.syllable - start, &constraint);
            }
            std::vector<LatticeLexeme> constrained_rows;
            if (!pins.empty())
            {
                // 受约束的跨度按位置筛，结果与起点有关，不进按拼音共享的 span_cache。
                const auto &source = options.constrained_lookup ? options.constrained_lookup : lookup;
                for (auto &row : source(span))
                {
                    if (lexeme_satisfies(row.value, span.size(), pins))
                        constrained_rows.push_back(std::move(row));
                }
            }
            auto cached = span_cache.end();
            if (pins.empty())
            {
                cached = span_cache.find(span_key);
                if (cached == span_cache.end())
                    cached = span_cache.emplace(span_key, lookup(span)).first;
            }
            const auto &rows = pins.empty() ? cached->second : constrained_rows;
            const size_t take = (std::min)(rows.size(), static_cast<size_t>(std::max(0, options.span_limit)));
            // 先验的分母：跨度内实际参与解码、读音相同的那些行。行数被 span_limit 截断，
            // 截掉的都是权重最低的尾巴，对和的影响可以忽略。按读音分开求和：模糊音把别的
            // 读音的行混进同一跨度，算进一个分母会把敲出读音的行的占比压低。
            std::unordered_map<std::string, std::int64_t> reading_totals;
            for (size_t i = 0; i < take; ++i)
                reading_totals[rows[i].key] += rows[i].weight > 0 ? rows[i].weight : 0;
            for (size_t i = 0; i < take; ++i)
            {
                const auto &row = rows[i];
                if (row.value.empty())
                    continue;
                LatticeEdge edge;
                edge.end = end;
                edge.word = row.value;
                edge.key = row.key.empty() ? span_key : row.key;
                edge.weight = row.weight;
                if (model)
                {
                    edge.index = model->index(edge.word);
                    edge.base_score = dictionary_tiebreak(edge.weight, options) +
                                      reading_prior(edge.weight, reading_totals[row.key], options);
                }
                else
                {
                    edge.base_score = heuristic_log_prob(edge.weight, end - start, options);
                }
                edge.base_score -= row.penalty;
                graph[start].push_back(std::move(edge));
            }
        }
    }
    return graph;
}

size_t utf8_codepoints(const std::string &text)
{
    size_t n = 0;
    for (unsigned char c : text)
    {
        if ((c & 0xC0) != 0x80)
            ++n;
    }
    return n;
}

// 搭配打分的上下文窗口：.gram 的查询键最多编入 8 个码点，尾窗留 8 个足够。
constexpr size_t kCollocationTailCodepoints = 8;

// 保留字符串末尾至多 n 个 UTF-8 码点。假设 text 是合法 UTF-8（词库行不保证，
// 但搭配查询遇到乱码最多损失一次命中，不值得为它做完整校验）。
std::string tail_codepoints(const std::string &text, size_t n)
{
    const size_t total = utf8_codepoints(text);
    if (total <= n)
        return text;
    size_t skip = total - n;
    size_t i = 0;
    while (i < text.size() && skip > 0)
    {
        ++i;
        --skip;
        while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80)
            ++i;
    }
    return text.substr(i);
}

bool covers_all_syllables(const WordItem &item, size_t n_syllables)
{
    if (n_syllables == 0)
        return false;
    if (utf8_codepoints(item.word) == n_syllables)
        return true;
    if (!item.canonical_pinyin.empty() && syllable_count_from_key(item.canonical_pinyin) == n_syllables)
        return true;
    return false;
}

} // namespace

size_t generated_sentence_insert_position(const std::vector<WordItem> &candidates, const Segments &syllables)
{
    if (syllables.empty())
        return 0;

    // 只有「键与输入完全相等」才算精确命中。音节数相等是不够的：词库查不到精确键时
    // 会退到前缀区间扫描（key >= "gun'qi" AND key < …），gun'qi 因此会捞出 gun'qiu
    // 的「滚球」——同样 2 个音节 2 个字，按音节数判会被误认成精确命中，把整句压到
    // 它们后面。模糊音展开出来的行同理，键与输入不同，也不该挡住整句。
    const std::string typed_key = join_span(syllables);
    const auto is_exact_full_key_hit = [&](const WordItem &item) {
        if (item.source != CandidateSource::Database && item.source != CandidateSource::UserDatabase)
            return false;
        if (!item.canonical_pinyin.empty())
            return item.canonical_pinyin == typed_key;
        // 没带 canonical key 的行无从比对，退回旧的字数判断。词典层现在每一行都带
        // key，这条只为不给历史路径制造回归。
        return covers_all_syllables(item, syllables.size());
    };

    size_t insert_at = 0;
    while (insert_at < candidates.size() && is_exact_full_key_hit(candidates[insert_at]))
        ++insert_at;
    return insert_at;
}

std::vector<LatticePath> decode_word_lattice(const Segments &syllables, const WordLatticeLookup &lookup,
                                             const WordLatticeOptions &options)
{
    if (syllables.empty() || !lookup)
        return {};

    const size_t n = syllables.size();
    const ngram::LanguageModel *model = active_model(options);
    const auto graph = build_graph(syllables, lookup, options);
    std::vector<std::vector<Hyp>> columns(n + 1);
    Hyp start;
    start.score = 0.0;
    // Not begin_state(): the shipped sc.lm is built from an ARPA without
    // <s>/</s>, so a sentence-start context would only carry a word the model
    // has no n-grams for. Starting from the empty context makes the first word
    // score as a plain unigram, which is what that model can actually express.
    if (model)
        start.state = model->null_state();
    columns[0].push_back(std::move(start));

    for (size_t pos = 0; pos < n; ++pos)
    {
        keep_beam(columns[pos], options.beam);
        if (columns[pos].empty())
            continue;
        for (int hi = 0; hi < static_cast<int>(columns[pos].size()); ++hi)
        {
            const Hyp &hyp = columns[pos][static_cast<size_t>(hi)];
            for (const auto &edge : graph[pos])
            {
                Hyp next;
                next.score = hyp.score + edge.base_score;
                if (model)
                    next.score += model->score(hyp.state, edge.index, next.state);
                if (options.collocation_scorer && options.collocation_weight > 0.0)
                {
                    // 句尾词把 is_rear 置真：.gram 的「词+$」rear 项据此参与打分。
                    next.score += options.collocation_weight *
                                  options.collocation_scorer(hyp.collocation_tail, edge.word, edge.end == n);
                    next.collocation_tail =
                        tail_codepoints(hyp.collocation_tail + edge.word, kCollocationTailCodepoints);
                }
                next.prev_pos = static_cast<int>(pos);
                next.prev_idx = hi;
                next.word = edge.word;
                next.key = edge.key;
                columns[edge.end].push_back(std::move(next));
            }
        }
    }

    auto &final_col = columns[n];
    if (final_col.empty())
        return {};
    std::sort(final_col.begin(), final_col.end(), [](const Hyp &a, const Hyp &b) { return a.score > b.score; });
    const int take = (std::min)(options.nbest, static_cast<int>(final_col.size()));

    std::vector<LatticePath> paths;
    paths.reserve(static_cast<size_t>(take));
    std::unordered_set<std::string> seen;
    for (int i = 0; i < static_cast<int>(final_col.size()) && static_cast<int>(paths.size()) < take; ++i)
    {
        LatticePath path;
        path.log_prob = final_col[static_cast<size_t>(i)].score;
        int pos = static_cast<int>(n);
        int idx = i;
        std::vector<std::string> keys;
        while (pos > 0 && idx >= 0)
        {
            const Hyp &h = columns[static_cast<size_t>(pos)][static_cast<size_t>(idx)];
            path.words.push_back(h.word);
            keys.push_back(h.key);
            pos = h.prev_pos;
            idx = h.prev_idx;
        }
        std::reverse(path.words.begin(), path.words.end());
        std::reverse(keys.begin(), keys.end());
        for (const auto &w : path.words)
            path.sentence += w;
        for (size_t k = 0; k < keys.size(); ++k)
        {
            if (k)
                path.key.push_back('\'');
            path.key += keys[k];
        }
        if (path.sentence.empty() || !seen.insert(path.sentence).second)
            continue;
        paths.push_back(std::move(path));
    }
    return paths;
}

std::vector<LatticePath> decode_fault_lattice(const size_t slot_count,
                                              const std::vector<std::vector<FaultLatticeEdge>> &edges_by_start,
                                              const WordLatticeOptions &options)
{
    if (slot_count == 0)
        return {};
    const ngram::LanguageModel *model = active_model(options);

    // 与 decode_word_lattice 同一套 Viterbi：边由调用方按容错槽位预先查出并已折进
    // 手误代价，这里只负责 kenlm 打分、beam 剪枝与 n-best 去重。裁决用的解码刻意
    // 不装配搭配 scorer（octagram 禁令），量纲与词格主分同域（log10）。
    std::vector<std::vector<Hyp>> columns(slot_count + 1);
    Hyp start;
    start.score = 0.0;
    // 同 decode_word_lattice：shipped sc.lm 无 <s>/</s>，从空上下文起步。
    if (model)
        start.state = model->null_state();
    columns[0].push_back(std::move(start));

    for (size_t pos = 0; pos < slot_count; ++pos)
    {
        keep_beam(columns[pos], options.beam);
        if (columns[pos].empty())
            continue;
        for (int hi = 0; hi < static_cast<int>(columns[pos].size()); ++hi)
        {
            const Hyp &hyp = columns[pos][static_cast<size_t>(hi)];
            if (pos >= edges_by_start.size())
                continue;
            for (const auto &edge : edges_by_start[pos])
            {
                if (edge.start != pos || edge.end <= pos || edge.end > slot_count)
                    continue;
                Hyp next;
                // penalty 是手误代价（正数，log10），从路径分里扣；与 decode_word_lattice
                // 的 base_score -= row.penalty 同向。
                next.score = hyp.score - edge.penalty;
                if (model)
                {
                    next.score += dictionary_tiebreak(edge.weight, options);
                    const ngram::WordIndex index = model->index(edge.word);
                    next.score += model->score(hyp.state, index, next.state);
                }
                else
                {
                    // 无模型时按覆盖槽位数近似短语长度（同 heuristic_log_prob 的长度奖励）；
                    // 手误代价已在上面扣过，权重只剩一个很小的对数项。
                    next.score += std::log(edge.weight > 0 ? static_cast<double>(edge.weight) : 1.0) +
                                  options.phrase_length_bonus * static_cast<double>(edge.end - pos);
                }
                next.prev_pos = static_cast<int>(pos);
                next.prev_idx = static_cast<int>(hi);
                next.word = edge.word;
                next.key = edge.key;
                columns[edge.end].push_back(std::move(next));
            }
        }
    }

    auto &final_col = columns[slot_count];
    if (final_col.empty())
        return {};
    std::sort(final_col.begin(), final_col.end(), [](const Hyp &a, const Hyp &b) { return a.score > b.score; });
    const int take = (std::min)(options.nbest, static_cast<int>(final_col.size()));

    std::vector<LatticePath> paths;
    paths.reserve(static_cast<size_t>(take));
    std::unordered_set<std::string> seen;
    for (int i = 0; i < static_cast<int>(final_col.size()) && static_cast<int>(paths.size()) < take; ++i)
    {
        LatticePath path;
        path.log_prob = final_col[static_cast<size_t>(i)].score;
        int pos = static_cast<int>(slot_count);
        int idx = i;
        std::vector<std::string> keys;
        while (pos > 0 && idx >= 0)
        {
            const Hyp &h = columns[static_cast<size_t>(pos)][static_cast<size_t>(idx)];
            path.words.push_back(h.word);
            keys.push_back(h.key);
            pos = h.prev_pos;
            idx = h.prev_idx;
        }
        std::reverse(path.words.begin(), path.words.end());
        std::reverse(keys.begin(), keys.end());
        for (const auto &w : path.words)
            path.sentence += w;
        for (size_t k = 0; k < keys.size(); ++k)
        {
            if (k)
                path.key.push_back('\'');
            path.key += keys[k];
        }
        if (path.sentence.empty() || !seen.insert(path.sentence).second)
            continue;
        paths.push_back(std::move(path));
    }
    return paths;
}

void merge_lattice_candidates(std::vector<WordItem> &candidates, const Segments &syllables,
                              const WordLatticeLookup &lookup, const std::string &typed_pinyin,
                              const WordLatticeOptions &options, const std::vector<SourcedLatticeReranker> &rerankers)
{
    if (!lookup || syllables.size() < 2)
        return;
    if (!has_only_complete_pinyin_segments(syllables))
        return;

    auto paths =
        options.precomputed_paths.empty() ? decode_word_lattice(syllables, lookup, options) : options.precomputed_paths;
    if (paths.empty())
        return;

    // n-best 是解码的中间产物，不是要给用户看的东西：整个 n-best 铺出来，候选区会被一批
    // 只差一个字的整句占满。这里只留各来源的首选，n-best 的其余部分仅供重排挑选。
    // 每个重排器都从相同的静态顺序开始，不能让前一个模型的结论变成后一个的输入。保留完整顺序，
    // 这样首选与已有候选重复时可以从该来源自己的次选继续补位。
    std::optional<std::vector<LatticePath>> desktop_paths;
    std::optional<std::vector<LatticePath>> keyboard_paths;
    std::vector<std::pair<std::vector<LatticePath>, CandidateSource>> other_ranked_sources;
    bool keyboard_enabled = false;
    for (const SourcedLatticeReranker &entry : rerankers)
    {
        if (entry.source == CandidateSource::NeuralKeyboard)
            keyboard_enabled = true;

        std::vector<LatticePath> reranked_paths = paths;
        if (!entry.rerank || !entry.rerank(reranked_paths))
            continue;

        if (entry.source == CandidateSource::NeuralDesktop)
            desktop_paths = std::move(reranked_paths);
        else if (entry.source == CandidateSource::NeuralKeyboard)
            keyboard_paths = std::move(reranked_paths);
        else
            other_ranked_sources.emplace_back(std::move(reranked_paths), entry.source);
    }

    const auto source_prefers = [](const std::optional<std::vector<LatticePath>> &ranked_paths,
                                   const std::string &sentence) {
        return ranked_paths && !ranked_paths->empty() && ranked_paths->front().sentence == sentence;
    };
    const auto unigram_position = std::find_if(candidates.begin(), candidates.end(), [](const WordItem &item) {
        return item.source == CandidateSource::Fallback;
    });
    const bool unigram_consensus =
        unigram_position != candidates.end() &&
        ((options.include_lattice_best && paths.front().sentence == unigram_position->word) ||
         source_prefers(keyboard_paths, unigram_position->word) ||
         source_prefers(desktop_paths, unigram_position->word));

    std::unordered_set<std::string> already;
    for (const auto &item : candidates)
        already.insert(item.word);

    const auto first_distinct = [&](const std::vector<LatticePath> &ranked_paths) -> const LatticePath * {
        for (const LatticePath &path : ranked_paths)
        {
            if (already.find(path.sentence) != already.end())
            {
                if (options.show_next_on_duplicate)
                    continue;
                break;
            }
            return &path;
        }
        return nullptr;
    };
    const auto select_distinct = [&](const std::vector<LatticePath> &ranked_paths,
                                     CandidateSource source) -> std::optional<WordItem> {
        const LatticePath *path = first_distinct(ranked_paths);
        if (path == nullptr)
            return std::nullopt;
        already.insert(path->sentence);
        // Often negative (log-space). Ranking is insert order, not weight.
        const auto weight = static_cast<std::int64_t>(path->log_prob * 1000.0);
        WordItem item(typed_pinyin, path->sentence, weight, source, path->key);
        item.sentence_association = true;
        return item;
    };

    // Existing rows include Unigram, so the lattice row avoids it before the neural models choose
    // their rows. The lattice's own best path is shown as Colocation when octagram is on (single
    // injection: octagram only affects the decode, so paths.front() is its result), otherwise as
    // Generated. Selecting Collocation first means the shared sentence keeps that label and the
    // Generated row is deduplicated away.
    std::optional<WordItem> lattice_item;
    std::optional<WordItem> keyboard_item;
    std::optional<WordItem> desktop_item;
    if (options.include_collocation_best)
        lattice_item = select_distinct(paths, CandidateSource::Collocation);
    if (!lattice_item && options.include_lattice_best)
        lattice_item = select_distinct(paths, CandidateSource::Generated);
    const bool lattice_consensus = lattice_item && lattice_item->word == paths.front().sentence &&
                                   (source_prefers(keyboard_paths, paths.front().sentence) ||
                                    source_prefers(desktop_paths, paths.front().sentence));
    bool keyboard_before_desktop = false;
    if (keyboard_paths && desktop_paths)
    {
        // Compare each model's best row after excluding Unigram and Trigram, but before the neural
        // models exclude each other. Keyboard owns a shared best; otherwise desktop keeps its own
        // best and is displayed first.
        const LatticePath *keyboard_best = first_distinct(*keyboard_paths);
        const LatticePath *desktop_best = first_distinct(*desktop_paths);
        keyboard_before_desktop =
            keyboard_best != nullptr && desktop_best != nullptr && keyboard_best->sentence == desktop_best->sentence;
        if (keyboard_before_desktop)
        {
            keyboard_item = select_distinct(*keyboard_paths, CandidateSource::NeuralKeyboard);
            desktop_item = select_distinct(*desktop_paths, CandidateSource::NeuralDesktop);
        }
        else
        {
            desktop_item = select_distinct(*desktop_paths, CandidateSource::NeuralDesktop);
            keyboard_item = select_distinct(*keyboard_paths, CandidateSource::NeuralKeyboard);
        }
    }
    else if (keyboard_paths)
    {
        keyboard_item = select_distinct(*keyboard_paths, CandidateSource::NeuralKeyboard);
    }
    // When both neural modes are enabled, desktop waits for keyboard to finish. Otherwise desktop
    // could briefly claim keyboard's best distinct row and then visibly change on the next query.
    else if (desktop_paths && !keyboard_enabled)
        desktop_item = select_distinct(*desktop_paths, CandidateSource::NeuralDesktop);

    std::vector<WordItem> other_items;
    for (const auto &[ranked_paths, source] : other_ranked_sources)
    {
        if (auto item = select_distinct(ranked_paths, source))
            other_items.push_back(std::move(*item));
    }

    std::optional<WordItem> promoted_unigram;
    if (unigram_consensus)
    {
        promoted_unigram = std::move(*unigram_position);
        candidates.erase(unigram_position);
    }

    // Consensus from another source promotes Unigram first and the lattice row ahead of neural rows.
    // A shared neural best belongs to keyboard; when the neural best rows differ, desktop goes first.
    std::vector<WordItem> extra;
    if (promoted_unigram)
        extra.push_back(std::move(*promoted_unigram));
    if (lattice_consensus)
        extra.push_back(std::move(*lattice_item));
    if (keyboard_before_desktop)
    {
        if (keyboard_item)
            extra.push_back(std::move(*keyboard_item));
        if (desktop_item)
            extra.push_back(std::move(*desktop_item));
    }
    else
    {
        if (desktop_item)
            extra.push_back(std::move(*desktop_item));
        if (keyboard_item)
            extra.push_back(std::move(*keyboard_item));
    }
    extra.insert(extra.end(), std::make_move_iterator(other_items.begin()), std::make_move_iterator(other_items.end()));
    if (lattice_item && !lattice_consensus)
        extra.push_back(std::move(*lattice_item));
    if (extra.empty())
        return;

    const size_t insert_at = generated_sentence_insert_position(candidates, syllables);
    candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(insert_at), extra.begin(), extra.end());
}

void merge_lattice_candidates(std::vector<WordItem> &candidates, const Segments &syllables,
                              const WordLatticeLookup &lookup, const std::string &typed_pinyin,
                              const WordLatticeOptions &options, const LatticeReranker &rerank,
                              CandidateSource rerank_source)
{
    std::vector<SourcedLatticeReranker> rerankers;
    if (rerank)
        rerankers.push_back({rerank, rerank_source});
    merge_lattice_candidates(candidates, syllables, lookup, typed_pinyin, options, rerankers);
}

} // namespace quanpin
