#pragma once
#include "scheme_type.h"
#include "word_item.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace metasequoia
{
// Editing intents a platform frontend maps its own key events onto. Printable input goes through
// handle_character instead, so this covers only the commands that act on an existing composition.
enum class Command
{
    Backspace,
    CommitCandidate,
    CommitRaw,
    Cancel,
    MoveLeft,
    MoveRight,
    MoveHome,
    MoveEnd,
    DeleteForward,
};

// Outcome of one dispatched key or selection. `handled` tells the frontend whether to swallow the
// event, and `commit` carries the text the frontend should insert into the client application.
struct KeyResult
{
    bool handled = false;
    std::optional<std::string> commit;
    std::optional<std::string> diagnostic;
};

enum class CandidateEdge
{
    FirstHan,
    LastHan,
};

enum class FrequencyAdjustmentMode
{
    Disabled,
    Pin,
    Halve,
    Linear,
    Promote,
};

struct FrequencyAdjustmentOptions
{
    FrequencyAdjustmentMode mode = FrequencyAdjustmentMode::Disabled;
    int trigger_count = 1;
    int linear_step = 1;
};

enum class LocalInputMode
{
    None,
    // U 模式：部件拆字反查（输入各部件拼音连写，候选出汉字）。
    Radical,
    DateTime,
    QuickPhrase,
    Emoji,
    Kaomoji,
    SuperJianpin,
    TemporaryEnglish,
    TemporaryJapanese,
};

struct LocalModeOptions
{
    // Shift+U 部件拆字反查。
    bool radical = true;
    bool date_time = true;
    // Shift+K 快捷短语模式，按编码前缀查。
    bool quick_phrase = true;
    // 全拼/双拼里把编码精确匹配的快捷短语混进候选；五笔不混。与 K 模式各自开关。
    bool quick_phrase_candidates = true;
    // 快捷短语参与调频：选择会把整组前移，越过它选普通候选会把它后移。关掉时组固定在首位。
    bool quick_phrase_frequency = true;
    bool emoji = true;
    bool kaomoji = true;
    bool super_jianpin = true;
    bool temporary_english = true;
    bool temporary_japanese = true;
};

struct EnglishInputOptions
{
    bool mixed_candidates = false;
    std::size_t minimum_prefix = 2;
};

struct MixedExpressiveOptions
{
    bool emoji_candidates = false;
    bool kaomoji_candidates = false;
};

struct WubiInputOptions
{
    // 五笔拼音混输：同一串字母同时交给五笔码表和全拼，五笔候选在前、拼音候选按权重追加在后，
    // 两个表都答得出的词只留五笔那份。与 z 键角色是两个独立设置。
    bool mixed_pinyin = false;
    // Treat z as a wildcard in the code (any letter at that position, first position included).
    // Independent of mixed input: there z is an ordinary pinyin letter, here it is a wildcard.
    bool z_wildcard = false;
};

// Immutable description of the current composition for asynchronous providers. Frontends copy
// this value into a request and return it unchanged with the result; InputSession revalidates it
// against the live composition before changing candidates.
struct OnlineQuery
{
    SchemeType scheme = SchemeType::Quanpin;
    std::uint64_t generation = 0;
    std::string identity;
    std::string query_text;
    std::string cache_key;
    std::vector<std::string> pinyin_segments;
    bool cloud_eligible = false;
    bool ai_eligible = false;
    // Appended to retain source compatibility with older aggregate initializers.
    std::uint64_t session_id = 0;
};

} // namespace metasequoia
