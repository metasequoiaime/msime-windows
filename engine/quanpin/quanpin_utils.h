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
// j/q/x/y 后面的 v 只是 u 的别名（jv、jve、xvan、yvn……）：这几个声母没有保留鱼眼的
// 合法音节，词库、解码器和双拼都只认 u 写法。
bool is_jqxy_v_alias(const std::string &syllable);
// ü 的两种写法归到词库存的那一种：j/q/x/y 系写 u（xvan→xuan），l/n 系的 nue/lue 写 v
// （→nve/lve），其余原样返回。nu/lu 是真实音节，n/l 系只改整段等于 nue/lue 的。词库查询、
// 词格和用户词入库共用这一条规则，别再各自维护别名表。
std::string canonical_umlaut_spelling(std::string syllable);
// 把切好的全拼串换成 Google 两个解码器（本地 googlepinyinime-rev 与 inputtools
// 云输入）认的 ü 写法：nve→nue、lve→lue、jv→ju……只在送进这两个解码器前用，
// 词库与词格查询要的是 canonical_umlaut_spelling 那一侧的写法。
std::string to_google_spelling(const std::string &segmentation);
size_t detect_active_helpcode_length(const std::string &raw_input, const std::string &raw_input_with_cases);
std::string strip_active_helpcodes(const std::string &raw_input, const std::string &raw_input_with_cases);
std::string strip_active_helpcodes_with_cases(const std::string &raw_input, const std::string &raw_input_with_cases);
std::vector<Segments> sparse_pinyin_fallback_segments(const Segments &segments);

// Autocorrection type bits passed to autocorrect_cut. One mask lets the dictionary
// layer gate the whole feature with a single value. Each bit has its own user-facing
// switch (quanpin.autocorrect_transposition / autocorrect_neighbor /
// autocorrect_deletion / autocorrect_insertion) and none of them implies another.
inline constexpr unsigned kAutocorrectTransposition = 1u << 0;
inline constexpr unsigned kAutocorrectNeighbor = 1u << 1;
// Deletion (one dropped letter) corrections, e.g. shng -> shang.
inline constexpr unsigned kAutocorrectDeletion = 1u << 2;
// Insertion (one extra QWERTY-neighbor letter) corrections, e.g. sjhang -> shang.
inline constexpr unsigned kAutocorrectInsertion = 1u << 3;

// 开关 -> 掩码的唯一实现。四个用户可见开关一一映射到各自的位，互不联动：开哪个开关
// 就只有哪个位，全关时恒为 0。产品可达的掩码因此是四位（0x1 / 0x2 / 0x4 / 0x8）的全部
// 组合，包括只带漏字位或多字位的掩码。
//
// 调用点（都只是适配，不再各写一份规则）：`engine.cpp` 的
// autocorrect_types_from_request 把 QueryRequest 的四个 bool 传进来；
// `input_session_composition.cpp` 的 QuanpinAutocorrectTypes 同理给预编辑用；
// `server/src/session/engine_input_session.cpp` 把四个配置项传进来再下发给会话。
inline constexpr unsigned autocorrect_types_from_switches(const bool transposition, const bool neighbor,
                                                          const bool deletion, const bool insertion)
{
    return (transposition ? kAutocorrectTransposition : 0u) | (neighbor ? kAutocorrectNeighbor : 0u) |
           (deletion ? kAutocorrectDeletion : 0u) | (insertion ? kAutocorrectInsertion : 0u);
}

// 掩码下手误别名层（cut_pinyin_by_mode 的 include_correction_aliases）是否生效。
// 别名层是「换位形状 / 多一个 h / 漏一个 g」这类拼写层改写，只归换位与邻键两个开关管，
// 漏字 / 多字开关独立后也不带别名层：只带后两位的掩码按「关」处理。方案、词典、
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
// 纠错键的长度上限。生成器（server/scripts/generate_quanpin_autocorrect.py 的
// MAX_KEY_LENGTH）与 autocorrect_cut_kbest 的逐片扫描都用它，两边必须一致：最长
// 合法音节是 6 字母，更长的键在任何查询路径上都查不到（k-best 扫描封顶 6，长句的
// 容错槽位也只会切出 <=6 的音节）。下限 3 只由生成器的 MIN_KEY_LENGTH 把关——运行期
// 对更短的键本来就不做改写（2 字母串属于简拼空间），这边没有可表达它的约束。
// 漂移不会编译失败，只会静默少召回或多召回，改这里要连生成器一起改。
inline constexpr std::size_t kAutocorrectMaxKeyLength = 6;

// 对齐筛选用的过采样上界：legal_input_correction_cuts 和 correction_cut_for_reading
// 都是「先多取几条 k-best，再从中筛出目标读音」，9 是经验上够筛的值。
// 与 quanpin_dictionary.cpp 的 kAutocorrectCutKBest（生产路径上真正交给查询层的 k）
// 不是一回事，不要混用：那个决定召回多少条备选切分，这个只决定筛选的视野。
inline constexpr std::size_t kAutocorrectAlignKBest = 9;

inline constexpr int kAutocorrectTranspositionWeight = 10;
inline constexpr int kAutocorrectDeletionWeight = 11;
// Between deletion and neighbor: insertion slips are estimated rarer than
// drops (patent [0052] estimated constant), and the QWERTY-neighbor constraint
// keeps the table small.
inline constexpr int kAutocorrectInsertionWeight = 12;
inline constexpr int kAutocorrectNeighborWeight = 13;

// 句中简拼展开（单字母声母 -> 它开头的全拼音节，shu'r'fa 的 r -> ru）。这是意图性
// 规则展开，不是有限错拼对：单字母属于简拼空间，autocorrect_table.h 的准入规则
// （key 为 >=3 字母的非合法拼音串）不收它，运行时按 intact_pinyin_list() 现算，
// 不进表也不动生成器。排序价位定在 insertion(12)/neighbor(13) 与 generated(15)
// 之间：展开不是手误（用户就是想打简拼），但它一次引入整组候选，噪声面比任何单字母
// 手误都宽，比邻键贵一档；又比生成式远键便宜（首字母是用户敲的，意图明确）。
// 这个价位只作序数说明：容错词格里的展开代价不走 kAutocorrect*Weight，按
// quanpin_dictionary.cpp 的 kJianpinExpandPenaltyLog 单独折算。

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
// Scope of that: the four switches are independent, so every combination of the
// four bits (0..0xf) is a mask a user can reach -- the bare-bit readings above are
// live user states now, not probes. The generated space therefore also adds
// user-visible recall under the neighbour bit (far substitutions), pinned
// end-to-end by engine/tests/src/test_input_session.cpp ("shatg" -> shang).
// The head+jianpin-tail fallback in quanpin_dictionary.cpp runs the head with the
// full mask too, so a neighbour-shaped typo in the head composes with a jianpin
// tail ("shabgz" -> shang + z) and is ranked by edit cost exactly like the
// structural typos. It does not compose the generated reads, though: a top cut
// marked has_generated is rejected (this ranking guarantee makes that exactly "no
// static cut explains the head"), so a generated read never rewrites a head the
// static tables can explain ("huanzh" -> huan + zh, not huang + h via the
// generated z->g substitution). On top of that, the fallback skips the whole
// composition when the input already has a self-consistent plain reading -- a
// legal jianpin tail (prefix, not intact) whose head passes
// looks_like_jianpin_spelling ("xish" = xi + sh, "xibd" = xi + b + d) --
// applying to the head the same jianpin-shape gate the whole-string path applies
// to the input. That guard, not the ranking, is what keeps the static neighbour
// table from splitting an intended "sh"/"zh" tail or turning a trailing jianpin
// initial into a final ("xibd" must not read xin + d), i.e. from rewriting
// correctly spelled input.
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

// ---- 长句容错槽位模型（方案 C：engine 级联合搜索） ----
//
// 贪心切分把原始字母切成槽位；每个槽有一个确定性的基础读法（合法音节、简拼尾或
// 空读法），词格展开时可以给非法槽换一种读法（纠错表目标或简拼展开）。槽模型只
// 描述「哪里可能读错」，不产生候选读音——候选由词格层在词典命中处现算。

// 一个槽的容错类型。determining 决定基础读法；可展开的槽允许词格在词典行里给它
// 换一种读音（行键在该槽的位置上与基础读法不同）。
enum class FaultSlotKind
{
    // 合法完整音节：基础读法固定，不展开。
    Determining,
    // 单字母声母：基础读法是它自己（简拼），词格可把它展开成同首字母全拼（词典行键在此位不等于它）。
    SingleInitial,
    // 多字母非法串（rfa、neicjiaol）：无基础读法；词格可用纠错表目标音节在此位开头。
    Fault,
};

struct FaultSlot
{
    // 该槽盖住的原始字母段（小写）。
    std::string raw_text;
    // 基础读法：Determining/SingleInitial 槽为字面切分音节；Fault 槽为空。
    std::string base_syllable;
    FaultSlotKind kind = FaultSlotKind::Determining;
};

// 字面贪心切分 -> 槽序列。任何输入都切得出（切分器对残余字母按简拼尾解释），
// 返回空只当 pinyin 为空或含手动分隔符。
std::vector<FaultSlot> build_fault_slots(const std::string &pinyin);

// 非法槽的容错读法起点（autocorrect_table 命中）：这段原始字母能纠成哪些合法音节，
// 附各目标按启用位折算的纠错权重（kAutocorrect*Weight）。只列表目标，简拼展开在词格层
// 现算。types 为 0 或无命中返回空。
std::vector<std::pair<std::string, int>> fault_slot_correction_targets(const std::string &raw_text,
                                                                       unsigned autocorrect_types);

// 该串是否值得走容错词格仲裁：字面切分含非法槽（不能整句解码）、槽多于短串
// 快速路径的上限（kMaxShortAutocorrectSyllables）、掩码开着、无手动分隔符。
bool fault_slots_eligible(const std::vector<FaultSlot> &slots, unsigned autocorrect_types);

// 胜出读音是否只是把单字母槽展开成同首字母的完整音节、合法音节原样保留。true 表示
// 这次改写属于简拼展开，不是拼写纠错，候选窗不应显示纠正标记。槽数与读音音节数
// 不一致时按 false 处理，宁可多打标记也不漏打。
bool fault_winner_is_pure_jianpin(const std::vector<FaultSlot> &slots, const Segments &winner);

// 短串（<= 该槽位数的切分）继续走 autocorrect_cut_kbest 旧路径，行为零变。
inline constexpr std::size_t kMaxShortAutocorrectSyllables = 4;
// 容错词格的槽位上限：超过它的长句每槽展开的组合面太大，仲裁放弃（保持字面切分）。
inline constexpr std::size_t kMaxFaultSlotCount = 20;

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

// 漏字变体形态判定：reading 能从 raw_text 删掉恰好一个字母（保序子序列）得到。
// loose scan（yang -> yan）的边生成、胜出读音的消耗对齐和分段显示三处共用这一条，
// 谁也不能比别处宽：宽了会把无关同首字母音节当成变体，窄了胜出读音对不回原串。
bool is_deletion_variant(const std::string &raw_text, const std::string &reading);

// 容错槽位读音对回原始字母：reading 逐音节对槽位走——Determining 槽必须等于基础读法
// （ü 归一后比较），SingleInitial 槽认简拼读法（等于槽字母）或同首字母的展开音节，
// Fault 槽认任意合法音节。全部对上时返回盖住的原始字母数，否则空。调用方照旧按
// 读音长度消耗。correction_cut_for_reading 对不上这类读音（简拼展开不在 k-best 切分
// 里），选中长句容错候选的消耗对齐走这里。
std::optional<size_t> fault_reading_raw_length(const std::string &raw_letters, const Segments &reading);

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
