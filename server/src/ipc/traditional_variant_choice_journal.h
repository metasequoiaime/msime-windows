#pragma once

#include "engine/user_dictionary/user_dictionary_journal.h"
#include "ipc/traditional_spelling_index_with_choices.h"
#include "ipc/traditional_variant_choice_policy.h"

#include <functional>
#include <string>
#include <utility>

namespace FanyImeIpc
{
/**
 * 把对写法记录的一次改动 change 写进日志库 user_db_path：Remember 记下写法、覆盖原有记录，Forget 删掉记录。
 * 字段不齐的改动（见 IsCompleteVariantChoiceChange）不写，返回 false；其余返回写库是否成功。
 */
inline bool WriteVariantChoiceChange(const std::string &user_db_path, const VariantChoiceChange &change)
{
    if (!IsCompleteVariantChoiceChange(change))
        return false;
    if (change.action == VariantChoiceAction::Remember)
        return user_dictionary::remember_traditional_variant(user_db_path, change.key, change.word, change.spelling);
    return user_dictionary::forget_traditional_variant(user_db_path, change.key, change.word);
}

/**
 * 从日志库查写法记录的查询函数。每次调用都现取 user_db_path() 并查一次日志库，查不到时返回空
 * （见 user_dictionary::traditional_variant_choice）。
 */
inline TraditionalSpellingIndexWithChoices::RememberedSpellingLookup JournalRememberedSpellingLookup(
    std::function<std::string()> user_db_path)
{
    return [user_db_path = std::move(user_db_path)](const std::string &key, const std::string &word) {
        return user_dictionary::traditional_variant_choice(user_db_path(), key, word);
    };
}
} // namespace FanyImeIpc
