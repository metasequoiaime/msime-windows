#include "japanese_candidate_provider.h"
#include "../japanese/japanese_matrix_search.h"
#include "../japanese/romaji_converter.h"
#include "../quanpin/quanpin_query.h"
#include <algorithm>
#include <mutex>
#include <iterator>
#include <unordered_map>
#include "../core/data_path.h"
#include "../contracts/assets/assets.h"
#include <unordered_set>
#include <utility>

namespace
{
constexpr int kNoMutation = 0;

using SharedDecoder = std::shared_ptr<const japanese::JapaneseSentenceDecoder>;

SharedDecoder SharedSentenceDecoder(const std::string &path)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, std::weak_ptr<const japanese::JapaneseSentenceDecoder>> models;
    std::lock_guard lock(mutex);
    for (auto it = models.begin(); it != models.end();)
        it = it->second.expired() ? models.erase(it) : std::next(it);
    auto &entry = models[path];
    if (auto existing = entry.lock())
        return existing;
    auto model = std::make_shared<const japanese::JapaneseSentenceDecoder>(path);
    if (model->ready())
        entry = model;
    return model;
}

void AppendUnique(std::vector<WordItem> &items, std::unordered_set<std::string> &seen, const std::string &code,
                  const std::string &value, std::int64_t weight, CandidateSource source = CandidateSource::Database)
{
    if (!value.empty() && seen.insert(value).second)
    {
        items.emplace_back(code, value, weight, source, code);
    }
}

std::string EscapeLikePrefix(const std::string &code)
{
    std::string escaped;
    escaped.reserve(code.size() + 1);
    for (const char ch : code)
    {
        if (ch == '%' || ch == '_' || ch == '#')
            escaped.push_back('#');
        escaped.push_back(ch);
    }
    escaped.push_back('%');
    return escaped;
}
} // namespace

JapaneseCandidateProvider::JapaneseCandidateProvider(std::string db_path, std::string model_path)
    : db_path_(db_path.empty() ? quanpin::get_default_db_path() : std::move(db_path)),
      model_path_(model_path.empty()
                      ? metasequoia::path_to_utf8(metasequoia::data_file_path(metasequoia::assets::japanese_model))
                      : std::move(model_path))
{
}

JapaneseCandidateProvider::~JapaneseCandidateProvider()
{
    close_database();
}

std::vector<WordItem> JapaneseCandidateProvider::query(const QueryRequest &request)
{
    if (!request.valid || request.scheme != SchemeType::JapaneseRomaji)
    {
        return {};
    }

    std::vector<WordItem> candidates;
    std::unordered_set<std::string> seen;

    // 单独按 '-' 时给出两个候选：长音符 ー 在前，普通连字符 '-' 在后。
    // 这里直接返回，避免句子搜索在两项之间插进无关候选。
    if (request.raw_input == "-")
    {
        AppendUnique(candidates, seen, request.raw_input_with_cases, "ー", 1000000, CandidateSource::Generated);
        AppendUnique(candidates, seen, request.raw_input_with_cases, "-", 999999, CandidateSource::Generated);
        return candidates;
    }

    const auto conversion = japanese::ConvertRomaji(request.raw_input);
    const bool kana_first = japanese::IsSingleKanaConversion(conversion);

    if (kana_first)
    {
        AppendUnique(candidates, seen, request.raw_input_with_cases, conversion.hiragana, 1000000,
                     CandidateSource::Generated);
        AppendUnique(candidates, seen, request.raw_input_with_cases, japanese::HiraganaToKatakana(conversion.hiragana),
                     999999, CandidateSource::Generated);
    }

    if (!sentence_decoder_)
        sentence_decoder_ = SharedSentenceDecoder(model_path_);
    if (sentence_decoder_ && sentence_decoder_->ready())
    {
        const auto pending_kana = japanese::KanaForRomajiPrefix(conversion.pending);
        if (!conversion.hiragana.empty() && !conversion.pending.empty())
        {
            for (const auto &kana : pending_kana)
            {
                // The kana prefix already matches the typed reading. Converting it
                // back to one romaji spelling would reject nn/n' and other aliases.
                for (const auto &lemma : sentence_decoder_->PrefixLemmas(conversion.hiragana + kana, 24))
                {
                    AppendUnique(candidates, seen, request.raw_input_with_cases, lemma.surface,
                                 980000 - lemma.word_cost, CandidateSource::Database);
                }
            }
        }
        japanese::JapaneseMatrixSearch search(*sentence_decoder_);
        const auto sentences = search.SearchConverted(conversion, 12);
        size_t next_sentence = 0;
        // Once the reading is complete, convert what was typed before predicting
        // longer words: the best whole-reading path and exact lemmas, then
        // predictions, then the remaining sentence tail (other paths, kana
        // fallbacks and lemmas covering only a prefix of the reading). Keep the
        // prediction-first path above for pending romaji.
        if (conversion.pending.empty() && conversion.hiragana.size() >= 6)
        {
            std::int64_t weight_floor = 980000;
            const auto append_exact = [&](const std::string &text, std::int64_t weight) {
                const size_t before = candidates.size();
                AppendUnique(candidates, seen, request.raw_input_with_cases, text, weight, CandidateSource::Database);
                if (candidates.size() != before)
                    weight_floor = (std::min)(weight_floor, weight);
            };
            if (!sentences.empty())
                append_exact(sentences[next_sentence++].text, 900000 - sentences.front().cost);
            for (const auto &lemma : sentence_decoder_->ExactLemmas(conversion.hiragana, 16))
                append_exact(lemma.surface, 900000 - lemma.word_cost);
            // Predictions must not outweigh the conversions above them, or a
            // weight-based re-sort would undo this order.
            for (const auto &lemma : sentence_decoder_->LongerPrefixLemmas(conversion.hiragana, 16))
                AppendUnique(candidates, seen, request.raw_input_with_cases, lemma.surface,
                             (std::min)(weight_floor, 980000 - static_cast<std::int64_t>(lemma.word_cost)),
                             CandidateSource::Database);
        }
        for (; next_sentence < sentences.size(); ++next_sentence)
        {
            AppendUnique(candidates, seen, request.raw_input_with_cases, sentences[next_sentence].text,
                         900000 - sentences[next_sentence].cost, CandidateSource::Database);
        }
    }

    if (ensure_query_statement())
    {
        sqlite3_reset(query_statement_);
        sqlite3_clear_bindings(query_statement_);
        sqlite3_bind_text(query_statement_, 1, request.raw_input_with_cases.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(query_statement_, 2, request.raw_input.c_str(), -1, SQLITE_TRANSIENT);
        const std::string like_raw = EscapeLikePrefix(request.raw_input);
        const std::string like_q = EscapeLikePrefix(std::string("q") + request.raw_input);
        sqlite3_bind_text(query_statement_, 3, like_raw.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(query_statement_, 4, like_q.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(query_statement_) == SQLITE_ROW)
        {
            const auto *code = reinterpret_cast<const char *>(sqlite3_column_text(query_statement_, 0));
            const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(query_statement_, 1));
            if (code && value)
            {
                AppendUnique(candidates, seen, code, value, sqlite3_column_int64(query_statement_, 2));
            }
        }
    }

    if (!conversion.hiragana.empty() && !kana_first)
    {
        AppendUnique(candidates, seen, request.raw_input_with_cases, conversion.hiragana, 1000000,
                     CandidateSource::Generated);
        AppendUnique(candidates, seen, request.raw_input_with_cases, japanese::HiraganaToKatakana(conversion.hiragana),
                     999999, CandidateSource::Generated);
    }

    const auto dynamic = dynamic_candidates_.get(request.raw_input);
    if (dynamic.has_value())
    {
        std::size_t insertion = std::min<std::size_t>(kana_first ? 2 : 1, candidates.size());
        for (const auto &item : *dynamic)
        {
            if (seen.insert(item.word).second)
            {
                candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(insertion), item);
                ++insertion;
            }
        }
    }
    return candidates;
}

std::optional<WordItem> JapaneseCandidateProvider::find_candidate(SchemeType scheme, const std::string &key,
                                                                  const std::string &value)
{
    if (scheme != SchemeType::JapaneseRomaji || !ensure_query_statement())
        return std::nullopt;
    sqlite3_reset(query_statement_);
    sqlite3_clear_bindings(query_statement_);
    sqlite3_bind_text(query_statement_, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(query_statement_, 2, key.c_str(), -1, SQLITE_TRANSIENT);
    const std::string like_raw = EscapeLikePrefix(key);
    const std::string like_q = EscapeLikePrefix(std::string("q") + key);
    sqlite3_bind_text(query_statement_, 3, like_raw.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(query_statement_, 4, like_q.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(query_statement_) == SQLITE_ROW)
    {
        const auto *code = reinterpret_cast<const char *>(sqlite3_column_text(query_statement_, 0));
        const auto *candidate = reinterpret_cast<const char *>(sqlite3_column_text(query_statement_, 1));
        if (code && candidate && value == candidate)
            return WordItem(code, candidate, sqlite3_column_int64(query_statement_, 2), CandidateSource::Database,
                            code);
    }
    return std::nullopt;
}

void JapaneseCandidateProvider::reset_cache()
{
    close_database();
    dynamic_candidates_.clear();
    // The sentence model is immutable and shared process-wide. Keep it warm when
    // SQLite/user-dictionary caches are reset.
}

int JapaneseCandidateProvider::create_word(SchemeType, std::string, std::string)
{
    return kNoMutation;
}
int JapaneseCandidateProvider::update_weight_by_pinyin_and_word(SchemeType, std::string, std::string)
{
    return kNoMutation;
}
int JapaneseCandidateProvider::delete_by_pinyin_and_word(SchemeType, std::string, std::string)
{
    return kNoMutation;
}
int JapaneseCandidateProvider::cache_dynamic_candidate(SchemeType scheme, const std::string &code,
                                                       const std::string &word, CandidateSource source)
{
    if (scheme != SchemeType::JapaneseRomaji || code.empty() || word.empty() ||
        source != CandidateSource::CloudSuggestion)
    {
        return -1;
    }
    auto items = dynamic_candidates_.get(code).value_or(std::vector<WordItem>{});
    items.erase(
        std::remove_if(items.begin(), items.end(), [source](const WordItem &item) { return item.source == source; }),
        items.end());
    items.emplace_back(code, word, 1, source, code);
    dynamic_candidates_.insert(code, items);
    return kNoMutation;
}

int JapaneseCandidateProvider::cache_dynamic_candidate_for_request(const QueryRequest &request, const std::string &word,
                                                                   CandidateSource source)
{
    return cache_dynamic_candidate(request.scheme, request.raw_input, word, source);
}

bool JapaneseCandidateProvider::ensure_query_statement()
{
    if (query_statement_)
        return true;
    if (!db_ &&
        sqlite3_open_v2(db_path_.c_str(), &db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK)
    {
        close_database();
        return false;
    }
    constexpr const char *sql = "SELECT code, value, weight FROM japanese_lexicon "
                                "WHERE code=?1 OR code=?2 OR code LIKE ?3 ESCAPE '#' OR code LIKE ?4 ESCAPE '#' "
                                "ORDER BY weight DESC, rowid ASC LIMIT 64";
    if (sqlite3_prepare_v2(db_, sql, -1, &query_statement_, nullptr) != SQLITE_OK)
    {
        close_database();
        return false;
    }
    return true;
}

void JapaneseCandidateProvider::close_database()
{
    if (query_statement_)
    {
        sqlite3_finalize(query_statement_);
        query_statement_ = nullptr;
    }
    if (db_)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}
