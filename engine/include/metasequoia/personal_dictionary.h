#pragma once

#include "../../core/runtime_paths.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace metasequoia
{
enum class PersonalDictionaryKind
{
    Pinyin,
    // 86 五笔。
    Wubi,
    QuickPhrase,
    English,
    // 98 五笔与新世纪五笔，编码规则同 86，只是表不同。只追加在末尾，已有的值不变。
    Wubi98,
    WubiXsj,
};
struct PersonalDictionaryEntry
{
    PersonalDictionaryKind kind = PersonalDictionaryKind::Pinyin;
    // Pinyin uses complete syllables separated by apostrophes (spaces and capitals are normalized).
    // Wubi uses 1..4 letters; quick phrases use 1..32 letters/digits; English uses 1..64 letters.
    std::string key;
    std::string value;
    std::int64_t weight = 100000;
};
struct PersonalDictionaryValidation
{
    std::optional<PersonalDictionaryEntry> entry;
    std::string error;
};
PersonalDictionaryValidation validate_personal_dictionary_entry(PersonalDictionaryEntry entry);
struct PersonalDictionaryEditResult
{
    bool success = false;
    std::string error;
};
// Add (previous=null), remove (replacement=null), or replace an entry and its replay journal in
// one attached-database transaction. Replacement may change kind. An expected previous entry
// must still be a user-inserted entry with the same weight; stale edits leave all entries intact.
// Quiesce/recreate sessions around writes to invalidate candidate/query caches. As with candidate
// edits, SQL/commit failures roll back; WAL mode does not guarantee cross-file power-loss atomicity.
// Hosts handing edits across processes can supply a durable request ID (1..128 ASCII letters,
// digits, hyphens/underscores). Retrying the same normalized edit is a no-op success; reusing an
// ID for different content fails. The receipt commits in the same transaction as the edit.
PersonalDictionaryEditResult edit_personal_dictionary(const RuntimePaths &paths,
                                                      const std::optional<PersonalDictionaryEntry> &previous,
                                                      const std::optional<PersonalDictionaryEntry> &replacement,
                                                      const std::string &request_id = {});
struct PersonalDictionaryPage
{
    std::vector<PersonalDictionaryEntry> entries;
    bool has_more = false;
    std::string error;
};
// Lists user-inserted words (including automatically created words), excluding ranking-only
// operations and deleted entries. Stable kind/key/value ordering; limit 1..1000, offset <= 1000000.
PersonalDictionaryPage personal_dictionary_entries(const RuntimePaths &paths, std::size_t offset = 0,
                                                   std::size_t limit = 100);
} // namespace metasequoia
