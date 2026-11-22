#pragma once

#include <string>
#include <vector>
#include <functional>
#include <optional>
#include "../core/word_item.h"
#include "../core/wubi_profile.h"

struct sqlite3;

namespace user_dictionary
{
enum class DictionaryKind
{
    Pinyin,
    // 86 五笔（日志里写 wubi，表 wubi86）。
    Wubi,
    QuickPhrase,
    English,
    // 98 五笔与新世纪五笔，日志里写 wubi98 / wubixsj，表名同。只追加在末尾，已有的值不变。
    Wubi98,
    WubiXsj,
};

// 五笔方案下这一版码表的调频、删词和用户词归入的种类。
DictionaryKind wubi_dictionary_kind(metasequoia::WubiProfile profile);
// 三版五笔共用编码规则（1–4 个 a–y 字母），只是表不同。
bool is_wubi_kind(DictionaryKind kind);
// msime.db 里这个种类的五笔码表；不是五笔种类时为 nullptr。
const char *wubi_table(DictionaryKind kind);
// 日志 user_dictionary_operations.dictionary 列里的种类名（pinyin、wubi、wubi98、wubixsj、quick、english）。
const char *dictionary_kind_name(DictionaryKind kind);
std::optional<DictionaryKind> dictionary_kind_from_name(const std::string &name);

std::string default_user_db_path();

// How long a journal connection waits for a lock before giving up. This is deliberately shorter than every client
// wait it can end up inside: the TSF client gives a commit reply FANY_IME_COMMIT_REPLY_TIMEOUT_MS (1500 ms) overall
// and FANY_IME_COMMIT_REPLY_SESSION_WAIT_MS (300 ms) in the one shot it takes inside an edit session, and it treats
// running out of either as a transport failure that drops the key, the queue and the composition. A wait longer than
// that window cannot end in success; all it can do is hold the caller until the client has already reset. Ending the
// wait early costs one failed read on a page that comes back a moment later, or one lost frequency adjustment, both
// invisible next to a reset composition.
constexpr int kJournalBusyTimeoutMs = 200;

// Pages of WAL after which the committing connection copies the log back into the database. SQLite's default is 1000
// pages, about 4 MB at the default 4096-byte page size; a real install's WAL was sitting at 4,132,392 bytes, i.e. right
// at the threshold, so the checkpoint was firing inside an ordinary keystroke's write. The threshold is in pages, not
// bytes, so the file size only tells us it was near the limit. Copying less at a time spreads that cost, and the
// writer runs a checkpoint of its own once its queue empties.
constexpr int kWalAutocheckpointPages = 128;

// Compatibility no-op; journal connections are operation-scoped.
void close_default_user_database();

// Copies the user journal's write-ahead log back into the database once it has grown past a few pages. The automatic
// checkpoint fires on whichever commit crosses the threshold, i.e. inside a selection the user is waiting for; running
// this while the writer has nothing queued moves that work off the keystroke path. PASSIVE never blocks a reader and
// simply does nothing while one is active, so calling it is always safe.
void checkpoint_user_journal_if_needed();

bool record_upsert(const std::string &user_db_path, DictionaryKind kind, const std::string &key,
                   const std::string &value, std::int64_t weight, const std::string &display = {});
// 用户在设置页给词库里已有的词改了权重。和 record_upsert 一样写一条 upsert，但记下这是用户
// 亲手定的值：出厂词库换过时回放照写这个权重，不像自动调频那样让位给新的出厂权重（见 replay）。
bool record_weight_edit(const std::string &user_db_path, DictionaryKind kind, const std::string &key,
                        const std::string &value, std::int64_t weight, const std::string &display = {});
bool record_user_insert(const std::string &user_db_path, DictionaryKind kind, const std::string &key,
                        const std::string &value, std::int64_t weight, const std::string &display = {});
// 自动造词先以低权重临时保留，第二次有效选用时转为正式词条。手动新增仍走 record_user_insert。
inline constexpr std::int64_t kTemporaryPinyinWordWeight = 1000;
inline constexpr std::int64_t kFormalPinyinWordWeight = 10000;
inline constexpr std::int64_t kTemporaryWordRetentionSeconds = 30 * 24 * 60 * 60;
// 日志里已有这一行的正式词条时不降级，同样返回 true；main_weight 收到主库这一行应有的权重：
// 记成临时词时是 kTemporaryPinyinWordWeight，已是正式词条时是日志记的正式权重。
bool record_automatic_pinyin_insert(const std::string &user_db_path, const std::string &key, const std::string &value,
                                    std::int64_t now, std::int64_t *main_weight = nullptr);
// true 表示命中临时状态且本次学习已处理：未过期就同事务转正，过期则刷新保留期；false 表示正式词或不存在。
bool confirm_temporary_pinyin_word(const std::string &main_db_path, const std::string &user_db_path,
                                   const std::string &key, const std::string &value);
// 用户在造词模式里亲手造出一个已作为临时词存在的词：不看保留期，直接转正。不是临时词时返回 false。
bool promote_temporary_pinyin_word(const std::string &main_db_path, const std::string &user_db_path,
                                   const std::string &key, const std::string &value);
// 清理超过保留期仍未第二次选用的自动新词。now 由调用方注入，便于测试时间边界。
int expire_temporary_pinyin_words(const std::string &main_db_path, const std::string &user_db_path, std::int64_t now);
bool record_delete(const std::string &user_db_path, DictionaryKind kind, const std::string &key,
                   const std::string &value);
bool is_user_inserted(const std::string &user_db_path, DictionaryKind kind, const std::string &key,
                      const std::string &value);
// 该键下是否有用户留下的权重（调频或造词的 upsert，不看具体词）。删除记录不算：
// 它表达的是「不要这个词」，不是对这组读音的偏好。
bool has_user_upsert_for_key(const std::string &user_db_path, DictionaryKind kind, const std::string &key);
bool ensure_user_database(const std::string &user_db_path);
// Switch an existing dictionary database (msime.db) to WAL so the key thread's
// queries never wait for a learning write's commit. The user journal does this
// on every connection it opens. Returns whether the database is now in WAL mode.
bool enable_write_ahead_log(const std::string &db_path);
bool record_pinyin_upsert_from_database(const std::string &main_db_path, const std::string &key,
                                        const std::string &value,
                                        const std::string &user_db_path = default_user_db_path());
// Raise the selected wubi candidate to the top of its own code group (max weight + 1, same
// semantics as the quanpin side) and persist the new weight plus a journal upsert in one
// attached-database transaction. Existing rows only: a missing (key,value) pair returns false
// without inserting. Row-missing behaviour matches update_wubi_weight.
// kind 选五笔版本的表（Wubi / Wubi98 / WubiXsj），不是五笔种类时返回 false。
bool bump_wubi_weight(const std::string &main_db_path, const std::string &user_db_path, const std::string &key,
                      const std::string &value, DictionaryKind kind = DictionaryKind::Wubi);
// 同上，拼音分表（key 是带分隔符的完整全拼，表名按格式契约从它算）。长词前缀补全（#1343）选中时用：
// 它的键和同页其他候选不同，不能走 adjust_candidate_ranking 按排位挪位置。
bool bump_pinyin_weight(const std::string &main_db_path, const std::string &user_db_path, const std::string &key,
                        const std::string &value);

// 用户在设置页加的字母词（T恤、A股，见 quanpin::LatinWords）。生效的那一行写在 msime.db 的 latin_words，
// 和出厂字母词同表；另在日志里单独一张 user_latin_words 记下，换词库时由 replay 写回。不进
// user_dictionary_operations：旧版本的 replay 见到不认识的 dictionary 种类会判整批失败、中止安装，
// 单独一张表旧版本根本不读，装回旧版也不受影响。出厂字母词不能在这里改或删。
struct UserLatinWord
{
    std::string key; // t'xu，见 quanpin::LatinEntry
    std::string value;
    std::int64_t weight = 0;
    bool exact_only = false;
};
enum class LatinWordWrite
{
    Written,
    Exists,  // latin_words 里已经有同样的键和词（出厂的或用户加过的）
    Missing, // 要改、要删的词不是用户加的字母词
    Failed,
};
// 逐条加进 latin_words 与日志，整批一个事务（词库是还没有这张表的旧版本时先建表）。结果与 words 一一对应；
// 事务本身失败时全部是 Failed，什么也没写。调用方先用 quanpin::parse_latin_entry 规范好键。
std::vector<LatinWordWrite> add_user_latin_words(const std::string &main_db_path, const std::string &user_db_path,
                                                 const std::vector<UserLatinWord> &words);
// 把用户加的 (old_key, old_value) 换成 word：两张表上删旧写新，一个事务。
LatinWordWrite update_user_latin_word(const std::string &main_db_path, const std::string &user_db_path,
                                      const std::string &old_key, const std::string &old_value,
                                      const UserLatinWord &word);
LatinWordWrite delete_user_latin_word(const std::string &main_db_path, const std::string &user_db_path,
                                      const std::string &key, const std::string &value);
// 日志里的全部用户字母词，按键排序。日志不存在或还没有这张表时为空。
std::vector<UserLatinWord> user_latin_words(const std::string &user_db_path);

// 快捷短语混排的调频状态。快捷短语与拼音候选的权重不在同一个量纲上，不能互相比较，所以
// 按编码记一个槽位：同码快捷短语组前面排几个普通候选（见
// metasequoia::local_modes::counts_toward_quick_phrase_slot）。没有记录即 0，组在首位。
int quick_phrase_slot(const std::string &user_db_path, const std::string &code);
// 用户选中了组里的一条快捷短语。累计到 trigger_count 次后按调频模式把组往前挪（槽位按
// ranking_target 缩小），选中的不是组内第一条时再把它的权重升到组内最高 + 1 并记日志。
// mode 为 "disabled" 时什么都不做。
bool learn_quick_phrase_selection(const std::string &main_db_path, const std::string &user_db_path,
                                  const std::string &code, const std::string &value, bool first_in_group,
                                  const std::string &mode, int linear_step, int trigger_count);
// 用户越过快捷短语组选了排在组后面的普通候选，ordinary_rank 是它在普通候选里的名次。
// 把整组当成它前面的一个位置，它按调频模式的目标位置越过这一组时，累计到 trigger_count
// 次后槽位 + 1，最多到 max_slot（首页最后一位）。
bool learn_quick_phrase_bypass(const std::string &user_db_path, const std::string &code, int ordinary_rank,
                               const std::string &mode, int linear_step, int trigger_count, int max_slot);

// 中英混输里英文候选的调频状态，思路同快捷短语：英文词库和中文词库的权重不在同一个量纲上，
// 不能比大小，所以按输入（小写）记一个槽位：首个英文候选在整个候选列表里的下标，0 是首位。
// 没有记录时由调用方决定默认位置（精确匹配紧跟第一个中文候选，补全词在首页末位）。
std::optional<int> english_slot(const std::string &user_db_path, const std::string &code);
// 用户显式置顶英文候选：直接写槽位，不走计数。
bool set_english_slot(const std::string &user_db_path, const std::string &code, int slot);
// 用户选中了占着槽位的英文候选，english_index 是它此刻在列表里的下标。累计到 trigger_count
// 次后，按调频模式从这个名次算出目标位置写成槽位，和中文调频一样可以一路升到首位。
bool learn_english_slot_selection(const std::string &user_db_path, const std::string &code, int english_index,
                                  const std::string &mode, int linear_step, int trigger_count);
// 用户越过英文候选选了排在它后面的候选。english_index / selected_index 是两者在列表里的下标，
// 选中的候选按调频模式的目标位置越过英文时，累计到 trigger_count 次后英文后退一位，最多到 max_slot。
bool learn_english_slot_bypass(const std::string &user_db_path, const std::string &code, int english_index,
                               int selected_index, const std::string &mode, int linear_step, int trigger_count,
                               int max_slot);

// 日期时间格式（rq / sj / xq 那几组）的调频状态。日期文本每天都变，不能按文字记，所以按格式 ID
// （WordItem::pinyin，见 local_modes::query_date_time）记一份整组的顺序，category 是
// local_modes::date_time_category 的分组名。Shift+T 模式和混输共用这一份：混输取的就是排第一的格式。
// default_ids 是这组格式的出厂顺序（local_modes::date_time_format_ids），后来新增的格式按它插回去。
//
// 按学到的顺序重排一组日期时间候选（learned_order 为 false 时跳过，调频关闭时用），再按固定位置摆放
// （fixed_position 置为固定的位置）。
void apply_date_time_order(const std::string &user_db_path, const std::string &category,
                           std::vector<WordItem> &candidates, bool learned_order = true);
// 用户选中了 format_id。累计到 trigger_count 次后按调频模式把它往前挪，mode 为 "disabled" 时什么都不做。
bool learn_date_time_selection(const std::string &user_db_path, const std::string &category,
                               const std::vector<std::string> &default_ids, const std::string &format_id,
                               const std::string &mode, int linear_step, int trigger_count);
// 用户显式置顶：直接挪到首位，不走计数。
bool pin_date_time_format(const std::string &user_db_path, const std::string &category,
                          const std::vector<std::string> &default_ids, const std::string &format_id);
// 固定位置写进 fixed_candidate_positions 用的 context_key，entry_key 与 value 都写格式 ID。
std::string date_time_fixed_position_context(const std::string &category);

// 繁体模式下用户选过的写法：同一个（全拼键 key, 简体底词 base_word）只留最后一次选的写法 spelling。
// 存在日志库自己的表里，不进操作日志：replay 不读它，出厂词库与完整状态导出里都没有它。
// 记下（key, base_word）→ spelling，覆盖原有记录。三项有一项为空时不写，返回 false。
bool remember_traditional_variant(const std::string &user_db_path, const std::string &key, const std::string &base_word,
                                  const std::string &spelling);
// 删掉（key, base_word）的记录；没有这条记录也返回 true。key 或 base_word 为空时不删，返回 false。
bool forget_traditional_variant(const std::string &user_db_path, const std::string &key, const std::string &base_word);
// 删掉 base_word 在所有键下的记录。base_word 为空时不删，返回 false。
bool forget_traditional_variants_of_word(const std::string &user_db_path, const std::string &base_word);
// （key, base_word）记住的写法。key 或 base_word 为空时不开日志库，返回空；没有记录、日志库打不开或查询出错时
// 也返回空。日志库被占用时查询本身不等待，直接返回空；这条线程还没有打开好连接时，打开连接那一段不在此列，
// 仍按 kJournalBusyTimeoutMs 等锁。
std::optional<std::string> traditional_variant_choice(const std::string &user_db_path, const std::string &key,
                                                      const std::string &base_word);

struct ReplayResult
{
    int applied = 0;
    int skipped = 0;
    int failed = 0;
    // 出厂词库换过时，日志里低于新出厂权重、因而保留新权重并改记进日志的自动调频行数。
    int rebased = 0;
    std::string error;
};

ReplayResult replay(const std::string &user_db_path, const std::string &main_db_path,
                    const std::string &english_db_path);

// 种子词库（按来源导入的外部词库）。
//
// 用户日志里的 dictionary_sources 记来源的名称、格式和启停/删除状态，seed_entries 记每个来源的词条，
// 同一个词可以同时属于多个来源。它们与个人日志 user_dictionary_operations 分开：种子词条不是用户
// 的编辑，不进个人列表、个人导出，也不带个人词条的合并与换库规则。
//
// 查询仍只查主词库：启用来源的词条物化进主库的拼音表和 latin_words，主库里的 seed_materialized
// 记下哪些行是种子层插进去的，停用、删除时只撤这些行，不碰出厂词条和用户自己的词。
inline constexpr const char *kSeedJournalTablesSql =
    "CREATE TABLE IF NOT EXISTS dictionary_sources("
    "id TEXT PRIMARY KEY,name TEXT NOT NULL,format TEXT NOT NULL,"
    "enabled INTEGER NOT NULL DEFAULT 1,deleted INTEGER NOT NULL DEFAULT 0,"
    "created_at INTEGER NOT NULL DEFAULT(unixepoch()),updated_at INTEGER NOT NULL DEFAULT(unixepoch()));"
    "CREATE TABLE IF NOT EXISTS seed_entries("
    "source_id TEXT NOT NULL,dictionary TEXT NOT NULL CHECK(dictionary IN ('pinyin','latin')),"
    "key TEXT NOT NULL,value TEXT NOT NULL,weight INTEGER NOT NULL DEFAULT 0,"
    "exact_only INTEGER NOT NULL DEFAULT 0,"
    "PRIMARY KEY(source_id,dictionary,key,value));"
    "CREATE INDEX IF NOT EXISTS seed_entries_by_entry ON seed_entries(dictionary,key,value);";
// 主词库里的标记表：dictionary 为 'pinyin' 或 'latin'。
inline constexpr const char *kSeedMaterializedTableSql =
    "CREATE TABLE IF NOT EXISTS main.seed_materialized("
    "dictionary TEXT NOT NULL,key TEXT NOT NULL,value TEXT NOT NULL,PRIMARY KEY(dictionary,key,value))";

// 把主库的种子层对齐到「启用来源的词条之并，去掉用户日志里自己拥有或删掉的词」：缺的插入并记标记，
// 多出来的（来源停用、删除，或用户删了这个词）撤下；词已由用户接管的只丢标记、不删行；
// 主库里本来就有、没有标记的行（出厂词条）既不覆盖也不删除。
// main_db 是主词库连接，用户日志以 journal_schema 为名附加在它上面，调用方负责事务。
// 日志还没有种子表时什么也不做，返回 true。
bool reconcile_seed_dictionaries(sqlite3 *main_db, const std::string &journal_schema, std::string &error);

bool adjust_candidate_ranking(const std::string &main_db_path, const std::string &user_db_path,
                              const std::string &context_key, const std::vector<WordItem> &ordered_candidates,
                              const std::string &entry_key, const std::string &value, const std::string &mode,
                              int linear_step, int trigger_count, bool force_top, bool *ranking_changed = nullptr,
                              DictionaryKind kind = DictionaryKind::Pinyin);
bool adjust_english_candidate_ranking(const std::string &english_db_path, const std::string &user_db_path,
                                      const std::string &context_key, const std::vector<WordItem> &ordered_candidates,
                                      const std::string &entry_key, const std::string &value, const std::string &mode,
                                      int linear_step, int trigger_count, bool force_top,
                                      bool *ranking_changed = nullptr);
// Delete an exact dictionary key and persist its tombstone in one attached-database
// transaction. Supports Pinyin, Wubi and English; false preserves candidate rows and journal operations
// on statement/commit failures (not a cross-file power-loss guarantee in WAL mode).
bool delete_dictionary_candidate(const std::string &dictionary_db_path, const std::string &user_db_path,
                                 DictionaryKind kind, const std::string &entry_key, const std::string &value);
bool delete_english_candidate(const std::string &english_db_path, const std::string &user_db_path,
                              const std::string &entry_key, const std::string &value);
bool learn_entered_english_word(const std::string &english_db_path, const std::string &user_db_path,
                                const std::string &display, std::int64_t weight = 10);
bool set_fixed_position(const std::string &user_db_path, const std::string &context_key, const std::string &entry_key,
                        const std::string &value, int position);
bool clear_fixed_position(const std::string &user_db_path, const std::string &context_key, const std::string &entry_key,
                          const std::string &value);
bool is_fixed(const std::string &user_db_path, const std::string &context_key, const std::string &entry_key,
              const std::string &value);
// Cloud/AI suggestions are normally hoisted back to their fixed slots (cloud at
// index 1, AI at index 2). Set keep_dynamic_candidate_positions when the caller
// already decided where they belong, e.g. after helpcode filtering.
void apply_fixed_positions(
    const std::string &user_db_path, const std::string &context_key, std::vector<WordItem> &candidates,
    bool include_missing,
    const std::function<std::optional<WordItem>(const std::string &, const std::string &)> &find_candidate = {},
    bool keep_dynamic_candidate_positions = false);
} // namespace user_dictionary
