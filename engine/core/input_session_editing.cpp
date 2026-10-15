#include "input_session.h"
#include "../contracts/date_time_input.h"
#include "../contracts/direct_helpcode.h"
#include "../shuangpin/shuangpin_query.h"
#include "../shuangpin/shuangpin_utils.h"
#include <algorithm>
#include <cctype>

namespace metasequoia
{
std::string InputSession::editing_text() const
{
    if (dedicated_english_mode_)
        return dedicated_english_preedit_;
    if (local_input_mode_ == LocalInputMode::TemporaryJapanese)
        return "R" + engine_.get_request().raw_input_with_cases;
    if (local_input_mode_ != LocalInputMode::None)
        return local_preedit_;
    return get_pinyin_sequence_with_cases();
}

std::size_t InputSession::caret_position() const
{
    const auto size = editing_text().size();
    return std::min(caret_.value_or(size), size);
}

namespace
{
// Unit boundaries of a quanpin spelling in raw coordinates. The visible preedit
// (BuildQuanpinAutocorrectDisplay) is always rebuilt from the raw letters -- the
// autocorrect cut and the alias layer only move or add separators -- so a
// separator in the display marks where the next raw unit starts. A display that
// cannot explain the raw letters yields no boundaries and the caller falls back
// to single-character editing.
std::vector<std::size_t> QuanpinRawBoundaries(const std::string &raw, const std::string &display)
{
    std::vector<std::size_t> boundaries;
    std::vector<std::size_t> raw_letter_offsets;
    raw_letter_offsets.reserve(raw.size());
    for (std::size_t index = 0; index < raw.size(); ++index)
    {
        if (raw[index] != '\'')
        {
            raw_letter_offsets.push_back(index);
        }
    }
    if (raw_letter_offsets.empty())
    {
        return boundaries;
    }

    boundaries.push_back(0);
    std::size_t letters_seen = 0;
    for (const char ch : display)
    {
        if (ch != '\'')
        {
            ++letters_seen;
            continue;
        }
        // The next unit starts at the raw offset of the next letter; a
        // trailing separator has no next letter and starts no unit.
        if (letters_seen < raw_letter_offsets.size())
        {
            boundaries.push_back(raw_letter_offsets[letters_seen]);
        }
    }
    if (letters_seen != raw_letter_offsets.size())
    {
        // The display no longer corresponds letter-for-letter: refuse to map
        // rather than delete an arbitrary span.
        return {};
    }
    boundaries.push_back(raw.size());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    return boundaries;
}

std::string LowercaseWithoutSeparators(const std::string &text)
{
    std::string result;
    result.reserve(text.size());
    for (const char ch : text)
    {
        if (ch != '\'')
        {
            result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return result;
}

// 带辅助码段的双拼（句中辅助码的反引号段 / 大写段、直接辅助码解析出的辅码字母）：按请求里的
// 段布局切单元，每个音节和挂在它后面的辅码段各算一个单元，按段删除可以只删辅码，光标也能停在
// 音节与辅码之间。布局对不上原串时不给边界，宿主退回逐字符编辑。
std::vector<std::size_t> SyllableHelpcodeRawBoundaries(const std::string &raw, const QueryRequest &request)
{
    auto units = request.syllable_helpcode_decorations;
    for (auto &unit : units)
    {
        unit.second.insert(unit.second.begin(), '\'');
    }
    const std::string &segmentation = request.raw_segmentation.empty() ? request.raw_input : request.raw_segmentation;
    const std::string display = shuangpin::decorate_segmentation(segmentation, units);
    if (LowercaseWithoutSeparators(display) != LowercaseWithoutSeparators(raw))
    {
        return {};
    }
    return QuanpinRawBoundaries(raw, display);
}
} // namespace

std::vector<std::size_t> InputSession::segment_raw_boundaries() const
{
    // Local modes and the dedicated English scheme spell words, not syllables:
    // the host treats these keys exactly like a plain Backspace (PRD R4).
    if (dedicated_english_mode_ || local_input_mode_ != LocalInputMode::None)
    {
        return {};
    }

    const std::string raw_with_cases = get_pinyin_sequence_with_cases();
    const std::string raw = get_pinyin_sequence();
    if (raw_with_cases.empty())
    {
        return {};
    }

    if (current_scheme_type() == SchemeType::Shuangpin)
    {
        // 句中辅助码段和直接辅助码的辅码字母混在音节之间，贪心切分算不出单元边界，改按请求带着的
        // 段布局切。直接辅助码没解析出辅码时请求原样放行，下面的普通切分就是它的切分。
        if (request().has_syllable_helpcode_layout)
        {
            return SyllableHelpcodeRawBoundaries(raw_with_cases, request());
        }
        const std::size_t helpcode_length =
            shuangpin::detect_active_double_helpcode_length(raw, raw_with_cases, shuangpin_profile_);
        const std::string base =
            helpcode_length > 0 ? shuangpin::trim_trailing_letters_preserve_delimiters(raw_with_cases, helpcode_length)
                                : raw_with_cases;
        std::vector<std::size_t> boundaries = shuangpin::segment_raw_boundaries(base, shuangpin_profile_);
        if (helpcode_length > 0 && !boundaries.empty())
        {
            // The active double helpcode is one editable unit of its own, the
            // same boundary raw_segmentation draws before it.
            boundaries.push_back(base.size());
            boundaries.push_back(raw_with_cases.size());
            std::sort(boundaries.begin(), boundaries.end());
            boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
        }
        return boundaries;
    }

    if (current_scheme_type() != SchemeType::Quanpin)
    {
        return {};
    }
    return QuanpinRawBoundaries(raw_with_cases, get_pinyin_segmentation_with_cases());
}

KeyResult InputSession::edit_at_caret(Command command)
{
    auto text = editing_text();
    auto caret = caret_position();
    // Local-mode markers are commands, not editable payload. Backspace at the
    // end of a marker-only composition retains the existing cancel behavior.
    const std::size_t begin = local_input_mode_ == LocalInputMode::None ? 0 : 1;
    switch (command)
    {
    case Command::MoveLeft:
        caret = caret > begin ? caret - 1 : begin;
        break;
    case Command::MoveRight:
        caret = std::min(caret + 1, text.size());
        break;
    case Command::MoveHome:
        caret = begin;
        break;
    case Command::MoveEnd:
        caret = text.size();
        break;
    case Command::Backspace:
        if (caret <= begin)
            return {true, std::nullopt, std::nullopt};
        text.erase(--caret, 1);
        return replace_editing_text(std::move(text), caret);
    case Command::DeleteForward:
        if (caret == text.size())
            return {true, std::nullopt, std::nullopt};
        text.erase(caret, 1);
        return replace_editing_text(std::move(text), caret);
    default:
        return {};
    }
    caret_ = caret;
    // Pure moves mutate the caret too, so they must re-evaluate the prefix boundary exactly
    // like an insert/delete does; otherwise candidates() would keep serving the previous
    // boundary's list until the next edit key (PRD R2: the boundary follows the caret).
    if (!dedicated_english_mode_ && local_input_mode_ == LocalInputMode::None)
    {
        update_mixed_candidates();
    }
    return {true, std::nullopt, std::nullopt};
}

KeyResult InputSession::insert_at_caret(char character)
{
    auto text = editing_text();
    const auto caret = caret_position();
    const bool lower = character >= 'a' && character <= 'z';
    const bool upper = character >= 'A' && character <= 'Z';
    bool accepted = lower || upper;
    if (!dedicated_english_mode_)
    {
        switch (local_input_mode_)
        {
        case LocalInputMode::Radical:
        case LocalInputMode::QuickPhrase:
            accepted = lower;
            break;
        case LocalInputMode::DateTime:
            // 唤醒词不在中间插字；指定的日期时间按与 TSF、Server 同一条形状规则收数字、/ 和 :。
            accepted = FanyImeDateTimeInput::AcceptsAt(text.data(), text.size(), caret, character);
            break;
        case LocalInputMode::None:
            accepted = lower || (upper && ((scheme() == SchemeType::Quanpin && quanpin_helpcode_enabled_) ||
                                           (scheme() == SchemeType::Shuangpin &&
                                            (shuangpin_helpcode_enabled_ || direct_helpcode_enabled_))));
            if (character == ';' && scheme() == SchemeType::Shuangpin &&
                ShuangpinProfileUsesSemicolonFinal(shuangpin_profile_))
            {
                accepted = direct_helpcode_enabled_
                               ? FanyImeDirectHelpcode::AcceptsSemicolonFinalAt(text.data(), text.size(), caret)
                               : FanyImeMidSentenceHelpcode::AcceptsSemicolonFinalAt(
                                     text.data(), text.size(), caret, engine_.mid_sentence_uppercase_trigger_active());
            }
            if (character == '/')
                accepted = accepts_direct_helpcode_slash_at(caret);
            if (character == '\'' && scheme() != SchemeType::Wubi)
                accepted = caret > 0;
            // 光标移回句中补句中辅助码：反引号和它后面的码按光标前的部分判断。
            if (character == shuangpin::kMidSentenceHelpcodeMarker)
                accepted = accepts_mid_sentence_helpcode_marker_at(caret);
            else if (upper && !accepted)
                accepted = accepts_mid_sentence_code_at(caret, character);
            break;
        case LocalInputMode::Emoji:
        case LocalInputMode::Kaomoji:
        case LocalInputMode::TemporaryJapanese:
            accepted = accepted || character == '\'';
            break;
        default:
            break;
        }
    }
    if (!accepted)
        return {};
    if (character == '\'' && ((caret > 0 && text[caret - 1] == '\'') || text[caret] == '\''))
        return {true, std::nullopt, std::nullopt};
    text.insert(caret, 1, character);
    return replace_editing_text(std::move(text), caret + 1);
}

KeyResult InputSession::replace_editing_text(std::string text, std::size_t caret)
{
    std::optional<std::string> diagnostic;
    if (dedicated_english_mode_)
    {
        dedicated_english_preedit_ = std::move(text);
        update_dedicated_english_candidates();
    }
    else if (local_input_mode_ != LocalInputMode::None && local_input_mode_ != LocalInputMode::TemporaryJapanese)
    {
        local_preedit_ = std::move(text);
        diagnostic = update_local_candidates();
    }
    else
    {
        const auto payload = local_input_mode_ == LocalInputMode::TemporaryJapanese ? text.substr(1) : text;
        auto normalized = payload;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        set_pinyin_sequence(normalized);
        set_pinyin_sequence_with_cases(payload);
        apply_pending_sequence();
        if (local_input_mode_ == LocalInputMode::TemporaryJapanese)
        {
            local_preedit_ = "R" + engine_.get_preedit();
            local_candidates_ = engine_.get_candidates();
        }
    }
    caret_ = caret;
    online_requests_.invalidate();
    discard_abandoned_phrase_progress();
    // apply_pending_sequence() already refreshed candidates while the caret was cleared; now
    // that it is back in place, re-evaluate the caret prefix boundary (a caret at the end of
    // the text deactivates the prefix decode, mid-text re-decodes it).
    if (!dedicated_english_mode_ && local_input_mode_ == LocalInputMode::None)
    {
        update_mixed_candidates();
    }
    return {true, std::nullopt, std::move(diagnostic)};
}
} // namespace metasequoia
