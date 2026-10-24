#pragma once

#include "engine/user_dictionary/user_dictionary_journal.h"
#include "ipc/traditional_variant_choice_policy.h"

#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace FanyImeIpc
{
/**
 * 繁体写法记录（日志库 traditional_variant_choices 表）在内存里的副本，可以从多个线程同时调用。
 * 整张表由调用方读出来交给 Load，之后的改动由调用方写库的同时用 Apply、ForgetWord 同步到这里。
 *
 * 使用顺序：ClaimLoad 返回真的调用方负责读表，读成功交给 Load，读失败调 AbandonLoad。
 * 调用方要保证 ClaimLoad 之前做的改动，写库都排在这次读表之前；这些改动只改内存，载入时以表为准。
 * ClaimLoad 之后、Load 之前的改动立即可查，Load 时按发生顺序重放在读到的内容之上，读到的旧内容盖不掉它们。
 */
class TraditionalVariantChoiceStore
{
  public:
    /** （key, word）记住的写法；没有记录时返回空。 */
    std::optional<std::string> Find(const std::string &key, const std::string &word) const
    {
        std::lock_guard lock(mutex_);
        const auto found = choices_.find({key, word});
        if (found == choices_.end())
            return std::nullopt;
        return found->second;
    }

    /** 字段不齐的改动（见 IsCompleteVariantChoiceChange）不起作用。 */
    void Apply(const VariantChoiceChange &change)
    {
        if (!IsCompleteVariantChoiceChange(change))
            return;
        std::lock_guard lock(mutex_);
        ApplyLocked(change);
        if (load_in_flight_)
            edits_during_load_.emplace_back(change);
    }

    /** 删掉底词 word 在所有键下的记录。word 为空时不起作用。 */
    void ForgetWord(const std::string &word)
    {
        if (word.empty())
            return;
        std::lock_guard lock(mutex_);
        ForgetWordLocked(word);
        if (load_in_flight_)
            edits_during_load_.emplace_back(ForgottenWord{word});
    }

    /**
     * 还没载入、也没有别的调用方正在读表时返回真，调用方负责读表并交给 Load 或 AbandonLoad；
     * 其余时候返回假。
     */
    bool ClaimLoad()
    {
        std::lock_guard lock(mutex_);
        if (loaded_ || load_in_flight_)
            return false;
        load_in_flight_ = true;
        return true;
    }

    /**
     * 以 rows 为整张表的内容，再按发生顺序重放 ClaimLoad 之后的改动。只在 ClaimLoad 返回真之后起作用，
     * 所以整张表只载入一次，载入之后的改动不会被再来的 Load 冲掉。
     */
    void Load(const std::vector<user_dictionary::TraditionalVariantChoice> &rows)
    {
        std::lock_guard lock(mutex_);
        if (!load_in_flight_)
            return;
        choices_.clear();
        for (const auto &row : rows)
            choices_[{row.key, row.base_word}] = row.spelling;
        for (const auto &edit : edits_during_load_)
        {
            if (const auto *change = std::get_if<VariantChoiceChange>(&edit))
                ApplyLocked(*change);
            else
                ForgetWordLocked(std::get<ForgottenWord>(edit).word);
        }
        edits_during_load_.clear();
        load_in_flight_ = false;
        loaded_ = true;
    }

    /**
     * 读表失败：副本保持现状，不算载入，下一次 ClaimLoad 重新返回真。这次 ClaimLoad 之后的改动，写库都已排在
     * 下一次读表之前，下一次载入不再重放它们。
     */
    void AbandonLoad()
    {
        std::lock_guard lock(mutex_);
        edits_during_load_.clear();
        load_in_flight_ = false;
    }

  private:
    struct ForgottenWord
    {
        std::string word;
    };

    void ApplyLocked(const VariantChoiceChange &change)
    {
        if (change.action == VariantChoiceAction::Remember)
            choices_[{change.key, change.word}] = change.spelling;
        else
            choices_.erase({change.key, change.word});
    }

    void ForgetWordLocked(const std::string &word)
    {
        for (auto it = choices_.begin(); it != choices_.end();)
            it = it->first.second == word ? choices_.erase(it) : std::next(it);
    }

    mutable std::mutex mutex_;
    // （全拼键, 简体底词）→ 写法。
    std::map<std::pair<std::string, std::string>, std::string> choices_;
    std::vector<std::variant<VariantChoiceChange, ForgottenWord>> edits_during_load_;
    bool loaded_ = false;
    bool load_in_flight_ = false;
};
} // namespace FanyImeIpc
