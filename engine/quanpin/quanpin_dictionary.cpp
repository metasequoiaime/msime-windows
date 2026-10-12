#include "fuzzy_pinyin.h"
#include "quanpin_dictionary.h"
#include "../user_dictionary/user_dictionary_journal.h"

#include "../common/helpcode_utils.h"
#include "quanpin_query.h"
#include "quanpin_utils.h"
#include "../shuangpin/shuangpin_utils.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <fmt/format.h>
#include <unordered_map>
#include <unordered_set>
#include <utf8/cpp17.h>

namespace
{
constexpr size_t kSparsePinyinFallbackThreshold = 8;
constexpr size_t kSyllableGraphPathLimit = 32;
constexpr size_t kMaxSyllablesForMultipleSegmentations = 4;
constexpr int kAlternativeSegmentationCandidateLimit = 128;
constexpr size_t kBestAlternativeSegmentationMaxIndex = 1;
// 保护位只对「排在首页之外」的备选读音生效。默认 page_size 是 6，所以自然排序已经
// 进了前 6 的候选一律按权重原样呈现——用户调频写的就是权重，再钉一次等于把刚调出来
// 的名次覆盖掉。page_size 可配（3..9），这里取默认值：调大页长最多是少提升一两个
// 本来就看得见的词，不会把该提升的漏掉。
constexpr size_t kAlternativeSegmentationFirstPageSize = 6;
// 备选切分保护位的词频门槛：备选读音的最佳词权重达到主切分首位的 1/RATIO 以上
// 才配保护位。跨表权重不可直接比（单字是语料计数、词组是小尺度词权），这个量级
// 判断只负责把「真歧义」和「罕见重码」分开：xian -> 西安(55K) 对 先(1.66M)，
// 比值 3.3% 仍提升；xie -> 西鄂(6) 对 些(3.75M) 被拒，jiang -> 激昂(23.7K) 对
// 将(2.63M) 同样被拒。
constexpr std::int64_t kAlternativeSegmentationPromotionRatio = 100;

// 同档纠错读法的上下文消解（阶段 1 design）：同档备选切分各自过一遍完整管线，
// 跨切分比较词格路径分（log10×1000，同档等罚，分差即纯上下文差）。胜出切分边际
// 达标时接管「主切」角色领衔候选；不达标或词格不可用时与现状逐位一致。
// 解码预算：只解通过静态门槛（上面 1/100 同款）且词库有行的备选，上限 4 个——
// uanli 这类掉声母输入有 10-15 个等权目标，全解会伤 p95。
constexpr size_t kAutocorrectContextDecodeLimit = 4;
// 贵档读法另给的解码预算：它们不要求整词、不过静态门槛，只按 k-best 次序取前几条。
constexpr size_t kCostlierContextDecodeLimit = 2;
// 纠错表权重换算成手误代价（路径分单位）：换位 10 -> 2.0（约 1% 的手误率），漏字 11 ->
// 2.2，多字 12 -> 2.4，邻键 13 -> 2.6，生成式 15 -> 3.0。只有跨档比较时它才起作用：
// 同档读法权重相同，代价相互抵消。ponytail: 线性换算；校准路径 = user_journal 的纠错
// 采纳事件（方向阶段 3）。
constexpr double kAutocorrectWeightToLog = 0.2;
// 接管边际：以打分器自身的对数单位计——词格 LM 打分是 log10（1.0 ≈ 10× 句概率），
// 无 LM 的启发式回退是 ln（1.0 ≈ e≈2.7×）。对标 CN105045778B 的 β 型先验比率边际
// （同音校对 β=0.01 → 2.0），手误场景放宽一档。ponytail: 朴素常数；校准路径 =
// user_journal 挖混淆先验（方向阶段 3）。
constexpr double kAutocorrectContextMarginLog = 1.0;

bool is_alpha_vk(ImeKeyCode vk)
{
    return vk >= 'A' && vk <= 'Z';
}

std::string remove_delimiters(const std::string &segmented)
{
    std::string normalized = segmented;
    normalized.erase(std::remove(normalized.begin(), normalized.end(), '\''), normalized.end());
    return normalized;
}

// 正字法别名归一：把非标准 ü 拼写改写成词库标准键，输出即进入查询管线的权威拼写
// （查库键、pinyin_segmentation_、调频/造词键因此全部落在标准拼法上）。标准拼音约定：
// ü 保留鱼眼的音节（n/l 系）键写 v（nv/nve/lv/lve），省略鱼眼的（j/q/x/y 系）键写 u
// （ju/jue/qu/.../yue）。nu/lu 是真实音节（怒/路），所以 n/l 系只按段精确匹配 nue/lue，
// 绝不改写 nv/lv/nu/lu。
quanpin::Segments normalize_umlaut_aliases(quanpin::Segments segments)
{
    for (auto &segment : segments)
    {
        if (segment.size() == 3 && segment[1] == 'u' && segment[2] == 'e' && (segment[0] == 'n' || segment[0] == 'l'))
        {
            // nue/lue → nve/lve：词库正键用 v，混拼 u 是它的别名。
            segment[1] = 'v';
        }
        else if (segment.size() >= 2 && segment[1] == 'v' &&
                 (segment[0] == 'j' || segment[0] == 'q' || segment[0] == 'x' || segment[0] == 'y'))
        {
            // j/q/x/y 系没有保留鱼眼的合法音节，v 一律是 u 的别名。
            segment[1] = 'u';
        }
    }
    return segments;
}

std::string series_cache_key(const std::string &raw_input, const std::string &segmentation)
{
    const char *prefix = raw_input.find('\'') == std::string::npos ? "A:" : "M:";
    return prefix + (segmentation.empty() ? raw_input : segmentation);
}

// Folds letters for autocorrect comparisons: lowercases and strips manual
// delimiters. v and u must compare as distinct letters — the ü-style alias
// rewrite is a first-class marking source (the trailing star badge), so
// 'nve' (primary segmentation) differing from typed 'nue' is exactly what
// produces the corrected_from label. The old contract ("aliases never look
// rewritten, hence never marked") was flipped by product decision.
std::string fold_autocorrect_letters(const std::string &text)
{
    std::string folded;
    folded.reserve(text.size());
    for (const char ch : text)
    {
        if (ch == '\'')
        {
            continue;
        }
        folded.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return folded;
}

// How many ranked correction cuts feed the query pipeline: the primary cut
// becomes the query key and the rest ride along as alternative segmentations
// for query-time disambiguation (patent CN 101133411 B M3). k=9 is set by the
// phase-4/5 evaluation data: dropped-initial keys such as uan have 10-15
// equally-weighted deletion targets (cuan/duan/guan/.../zuan), and k=3 cut the
// list by table order before reaching the correct reading (quan'li for uanli).
// k=9 lifts R@1/R@3 across the deletion and mixed models with no p95 change.
constexpr std::size_t kAutocorrectCutKBest = 9;
// 合法输入上的纠错读法个数上限：每条都要在按键路径上多解一次词格。
constexpr std::size_t kLegalCorrectionCutLimit = 3;
// 合法输入上每处纠正的手误代价，与词格路径分同单位（加载 sc.lm 时是 log10：2.0 =
// 纠错读法的句概率要高出 100 倍才抵得过一处纠正，约等于把手误率估成 1%）。改变字母
// 数的纠正（多字、漏字）贵一档：合法输入里第二个音节是 e/a/er/an 的真词很多（邪恶
// xie'e、答案 da'an、其二 qi'er），按多一个字母读都能拼出另一个读音；同价时词库扫描
// 第 2 位的纠错从 6 个涨到 34 个，jioa 也被读成多了个 o 的「家」而不是换位的「叫」。
// 3.0 让它们只在语言模型分不低于原读法时才露面。与纠错表权重（换位 10 < 漏字 11 <
// 多字 12）同序。ponytail: 朴素常数；校准路径 = user_journal 的纠错采纳事件（第 2 期）。
constexpr double kLegalInputTranspositionPenaltyLog = 2.0;
constexpr double kLegalInputLengthChangePenaltyLog = 3.0;
// 原读法领衔时，纠错读法离它在这个分差以内才出现在第 2 位；更远的不出现。没有它，
// 每个碰巧能纠成别的音节的输入都会在第 2 位挂一个语言模型都不认的读法。
constexpr double kLegalInputRunnerUpGapLog = 3.0;
// 合法输入的等音节数字面切分（ban'an / ba'nan）：最多枚举几条、最多多解几条词格。
// 每多解一条，按键路径上就多一次词格解码；绝大多数输入只有一两处歧义边界。
constexpr size_t kLiteralSegmentationEnumerateLimit = 8;
constexpr size_t kLiteralSegmentationDecodeLimit = 3;

quanpin::Segments cut_syllables(const quanpin::AutocorrectCut &cut)
{
    quanpin::Segments syllables;
    syllables.reserve(cut.segments.size());
    for (const auto &segment : cut.segments)
    {
        syllables.push_back(segment.syllable);
    }
    return syllables;
}

SeriesQueryResolution resolve_series_query(const std::string &raw_input, const quanpin::Segments &segments,
                                           unsigned autocorrect_types)
{
    SeriesQueryResolution result;
    // Guard order matters: the jianpin-shape predicate runs before autocorrect_cut
    // so a guarded input never pays for the BFS. Both guards express "the user did
    // not mistype" and either one disables the rewrite entirely. The base
    // segmentation is deliberately NOT part of the gate: the BFS works on the raw
    // letters, so an empty base cut (nothing segmentable, e.g. a dropped initial
    // such as uanli for quan'li) is often exactly the input that needs correction.
    // With no correction reading the search returns nothing and the plain path is
    // kept unchanged.
    const bool eligible = autocorrect_types != 0 && raw_input.find('\'') == std::string::npos &&
                          !quanpin::has_only_complete_pinyin_segments(segments) &&
                          !quanpin::looks_like_jianpin_spelling(raw_input);
    if (eligible)
    {
        // Fill corrected_segments and the two alternative tiers from cost-ranked
        // cuts, optionally appending a trailing jianpin segment (used by the
        // head-correction + jianpin-tail composition below). cuts are ranked
        // cheapest-first (edge count, then weight): the primary defines the cost
        // tier, same-cost readings frequency-compete with it, costlier readings
        // stay behind it (see the two vectors' contracts in the header).
        const auto populate_from_cuts = [&](const std::vector<quanpin::AutocorrectCut> &cuts,
                                            const std::string &jianpin_tail) {
            const auto to_segments = [&](const quanpin::AutocorrectCut &cut) {
                quanpin::Segments segs = cut_syllables(cut);
                if (!jianpin_tail.empty())
                {
                    segs.push_back(jianpin_tail);
                }
                return segs;
            };
            const auto &primary_cut = cuts.front();
            result.corrected_segments = to_segments(primary_cut);
            result.corrected_weight = primary_cut.weight;
            for (std::size_t i = 1; i < cuts.size(); ++i)
            {
                if (cuts[i].same_cost_as(primary_cut))
                {
                    result.alternative_corrected_cuts.push_back(to_segments(cuts[i]));
                }
                else
                {
                    result.costlier_corrected_cuts.push_back(to_segments(cuts[i]));
                    result.costlier_weights.push_back(cuts[i].weight);
                    result.costlier_promotable.push_back(
                        std::all_of(cuts[i].segments.begin(), cuts[i].segments.end(), [](const auto &segment) {
                            return segment.weight <= quanpin::kAutocorrectInsertionWeight;
                        }));
                }
            }
        };

        // 静态优先语义由 k-best 搜索自身保证（见 quanpin_utils.h 契约）：最优
        // 切分为纯静态时生成切分已在搜索内被丢弃，下面的 head+简拼尾组合路径
        // 与 k=1 预编辑投影等调用方同享该保证；生成读法只在静态空间完全无法
        // 解释输入时出场。混表竞争的加权仲裁依赖训练权重，留给方向阶段 3。
        const auto cuts = quanpin::autocorrect_cut_kbest(raw_input, autocorrect_types, kAutocorrectCutKBest);
        if (!cuts.empty())
        {
            result.corrected_input = true;
            populate_from_cuts(cuts, "");
        }
        else
        {
            // Compose a correction with a trailing jianpin tail. The k-best search
            // only reaches the end when every segment is a complete syllable, so an
            // input like "hauzh" (hau typo + zh jianpin) or "hauz" fails outright.
            // Retry on the head with the trailing incomplete-syllable prefix removed;
            // query_series then handles the tail exactly as it does for the correctly
            // spelled "huazh" (hua + zh). Shortest tail first, so the correction
            // explains as much of the input as possible.
            //
            // Neighbor corrections are excluded from the head: they carry the widest
            // false-positive surface (largest table), and this path already relaxes
            // the "whole input is a legal cut" constraint by trusting a speculative
            // jianpin boundary. Stacking the two turns deletion-shaped input into
            // noise (e.g. "shng" -> "sun" + "g" -> 笋干). Structural typos
            // (transposition / deletion / insertion) are confident enough to compose.
            const unsigned head_types = autocorrect_types & ~quanpin::kAutocorrectNeighbor;
            const auto &prefixes = quanpin::prefix_pinyin_set();
            const auto &intact = quanpin::intact_pinyin_set();
            for (std::size_t tail_len = 1; head_types != 0 && tail_len <= 2 && tail_len < raw_input.size(); ++tail_len)
            {
                const std::string tail = raw_input.substr(raw_input.size() - tail_len);
                // A jianpin tail is an incomplete syllable: a valid pinyin prefix
                // that is not itself a complete syllable (e.g. "zh", "z", "h").
                if (prefixes.count(tail) == 0 || intact.count(tail) != 0)
                {
                    continue;
                }
                const std::string head = raw_input.substr(0, raw_input.size() - tail_len);
                const auto head_cuts = quanpin::autocorrect_cut_kbest(head, head_types, kAutocorrectCutKBest);
                if (head_cuts.empty())
                {
                    continue;
                }
                result.corrected_input = true;
                populate_from_cuts(head_cuts, tail);
                break;
            }
        }
    }
    else if (autocorrect_types != 0)
    {
        // 合法输入：上面的闸把它整个挡在纠错外。这里只列出纠错读法，主切不动，
        // 取舍在 arbitrate_legal_corrections 按整句打分。
        result.legal_corrected_cuts =
            quanpin::legal_input_correction_cuts(raw_input, segments, autocorrect_types, kLegalCorrectionCutLimit);
    }
    // Both branches rebuild the segmentation string from segments: they already
    // carry the canonical (alias-normalised) spelling, while a caller-passed
    // explicit string could keep the alias spelling — the query would succeed
    // yet nothing would look rewritten, so alias inputs with manual delimiters
    // ("nue'hao") could never get marked. Delimiters sit on syllable boundaries,
    // so split/join round-trips them unchanged ("nu'e" stays "nu'e").
    result.segmentation = result.corrected_input ? quanpin::join_segments(result.corrected_segments)
                                                 : (segments.empty() ? raw_input : quanpin::join_segments(segments));
    // 合法输入带纠错读法时候选列表也变了，同样要另占缓存槽：关掉纠错后不能读到它。
    const char *cache_prefix = result.corrected_input ? "C:" : (result.legal_corrected_cuts.empty() ? "" : "L:");
    result.cache_key = cache_prefix + series_cache_key(raw_input, result.segmentation);
    // The literal fallback depends on typed letters, even when two typos share a correction key.
    if (result.corrected_input)
        result.cache_key += '\x1f' + raw_input;
    return result;
}

// 输入能切成词库里的真词时，首位留给这个字面整词：纠错与改写字母的别名读法
// （nia 的 nai「奶」、yongan 复制辅音读成 yong'gan「勇敢」）最多排第 2。字面整词
// = 词库行、读音字母与敲的字母逐个相同；同一串字母的另一种切分（fangan 的方案/
// 反感、xian 的先/西安）字母没变，照旧按权重竞争，不受这里约束。字面切分只拼得出
// 单字或整句时（zi'a'zhe'li'ya、ji'o'a）没有字面整词，纠错照常可以领衔。
std::string candidate_letters(const WordItem &item)
{
    return fold_autocorrect_letters(item.canonical_pinyin.empty() ? item.pinyin : item.canonical_pinyin);
}

// 字面整词：词库行、读音字母与敲的字母逐个相同（任一种切分都算）。
std::vector<WordItem>::iterator find_literal_whole_word(std::vector<WordItem> &candidates, const std::string &raw_input)
{
    const std::string typed = fold_autocorrect_letters(raw_input);
    return std::find_if(candidates.begin(), candidates.end(), [&](const WordItem &item) {
        return (item.source == CandidateSource::Database || item.source == CandidateSource::UserDatabase) &&
               candidate_letters(item) == typed;
    });
}

void keep_literal_whole_word_first(std::vector<WordItem> &candidates, const std::string &raw_input)
{
    if (candidates.empty() || candidate_letters(candidates.front()) == fold_autocorrect_letters(raw_input))
    {
        return;
    }
    const auto literal = find_literal_whole_word(candidates, raw_input);
    if (literal != candidates.end())
    {
        std::rotate(candidates.begin(), literal, literal + 1);
    }
}

unsigned literal_mixed_match(const WordItem &item, const std::string &raw_input)
{
    return item.source == CandidateSource::Database || item.source == CandidateSource::UserDatabase
               ? quanpin::literal_mixed_pinyin_match(raw_input, item.pinyin, item.canonical_pinyin)
               : 0;
}

bool literal_jianpin_match(const WordItem &item, const std::string &raw_input)
{
    return (item.source == CandidateSource::Database || item.source == CandidateSource::UserDatabase) &&
           quanpin::literal_jianpin_pinyin_match(raw_input, item.pinyin, item.canonical_pinyin);
}

// 读音就是 key 这条切分的行（整键块）。整句行按敲的读音（pinyin）认：词格整句的
// canonical_pinyin 是它实际走的读音，开着模糊音时可能与切分不同（min 走成 ming）。
bool reads_full_key(const WordItem &item, const std::string &key)
{
    return item.canonical_pinyin == key || (item.sentence_association && item.pinyin == key);
}

void keep_literal_mixed_words_first(std::vector<WordItem> &candidates, const std::string &raw_input)
{
    // Stable groups preserve dictionary/user order without manufacturing frequency weights.
    const auto whole_end = std::stable_partition(candidates.begin(), candidates.end(), [&](const WordItem &item) {
        return literal_mixed_match(item, raw_input) == 1;
    });
    std::stable_partition(whole_end, candidates.end(),
                          [&](const WordItem &item) { return literal_mixed_match(item, raw_input) == 2; });
}

std::string escape_sql_text(std::string text)
{
    size_t pos = 0;
    while ((pos = text.find('\'', pos)) != std::string::npos)
    {
        text.insert(pos, 1, '\'');
        pos += 2;
    }
    return text;
}

} // namespace

QuanpinDictionary::QuanpinDictionary(std::string db_path, metasequoia::RuntimePaths paths)
    : cache_(128), series_cache_(128), segmentation_cache_(128), prefix_miss_cache_(128), resolution_cache_(128),
      lattice_best_path_cache_(64), paths_(std::move(paths)),
      decoder_(paths_.resource(metasequoia::assets::pinyin_model),
               paths_.user(metasequoia::assets::pinyin_user_dictionary)),
      language_model_(&ngram::shared_language_model(paths_.resource(metasequoia::assets::language_model))),
      neural_keyboard_model_(metasequoia::path_to_utf8(paths_.resource(metasequoia::assets::neural_model_keyboard))),
      db_path_(db_path.empty() ? metasequoia::path_to_utf8(paths_.dictionary(metasequoia::assets::main_dictionary))
                               : std::move(db_path))
{

    const int exit = sqlite3_open(db_path_.c_str(), &db_);
    if (exit != SQLITE_OK)
    {
        (void)0;
    }
    else
    {
        // A learning write (or the settings app) holds the write lock for a few
        // milliseconds at commit; without a busy timeout a query landing in that
        // window fails with SQLITE_BUSY and the page comes back empty.
        sqlite3_busy_timeout(db_, quanpin::kDictionaryBusyTimeoutMs);
    }

    quanpin::warm_up(db_, statement_cache_);
    reset_cache_if_database_changed();
}

QuanpinDictionary::~QuanpinDictionary()
{
    for (auto &[sql, stmt] : statement_cache_)
    {
        if (stmt != nullptr)
        {
            sqlite3_finalize(stmt);
        }
    }
    if (db_ != nullptr)
    {
        sqlite3_close(db_);
    }
}

std::vector<WordItem> QuanpinDictionary::query_exact(const std::string &raw_input, const std::string &segmentation,
                                                     unsigned autocorrect_types)
{
    if (raw_input.empty())
    {
        current_candidate_list_.clear();
        return {};
    }

    pinyin_sequence_ = raw_input;
    const auto segments =
        resolve_segments(raw_input, segmentation, quanpin::correction_aliases_enabled(autocorrect_types));

    // Typing autocorrection: when the spelling is not a legal pinyin
    // combination (the correction cut already fell back to greedy), try to
    // rewrite the whole string into legal syllables. The corrected
    // segmentation becomes the primary key so that selection and weight
    // updates land on the right dictionary entries, and the original
    // (garbage-leaning) candidates stay behind as a fallback tail.
    // Memoize the k-best correction search: it is the expensive part of every
    // keystroke, yet a pure function of this input tuple, so a re-typed or
    // backspaced prefix should reuse it instead of re-running the k=9 beam.
    const std::string resolution_key = std::to_string(autocorrect_types) + '\x1f' + raw_input + '\x1f' + segmentation;
    SeriesQueryResolution resolution;
    if (const auto *cached_resolution = resolution_cache_.find(resolution_key))
    {
        resolution = *cached_resolution;
    }
    else
    {
        resolution = resolve_series_query(raw_input, segments, autocorrect_types);
        resolution_cache_.insert(resolution_key, resolution);
    }
    pinyin_segmentation_ = resolution.segmentation;
    // The alternative readings must be published even on the cache-hit path:
    // mark_autocorrect_candidates runs after every query, cached or not.
    pinyin_alternative_segmentations_.clear();
    for (const auto &alternative : resolution.alternative_corrected_cuts)
    {
        pinyin_alternative_segmentations_.push_back(quanpin::join_segments(alternative));
    }
    // Costlier readings are still corrections, so they must be marked as such
    // even though they rank below the primary tier (see the tail append below).
    for (const auto &alternative : resolution.costlier_corrected_cuts)
    {
        pinyin_alternative_segmentations_.push_back(quanpin::join_segments(alternative));
    }
    // 合法输入上的纠错读法不进这张表：它们多半会被裁决丢掉，而标记按读音字母匹配，
    // 丢掉的读法会把原读法里字母恰好相同的前缀单字误标（xiee 的 xie 读法让 写/些/血
    // 全带上纠错星标）。真正用上的那条在 arbitrate_legal_corrections 里直接打标。

    // Autocorrected results get their own cache slot so they never leak the
    // fallback tail into plain (correct) spellings sharing the same key.
    if (series_cache_.find(resolution.cache_key))
    {
        reset_cache_if_database_changed();
        if (const auto *cached = series_cache_.find(resolution.cache_key))
        {
            current_candidate_list_ = *cached;
            return current_candidate_list_;
        }
    }

    std::vector<quanpin::Segments> alternative_segmentations;
    std::unordered_set<std::string> seen_segmentations = {pinyin_segmentation_};
    // Keep costlier readings out of the frequency-competing merge tier; they are
    // appended after it below so dictionary frequency never lifts a dearer
    // correction above the cheaper primary. Seeding "seen" here also blocks the
    // greedy correction paths from re-introducing them into the merge tier.
    for (const auto &costlier : resolution.costlier_corrected_cuts)
    {
        seen_segmentations.insert(quanpin::join_segments(costlier));
    }
    const auto append_alternative = [&](const quanpin::Segments &candidate) {
        const std::string key = quanpin::join_segments(candidate);
        if (!key.empty() && seen_segmentations.insert(key).second &&
            alternative_segmentations.size() < kSyllableGraphPathLimit)
        {
            alternative_segmentations.push_back(candidate);
        }
    };

    // Correction-mode segmentation is part of autocorrection and must follow the
    // same switch. query() defaults autocorrect_types to 0, so running this
    // unconditionally changed the default behaviour for every caller: a misspelled
    // input such as "sahng" started offering the corrected candidate even with
    // autocorrection explicitly turned off.
    if (autocorrect_types != 0)
    {
        // Correction alternatives first: they explain the letters the user
        // actually typed, so they outrank the phonetic-shape alias readings that
        // follow (the alias layer rewrites legal-looking spellings regardless).
        for (const auto &candidate : resolution.alternative_corrected_cuts)
        {
            append_alternative(candidate);
        }
        const auto correction_paths = quanpin::cut_pinyin_by_mode(
            raw_input, "correction", quanpin::correction_aliases_enabled(autocorrect_types));
        for (const auto &candidate : correction_paths)
        {
            append_alternative(candidate);
        }
    }

    if (raw_input.find('\'') == std::string::npos && segments.size() <= kMaxSyllablesForMultipleSegmentations &&
        quanpin::has_only_complete_pinyin_segments(segments))
    {
        const auto complete_paths = quanpin::enumerate_complete_segmentations(quanpin::build_syllable_graph(raw_input),
                                                                              kSyllableGraphPathLimit);
        for (const auto &candidate : complete_paths)
        {
            append_alternative(candidate);
        }
    }

    std::vector<WordItem> result;
    std::vector<WordItem> literal_mixed;
    if (resolution.corrected_input)
    {
        result = query_series(raw_input, pinyin_segmentation_, resolution.corrected_segments);
        const std::string fallback_segmentation =
            segmentation.empty() ? quanpin::join_segments(segments) : segmentation;
        const auto literal = query_series(raw_input, fallback_segmentation, segments);
        // 纯简拼字面整词与混合简拼同样领衔：全辅音串照常进纠错（形状分不出 nbn 与漏了元音
        // 的 bng），但词库里真有这个简拼读法时（nbn「能不能」、bqng「并且能够」），纠错读法
        // 只排在它后面，仍可选、仍带纠错标记。
        for (const auto &item : literal)
        {
            if (literal_mixed_match(item, raw_input) || literal_jianpin_match(item, raw_input))
                literal_mixed.push_back(item);
        }
        append_unique_words(result, literal);

        // 上下文消解（阶段 1）：任一整句来源开着时直接解出主切、同档备选与前几条贵档
        // 读法的最佳路径分，扣掉各自的手误代价后裁决，静态词频只承担同档的解码前置过滤。
        // 门控看 any_sentence_source 而不是 word_lattice：只开万象重排（不显示 Trigram 整句）
        // 时词格照样在内部解码，shiideya 曾因此退回静态排序、让「是爹呀」领衔。
        // 不能从候选列表读分——
        // merge_lattice_candidates 会把与已有候选同词的生成行去重掉（already 集），
        // 最优路径通常恰好就是词库首行，列表里根本看不到它。整句来源全关或边际不足
        // 时，下面的一切与改造前逐位一致（降级矩阵见任务 design.md）。
        bool context_reordered = false;
        if (sentence_association_.any_sentence_source() &&
            (!resolution.alternative_corrected_cuts.empty() || !resolution.costlier_corrected_cuts.empty()) &&
            resolution.corrected_segments.size() >= 2 &&
            quanpin::has_only_complete_pinyin_segments(resolution.corrected_segments))
        {
            const auto best_path_score = [this](const quanpin::Segments &cut) -> std::optional<double> {
                if (cut.size() < 2)
                {
                    return std::nullopt;
                }
                return lattice_best_path_score(cut);
            };
            const auto primary_score = best_path_score(resolution.corrected_segments);
            if (primary_score.has_value())
            {
                // 前置过滤沿用保护位同款 1/100 量级：今天就赢不了晋升的备选不付解码钱。
                // 基准必须与 merge_alternative_segmentations 的 primary_top_weight 同源，
                // 即取正读**整键**命中的首位权重、没有整键行时取 0。不能取 result.front()：
                // result 是 query_series 的完整输出，头部可能是高权重前缀单字（真实词库里
                // 单字是 1e6+ 的语料计数，见 heuristic_log_prob）或生成的整句。正读两音节
                // 在词库里没有整词、词格只能用单字拼出路径时，那正是重排价值最高的场景，拿单字
                // 当基准会在解码之前就把正常备选全筛掉，重排悄悄不生效。同参调用
                // query_single_path 走缓存，不额外查库。
                const std::string primary_key = quanpin::join_segments(resolution.corrected_segments);
                const auto primary_full = query_single_path(raw_input, primary_key, resolution.corrected_segments);
                const std::int64_t primary_top_weight = primary_full.empty() ? 0 : primary_full.front().weight;
                const auto alternative_items = quanpin::query_exact_segmentations_keyed_flat(
                    resolution.alternative_corrected_cuts, db_, statement_cache_,
                    kAlternativeSegmentationCandidateLimit);
                // 每个备选切分的最佳词权重：行按权重降序返回，emplace 只留首行。
                std::unordered_map<std::string, std::int64_t> best_weight_by_key;
                best_weight_by_key.reserve(alternative_items.size());
                for (const auto &item : alternative_items)
                {
                    best_weight_by_key.emplace(item.key, item.weight);
                }
                struct DecodeCandidate
                {
                    std::int64_t top_weight = 0;
                    quanpin::Segments cut;
                    double penalty = 0.0;
                };
                const double primary_penalty = kAutocorrectWeightToLog * resolution.corrected_weight;
                std::vector<DecodeCandidate> decode_pool;
                for (const auto &cut : resolution.alternative_corrected_cuts)
                {
                    const auto found = best_weight_by_key.find(quanpin::join_segments(cut));
                    if (found == best_weight_by_key.end() ||
                        found->second * kAlternativeSegmentationPromotionRatio < primary_top_weight)
                    {
                        continue;
                    }
                    decode_pool.push_back({found->second, cut, primary_penalty});
                }
                std::stable_sort(decode_pool.begin(), decode_pool.end(),
                                 [](const auto &lhs, const auto &rhs) { return lhs.top_weight > rhs.top_weight; });
                if (decode_pool.size() > kAutocorrectContextDecodeLimit)
                {
                    decode_pool.resize(kAutocorrectContextDecodeLimit);
                }
                // 贵档读法按 k-best 次序再解几条，不要求词库有整词：shiideya 的多字读法
                // shi'de'ya 拼不出整词，却是比换位读法 shi'die'ya（是爹呀）通顺得多的句子。
                // 它们多付的手误代价按纠错表权重差计入总分。只收结构性手误的读法，见
                // SeriesQueryResolution::costlier_promotable。
                size_t costlier_decoded = 0;
                for (size_t i = 0;
                     i < resolution.costlier_corrected_cuts.size() && costlier_decoded < kCostlierContextDecodeLimit;
                     ++i)
                {
                    if (!resolution.costlier_promotable[i])
                    {
                        continue;
                    }
                    ++costlier_decoded;
                    const auto &cut = resolution.costlier_corrected_cuts[i];
                    const auto found = best_weight_by_key.find(quanpin::join_segments(cut));
                    decode_pool.push_back({found == best_weight_by_key.end() ? 0 : found->second, cut,
                                           kAutocorrectWeightToLog * resolution.costlier_weights[i]});
                }
                // 第一遍只解路径分选胜者，只为胜出切分付完整管线的钱。总分 = 路径分 - 手误代价，
                // 同档读法代价相同，比的仍是纯上下文差。
                std::optional<double> winner_total;
                std::int64_t winner_top_weight = 0;
                quanpin::Segments winner_segments;
                for (const auto &candidate : decode_pool)
                {
                    const auto cut_score = best_path_score(candidate.cut);
                    if (!cut_score.has_value())
                    {
                        continue;
                    }
                    const double total = *cut_score - candidate.penalty;
                    if (!winner_total.has_value() || total > *winner_total)
                    {
                        winner_total = total;
                        winner_top_weight = candidate.top_weight;
                        winner_segments = candidate.cut;
                    }
                }
                // 用户在主切读音上的选择优先于上下文。调频只改词库权重，而加载 LM 时
                // 路径分里的权重只剩 dictionary_tiebreak（≤0.07）和 reading_prior，翻不过
                // 1.0 的边际：不拦的话，用户每次都选班长，办证系照样每次领衔。调频把选中词
                // 抬到它越过的词之上，所以「主切整键首位权重不低于胜出切分首位」正是用户已经
                // 把主切读音选到前面的样子；再要求主切键有用户 upsert，把这条限制在用户
                // 表达过偏好的键上，出货词频本身仍交给上下文裁决。不接管时走下面的静态
                // 合并，它按权重排序，调频照常生效。只在边际达标后才查日志，不给每次按键加查询。
                const auto user_prefers_primary = [&] {
                    return primary_top_weight >= winner_top_weight && user_prefers_reading(primary_key);
                };
                if (winner_total.has_value() &&
                    *winner_total - (*primary_score - primary_penalty) >= kAutocorrectContextMarginLog &&
                    !user_prefers_primary())
                {
                    // 边际达标：胜出切分领衔。merge_alternative_segmentations 假定传入
                    // result 的头部是主切词库行，所以这里仍以主切为锚组装其余列表（从
                    // 合并池摘掉胜出切分），再把胜出列表接到最前。胜出列表占满首页时，
                    // 原保护位钉的名次随整体后移——首页本就该是胜出读音的词。
                    std::vector<quanpin::Segments> remaining_alternatives;
                    remaining_alternatives.reserve(alternative_segmentations.size());
                    const std::string winner_key = quanpin::join_segments(winner_segments);
                    for (const auto &alternative : alternative_segmentations)
                    {
                        if (quanpin::join_segments(alternative) != winner_key)
                        {
                            remaining_alternatives.push_back(alternative);
                        }
                    }
                    std::vector<WordItem> rest =
                        merge_alternative_segmentations(raw_input, pinyin_segmentation_, resolution.corrected_segments,
                                                        remaining_alternatives, std::move(result));
                    // query_series 的 count 从整键递减到 1，返回的不只是胜出切分的整键行，还有它
                    // 首音节的全部单字。整句与词格行跟整键行同键——append_ime_fallback 用完整
                    // segmentation 填 canonical_pinyin，词格 select_distinct 用 path->key——所以
                    // 按 canonical_pinyin == winner_key 能干净地把前缀行摘出去。
                    //
                    // 同档胜者只能出自用 query_exact_segmentations_keyed_flat 收精确键的那段池，
                    // 整键档走 query_single_cut_keyed 的精确键分支，canonical 就是 winner_key。
                    // 贵档胜者可能没有整词（shi'de'ya），整键档会退到前缀区间扫描，那种行的 key
                    // 是别的读音，按这个判据沉到前缀块——它们本来就不是用户敲的读音，沉下去正好；
                    // 整句行仍带 winner_key，照样领衔。
                    //
                    // 不切开会破坏 merge_alternative_segmentations 的既有分层（merged_full 在前、
                    // 前缀作为 remaining 追加）：query_series(ban'zheng) 先出 办证/辩证/整句，再出
                    // 班/办/半/般…，而班长、搬账这些同键位整词在 rest 里、排在单字之后，整组被挤
                    // 出首页。这里恢复同一分层：整键块、rest、前缀块。
                    std::vector<WordItem> winner_full;
                    std::vector<WordItem> winner_prefix;
                    for (auto &item : query_series(raw_input, winner_key, winner_segments))
                    {
                        (reads_full_key(item, winner_key) ? winner_full : winner_prefix).push_back(std::move(item));
                    }
                    // 贵档胜者没有整词、也没有整句行时（整句来源只开了重排、模型没载入），
                    // 整键块是空的；不能让主切的行顶回首位，由胜者的最长前缀词领衔。
                    if (winner_full.empty())
                    {
                        winner_full.swap(winner_prefix);
                    }
                    result = std::move(winner_full);
                    append_unique_words(result, std::move(rest));
                    append_unique_words(result, std::move(winner_prefix));
                    context_reordered = true;
                }
            }
        }

        // Query-time disambiguation (patent M3): the alternative readings of the
        // same typo compete with the primary cut under dictionary word
        // frequency; the best one keeps a protected slot near the top.
        if (!context_reordered && !alternative_segmentations.empty())
        {
            result = merge_alternative_segmentations(raw_input, pinyin_segmentation_, resolution.corrected_segments,
                                                     alternative_segmentations, std::move(result));
        }
        // Costlier readings sit below the whole primary cost tier: append them
        // last so a high-frequency dearer correction (gau -> gai, weight 13)
        // can never precede the cheaper one (gau -> gua, weight 10) on
        // dictionary frequency alone. Only the context rerank above may lift one,
        // and only after paying its larger typo cost.
        for (const auto &costlier : resolution.costlier_corrected_cuts)
        {
            append_unique_words(result, query_series(raw_input, quanpin::join_segments(costlier), costlier));
        }
    }
    else
    {
        result = query_series(raw_input, pinyin_segmentation_, segments);
        for (const auto &item : result)
        {
            if (literal_mixed_match(item, raw_input))
                literal_mixed.push_back(item);
        }
        if (!alternative_segmentations.empty())
        {
            result = merge_alternative_segmentations(raw_input, pinyin_segmentation_, segments,
                                                     alternative_segmentations, std::move(result));
        }
        // 字面切分先争出领衔，纠错读法再与领衔的那条比：纠错胜出时第 2 位是得分最高的字面读法。
        quanpin::Segments leading_segments;
        result = arbitrate_literal_segmentations(raw_input, segments, std::move(result), leading_segments);
        if (!resolution.legal_corrected_cuts.empty())
        {
            result = arbitrate_legal_corrections(raw_input, leading_segments, resolution.legal_corrected_cuts,
                                                 std::move(result));
        }
        keep_literal_whole_word_first(result, raw_input);
    }
    if (!literal_mixed.empty())
    {
        // Merge after reranking, with original metadata and frequency order winning same-word deduplication.
        keep_literal_mixed_words_first(literal_mixed, raw_input);
        append_unique_words(literal_mixed, result);
        result = std::move(literal_mixed);
    }
    series_cache_.insert(resolution.cache_key, result);
    current_candidate_list_ = result;
    return current_candidate_list_;
}

std::optional<WordItem> QuanpinDictionary::find_candidate(const std::string &key, const std::string &value)
{
    const std::string table = quanpin::build_table_name(quanpin::split_segments(key));
    if (!db_ || table.empty())
        return std::nullopt;
    sqlite3_stmt *stmt = nullptr;
    const std::string sql = "SELECT weight FROM \"" + table + "\" WHERE key=?1 AND value=?2 LIMIT 1";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, sqlite3_finalize);
    if (sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_ROW)
        return std::nullopt;
    return WordItem(key, value, sqlite3_column_int64(stmt, 0), CandidateSource::Database, key);
}

bool QuanpinDictionary::expand_initial_candidates(const std::string &code, std::vector<WordItem> &candidates)
{
    if (code.size() != 1)
    {
        return false;
    }

    const auto is_limited_initial = [&](const WordItem &item) {
        return item.source == CandidateSource::Database && item.pinyin == code;
    };
    const size_t limited_count =
        static_cast<size_t>(std::count_if(candidates.begin(), candidates.end(), is_limited_initial));
    constexpr size_t kInitialCandidateLimit = 24;
    if (limited_count != kInitialCandidateLimit)
    {
        return false;
    }

    auto expanded = query_initial(code, INT_MAX);
    if (expanded.size() <= limited_count)
    {
        return false;
    }
    for (auto &item : expanded)
    {
        item.canonical_pinyin = item.pinyin;
        item.pinyin = code;
    }

    std::vector<WordItem> merged;
    merged.reserve(candidates.size() - limited_count + expanded.size());
    bool inserted = false;
    for (auto &item : candidates)
    {
        if (is_limited_initial(item))
        {
            if (!inserted)
            {
                merged.insert(merged.end(), expanded.begin(), expanded.end());
                inserted = true;
            }
            continue;
        }
        merged.push_back(std::move(item));
    }

    candidates = std::move(merged);
    cache_.insert(code, expanded);
    series_cache_.insert(series_cache_key(pinyin_sequence_, pinyin_segmentation_), candidates);
    current_candidate_list_ = candidates;
    return true;
}

std::vector<WordItem> QuanpinDictionary::query_series(const std::string &raw_input, const std::string &segmentation,
                                                      const quanpin::Segments &segments)
{
    if (segments.empty())
    {
        return query_single_path(raw_input, segmentation, segments);
    }

    std::vector<WordItem> result;
    for (size_t count = segments.size(); count > 0; --count)
    {
        quanpin::Segments partial_segments(segments.begin(), segments.begin() + static_cast<std::ptrdiff_t>(count));
        const std::string partial_segmentation = quanpin::join_segments(partial_segments);
        const std::string partial_input = remove_delimiters(partial_segmentation);
        // 整句联想只对完整输入出一句。前缀查不到词时 query_single_path 会补一条 Google 整句，
        // 留着就会在长句后面缀上一串越来越短的子串整句；前缀只取词库里真有的词，
        // 查不到就交给更短的前缀。
        auto partial_result = count < segments.size()
                                  ? query_prefix_path(partial_input, partial_segmentation, partial_segments)
                                  : query_single_path(partial_input, partial_segmentation, partial_segments);
        // 前缀命中的 cache_ 条目可能是这串当初作为完整输入时查的，带着兜底整句。
        if (count < segments.size())
        {
            partial_result.erase(std::remove_if(partial_result.begin(), partial_result.end(),
                                                [](const WordItem &item) {
                                                    return item.source == CandidateSource::Fallback &&
                                                           item.sentence_association;
                                                }),
                                 partial_result.end());
        }
        result.insert(result.end(), partial_result.begin(), partial_result.end());
    }

    if (segments.size() >= 2 && quanpin::has_only_complete_pinyin_segments(segments))
    {
        // 两条整句来源各出一句：词格（kenlm 三元模型打分）在前，Google 解码器在后。
        // 词格换成 sc.lm 之后整体比 Google 那条准，所以由它占首位；Google 那条保留，
        // 它在词格覆盖不到的输入上仍然有用。两边都只出一句，免得近似重复的整句把
        // 候选页挤满。
        //
        // 两条都插在 generated_sentence_insert_position 给的位置上，也就是开头那串
        // 「整串拼音精确命中词库」的候选之后：整句是猜出来的，不该压过词库里真有的
        // 短语。相对次序仍由插入点决定——merge_lattice_candidates 算出的位置就是这条
        // Fallback 所在的下标，词格随后落在它之前。
        //
        // Google 解码器自己重新切分，这正是它的价值所在（词格覆盖不到的输入靠它），
        // 所以默认喂裸串。但手动分隔符是用户明确表达的切分意图，去掉之后 nu'e 会被
        // 它读成 nüe 而出「虐」。撇号本来就是它认的音节分隔符（双拼那条一直是连着
        // 撇号传的），手打分隔符时原样传下去即可。
        //
        // 不管走哪条，送进去之前都要把 ü 换成它认的写法：它的音节表里只有 nue/lue，
        // nve 会被拆成 nv + e。见 quanpin::to_google_spelling。
        // Google 整句（Fallback）：仅在开关打开时才解码、才出候选。
        if (sentence_association_.google)
        {
            const std::string google_input =
                raw_input.find('\'') != std::string::npos
                    ? quanpin::to_google_spelling(raw_input)
                    : remove_delimiters(quanpin::to_google_spelling(segmentation.empty() ? raw_input : segmentation));
            const std::string google_sentence = search_sentence_from_ime_engine(google_input);
            if (!google_sentence.empty())
            {
                const auto duplicate = std::find_if(result.begin(), result.end(),
                                                    [&](const WordItem &item) { return item.word == google_sentence; });
                if (duplicate == result.end())
                {
                    // 整句 fallback 必须带上 canonical quanpin，否则以它结尾的造词无法落库：
                    // update_creating_word_progress 依赖 canonical_pinyin 才能拼出完整读音。
                    // segmentation 为空时保持为空，交由既有逻辑判定为不可落库。
                    const size_t insert_at = quanpin::generated_sentence_insert_position(result, segments);
                    WordItem sentence(segmentation.empty() ? raw_input : segmentation, google_sentence, 1,
                                      CandidateSource::Fallback, segmentation);
                    sentence.sentence_association = true;
                    result.insert(result.begin() + static_cast<std::ptrdiff_t>(insert_at), std::move(sentence));
                }
            }
        }

        // 词格给 Trigram 候选和神经模型共用：只开神经时仍在内部解出 n-best 供它重排，
        // 但不把词格自己的首选显示出来。神经模型不自己造句，只在这批路径中选一句。
        std::vector<quanpin::SourcedLatticeReranker> neural_rerankers;
        if (sentence_association_.neural_keyboard && neural_keyboard_model_.get() != nullptr)
        {
            neural_rerankers.push_back({quanpin::make_neural_reranker(neural_keyboard_model_.get(), rescoring_context_),
                                        CandidateSource::NeuralKeyboard});
        }
        if (sentence_association_.neural_desktop && neural_desktop_model_.get() != nullptr)
        {
            neural_rerankers.push_back({quanpin::make_neural_reranker(neural_desktop_model_.get(), rescoring_context_),
                                        CandidateSource::NeuralDesktop});
        }
        // 万象语法模型不再单独重排：加成分只在词格解码期注入一次，〔万象〕行即解码最优路径
        // （见 WordLatticeOptions::include_collocation_best）。
        quanpin::WordLatticeOptions lattice_options;
        const bool has_collocation =
            collocation_db_ != nullptr && collocation_db_->valid() && sentence_association_.collocation_weight != 0.0;
        // 神经模型和去重补位都需要完整候选池；否则只解最优的一条。
        const bool needs_alternatives = !neural_rerankers.empty() || (sentence_association_.word_lattice &&
                                                                      sentence_association_.show_next_on_duplicate);
        lattice_options.nbest = needs_alternatives ? static_cast<int>(neural::RerankOptions{}.max_paths) : 1;
        lattice_options.include_lattice_best = sentence_association_.word_lattice;
        lattice_options.include_collocation_best = sentence_association_.collocation_rerank && has_collocation;
        lattice_options.show_next_on_duplicate = sentence_association_.show_next_on_duplicate;
        lattice_options.language_model = language_model_;
        // 万象语法模型的搭配项：逐边叠加字级搭配分，与三元模型在同一 Viterbi 里竞争。
        // query() 是自然对数域，乘 kLnToLog10 换到词格的 log10 域；六参数来自配置。
        if (has_collocation)
        {
            lattice_options.collocation_scorer = [db = collocation_db_,
                                                  config = sentence_association_.collocation_grammar](
                                                     std::string_view tail, std::string_view word, bool is_rear) {
                return db->query(std::string(tail), std::string(word), is_rear, config) * quanpin::kLnToLog10;
            };
            lattice_options.collocation_weight = sentence_association_.collocation_weight;
        }
        if (sentence_association_.word_lattice || !neural_rerankers.empty() || lattice_options.include_collocation_best)
        {
            quanpin::merge_lattice_candidates(result, segments, lattice_lookup(lattice_options.span_limit),
                                              segmentation.empty() ? raw_input : segmentation, lattice_options,
                                              neural_rerankers);
        }
    }

    if (result.size() < kSparsePinyinFallbackThreshold)
    {
        result = append_sparse_pinyin_fallbacks(segments, std::move(result));
    }

    return result;
}

std::vector<WordItem> QuanpinDictionary::query_single_path(const std::string &raw_input,
                                                           const std::string &segmentation,
                                                           const quanpin::Segments &segments)
{
    const std::string cache_key = segmentation.empty() ? raw_input : segmentation;
    if (const auto *cached = cache_.find(cache_key))
    {
        return *cached;
    }

    std::vector<WordItem> result = query_database(segments, segmentation);
    result = append_ime_fallback(raw_input, segmentation, std::move(result));
    cache_.insert(cache_key, result);
    return result;
}

std::vector<WordItem> QuanpinDictionary::query_prefix_path(const std::string &raw_input,
                                                           const std::string &segmentation,
                                                           const quanpin::Segments &segments)
{
    const std::string cache_key = segmentation.empty() ? raw_input : segmentation;
    if (const auto *cached = cache_.find(cache_key))
    {
        return *cached;
    }
    if (prefix_miss_cache_.find(cache_key))
    {
        return {};
    }

    std::vector<WordItem> result = query_database(segments, segmentation);
    if (result.empty())
    {
        prefix_miss_cache_.insert(cache_key, true);
    }
    else
    {
        // 兜底整句只在词库查不到时才补，所以这正是 query_single_path 会写入的值。
        cache_.insert(cache_key, result);
    }
    return result;
}

quanpin::Segments QuanpinDictionary::resolve_segments(const std::string &raw_input, const std::string &segmentation,
                                                      bool include_correction_aliases)
{
    // 单一归一咽喉点：显式 segmentation 与自动切分两条路径都过这里，进入查询
    // 管线的 segments 由此保证已归一恰一次。查库、调频、造词各调用点因此
    // 不必各自处理别名。
    quanpin::Segments segments = segmentation.empty() ? get_or_compute_segments(raw_input, include_correction_aliases)
                                                      : quanpin::split_segments(segmentation);
    return normalize_umlaut_aliases(std::move(segments));
}

quanpin::Segments QuanpinDictionary::get_or_compute_segments(const std::string &raw_input,
                                                             bool include_correction_aliases)
{
    // 两种掩码状态下同名输入的切分可能不同（手误别名层在 mask != 0 时才跑），缓存键
    // 带上该标志，避免「开着纠错算出来的 shang」被关掉纠错的同名查询复用。
    const std::string cache_key = (include_correction_aliases ? "1:" : "0:") + raw_input;
    if (const auto *cached = segmentation_cache_.find(cache_key))
    {
        return *cached;
    }

    const auto cuts = quanpin::cut_pinyin_by_mode(raw_input, "correction", include_correction_aliases);
    const auto segments = cuts.empty() ? quanpin::Segments{} : cuts.front();
    segmentation_cache_.insert(cache_key, segments);
    return segments;
}

std::vector<quanpin::Segments> QuanpinDictionary::cut_for_write(const std::string &pinyin) const
{
    return quanpin::cut_pinyin_by_mode(pinyin, "correction", correction_aliases_enabled_);
}

int QuanpinDictionary::handleVkCode(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch)
{
    (void)modifiers_down;

    if (vk == ImeKey::Backspace)
    {
        if (!pinyin_sequence_.empty())
        {
            pinyin_sequence_.pop_back();
        }
    }
    else if (vk == ImeKey::Escape || vk == ImeKey::Return || vk == ImeKey::Space)
    {
        reset_state();
        return OK;
    }
    else if (vk == ImeKey::Apostrophe)
    {
        pinyin_sequence_.push_back('\'');
    }
    else if (is_alpha_vk(vk))
    {
        if (wch >= u'A' && wch <= u'Z')
        {
            pinyin_sequence_.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(wch))));
        }
        else if (wch >= u'a' && wch <= u'z')
        {
            pinyin_sequence_.push_back(static_cast<char>(wch));
        }
        else
        {
            pinyin_sequence_.push_back(static_cast<char>(vk + ('a' - 'A')));
        }
    }

    query(pinyin_sequence_);
    return OK;
}

std::vector<WordItem> QuanpinDictionary::query_database(const quanpin::Segments &segments,
                                                        const std::string &segmentation)
{
    if (db_ == nullptr)
    {
        return {};
    }

    try
    {
        if (segments.size() == 1 && segments.front().size() == 1)
        {
            constexpr int kInitialCandidateLimit = 24;
            auto result = query_initial(segments.front(), kInitialCandidateLimit);
            const std::string matched_code = segmentation.empty() ? segments.front() : segmentation;
            for (auto &item : result)
            {
                item.canonical_pinyin = item.pinyin;
                item.pinyin = matched_code;
            }
            return result;
        }

        // segments 已在 resolve_segments 归一，直接按标准键查库。
        const auto flat_items = quanpin::query_segments_keyed_flat(segments, db_, statement_cache_, INT_MAX);
        std::vector<WordItem> result;
        result.reserve(flat_items.size());
        const std::string code = segmentation.empty() ? quanpin::join_segments(segments) : segmentation;
        for (const auto &item : flat_items)
        {
            result.emplace_back(code, item.value, item.weight, CandidateSource::Database, item.key);
        }
        return result;
    }
    catch (const std::exception &)
    {
        (void)0;
        return {};
    }
}

std::vector<WordItem> QuanpinDictionary::query_initial(const std::string &code, int limit)
{
    if (db_ == nullptr || code.size() != 1)
    {
        return {};
    }

    const auto rows = quanpin::query_initial(db_, code, limit);
    std::vector<WordItem> result;
    result.reserve(rows.size());
    for (const auto &item : rows)
    {
        result.emplace_back(item.key, item.value, item.weight, CandidateSource::Database, item.key);
    }
    return result;
}

std::vector<WordItem> QuanpinDictionary::merge_alternative_segmentations(
    const std::string &raw_input, const std::string &primary_segmentation, const quanpin::Segments &primary_segments,
    const std::vector<quanpin::Segments> &alternative_segmentations, std::vector<WordItem> result)
{
    const auto alternative_items = quanpin::query_exact_segmentations_keyed_flat(
        alternative_segmentations, db_, statement_cache_, kAlternativeSegmentationCandidateLimit);
    if (alternative_items.empty())
    {
        return result;
    }

    const auto primary_full = query_single_path(raw_input, primary_segmentation, primary_segments);
    std::vector<WordItem> alternative_full;
    alternative_full.reserve(alternative_items.size());
    for (const auto &item : alternative_items)
    {
        alternative_full.emplace_back(item.key, item.value, item.weight, CandidateSource::Database, item.key);
    }

    std::vector<WordItem> merged_full = primary_full;
    merged_full.insert(merged_full.end(), alternative_full.begin(), alternative_full.end());
    std::stable_sort(merged_full.begin(), merged_full.end(),
                     [](const WordItem &lhs, const WordItem &rhs) { return lhs.weight > rhs.weight; });
    std::unordered_set<std::string> seen_full_words;
    merged_full.erase(std::remove_if(merged_full.begin(), merged_full.end(),
                                     [&](const WordItem &item) { return !seen_full_words.insert(item.word).second; }),
                      merged_full.end());

    // Weights rank candidates reliably within one pinyin key, but are not directly comparable across
    // different segmentations. Keep the best alternative interpretation visible without letting every
    // segmentation occupy a protected slot on the first page -- but only when the best alternative
    // word is not dwarfed by the primary reading's top candidate, otherwise the slot goes to noise
    // like 西鄂 for "xie" instead of a reading the user might actually have meant.
    const std::int64_t primary_top_weight = primary_full.empty() ? 0 : primary_full.front().weight;
    const bool promote_alternative =
        static_cast<std::int64_t>(alternative_items.front().weight) * kAlternativeSegmentationPromotionRatio >=
        primary_top_weight;
    const std::string &best_alternative_word = alternative_items.front().value;
    const auto best_alternative = std::find_if(merged_full.begin(), merged_full.end(), [&](const WordItem &item) {
        return item.word == best_alternative_word;
    });
    // 这个保护位只负责「把首页外的备选读音拉进首页」，不负责给它排座次。已经排进首页的
    // 候选一律不动：它的位置是权重排出来的，而用户调频写的就是权重，再钉一次等于把调频
    // 的结果覆盖掉。吉安 就是这么被弹到第 2 位的——调频按 promote 把它放到 index 4，
    // 它因此成了 ji'an 组里权重最高的词、过了上面的门槛，于是又被拽到 index 1，用户看到
    // 的是「选一次就跳到第二」。西安（自然位置 16，在首页外）不受影响，照旧进 index 1。
    const size_t best_alternative_index =
        best_alternative == merged_full.end()
            ? 0
            : static_cast<size_t>(std::distance(merged_full.begin(), best_alternative));
    if (promote_alternative && best_alternative != merged_full.end() &&
        best_alternative_index >= kAlternativeSegmentationFirstPageSize)
    {
        WordItem promoted = std::move(*best_alternative);
        merged_full.erase(best_alternative);
        merged_full.insert(merged_full.begin() + static_cast<std::ptrdiff_t>(kBestAlternativeSegmentationMaxIndex),
                           std::move(promoted));
    }

    std::vector<WordItem> merged = std::move(merged_full);
    const size_t primary_full_count = primary_full.size() < result.size() ? primary_full.size() : result.size();
    std::vector<WordItem> remaining(result.begin() + static_cast<std::ptrdiff_t>(primary_full_count), result.end());
    append_unique_words(merged, remaining);
    return merged;
}

// 合法输入常有不止一种等音节数的切分：yibananzhao 既是 yi'ba'nan'zhao 也是
// yi'ban'an'zhao，最少音节规则平局时取较短的首音节，于是固定落在前者、出「一把难找」。
// 切分是在查词之前定的，没有上下文，这里把它交给整句打分：
//   - 任一条切分在词库里有整词（fang'an 的方案 / fan'gan 的反感、西安）时不介入，那是
//     merge_alternative_segmentations 按词频比的单词级歧义，已经调好，不该被整句分翻动；
//   - 否则各切分解一遍词格，路径分最高的领衔，同分仍是原切分（最少音节规则降为平局判据）；
//   - 输的一方分差不超过 kLegalInputRunnerUpGapLog 时，它的整句占第 2 位；更远就不出，
//     免得每个带 n/ng 歧义的输入都在第 2 位挂一句不通的话。
// 只在整句来源开着时介入：整句来源全关时没有整句可比，结果与改造前逐位一致。用户在原
// 切分键上调过频或造过词时，原切分不让出领衔（同 arbitrate_legal_corrections）。
std::vector<WordItem> QuanpinDictionary::arbitrate_literal_segmentations(const std::string &raw_input,
                                                                         const quanpin::Segments &segments,
                                                                         std::vector<WordItem> result,
                                                                         quanpin::Segments &leading_segments)
{
    leading_segments = segments;
    if (!sentence_association_.any_sentence_source() || result.empty() || segments.size() < 2 ||
        raw_input.find('\'') != std::string::npos || !quanpin::has_only_complete_pinyin_segments(segments))
    {
        return result;
    }

    // 主切分必须就是字面切分之一（别名改写过字母的读法不算），备选是其余等音节数的切分。
    bool primary_is_literal = false;
    std::vector<quanpin::Segments> alternatives;
    for (auto &cut : quanpin::enumerate_min_segmentations(raw_input, kLiteralSegmentationEnumerateLimit))
    {
        cut = normalize_umlaut_aliases(std::move(cut));
        if (cut == segments)
        {
            primary_is_literal = true;
        }
        else if (alternatives.size() < kLiteralSegmentationDecodeLimit)
        {
            alternatives.push_back(std::move(cut));
        }
    }
    if (!primary_is_literal || alternatives.empty())
    {
        return result;
    }

    std::vector<quanpin::Segments> all_cuts = alternatives;
    all_cuts.push_back(segments);
    if (!quanpin::query_exact_segmentations_keyed_flat(all_cuts, db_, statement_cache_, 1).empty())
    {
        return result;
    }

    const auto primary_score = lattice_best_path_score(segments);
    if (!primary_score.has_value())
    {
        return result;
    }
    std::optional<double> best_score;
    const quanpin::Segments *best_cut = nullptr;
    for (const auto &cut : alternatives)
    {
        const auto score = lattice_best_path_score(cut);
        if (score.has_value() && (!best_score.has_value() || *score > *best_score))
        {
            best_score = score;
            best_cut = &cut;
        }
    }
    if (best_cut == nullptr)
    {
        return result;
    }

    const std::string primary_key = quanpin::join_segments(segments);
    const std::string best_key = quanpin::join_segments(*best_cut);
    const auto is_sentence_of = [](const std::string &key) {
        return [&key](const WordItem &item) { return reads_full_key(item, key); };
    };
    const bool alternative_leads = *best_score > *primary_score && !user_prefers_reading(primary_key);
    const double gap = alternative_leads ? *best_score - *primary_score : *primary_score - *best_score;
    if (!alternative_leads)
    {
        if (gap > kLegalInputRunnerUpGapLog)
        {
            return result;
        }
        // 第 2 位只要一句。Trigram 整句开着时直接用打分时解出的那条最优路径，与 merge_lattice_candidates
        // 造的行同形；不必为这一行再跑一遍带神经重排的 query_series。只开神经等来源时 Trigram 行不显示，
        // 那就照常查一遍，取它的整键块首行（词库里没有这条切分的整词，上面已经排除，整键块就是整句行）。
        std::optional<WordItem> runner;
        if (sentence_association_.word_lattice)
        {
            if (const auto path = lattice_best_path(*best_cut))
            {
                runner = WordItem(best_key, path->sentence, static_cast<std::int64_t>(path->log_prob * 1000.0),
                                  CandidateSource::Generated, path->key);
                runner->sentence_association = true;
            }
        }
        else
        {
            const auto alternative = query_series(raw_input, best_key, *best_cut);
            const auto found = std::find_if(alternative.begin(), alternative.end(), is_sentence_of(best_key));
            if (found != alternative.end())
            {
                runner = *found;
            }
        }
        if (!runner.has_value())
        {
            return result;
        }
        std::vector<WordItem> merged;
        merged.push_back(result.front());
        append_unique_words(merged, {*runner});
        append_unique_words(merged, result);
        return merged;
    }

    leading_segments = *best_cut;
    // 同 query_exact 里纠错的上下文消解：胜出切分的整键块（整句行）领衔，其余照旧，
    // 胜出切分的前缀词垫底；整键块为空时（整句只开了重排、模型没载入）由前缀词领衔。
    std::vector<WordItem> winner_full;
    std::vector<WordItem> winner_prefix;
    for (auto &item : query_series(raw_input, best_key, *best_cut))
    {
        (reads_full_key(item, best_key) ? winner_full : winner_prefix).push_back(std::move(item));
    }
    if (winner_full.empty())
    {
        winner_full.swap(winner_prefix);
    }
    if (winner_full.empty())
    {
        leading_segments = segments;
        return result;
    }
    // 原切分的整句：分差以内占第 2 位，更远就整组不出（前缀词照留）。
    // 原切分的前缀区间扫描会捞出胜出切分的词，读音却记在原切分上（yi'ba 扫出「一般」，pinyin 是
    // yi'ba）：同一个词两边都有时换成胜出切分那一行，选它才按 yi'ban 消耗字母。位置不变。
    std::unordered_map<std::string, const WordItem *> winner_prefix_by_word;
    for (const auto &item : winner_prefix)
    {
        winner_prefix_by_word.emplace(item.word, &item);
    }
    std::vector<WordItem> primary_sentences;
    std::vector<WordItem> rest;
    for (auto &item : result)
    {
        if (reads_full_key(item, primary_key))
        {
            primary_sentences.push_back(std::move(item));
            continue;
        }
        const auto same_word = winner_prefix_by_word.find(item.word);
        rest.push_back(same_word == winner_prefix_by_word.end() ? std::move(item) : *same_word->second);
    }
    std::vector<WordItem> merged;
    merged.push_back(winner_full.front());
    if (gap <= kLegalInputRunnerUpGapLog && !primary_sentences.empty())
    {
        append_unique_words(merged, {primary_sentences.front()});
        append_unique_words(merged, winner_full);
        append_unique_words(merged, primary_sentences);
    }
    else
    {
        append_unique_words(merged, winner_full);
    }
    append_unique_words(merged, rest);
    append_unique_words(merged, winner_prefix);
    return merged;
}

// 合法输入上的手误（ziazheliya、jioa、jiuzheeyang、nia）按噪声信道整句打分：
//   总分 = 词格最佳路径分 - 每处纠正的手误代价（换位 2.0，多字/漏字 3.0）
// 原读法代价为 0。最高分领衔，另一方的最佳读法占第 2 位：纠错领衔时原读法永远在
// 第 2 位（纠错可能误判，原读法要留在手边）；原读法领衔时，纠错读法只在分差不超过
// kLegalInputRunnerUpGapLog 时出现，否则整组不出——碰巧能纠成别的音节的输入很多，
// 语言模型都不认的读法不该挂在第 2 位。
// 打分不看整句候选开关：那个开关管的是显示不显示整句候选，判断用户想打哪个读音
// 是另一件事。用户在原读法键上调过频或造过词时，纠错不抢领衔（同上下文接管）。
std::vector<WordItem> QuanpinDictionary::arbitrate_legal_corrections(
    const std::string &raw_input, const quanpin::Segments &plain_segments,
    const std::vector<quanpin::AutocorrectCut> &corrected_cuts, std::vector<WordItem> result)
{
    // 字面切分本身就是真词时（邪恶 xie'e、答案 da'an），不认改变字母数的读法：少一个
    // 音节的句子在语言模型里天然分高，「些」「但」会稳定挂在第 2 位，而这些输入几乎都是
    // 照原样打的。换位读法不改音节数，没有这个偏差，照常参与（你啊 / 奶）。
    const bool literal_is_word = find_literal_whole_word(result, raw_input) != result.end();
    std::optional<double> best_total;
    const quanpin::AutocorrectCut *best_cut = nullptr;
    for (const auto &cut : corrected_cuts)
    {
        double penalty = 0.0;
        bool changes_length = false;
        for (const auto &segment : cut.segments)
        {
            if (!segment.corrected)
            {
                continue;
            }
            const bool same_length = segment.raw_text.size() == segment.syllable.size();
            changes_length = changes_length || !same_length;
            penalty += same_length ? kLegalInputTranspositionPenaltyLog : kLegalInputLengthChangePenaltyLog;
        }
        if (literal_is_word && changes_length)
        {
            continue;
        }
        const auto score = lattice_best_path_score(cut_syllables(cut));
        if (!score.has_value())
        {
            continue;
        }
        const double total = *score - penalty;
        if (!best_total.has_value() || total > *best_total)
        {
            best_total = total;
            best_cut = &cut;
        }
    }
    if (best_cut == nullptr)
    {
        return result;
    }
    const auto plain_total = lattice_best_path_score(plain_segments);

    const quanpin::Segments corrected_segments = cut_syllables(*best_cut);
    const std::string corrected_key = quanpin::join_segments(corrected_segments);
    std::vector<WordItem> corrected = query_series(raw_input, corrected_key, corrected_segments);
    if (corrected.empty())
    {
        return result;
    }
    // 只给纠错读法的整读音候选打标（与 mark_autocorrect_candidates 同一判据：读音字母
    // 等于纠错读法、又不同于敲的字母），前缀单字不标。标记随结果进缓存。
    const std::string typed_letters = fold_autocorrect_letters(raw_input);
    const std::string corrected_letters = fold_autocorrect_letters(corrected_key);
    for (auto &item : corrected)
    {
        if (item.corrected_from.empty() && fold_autocorrect_letters(item.pinyin) == corrected_letters &&
            corrected_letters != typed_letters)
        {
            item.corrected_from = typed_letters;
        }
    }

    const bool correction_leads = (!plain_total.has_value() || *best_total > *plain_total) &&
                                  !user_prefers_reading(quanpin::join_segments(plain_segments));
    if (!correction_leads && (!plain_total.has_value() || *plain_total - *best_total > kLegalInputRunnerUpGapLog))
    {
        return result;
    }

    std::vector<WordItem> &leader = correction_leads ? corrected : result;
    std::vector<WordItem> &runner = correction_leads ? result : corrected;
    std::vector<WordItem> merged;
    if (!leader.empty())
    {
        merged.push_back(leader.front());
    }
    if (!runner.empty())
    {
        append_unique_words(merged, {runner.front()});
    }
    append_unique_words(merged, leader);
    append_unique_words(merged, runner);
    return merged;
}

std::optional<double> QuanpinDictionary::lattice_best_path_score(const quanpin::Segments &cut)
{
    const auto path = lattice_best_path(cut);
    return path.has_value() ? std::optional<double>(path->log_prob) : std::nullopt;
}

std::optional<quanpin::LatticePath> QuanpinDictionary::lattice_best_path(const quanpin::Segments &cut)
{
    if (cut.empty() || !quanpin::has_only_complete_pinyin_segments(cut))
    {
        return std::nullopt;
    }
    const std::string cache_key = quanpin::join_segments(cut);
    if (const auto *cached = lattice_best_path_cache_.find(cache_key))
    {
        return *cached;
    }
    // 仲裁回到只用 kenlm：切分/纠错裁决不再让 octagram 参与。搭配项是当前同音/切分被带偏的
    // 放大器之一，而 librime 的切分器也不由 grammar 驱动（见 design D1）。
    quanpin::WordLatticeOptions lattice_options;
    lattice_options.language_model = language_model_;
    // 不带模糊音：这个分用来裁决切分与纠错，模糊读音只给敲定的切分多出整句，不参与投票。
    // 带上它，字面读法会借模糊读音的分压过纠错（jiuzheeyang 的「就这恶言」压过「就这样」）。
    const auto lookup = lattice_lookup(lattice_options.span_limit, false);
    auto cut_paths = quanpin::decode_word_lattice(cut, lookup, lattice_options);
    std::optional<quanpin::LatticePath> best;
    if (!cut_paths.empty())
    {
        best = std::move(cut_paths.front());
    }
    lattice_best_path_cache_.insert(cache_key, best);
    return best;
}

// 用户在这个读音键上有过调频或造词记录。只在边际已达标、要决定是否接管时才查，
// 不给每次按键加一次日志查询。
bool QuanpinDictionary::user_prefers_reading(const std::string &key)
{
    return user_dictionary::has_user_upsert_for_key(
        metasequoia::path_to_utf8(paths_.user(metasequoia::assets::user_journal)),
        user_dictionary::DictionaryKind::Pinyin, key);
}

std::vector<WordItem> QuanpinDictionary::append_ime_fallback(const std::string &raw_input,
                                                             const std::string &segmentation,
                                                             std::vector<WordItem> result)
{
    if (!result.empty())
    {
        return result;
    }
    // 这条兜底整句也来自 Google 解码器，随「Google 整句联想」开关一起关闭。
    if (!sentence_association_.google)
    {
        return result;
    }

    const std::string normalized = remove_delimiters(segmentation.empty() ? raw_input : segmentation);
    const std::string sentence = search_sentence_from_ime_engine(normalized);
    if (sentence.empty())
    {
        return result;
    }

    const auto exists =
        std::find_if(result.begin(), result.end(), [&](const WordItem &item) { return item.word == sentence; });
    if (exists == result.end())
    {
        // 同上，整句 fallback 需要 canonical quanpin 才能参与造词落库。
        result.emplace_back(segmentation.empty() ? raw_input : segmentation, sentence, 1, CandidateSource::Fallback,
                            segmentation);
        result.back().sentence_association = true;
    }
    return result;
}

std::vector<WordItem> QuanpinDictionary::append_sparse_pinyin_fallbacks(const quanpin::Segments &segments,
                                                                        std::vector<WordItem> result)
{
    for (const auto &fallback_segments : quanpin::sparse_pinyin_fallback_segments(segments))
    {
        if (fallback_segments.empty())
        {
            continue;
        }

        const std::string fallback_segmentation = quanpin::join_segments(fallback_segments);
        const std::string fallback_input = remove_delimiters(fallback_segmentation);
        const auto fallback_result = query_single_path(fallback_input, fallback_segmentation, fallback_segments);
        append_unique_words(result, fallback_result);
    }
    return result;
}

void QuanpinDictionary::append_unique_words(std::vector<WordItem> &result, const std::vector<WordItem> &extra)
{
    for (const auto &item : extra)
    {
        const auto exists = std::find_if(result.begin(), result.end(),
                                         [&](const WordItem &existing) { return existing.word == item.word; });
        if (exists == result.end())
        {
            result.push_back(item);
        }
    }
}

void QuanpinDictionary::mark_autocorrect_candidates(std::vector<WordItem> &candidates, const std::string &raw_input)
{
    // A candidate comes from a corrected interpretation exactly when its code
    // letters equal any correction cut's letters (primary or ranked
    // alternative) while those differ from the typed letters. The letters-only
    // comparison alone would also sweep up prefix candidates (keneng -> ke,
    // single-letter jianpin expansions) that the user spelled correctly; both
    // rules together keep those unmarked. The ü-style alias rewrite (nue->nve,
    // jv->ju) is a first-class marking source: it runs in resolve_segments
    // regardless of the autocorrect switches, so an alias-rewritten query marks
    // its candidates with the typed spelling, while a standard spelling exits
    // at the all-equal gate because every set equals the typed letters.
    std::vector<std::string> corrected_letter_sets;
    corrected_letter_sets.reserve(1 + pinyin_alternative_segmentations_.size());
    corrected_letter_sets.push_back(fold_autocorrect_letters(pinyin_segmentation_));
    for (const auto &alternative : pinyin_alternative_segmentations_)
    {
        corrected_letter_sets.push_back(fold_autocorrect_letters(alternative));
    }
    corrected_letter_sets.erase(std::remove_if(corrected_letter_sets.begin(), corrected_letter_sets.end(),
                                               [](const std::string &letters) { return letters.empty(); }),
                                corrected_letter_sets.end());
    const std::string raw_letters = fold_autocorrect_letters(raw_input);
    if (corrected_letter_sets.empty() ||
        std::all_of(corrected_letter_sets.begin(), corrected_letter_sets.end(),
                    [&](const std::string &letters) { return letters == raw_letters; }))
    {
        return;
    }
    for (auto &item : candidates)
    {
        if (!item.corrected_from.empty() || literal_mixed_match(item, raw_input))
        {
            continue;
        }
        const std::string item_letters = fold_autocorrect_letters(item.pinyin);
        // Match only against readings that actually differ from the typed
        // letters. An alternative cut can fold back to exactly raw_letters
        // (letters removed by the fold but rewritten by the cut); matching that
        // set would stamp corrected_from onto a candidate the user spelled
        // correctly. The all-equal gate above only guards the case where EVERY
        // set equals raw_letters, so this per-set filter is needed.
        if (std::any_of(corrected_letter_sets.begin(), corrected_letter_sets.end(),
                        [&](const std::string &letters) { return letters != raw_letters && item_letters == letters; }))
        {
            item.corrected_from = raw_letters;
        }
    }
}

int QuanpinDictionary::create_word(std::string pinyin, std::string word)
{
    pinyin = remove_delimiters(pinyin);
    const auto cuts = cut_for_write(pinyin);
    if (cuts.empty())
    {
        return ERROR_CODE;
    }

    pinyin = quanpin::join_segments(cuts.front());
    const std::string jp = quanpin::segments_to_jianpin(cuts.front());
    if (!do_validate(pinyin, jp, word))
    {
        return ERROR_CODE;
    }

    if (check_data(build_sql_for_checking_word(pinyin, word)))
    {
        return OK;
    }

    if (insert_data(build_sql_for_inserting_word(pinyin, jp, word)) != OK)
    {
        return ERROR_CODE;
    }
    (void)user_dictionary::record_user_insert(metasequoia::path_to_utf8(paths_.user(metasequoia::assets::user_journal)),
                                              user_dictionary::DictionaryKind::Pinyin, pinyin, word, 10000);
    reset_cache();
    return OK;
}

int QuanpinDictionary::create_word_from_canonical_pinyin(std::string pinyin, std::string word)
{
    const auto segments = quanpin::split_segments(pinyin);
    const size_t han_count = HelpcodeUtils::count_han_chars(word);
    if (segments.empty() || segments.size() != han_count ||
        std::any_of(segments.begin(), segments.end(), [](const std::string &segment) {
            return segment.empty() || !quanpin::is_complete_pinyin_input(segment);
        }))
    {
        return ERROR_CODE;
    }

    pinyin = quanpin::join_segments(segments);
    const std::string jp = quanpin::segments_to_jianpin(segments);
    // 不再走 do_validate：这里的音节边界由调用方（canonical 键）显式给定，上面已按
    // split_segments 校验过段数、逐段完整性，jp 也是每段一字母、长度必然等于字数。
    // do_validate 会先 remove_delimiters 再用贪心 correction 重切，把 qi'e'huan 断成
    // qie'huan（段数 3→2），从而误杀这条合法整句——正是 AI / 云联想选中却入不了库的根因。
    if (check_data(build_sql_for_checking_word(pinyin, word)))
    {
        return OK;
    }
    if (insert_data(build_sql_for_inserting_word(pinyin, jp, word)) != OK)
    {
        return ERROR_CODE;
    }
    (void)user_dictionary::record_user_insert(metasequoia::path_to_utf8(paths_.user(metasequoia::assets::user_journal)),
                                              user_dictionary::DictionaryKind::Pinyin, pinyin, word, 10000);
    reset_cache();
    return OK;
}

int QuanpinDictionary::update_weight_by_word(std::string word)
{
    return update_weight_by_pinyin_and_word(remove_delimiters(pinyin_segmentation_), std::move(word));
}

int QuanpinDictionary::update_weight_by_pinyin_and_word(std::string pinyin, std::string word)
{
    pinyin = remove_delimiters(pinyin);
    const auto cuts = cut_for_write(pinyin);
    if (cuts.empty())
        return ERROR_CODE;
    auto segments = cuts.front();
    // 别名拼写（nue/lue 等词库不存在的键）不得承载用户调频数据：改写为标准键，
    // 保证权重更新落在标准字典行上（PRD R3）。
    segments = normalize_umlaut_aliases(std::move(segments));
    const size_t han_count = HelpcodeUtils::count_han_chars(word);
    if (segments.size() > han_count)
        segments.resize(han_count);
    const std::string normalized = quanpin::join_segments(segments);
    if (update_data(build_sql_for_updating_word(normalized, word)) != OK)
    {
        return ERROR_CODE;
    }
    (void)user_dictionary::record_pinyin_upsert_from_database(
        db_path_, normalized, word, metasequoia::path_to_utf8(paths_.user(metasequoia::assets::user_journal)));
    reset_cache();
    return OK;
}

int QuanpinDictionary::delete_by_pinyin_and_word(std::string pinyin, std::string word)
{
    pinyin = remove_delimiters(pinyin);
    const auto cuts = cut_for_write(pinyin);
    if (cuts.empty())
        return ERROR_CODE;
    const std::string normalized = quanpin::join_segments(cuts.front());
    if (!user_dictionary::delete_dictionary_candidate(
            db_path_, metasequoia::path_to_utf8(paths_.user(metasequoia::assets::user_journal)),
            user_dictionary::DictionaryKind::Pinyin, normalized, word))
        return ERROR_CODE;
    reset_cache();
    return OK;
}

int QuanpinDictionary::insert_word_to_series_cache(const std::string &pinyin, const std::string &word,
                                                   CandidateSource source)
{
    if (pinyin.empty() || word.empty())
    {
        return ERROR_CODE;
    }

    const auto cuts = cut_for_write(pinyin);
    const std::string segmentation = cuts.empty() ? pinyin : quanpin::join_segments(cuts.front());
    const std::string cache_key = series_cache_key(pinyin, segmentation);
    return insert_word_to_series_cache_key(cache_key, pinyin, word, source);
}

int QuanpinDictionary::insert_word_to_series_cache(const std::string &raw_input, const std::string &segmentation,
                                                   unsigned autocorrect_types, const std::string &word,
                                                   CandidateSource source)
{
    if (raw_input.empty() || word.empty())
    {
        return ERROR_CODE;
    }

    const auto segments =
        resolve_segments(raw_input, segmentation, quanpin::correction_aliases_enabled(autocorrect_types));
    const auto resolution = resolve_series_query(raw_input, segments, autocorrect_types);
    return insert_word_to_series_cache_key(resolution.cache_key, raw_input, word, source);
}

int QuanpinDictionary::insert_word_to_series_cache_key(const std::string &cache_key, const std::string &pinyin,
                                                       const std::string &word, CandidateSource source)
{
    auto list = series_cache_.get(cache_key).value_or(std::vector<WordItem>{});

    // Keep at most one cloud/AI suggestion in the series cache for this key.
    if (source == CandidateSource::AiSuggestion || source == CandidateSource::CloudSuggestion)
    {
        list.erase(
            std::remove_if(list.begin(), list.end(), [source](const WordItem &item) { return item.source == source; }),
            list.end());
    }

    const auto exists = std::find_if(list.begin(), list.end(), [&](const WordItem &item) { return item.word == word; });
    if (exists == list.end())
    {
        if (list.empty())
        {
            list.emplace_back(pinyin, word, 1, source);
        }
        else
        {
            const size_t index = source == CandidateSource::AiSuggestion ? std::min<size_t>(2, list.size()) : 1;
            list.insert(list.begin() + index, WordItem(pinyin, word, 1, source));
        }
    }

    if (source == CandidateSource::CloudSuggestion)
    {
        const auto ai = std::find_if(list.begin(), list.end(),
                                     [](const WordItem &item) { return item.source == CandidateSource::AiSuggestion; });
        if (ai != list.end())
        {
            WordItem ai_item = std::move(*ai);
            list.erase(ai);
            list.insert(list.begin() + std::min<size_t>(2, list.size()), std::move(ai_item));
        }
    }

    series_cache_.insert(cache_key, list);
    return OK;
}

std::string QuanpinDictionary::search_sentence_from_ime_engine(const std::string &user_pinyin)
{
    return decoder_.sentence(user_pinyin);
}

void QuanpinDictionary::reset_state()
{
    pinyin_sequence_.clear();
    pinyin_segmentation_.clear();
    pinyin_alternative_segmentations_.clear();
    current_candidate_list_.clear();
}

void QuanpinDictionary::reset_cache()
{
    cache_.clear();
    series_cache_.clear();
    segmentation_cache_.clear();
    prefix_miss_cache_.clear();
    lattice_best_path_cache_.clear();
    lattice_span_cache_.clear();
}

quanpin::WordLatticeLookup QuanpinDictionary::lattice_lookup(int limit, bool with_fuzzy)
{
    const auto fuzzy = with_fuzzy ? fuzzy_pinyin_ : metasequoia::FuzzyPinyinOptions{};
    return [this, lookup = quanpin::make_lattice_db_lookup(db_, statement_cache_, limit, fuzzy), limit,
            fuzzy](const quanpin::Segments &span) {
        // 超过上限整体清掉，免得一次长时间输入把内存撑大（同双拼的 direct_span_cache_）。
        constexpr std::size_t kLatticeSpanCacheLimit = 8192;
        std::string key = quanpin::join_segments(span);
        key.push_back('#');
        key += std::to_string(limit);
        if (fuzzy.rules)
        {
            key += "#f";
        }
        auto found = lattice_span_cache_.find(key);
        if (found == lattice_span_cache_.end())
        {
            if (lattice_span_cache_.size() >= kLatticeSpanCacheLimit)
            {
                lattice_span_cache_.clear();
            }
            found = lattice_span_cache_.emplace(std::move(key), lookup(span)).first;
        }
        return found->second;
    };
}

void QuanpinDictionary::set_fuzzy_pinyin(metasequoia::FuzzyPinyinOptions options)
{
    if (fuzzy_pinyin_.rules == options.rules)
    {
        return;
    }
    fuzzy_pinyin_ = options;
    // 词格跨度、最佳路径分和按它们排出的候选列表都按旧规则算出。规则只在设置页改，
    // 清一次比给每层缓存的键都带上规则简单，也不会让关掉模糊音后读到带模糊行的旧列表。
    reset_cache();
}

void QuanpinDictionary::reset_sentence_cache()
{
    // 词格整句只在 query_series 里合入，结果只经 series_cache_ 缓存；cache_ 是逐前缀的
    // 词库行加 Google 整句，segmentation_cache_ 是纯切分，都不受重排影响。
    series_cache_.clear();
}

void QuanpinDictionary::set_sentence_association(const SentenceAssociationOptions &options)
{
    if (sentence_association_ == options)
    {
        return;
    }
    sentence_association_ = options;
    // 模型路径随开关一起变更时才重新解析：进程内按路径共享（shared_gram_db），
    // 重复解析只是查一次表，路径无效时拿到 valid()==false 的实例，词格静默降级。
    // 路径是宿主下发的 UTF-8，必须显式按 UTF-8 转，隐式转换走 ANSI 代码页。
    collocation_db_ =
        sentence_association_.collocation_model.empty()
            ? nullptr
            : gram::shared_gram_db(metasequoia::path_from_utf8(sentence_association_.collocation_model.c_str()));
    // 高精度档模型的路径同样随选项下发（模型按需下载，不在资源目录）。重建句柄只是丢掉
    // 已解析的指针，模型本身由 shared_sentence_model 按路径持有，重排线程手里的指针不失效。
    neural_desktop_model_ = neural::LazySentenceModel(sentence_association_.neural_desktop_model);
    // 开关变了，之前缓存的候选列表按旧开关算出，必须清掉，否则打开/关闭后要到缓存过期
    // 才见效。整句候选都进这几个缓存，见 query_series / query_single_path。
    reset_cache();
}

void QuanpinDictionary::set_rescoring_context(const std::string &context)
{
    if (rescoring_context_ == context)
    {
        return;
    }
    rescoring_context_ = context;
    // 上文变了，缓存里的顺序是按旧上文重排出来的，同 set_sentence_association。
    // 上文只经神经重排进入候选；没有生效的神经模型时缓存与上文无关，不清。
    // 每次上屏都会换上文，无条件清会让下一个词的前几键都变成冷查询。之后
    // 打开神经重排会经 set_sentence_association 清缓存，不会沿用旧结果。
    const bool rescoring_active = (sentence_association_.neural_keyboard && neural_keyboard_model_.get() != nullptr) ||
                                  (sentence_association_.neural_desktop && neural_desktop_model_.get() != nullptr);
    if (rescoring_active)
    {
        reset_sentence_cache();
    }
}

void QuanpinDictionary::reset_cache_if_database_changed()
{
    if (db_ == nullptr)
    {
        return;
    }
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA data_version", -1, &statement, nullptr) != SQLITE_OK)
    {
        return;
    }
    if (sqlite3_step(statement) != SQLITE_ROW)
    {
        sqlite3_finalize(statement);
        return;
    }
    const sqlite3_int64 current_version = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    if (data_version_ >= 0 && current_version != data_version_)
    {
        reset_cache();
    }
    data_version_ = current_version;
}

std::vector<std::string> QuanpinDictionary::select_data(const std::string &sql_str)
{
    std::vector<std::string> candidate_list;
    if (db_ == nullptr)
    {
        return candidate_list;
    }

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql_str.c_str(), -1, &stmt, 0) != SQLITE_OK)
    {
        (void)0;
        return candidate_list;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        candidate_list.push_back(std::string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2))));
    }
    sqlite3_finalize(stmt);
    return candidate_list;
}

std::vector<WordItem> QuanpinDictionary::select_complete_data(const std::string &sql_str)
{
    std::vector<WordItem> candidate_list;
    if (db_ == nullptr)
    {
        return candidate_list;
    }

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql_str.c_str(), -1, &stmt, 0) != SQLITE_OK)
    {
        (void)0;
        return candidate_list;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        candidate_list.emplace_back(std::string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0))),
                                    std::string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2))),
                                    sqlite3_column_int64(stmt, 3), CandidateSource::Database,
                                    std::string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0))));
    }
    sqlite3_finalize(stmt);
    return candidate_list;
}

int QuanpinDictionary::check_data(const std::string &sql_str)
{
    if (db_ == nullptr)
    {
        return false;
    }

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql_str.c_str(), -1, &stmt, 0) != SQLITE_OK)
    {
        (void)0;
        return false;
    }

    const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return exists;
}

int QuanpinDictionary::insert_data(const std::string &sql_str)
{
    if (db_ == nullptr)
    {
        return ERROR_CODE;
    }

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql_str.c_str(), -1, &stmt, 0) != SQLITE_OK)
    {
        (void)0;
        return ERROR_CODE;
    }
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok ? OK : ERROR_CODE;
}

int QuanpinDictionary::update_data(const std::string &sql_str)
{
    if (db_ == nullptr)
    {
        return ERROR_CODE;
    }

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql_str.c_str(), -1, &stmt, 0) != SQLITE_OK)
    {
        (void)0;
        return ERROR_CODE;
    }
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok ? OK : ERROR_CODE;
}

int QuanpinDictionary::delete_data(const std::string &sql_str)
{
    if (db_ == nullptr)
    {
        return ERROR_CODE;
    }

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql_str.c_str(), -1, &stmt, 0) != SQLITE_OK)
    {
        (void)0;
        return ERROR_CODE;
    }
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok ? OK : ERROR_CODE;
}

std::string QuanpinDictionary::build_sql_for_creating_word(const std::string &pinyin)
{
    const auto cuts = cut_for_write(pinyin);
    if (cuts.empty())
    {
        return "";
    }

    std::string sql;
    for (size_t i = 1; i <= cuts.front().size(); ++i)
    {
        std::vector<std::string> partial(cuts.front().begin(), cuts.front().begin() + i);
        const std::string key = quanpin::join_segments(partial);
        const std::string table = quanpin::build_table_name(partial);
        const std::string each =
            fmt::format("select * from(select * from {} where key = '{}' order by weight desc)", table, key);
        sql = sql.empty() ? each : each + " union all " + sql;
    }
    return sql;
}

std::string QuanpinDictionary::build_sql_for_checking_word(const std::string &key, const std::string &value)
{
    const auto cuts = cut_for_write(key);
    if (cuts.empty())
    {
        return "";
    }
    const std::string table = quanpin::build_table_name(cuts.front());
    return fmt::format("select 1 from {} where key = '{}' and value = '{}';", table, escape_sql_text(key),
                       escape_sql_text(value));
}

std::string QuanpinDictionary::build_sql_for_inserting_word(const std::string &key, const std::string &jp,
                                                            const std::string &value)
{
    const auto cuts = cut_for_write(key);
    if (cuts.empty())
    {
        return "";
    }
    const std::string table = quanpin::build_table_name(cuts.front());
    return fmt::format("insert into {} (key, jp, value, weight) values ('{}', '{}', '{}', '{}');", table,
                       escape_sql_text(key), escape_sql_text(jp), escape_sql_text(value), 10000);
}

std::string QuanpinDictionary::build_sql_for_updating_word(const std::string &word)
{
    return build_sql_for_updating_word(remove_delimiters(pinyin_segmentation_), word);
}

std::string QuanpinDictionary::build_sql_for_updating_word(std::string pinyin, const std::string &word)
{
    pinyin = remove_delimiters(pinyin);
    const auto cuts = cut_for_write(pinyin);
    if (cuts.empty())
    {
        return "";
    }

    size_t han_cnt = HelpcodeUtils::count_han_chars(word);
    auto segments = cuts.front();
    if (segments.size() > han_cnt)
    {
        segments.resize(han_cnt);
    }

    pinyin = quanpin::join_segments(segments);
    const std::string jp = quanpin::segments_to_jianpin(segments);
    if (!do_validate(pinyin, jp, word))
    {
        return "";
    }

    const std::string table = quanpin::build_table_name(segments);
    return fmt::format("update {0} set weight = ( select MAX(weight) + 1 from {0} AS sub where sub.key = '{1}') "
                       "where key = '{1}' and value = '{2}';",
                       table, escape_sql_text(pinyin), escape_sql_text(word));
}

std::string QuanpinDictionary::build_sql_for_deleting_word(std::string pinyin, const std::string &word)
{
    pinyin = remove_delimiters(pinyin);
    const auto cuts = cut_for_write(pinyin);
    if (cuts.empty())
    {
        return "";
    }

    const std::string normalized = quanpin::join_segments(cuts.front());
    const std::string jp = quanpin::segments_to_jianpin(cuts.front());
    if (!do_validate(normalized, jp, word))
    {
        return "";
    }

    return fmt::format("delete from {} where key = '{}' and value = '{}';", quanpin::build_table_name(cuts.front()),
                       escape_sql_text(normalized), escape_sql_text(word));
}

bool QuanpinDictionary::do_validate(const std::string &key, const std::string &jp, const std::string &value)
{
    const std::string pure_key = remove_delimiters(key);
    if (pure_key.empty())
    {
        return false;
    }

    const size_t han_count = HelpcodeUtils::count_han_chars(value);
    if (jp.size() != han_count)
    {
        return false;
    }

    const auto cuts = cut_for_write(pure_key);
    if (cuts.empty())
    {
        return false;
    }

    return cuts.front().size() == han_count;
}

std::vector<WordItem> QuanpinDictionary::fuzzy_candidates(const std::string &segmentation,
                                                          metasequoia::FuzzyPinyinOptions options)
{
    std::vector<WordItem> result;
    if (!options.rules || !db_)
        return result;
    reset_cache_if_database_changed();
    const auto cache_key = "fuzzy:" + std::to_string(options.rules) + ":" + segmentation;
    if (const auto *cached = series_cache_.find(cache_key))
        return *cached;
    const auto segments = quanpin::split_segments(segmentation);
    std::size_t budget = 128;
    for (std::size_t count = segments.size(); count > 0 && budget > 1; --count)
    {
        const quanpin::Segments prefix(segments.begin(), segments.begin() + count);
        const auto paths = quanpin::fuzzy_segmentations(prefix, options, std::min<std::size_t>(64, budget));
        if (paths.empty())
            continue;
        budget -= paths.size();
        const auto rows = quanpin::query_exact_segmentations_keyed_flat(paths, db_, statement_cache_, 128);
        for (const auto &row : rows)
        {
            WordItem item(quanpin::join_segments(prefix), row.value, row.weight, CandidateSource::Database, row.key);
            item.fuzzy = true;
            result.push_back(std::move(item));
        }
    }
    series_cache_.insert(cache_key, result);
    return result;
}

std::vector<WordItem> QuanpinDictionary::query(const std::string &raw_input, const std::string &segmentation,
                                               unsigned autocorrect, metasequoia::FuzzyPinyinOptions fuzzy)
{
    // 词格要按同一套规则收模糊读音，先于 query_exact 下发。
    set_fuzzy_pinyin(fuzzy);
    // 写回路径随后按这次查询的切分重切（cut_for_write），状态要和查询一致。
    correction_aliases_enabled_ = quanpin::correction_aliases_enabled(autocorrect);
    auto result = query_exact(raw_input, segmentation, autocorrect);
    if (fuzzy.rules && !raw_input.empty())
    {
        // Keep ordinary cache slots free of preference-specific candidates.
        const auto typed =
            segmentation.empty()
                ? quanpin::join_segments(resolve_segments(raw_input, segmentation, correction_aliases_enabled_))
                : segmentation;
        // Specificity is how many typed letters a candidate accounts for, not the raw
        // length of its stored key: alternative cuts ("xi'e" for "xie", "you'di'an"
        // for "you'dian") and fuzzy variants carry apostrophes, so comparing pinyin
        // size() verbatim lifted rarer re-segmentations above the exact reading.
        const auto matched_letters = [](const WordItem &item) {
            size_t letters = 0;
            for (const char ch : item.pinyin)
            {
                letters += ch != '\'';
            }
            return letters;
        };
        result = quanpin::merge_fuzzy_candidates(std::move(result), fuzzy_candidates(typed, fuzzy), matched_letters);
    }
    // Labeling runs after every mutation (including the fuzzy merge) so the
    // returned list and the published candidate list always agree.
    if (fuzzy.rules)
        keep_literal_mixed_words_first(result, raw_input);
    mark_autocorrect_candidates(result, raw_input);
    current_candidate_list_ = result;
    return result;
}
