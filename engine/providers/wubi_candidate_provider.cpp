#include "wubi_candidate_provider.h"
#include "../contracts/assets/assets.h"
#include "../core/data_path.h"
#include "../quanpin/quanpin_query.h"
#include "../user_dictionary/user_dictionary_journal.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <initializer_list>
#include <unordered_set>
#include <utility>

namespace
{
constexpr int kNoMutation = 0;
// 与 QuanpinDictionary::ERROR_CODE 同值：调用方（learn_candidate）丢弃返回值，这里只保持
// 「成功 0 / 失败非 0」的既有约定。
constexpr int kMutationFailed = -1;
// 前缀查询截断上限：候选窗每页最多 10 个，50 给固定位置/重排留足余量。1 码前缀在真实
// 词库上可命中数千行，必须截断；provider 在引擎层，不读 server 配置（分层边界）。精确码行、提示码个数、
// 合并后的候选数都按这个上限截。
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

    // 码表的权重是「码内名次」（同码组里 20、10、9……），不同码之间不可比：重码多的码组名次值大，按权重
    // 跨码排序会让它们霸占提示位，新世纪表里还混着原始词频量级的行。所以只在同一个码内部按权重排，码与码
    // 之间按「各码的首选先于任何码的次选」轮流取，同一名次里短码在前、再按字母序——敲一个字母时看到的是
    // 二级简码，敲三个字母时看到的是每个四码的首选，即逐码提示。
    if (request.wubi_z_wildcard)
    {
        // Wildcard code: z stands for one unknown letter, so the pattern is the code with each z
        // replaced by GLOB's single-character '?' and the rest anchored by '*'. GLOB rather than
        // LIKE because the code is lowercase ASCII and the key column is BINARY: a literal prefix
        // still bounds the index scan (a?* scans a--b), while LIKE would need case_sensitive_like
        // to do the same. The pattern is never itself a code, so there is no exact row to lead
        // with; the shortest matching codes (same length as the input) come first.
        std::string pattern;
        pattern.reserve(request.normalized_input.size() + 1);
        for (const char ch : request.normalized_input)
        {
            pattern.push_back(ch == 'z' ? '?' : ch);
        }
        pattern.push_back('*');
        std::vector<std::string> keys;
        sqlite3_reset(wildcard_key_statement_);
        if (sqlite3_bind_text(wildcard_key_statement_, 1, pattern.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            !collect_keys(wildcard_key_statement_, keys))
        {
            return {};
        }
        return assemble({}, keys);
    }

    std::vector<Row> exact;
    if (!code_rows(request.normalized_input, kWubiQueryRowLimit, exact))
    {
        return {};
    }
    // 上界 = 前缀末字节 +1：五笔码字母域是 a–y（z 只在通配模式进码，上面已分流），末字节自增
    // 不会越过域边界，无需进位处理。下界用 >：精确码本身已在上面取过。
    std::string upper_bound = request.normalized_input;
    ++upper_bound.back();
    std::vector<std::string> keys;
    sqlite3_reset(hint_key_statement_);
    if (sqlite3_bind_text(hint_key_statement_, 1, request.normalized_input.c_str(), -1, SQLITE_TRANSIENT) !=
            SQLITE_OK ||
        sqlite3_bind_text(hint_key_statement_, 2, upper_bound.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        !collect_keys(hint_key_statement_, keys))
    {
        return {};
    }
    return assemble(std::move(exact), keys);
}

bool WubiCandidateProvider::code_rows(const std::string &code, std::size_t limit, std::vector<Row> &rows)
{
    // 上一轮可能停在 SQLITE_ROW 上（截断或出错分支不会走完），先归零，否则 bind 会以 SQLITE_BUSY 失败。
    sqlite3_reset(code_statement_);
    if (sqlite3_bind_text(code_statement_, 1, code.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
    {
        return false;
    }
    int result = SQLITE_ROW;
    while (rows.size() < limit && (result = sqlite3_step(code_statement_)) == SQLITE_ROW)
    {
        const auto *key = reinterpret_cast<const char *>(sqlite3_column_text(code_statement_, 0));
        const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(code_statement_, 1));
        if (key == nullptr || value == nullptr)
        {
            continue;
        }
        rows.push_back({key, value, sqlite3_column_int64(code_statement_, 2)});
    }
    sqlite3_reset(code_statement_);
    return result == SQLITE_ROW || result == SQLITE_DONE;
}

bool WubiCandidateProvider::collect_keys(sqlite3_stmt *statement, std::vector<std::string> &keys)
{
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW)
    {
        if (const auto *key = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)))
        {
            keys.emplace_back(key);
        }
    }
    sqlite3_reset(statement);
    return result == SQLITE_DONE;
}

std::vector<WordItem> WubiCandidateProvider::assemble(std::vector<Row> leading, const std::vector<std::string> &keys)
{
    // 每个提示码最多取剩余名额那么多行就够了：轮流取时任何一个码都轮不到更多。
    const std::size_t remaining = leading.size() < static_cast<std::size_t>(kWubiQueryRowLimit)
                                      ? static_cast<std::size_t>(kWubiQueryRowLimit) - leading.size()
                                      : 0;
    std::vector<std::vector<Row>> per_key;
    per_key.reserve(keys.size());
    std::size_t deepest = 0;
    for (const std::string &key : keys)
    {
        if (remaining == 0)
        {
            break;
        }
        std::vector<Row> rows;
        if (!code_rows(key, remaining, rows))
        {
            return {};
        }
        deepest = std::max(deepest, rows.size());
        per_key.push_back(std::move(rows));
    }

    std::vector<Row> ordered = std::move(leading);
    for (std::size_t rank = 0; rank < deepest && ordered.size() < static_cast<std::size_t>(kWubiQueryRowLimit); ++rank)
    {
        for (auto &rows : per_key)
        {
            if (rank < rows.size())
            {
                ordered.push_back(std::move(rows[rank]));
                if (ordered.size() >= static_cast<std::size_t>(kWubiQueryRowLimit))
                {
                    break;
                }
            }
        }
    }

    std::vector<WordItem> candidates;
    candidates.reserve(ordered.size());
    // 同一个字常同时有简码与全码（工 = a / aaaa），前缀查询会把两行都带出来。按上面的顺序只保留每个词
    // 的第一行：精确码行排在最前，调频也就落在用户实际敲的那个码上。
    std::unordered_set<std::string> seen_words;
    for (Row &row : ordered)
    {
        if (!seen_words.insert(row.value).second)
        {
            continue;
        }
        candidates.emplace_back(std::move(row.key), std::move(row.value), row.weight);
        // 五笔码表候选显式标成 Wubi：混输组合里它们与追加的拼音候选共存，下游按候选自己的
        // 方案决定调频/删除/固定位置与上屏推进，不能再靠会话方案一刀切。
        candidates.back().scheme = SchemeType::Wubi;
    }
    return candidates;
}

void WubiCandidateProvider::reset_cache()
{
    close_database();
    value_unavailable_ = false;
}

void WubiCandidateProvider::set_profile(metasequoia::WubiProfile profile)
{
    // 换版本就重开词库，缺表的结论一并作废。这一路不是每键都走：服务端配置没变时
    // InputSession::set_wubi_input_options 会早退，真正的每键自愈在 ensure_query_statement。
    value_unavailable_ = false;
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
    if (code_statement_ != nullptr && hint_key_statement_ != nullptr && wildcard_key_statement_ != nullptr)
    {
        // 三条查询语句就绪就证明这一版的表在词库里，反查「缺表」的结论随之作废。query() 每键都走到
        // 这里，所以补装词库后不必换版本或重启，下一键就能重新反查；只在 set_profile 里清是不够的，
        // 服务端配置没变时它根本不会被调用。前提是反查语句与候选语句始终同一条连接、同一张表：
        // 表名同出自 wubi_profile_table(profile_)，连接和语句由 close_database 一起回收。
        value_unavailable_ = false;
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
    // 码内按权重降序，rowid 做稳定序。走 (key, weight DESC) 索引，不排序。
    const std::string code_sql = "SELECT \"key\", \"value\", \"weight\" FROM " + table +
                                 " WHERE \"key\" = ?1 ORDER BY \"weight\" DESC, rowid ASC";
    // 提示码：以输入为前缀、比输入长的码，短码在前、同长按字母序。GROUP BY 沿 key 索引扫一遍范围，只对
    // 去重后的码排序；在真实词库上一个字母的前缀（约三万行）约 5ms，与原来按权重排整个范围同一量级。
    const std::string limit = " LIMIT " + std::to_string(kWubiQueryRowLimit);
    const std::string hint_sql = "SELECT \"key\" FROM " + table +
                                 " WHERE \"key\" > ?1 AND \"key\" < ?2 GROUP BY \"key\" "
                                 "ORDER BY length(\"key\") ASC, \"key\" ASC" +
                                 limit;
    // 通配语句没有精确行可优先（模式里的 '?' 永远不等于真实 key 的那个字节）；与输入等长的码最短，排在前面。
    const std::string wildcard_sql = "SELECT \"key\" FROM " + table +
                                     " WHERE \"key\" GLOB ?1 GROUP BY \"key\" "
                                     "ORDER BY length(\"key\") ASC, \"key\" ASC" +
                                     limit;
    // 反查的语句不在这里准备：它只服务候选注解，失败不能连带候选查询。见 ensure_value_statement。
    if (sqlite3_prepare_v2(db_, code_sql.c_str(), -1, &code_statement_, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db_, hint_sql.c_str(), -1, &hint_key_statement_, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db_, wildcard_sql.c_str(), -1, &wildcard_key_statement_, nullptr) != SQLITE_OK)
    {
        (void)0;
        close_database();
        return false;
    }
    value_unavailable_ = false;
    return true;
}

bool WubiCandidateProvider::ensure_value_statement()
{
    if (value_statement_ != nullptr)
    {
        return true;
    }
    if (value_unavailable_)
    {
        return false;
    }
    // 打开连接的人负责关：连接早就为候选查询开好时不能关，否则同一条连接上已准备好的三条候选语句
    // 会被一起 finalize，下一键还得重开重备。
    const bool opened_here = db_ == nullptr;
    if (opened_here && sqlite3_open_v2(db_path_.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        // 打不开多半是瞬时的（词库正被替换、被占用），不记忆，下一次再试。
        close_database();
        return false;
    }
    const std::string table = "\"" + std::string(metasequoia::wubi_profile_table(profile_)) + "\"";
    // 反查：按词取对应的码。走 idx_<table>_value 命中的行通常只有简码到全码两三条，排序代价可忽略；
    // 最长的那条就是全码。
    const std::string value_sql =
        "SELECT \"key\" FROM " + table + " WHERE \"value\" = ?1 ORDER BY length(\"key\") DESC, \"key\" ASC LIMIT 1";
    const int rc = sqlite3_prepare_v2(db_, value_sql.c_str(), -1, &value_statement_, nullptr);
    if (rc != SQLITE_OK)
    {
        value_statement_ = nullptr;
        // 词库里没有这一版的表（还没带 98、新世纪表的旧词库）是结构性失败，会一直错到底：组页循环里
        // 每行候选都会来问一次，记下来让同一页只付一次开库代价。BUSY/LOCKED/IOERR/NOMEM 这些是瞬时的，
        // 记下来会把一次抖动放大成整会话没有注解，交给下一次重试。
        value_unavailable_ = rc == SQLITE_ERROR;
        if (opened_here)
        {
            close_database();
        }
        return false;
    }
    return true;
}

std::optional<std::string> WubiCandidateProvider::full_code_for_word(const std::string &word)
{
    if (word.empty() || !ensure_value_statement())
    {
        return std::nullopt;
    }
    // 上一轮可能停在 SQLITE_ROW 上，先归零再 bind，同 code_rows。
    sqlite3_reset(value_statement_);
    if (sqlite3_bind_text(value_statement_, 1, word.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
    {
        return std::nullopt;
    }
    std::optional<std::string> code;
    if (sqlite3_step(value_statement_) == SQLITE_ROW)
    {
        if (const auto *key = reinterpret_cast<const char *>(sqlite3_column_text(value_statement_, 0)); key != nullptr)
        {
            code = key;
        }
    }
    sqlite3_reset(value_statement_);
    return code;
}

std::string WubiCandidateProvider::journal_db_path() const
{
    return metasequoia::path_to_utf8(paths_.user(metasequoia::assets::user_journal));
}

void WubiCandidateProvider::close_database()
{
    for (sqlite3_stmt **statement :
         {&code_statement_, &hint_key_statement_, &wildcard_key_statement_, &value_statement_})
    {
        if (*statement != nullptr)
        {
            sqlite3_finalize(*statement);
            *statement = nullptr;
        }
    }
    if (db_ != nullptr)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}
