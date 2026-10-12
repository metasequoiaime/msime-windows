#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace quanpin
{
using Segments = std::vector<std::string>;

struct SyllableEdge
{
    size_t end = 0;
    std::string syllable;
};

struct SyllableGraph
{
    size_t input_length = 0;
    std::vector<std::vector<SyllableEdge>> edges;
};

const std::vector<std::string> &intact_pinyin_list();
const std::unordered_set<std::string> &intact_pinyin_set();
const std::unordered_set<std::string> &prefix_pinyin_set();
bool has_only_complete_pinyin_segments(const Segments &segments);
// 0: no literal mixed match; 1: whole input; 2: one unfinished 1-2-letter syllable remains.
// Both full syllables and single-letter initials must match the candidate's canonical key.
unsigned literal_mixed_pinyin_match(const std::string &raw, const std::string &code, const std::string &canonical);
// 纯简拼字面整词：两个及以上单字母声母（a/e/o 是完整音节，不算）盖住整串输入，且逐个是
// 候选正读音节的首字母（nbn = n'b'n -> neng'bu'neng）。前缀单字和带完整音节的混合简拼不算。
bool literal_jianpin_pinyin_match(const std::string &raw, const std::string &code, const std::string &canonical);
SyllableGraph build_syllable_graph(const std::string &pinyin);
std::vector<Segments> enumerate_complete_segmentations(const SyllableGraph &graph, size_t path_limit = 32);
std::vector<std::string> cut_one_piece_greedy(const std::string &pinyin, bool intact_only);
std::vector<std::string> cut_one_piece_min_segments(const std::string &pinyin, bool intact_only);
// 音节数与 cut_one_piece_min_segments 相同的全部合法切分（最多 path_limit 条），即只在
// ban'an / ba'nan 这类等长歧义边界上分叉。cut_one_piece_min_segments 的结果总在其中。
std::vector<Segments> enumerate_min_segmentations(const std::string &pinyin, size_t path_limit);
bool is_complete_pinyin_input(const std::string &pinyin);
// 把切好的全拼串换成 Google 两个解码器（本地 googlepinyinime-rev 与 inputtools
// 云输入）认的 ü 写法：nve→nue、lve→lue、jv→ju……只在送进这两个解码器前用，
// 词库与词格查询要的是 canonical_syllable 那一侧的写法。
std::string to_google_spelling(const std::string &segmentation);
size_t detect_active_helpcode_length(const std::string &raw_input, const std::string &raw_input_with_cases);
std::string strip_active_helpcodes(const std::string &raw_input, const std::string &raw_input_with_cases);
std::string strip_active_helpcodes_with_cases(const std::string &raw_input, const std::string &raw_input_with_cases);
std::vector<Segments> sparse_pinyin_fallback_segments(const Segments &segments);

// Autocorrection type bits passed to autocorrect_cut. One mask lets the dictionary
// layer gate the whole feature with a single value while each type stays
// independently toggleable (quanpin.autocorrect_transposition / autocorrect_neighbor).
inline constexpr unsigned kAutocorrectTransposition = 1u << 0;
inline constexpr unsigned kAutocorrectNeighbor = 1u << 1;
// Deletion (one dropped letter) corrections. The server side wires this bit to
// the two existing autocorrect switches (either one on also enables deletion,
// design D2); it stays a separate bit so a future config key can gate it alone.
inline constexpr unsigned kAutocorrectDeletion = 1u << 2;
// Insertion (one extra QWERTY-neighbor letter) corrections. Same linkage model
// as deletion: the legacy switches carry this bit too (design D4 of the
// insertion task); the bit stays independent for a future config key.
inline constexpr unsigned kAutocorrectInsertion = 1u << 3;

// 掩码下手误别名层（cut_pinyin_by_mode 的 include_correction_aliases）是否生效。
// 用户可见的开关只有换位与邻键两位，删除 / 插入位只随它们联动（见 engine.cpp 的
// autocorrect_types_from_request），所以只带后两位的掩码按「关」处理。方案、词典、
// 辅助码与排序上下文 key 都按这一条判断，避免各层对同一个掩码给出不同的切分。
inline constexpr bool correction_aliases_enabled(unsigned autocorrect_types)
{
    return (autocorrect_types & (kAutocorrectTransposition | kAutocorrectNeighbor)) != 0;
}

/**
 * Integer correction-edge weights (patent CN 101133411 B, [0052]).
 *
 * The patent prescribes probability-weighted edit distances trained on user
 * data (fig. 10); until such calibration data exists it explicitly allows
 * estimated constants, which is what these are. The prior: transpositions are
 * the most common typo, deletions slightly less, insertions next (real extra-key
 * slips are rarer than drops, and the QWERTY-neighbor constraint keeps the
 * table small), neighbor substitutions carry the widest false-positive surface
 * (largest table), so they cost the most.
 * Calibration path: mine user_journal for corrected_from candidate hits.
 *
 * Ranking contract: the corrected-edge COUNT stays the primary sort key (the
 * least-intrusive-correction semantics), weights only break ties between cuts
 * with the same edge count. The engine never trades one extra correction for a
 * lower total weight.
 */
inline constexpr int kAutocorrectTranspositionWeight = 10;
inline constexpr int kAutocorrectDeletionWeight = 11;
// Between deletion and neighbor: insertion slips are estimated rarer than
// drops (patent [0052] estimated constant), and the QWERTY-neighbor constraint
// keeps the table small.
inline constexpr int kAutocorrectInsertionWeight = 12;
inline constexpr int kAutocorrectNeighborWeight = 13;

// Out-of-table generated shapes (non-neighbor substitutions, see
// CorrectionTarget::generated): fixed expensive-tier weight.
// These slips are rarer than any in-table shape, and the ranking contract
// requires them to never displace a table hit on the same wrong key. ponytail:
// flat constant; calibration path = user_journal confusion priors (direction
// task phase 3).
inline constexpr int kAutocorrectGeneratedShapeWeight = 15;

// One segment of an autocorrect-aware cut. syllable is the canonical (possibly
// table-corrected) text used for dictionary lookups; raw_text/start describe the
// original letters the segment consumed so the preedit can keep showing what the
// user typed while separators follow the actual cut positions. Deletion edges
// consume one letter less, insertion edges one letter more, than the syllable
// they produce, so raw_text may differ in length from syllable
// ("zhng" -> zhang, "sghang" -> shang).
struct AutocorrectCutSegment
{
    std::string syllable;
    std::string raw_text;
    size_t start = 0;
    bool corrected = false;
    // 这一段纠正的表权重（kAutocorrect*Weight，生成式为 kAutocorrectGeneratedShapeWeight），
    // 未纠正为 0。用来区分结构性手误（换位、漏字、多字）与邻键、生成式这类噪声面大的纠正。
    int weight = 0;
};

struct AutocorrectCut
{
    std::vector<AutocorrectCutSegment> segments;
    // Edit cost of this cut, matching the k-best ranking key: number of
    // corrected edges first, then the summed correction weights. Two cuts have
    // the same cost iff both fields are equal. The query layer uses this to keep
    // frequency disambiguation within a single cost tier -- a strictly cheaper
    // reading (transposition gau -> gua, weight 10) must lead a costlier one
    // (neighbor gau -> gai, weight 13) regardless of dictionary frequency.
    size_t edge_count = 0;
    int weight = 0;
    // 读法里有生成式空间的边（静态表之外的远键替换）。合法输入上的
    // 纠错（legal_input_correction_cuts）不认这种读法。
    bool has_generated = false;

    bool empty() const
    {
        return segments.empty();
    }

    // Same edit cost = same tier for frequency disambiguation.
    bool same_cost_as(const AutocorrectCut &other) const
    {
        return edge_count == other.edge_count && weight == other.weight;
    }
};

// Range-carrying variant of autocorrect_cut: identical gating (no type enabled,
// manual delimiters, overlong input) and identical cost model (it is the k=1
// projection of autocorrect_cut_kbest), but every segment also reports which raw
// letters it replaced.
AutocorrectCut autocorrect_cut_detail(const std::string &pinyin, unsigned autocorrect_types);

// Cuts the input into syllables, allowing at most kMaxAutocorrectEdges correction
// edges from the tables selected by autocorrect_types. Returns {} when no type is
// enabled, the input contains a manual delimiter, or no correction path exists.
// This is the k=1 projection of autocorrect_cut_kbest.
Segments autocorrect_cut(const std::string &pinyin, unsigned autocorrect_types);

// Up to k ranked correction cuts of the input: per-position top-k hypothesis
// propagation (label-correcting left-to-right sweep) over the same syllable
// graph and gating as autocorrect_cut, but keeping ambiguous table entries
// alive as parallel hypotheses. Ranking key: (corrected edge count, summed
// edge weight, generation order = table order); hypotheses explaining the same
// syllable sequence are deduplicated, keeping the best-ranked one. Static
// priority: generated-containing hypotheses rank behind every static one, and
// when the top cut is pure static all generated cuts are dropped before
// returning -- in-table inputs get exactly the pre-generated-space cut set,
// and out-of-table shapes surface only when no static cut explains the input.
//
// One caveat that the ranking guarantee does not cover: the generated space hangs
// its out-of-table SUBSTITUTIONS off kAutocorrectNeighbor, so that bit no longer
// maps one-to-one onto a table. An input with no static cut at all is now
// reachable through the neighbour bit alone -- "sahng" reads as "sa'ang", "shng"
// as "sang". Callers must not read a bit as "only this family of typo".
//
// Scope of that: the BARE-bit readings above are unreachable in the product,
// because engine.cpp always pairs the deletion and insertion bits with the legacy
// switches, so the only masks that ever occur are 0 / 0xd / 0xe / 0xf. The
// generated space itself is NOT unreachable: 0xe and 0xf still carry the
// neighbour bit, so it does add user-visible recall (far substitutions under
// 0xe/0xf). That recall is the intended gain, pinned end-to-end by
// engine/tests/src/test_input_session.cpp ("shatg" -> shang).
// The head+jianpin-tail fallback in quanpin_dictionary.cpp additionally masks the
// neighbour bit off, which suppresses generated substitutions on that path only.
// Every returned cut contains at least one corrected edge, so a fully legal input
// with no correction reading yields an empty vector (the caller owns the plain
// segmentation). This is the query-time disambiguation surface of CN 101133411
// B: ambiguity survives the cut layer and is settled by dictionary frequency.
// k is a policy knob: the production caller pins kAutocorrectCutKBest (see
// quanpin_dictionary.cpp) tuned by evaluation; tests use the small default
// below to keep their assertions focused.
std::vector<AutocorrectCut> autocorrect_cut_kbest(const std::string &pinyin, unsigned autocorrect_types,
                                                  std::size_t k = 3);

// 合法输入上的手误读法：普通切分全是合法音节时，resolve_series_query 不进上面的
// 纠错（那条路按「切不成合法音节」设闸），可 ziazheliya 切成 zi'a'zhe'li'ya、
// jioa 切成 ji'o'a、jiuzheeyang 切成 jiu'zhe'e'yang 都合法，纠错表却各有一种读法
// （zia -> zai 换位、jioa -> jiao 换位、zhee -> zhe 多字）。这里只负责列出读法，
// 不判断哪条对：
// - 静态表（换位、多字、漏字，不含邻键与生成式空间）在原始字母上给出的 k-best
//   读法，至多 kMaxLegalInputCorrections 处纠正，且纠正的那段必须盖住字面切分里
//   某个非首位零声母音节（zi'a 的 a、jiu'zhe'e'yang 的 e）——挤出或多出的元音是
//   合法输入上手误的标志，没有它 k-best 会跨边界重切任何普通输入；
// - 罕见合法音节换位成常用音节（lia -> lai、dia -> dai），k-best 只给非法片段找
//   纠正、走不到这一种。只在多音节输入里认，单打 lia 多半就是要「俩」；跟随换位开关。
// 取舍交给查询层按整句打分（语言模型分 + 每处纠正的手误代价）。返回至多 k 条，每条
// 至少一处纠正；segments 必须是 pinyin 的逐字母切分，否则返回空。
std::vector<AutocorrectCut> legal_input_correction_cuts(const std::string &pinyin, const Segments &segments,
                                                        unsigned autocorrect_types, std::size_t k);

// 纠错切分里音节前缀与 reading 相同的那条（k-best 次序里最靠前的），找不到返回空。
// 用来把一个纠错读音对回原始字母：它的各段 start / raw_text 就是读音每个音节实际
// 盖住的原始区间。
std::optional<AutocorrectCut> correction_cut_for_reading(const std::string &raw_letters, const Segments &reading,
                                                         unsigned autocorrect_types);

// 选中的读音来自纠错切分时，它在原始输入里实际盖住了几个字母。多字、漏字纠错的读音
// 与原始字母不等长（buuhui -> bu'hui，zhngguo -> zhang'guo），按读音长度消耗会剩下
// 或多吞字母：多打一个 u 时选「不会」会剩个 i，选「张」会把 zhngg 一起吞掉。这里在
// 纠错切分里找音节前缀与读音相同的那条，用它记录的原始区间。整串是合法拼音且读音
// 就是原始字母前缀、或找不到对应切分时返回空，调用方照旧按读音长度消耗（换位纠错
// 长度不变，也走那条路）。raw_letters 是不含分隔符的原始字母。
std::optional<size_t> corrected_reading_raw_length(const std::string &raw_letters, const Segments &reading,
                                                   unsigned autocorrect_types);

// True for a leading initial followed by complete syllables ("ywen" = y + wen),
// or one or more legal syllables plus at most one trailing letter ("zheg" = zhe + g)
// These jianpin-intent shapes are not typos
// Repeated leading initials are included ("sshang" = s + shang)
// Deliberately NOT true for all-consonant strings of 3+ letters: the engine
// has no multi-letter jianpin, so correction is the only useful reading of e.g.
// "bqng" -> bang. Inputs with manual delimiters return false; the correction path
// excludes them on its own.
//
// Who enforces it: the CALLERS, not the search above. autocorrect_cut,
// autocorrect_cut_kbest and autocorrect_cut_detail never consult this predicate, so
// calling them directly on a jianpin shape still cuts one -- "zheg" reads as "zhei"
// through the generated space, "zher" as "zhe" through the insertion table (both
// pinned by engine/tests/src/test_pinyin.cpp). Both production callers apply the
// guard before reaching the search: quanpin_dictionary.cpp (query layer) and
// input_session_composition.cpp (preedit layer). A new caller has to do the same;
// none of these signatures carries the constraint, so omitting it fails silently
// rather than at compile time.
bool looks_like_jianpin_spelling(const std::string &pinyin);

} // namespace quanpin
