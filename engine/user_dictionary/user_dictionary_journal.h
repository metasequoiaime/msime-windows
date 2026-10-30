#pragma once

#include <string>
#include <vector>
#include <functional>
#include <optional>
#include "../core/word_item.h"

namespace user_dictionary
{
enum class DictionaryKind
{
    Pinyin,
    Wubi,
    QuickPhrase,
    English,
};

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
bool bump_wubi_weight(const std::string &main_db_path, const std::string &user_db_path, const std::string &key,
                      const std::string &value);

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
