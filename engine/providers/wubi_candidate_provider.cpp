#include "wubi_candidate_provider.h"
#include "../contracts/assets/assets.h"
#include "../core/data_path.h"
#include "../quanpin/quanpin_query.h"
#include "../user_dictionary/user_dictionary_journal.h"
#include <spdlog/spdlog.h>
#include <unordered_set>
#include <utility>

namespace
{
constexpr int kNoMutation = 0;
// 与 QuanpinDictionary::ERROR_CODE 同值：调用方（learn_candidate）丢弃返回值，这里只保持
// 「成功 0 / 失败非 0」的既有约定。
constexpr int kMutationFailed = -1;
// 前缀查询截断上限：候选窗每页最多 10 个，50 给固定位置/重排留足余量。1 码前缀在真实
// 词库上可命中数千行，必须截断；provider 在引擎层，不读 server 配置（分层边界）。
constexpr int kWubiQueryRowLimit = 50;
} // namespace

WubiCandidateProvider::WubiCandidateProvider(std::string db_path, metasequoia::RuntimePaths paths)
    : db_path_(db_path.empty() ? quanpin::get_default_db_path() : std::move(db_path)), paths_(std::move(paths))
{
}

WubiCandidateProvider::~WubiCandidateProvider()
{
    close_database();
}

std::vector<WordItem> WubiCandidateProvider::query(const QueryRequest &request)
{
    if (!request.valid || request.scheme != SchemeType::Wubi || request.normalized_input.empty() ||
        !ensure_query_statement())
    {
        return {};
    }

    // 上一轮可能停在 SQLITE_ROW 上（出错分支不会走完），两个语句都先归零，否则下一次 bind
    // 会以 SQLITE_BUSY 失败；reset 不清绑定，随后的 bind 覆盖全部参数。
    sqlite3_reset(query_statement_);
    sqlite3_reset(wildcard_statement_);
    if (request.wubi_z_wildcard)
    {
        // Wildcard code: z stands for one unknown letter, so the pattern is the code with each z
        // replaced by GLOB's single-character '?' and the rest anchored by '*'. GLOB rather than
        // LIKE because the code is lowercase ASCII and the key column is BINARY: a literal prefix
        // still bounds the index scan (a?* scans a--b), while LIKE would need case_sensitive_like
        // to do the same. The leading-z case (?bc*) scans the table and the weight order turns it
        // into "most frequent codes first", which is what a learner typing z wants.
        std::string pattern;
        pattern.reserve(request.normalized_input.size() + 1);
        for (const char ch : request.normalized_input)
        {
            pattern.push_back(ch == 'z' ? '?' : ch);
        }
        pattern.push_back('*');
        if (sqlite3_bind_text(wildcard_statement_, 1, pattern.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
        {
            return {};
        }
        return collect_rows(wildcard_statement_);
    }

    // 上界 = 前缀末字节 +1：五笔码字母域是 a–y（z 只在通配模式进码，上面已分流），末字节自增
    // 不会越过域边界，无需进位处理。
    std::string upper_bound = request.normalized_input;
    ++upper_bound.back();
    if (sqlite3_bind_text(query_statement_, 1, request.normalized_input.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(query_statement_, 2, upper_bound.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
    {
        return {};
    }
    return collect_rows(query_statement_);
}

std::vector<WordItem> WubiCandidateProvider::collect_rows(sqlite3_stmt *statement)
{
    std::vector<WordItem> candidates;
    // 同一个字常同时有简码与全码（工 = a / aaaa），前缀查询会把两行都带出来。按 ORDER BY 顺序
    // 只保留每个词的第一行：精确码行排在最前，调频也就落在用户实际敲的那个码上。
    std::unordered_set<std::string> seen_words;
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW)
    {
        const auto *key = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
        const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement, 1));
        if (key == nullptr || value == nullptr || !seen_words.insert(value).second)
        {
            continue;
        }
        candidates.emplace_back(key, value, sqlite3_column_int64(statement, 2));
        // 五笔码表候选显式标成 Wubi：混输组合里它们与追加的拼音候选共存，下游按候选自己的
        // 方案决定调频/删除/固定位置与上屏推进，不能再靠会话方案一刀切。
        candidates.back().scheme = SchemeType::Wubi;
    }

    if (result != SQLITE_DONE)
    {
        (void)0;
        return {};
    }
    return candidates;
}

void WubiCandidateProvider::reset_cache()
{
    close_database();
}

void WubiCandidateProvider::set_profile(metasequoia::WubiProfile profile)
{
    if (profile_ == profile)
        return;
    profile_ = profile;
    close_database();
}

int WubiCandidateProvider::create_word(SchemeType, std::string, std::string)
{
    // 在线造词是后续任务（PRD Non-goals）；设置页词典管理已覆盖手工造词。这里保持 no-op，
    // 不伪造一条无法通过词组取码规则校验的插入。
    return kNoMutation;
}

int WubiCandidateProvider::update_weight_by_pinyin_and_word(SchemeType, std::string code, std::string word)
{
    // 增量语义对齐全拼侧 build_sql_for_updating_word：新权重 = 同码组当前最高权重 + 1，
    // 选中候选升到组内首位；主库更新与 journal upsert 同事务完成，升级回放可重建。
    if (!user_dictionary::bump_wubi_weight(db_path_, journal_db_path(), code, word,
                                           user_dictionary::wubi_dictionary_kind(profile_)))
    {
        return kMutationFailed;
    }
    // 权重参与 ORDER BY，丢弃查询连接让下次查询按新权重重新排序。
    reset_cache();
    return kNoMutation;
}

int WubiCandidateProvider::delete_by_pinyin_and_word(SchemeType, std::string code, std::string word)
{
    // user_dictionary 删词本就按 DictionaryKind 泛化，五笔此前只是没有接线。
    if (!user_dictionary::delete_dictionary_candidate(db_path_, journal_db_path(),
                                                      user_dictionary::wubi_dictionary_kind(profile_), code, word))
    {
        return kMutationFailed;
    }
    reset_cache();
    return kNoMutation;
}

int WubiCandidateProvider::cache_dynamic_candidate(SchemeType, const std::string &, const std::string &,
                                                   CandidateSource)
{
    // 五笔候选全部来自码表，没有云/AI 动态候选可缓存；no-op 是终态而非缺口。
    return kNoMutation;
}

int WubiCandidateProvider::cache_dynamic_candidate_for_request(const QueryRequest &, const std::string &,
                                                               CandidateSource)
{
    // 同 cache_dynamic_candidate：码表型方案没有动态候选来源。
    return kNoMutation;
}

bool WubiCandidateProvider::ensure_query_statement()
{
    if (query_statement_ != nullptr && wildcard_statement_ != nullptr)
    {
        return true;
    }

    if (db_ == nullptr && sqlite3_open_v2(db_path_.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        (void)0;
        close_database();
        return false;
    }

    // 表名来自固定的版本表（wubi_profile_table），不是外部输入。词库里没有这一版的表（还没带 98、新世纪表的
    // 旧词库）时 prepare 失败，查询照常返回空候选，不影响别的方案。
    const std::string table = "\"" + std::string(metasequoia::wubi_profile_table(profile_)) + "\"";
    const std::string query_sql =
        "SELECT \"key\", \"value\", \"weight\" FROM " + table +
        " WHERE \"key\" >= ?1 AND \"key\" < ?2 "
        // 精确等长行优先（保住一级/二级简码），其余前缀行按权重降序；rowid 做稳定序，与现状一致。
        // key >= ?1 AND key < ?2 是范围扫描，受益于 key 上任何索引；LIMIT 只在截断处付出排序代价。
        "ORDER BY (\"key\" = ?1) DESC, \"weight\" DESC, \"key\" ASC, rowid ASC "
        "LIMIT " +
        std::to_string(kWubiQueryRowLimit);
    // 通配语句没有精确行可优先（模式里的 '?' 永远不等于真实 key 的那个字节），所以只按权重排。
    const std::string wildcard_sql = "SELECT \"key\", \"value\", \"weight\" FROM " + table +
                                     " WHERE \"key\" GLOB ?1 "
                                     "ORDER BY \"weight\" DESC, \"key\" ASC, rowid ASC "
                                     "LIMIT " +
                                     std::to_string(kWubiQueryRowLimit);
    if (sqlite3_prepare_v2(db_, query_sql.c_str(), -1, &query_statement_, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db_, wildcard_sql.c_str(), -1, &wildcard_statement_, nullptr) != SQLITE_OK)
    {
        (void)0;
        close_database();
        return false;
    }
    return true;
}

std::string WubiCandidateProvider::journal_db_path() const
{
    return metasequoia::path_to_utf8(paths_.user(metasequoia::assets::user_journal));
}

void WubiCandidateProvider::close_database()
{
    if (query_statement_ != nullptr)
    {
        sqlite3_finalize(query_statement_);
        query_statement_ = nullptr;
    }
    if (wildcard_statement_ != nullptr)
    {
        sqlite3_finalize(wildcard_statement_);
        wildcard_statement_ = nullptr;
    }
    if (db_ != nullptr)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}
