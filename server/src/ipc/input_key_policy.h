#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace FanyImeIpc
{
inline constexpr uint32_t kVirtualKeyShift = 0x10;
inline constexpr uint32_t kVirtualKeyEscape = 0x1B;
inline constexpr uint32_t kVirtualKeyBackspace = 0x08;
inline constexpr uint32_t kVirtualKeyLeft = 0x25;
inline constexpr uint32_t kVirtualKeyRight = 0x27;
inline constexpr uint32_t kVirtualKeyLeftShift = 0xA0;
inline constexpr uint32_t kVirtualKeyRightShift = 0xA1;
inline constexpr uint32_t kVirtualKeyNumpad0 = 0x60;
inline constexpr uint32_t kVirtualKeyNumpad9 = 0x69;
inline constexpr uint32_t kModifierShift = 0b00000001u;
inline constexpr uint32_t kModifierControl = 0b00000010u;
inline constexpr uint32_t kModifierAlt = 0b00000100u;
inline constexpr uint32_t kModifierUiLess = 0x80000000u;
inline constexpr uint32_t kEnglishModeToggleModifiers = kModifierShift | kModifierControl;
inline constexpr uint32_t kKeyModifierMask = kModifierShift | kModifierControl | kModifierAlt;

constexpr bool IsEnglishModeToggleKey(uint32_t keycode, uint32_t modifiers_down)
{
    return keycode == static_cast<uint32_t>('E') && (modifiers_down & kKeyModifierMask) == kEnglishModeToggleModifiers;
}

constexpr bool IsPinyinCommitKey(uint32_t keycode, uint32_t modifiers_down)
{
    return keycode == 0x0D && (modifiers_down & kKeyModifierMask) == kModifierShift;
}

inline std::string EnteredPinyinText(const std::string &raw, std::string quanpin, bool convert_shuangpin)
{
    if (!convert_shuangpin || quanpin.empty())
        return raw;
    quanpin.erase(std::remove(quanpin.begin(), quanpin.end(), '\''), quanpin.end());
    return quanpin;
}

// Return -1 / +1 for the first / last Han character, or zero for an ordinary key.
constexpr int WordToCharacterDirection(uint32_t keycode, uint32_t character, uint32_t modifiers, bool enabled,
                                       bool minus_equal)
{
    if (!enabled || (modifiers & kKeyModifierMask) != 0)
        return 0;
    if (minus_equal)
        return keycode == 0xBD && character == '-' ? -1 : keycode == 0xBB && character == '=' ? 1 : 0;
    return keycode == 0xDB && character == '[' ? -1 : keycode == 0xDD && character == ']' ? 1 : 0;
}

// The TSF side treats numpad digits exactly like the corresponding candidate
// digit. Canonicalize them at the Server boundary so every downstream policy
// sees the same key code and, crucially, produces a reply for the request.
constexpr uint32_t NormalizeNumpadDigitKey(uint32_t keycode)
{
    return keycode >= kVirtualKeyNumpad0 && keycode <= kVirtualKeyNumpad9
               ? static_cast<uint32_t>('0') + (keycode - kVirtualKeyNumpad0)
               : keycode;
}

// TSF locally consumes these keys and completes/cancels its composition. The
// Server must reset every backend without producing a reply.
constexpr bool IsBackendIndependentCompositionResetKey(uint32_t keycode)
{
    return keycode == kVirtualKeyShift || keycode == kVirtualKeyEscape || keycode == kVirtualKeyLeftShift ||
           keycode == kVirtualKeyRightShift;
}

constexpr bool ShouldResetCompositionForImeMode(bool chinese_mode)
{
    return !chinese_mode;
}

// A complete four-letter wubi code the table answered with exactly one candidate is committed as
// soon as the fourth letter lands, so the user never has to press space. The switch defaults on
// (industry wubi IMEs default this on), so leaving it alone keeps that behavior; turning it off is
// the only exit a user who keeps hitting the wrong commit has. On top of the switch, the wubi
// engine's own report that the code is complete, table-answered and unique is the only other gate.
// A word being created keeps the composition open: the raw belongs to the prefix the user is still
// assembling, and committing it would end that word early.
constexpr bool ShouldAutoCommitCompleteWubiCode(bool auto_commit_enabled, bool unique_four_code,
                                                bool creating_word_active)
{
    return auto_commit_enabled && unique_four_code && !creating_word_active;
}

// The user is typing past a complete four-letter wubi code (a letter key with the caret at the end
// of a four-letter table-answered code). The first candidate is committed and the key that was just
// typed starts the next composition instead of being dropped. Committing the first candidate matches
// the user, who is already typing the next word and is not looking at the candidate window. Its own
// switch defaults on and is independent of the auto-commit switch above. Turning it off only has
// somewhere to put the letter when mixed input is on: the composition then grows into a mixed
// spelling. With mixed input off the engine clips the code back to four letters, which drops the
// key on the Server while a raw-preedit client already shows it, so the switch yields to the top
// commit there: an extra key never loses input, whatever the setting says.
constexpr bool ShouldCommitCompleteWubiCodeOnNextKey(bool top_commit_enabled, bool mixed_pinyin_enabled,
                                                     bool four_code_is_complete, bool key_is_letter, bool caret_at_end,
                                                     bool creating_word_active)
{
    return (top_commit_enabled || !mixed_pinyin_enabled) && four_code_is_complete && key_is_letter && caret_at_end &&
           !creating_word_active;
}

// Enter commits the raw composition instead of choosing a special-mode
// candidate. Therefore a Shift+letter wake key must not by itself prevent an
// otherwise non-pinyin English word (for example "Metasequoia") from being
// learned. The database layer still validates the final string.
constexpr bool ShouldLearnEnteredEnglishWord(bool dedicated_english_mode, bool shift_letter_special_mode,
                                             bool chinese_scheme, bool all_complete_pinyin)
{
    return dedicated_english_mode || shift_letter_special_mode || (chinese_scheme && !all_complete_pinyin);
}

constexpr bool InputSessionMatchesConfig(bool configured_scheme_matches, bool temporary_r_mode_active,
                                         bool session_is_japanese)
{
    return configured_scheme_matches || (temporary_r_mode_active && session_is_japanese);
}

// is_date_time_input_key：Shift+T 指定日期时间里进输入串的数字、/ 和 :。TSF 把它们当输入键，pinyin 预编辑
// 样式和 UILess 下同样要等这一帧。
constexpr bool ShouldSendCompositionReply(bool is_alpha_key, bool is_manual_pinyin_separator,
                                          bool is_microsoft_shuangpin_ing_key, bool is_unicode_hex_digit,
                                          bool is_unicode_plus, bool is_japanese_long_vowel,
                                          bool is_date_time_input_key = false)
{
    return is_alpha_key || is_manual_pinyin_separator || is_microsoft_shuangpin_ing_key || is_unicode_hex_digit ||
           is_unicode_plus || is_japanese_long_vowel || is_date_time_input_key;
}

// Backspace inside a live creating-word state retracts the newest selection
// first -- Rime/WeChat style "backspace undoes the last pick" -- instead of the
// old caret-qualified triggers (delete the remaining raw down to the last
// character, or stand right behind the word). The remaining raw length and the
// caret no longer matter; the two guards that still do are:
//  - selection_history empty: nothing to retract, so the shape alone only owes
//    the client a frame describing whatever state the key left behind;
//  - last_selection_raw_edited: a character typed after the selection locks it
//    (Rime's selected_before_editing) so Backspace keeps deleting the freshly
//    typed input. An empty raw overrides the lock: with nothing to delete the
//    key can only mean retract, and dropping the selection instead would
//    regress #35 (whole composition discarded).
// Both need a client that negotiated the CompositionRestore capability: UILess
// hosts draw their own candidate UI, and a DLL without the capability treats
// the reply as a transport fault rather than ignoring it.
//
// The shape itself is the contract with the client: TSF arms its reply hold
// whenever its mirror of the creating-word state is non-empty
// (word_for_creating_word), because that is the only part of this predicate it
// can see -- snapshot existence and the edit lock live Server-side. HandleImeKey
// therefore answers every Backspace inside this shape through
// HasRetreatBackspaceShape, retreat or not, so the two conditions must stay in
// step: raw styles would otherwise send no reply at all and the hold would burn
// its full 50 ms timeout. Clearing the creating word also removes the post-key
// shape, so the reply decision additionally carries the shape captured before
// the edit (see ShouldAnswerRetreatBackspace below).
constexpr bool HasRetreatBackspaceShape(bool creating_word_active, bool ui_less, bool client_supports_restore)
{
    return creating_word_active && !ui_less && client_supports_restore;
}

// The reply condition for a Backspace inside the shape: the frame is owed
// whenever the client may be holding for one. shape_before_key is the mirror the
// client armed its hold from, shape_after_key is what the key left behind.
// Testing only the post-key shape drops the frame exactly when the edit lock let
// the key delete the last raw character and the tail then cleared the creating
// word: the client, which saw the pre-key word, is still holding, and the wait
// burns its full 50 ms timeout before it falls back to its local deletion. The
// payload describes whatever the key left behind (ended, shorter, or unchanged),
// which is what the hold applies in every outcome.
constexpr bool ShouldAnswerRetreatBackspace(bool composition_restored, bool shape_before_key, bool shape_after_key)
{
    return composition_restored || shape_before_key || shape_after_key;
}

// The shape plus a snapshot that may actually rewrite the composition; the
// shape alone only owes the client a frame. This is the only combination that
// may rewrite state on a Backspace.
constexpr bool ShouldRetreatCreatingWordSelection(bool creating_word_active, bool ui_less, bool client_supports_restore,
                                                  std::size_t raw_length, std::size_t selection_history_size,
                                                  bool last_selection_raw_edited)
{
    return HasRetreatBackspaceShape(creating_word_active, ui_less, client_supports_restore) &&
           selection_history_size > 0 && (!last_selection_raw_edited || raw_length == 0);
}

// A Backspace that deletes the last raw character while a word is being created
// keeps that word (and its snapshots): the composition stays alive showing the
// selected segments alone, and the reply tells the client to keep composing with
// the word instead of cancelling. That is the R3 state a segment Backspace
// already produces, and from it the next Backspace retracts the newest selection
// (the empty-raw override above) instead of discarding everything the user
// picked. It needs the same client the retreat reply needs: only a negotiated,
// non-UILess client applies the frame that keeps the word on screen -- an old
// DLL or a UILess host cancels its own composition locally, so keeping the state
// there would leave the two sides out of step. Without a snapshot there is
// nothing left to retract, so an empty raw keeps ending the composition, as it
// always did.
constexpr bool ShouldKeepCreatingWordAfterRawEmptied(bool creating_word_active, bool ui_less,
                                                     bool client_supports_restore, std::size_t selection_history_size)
{
    return HasRetreatBackspaceShape(creating_word_active, ui_less, client_supports_restore) &&
           selection_history_size > 0;
}

// Esc inside the creating-word shape owes the client a CompositionRestored
// frame: TSF arms its reply hold from the same word_for_creating_word mirror
// the Backspace hold uses, and cannot see input.escape_keeps_selected_word.
// The frame therefore goes out in both outcomes, with an empty payload when
// the composition is cancelled.
constexpr bool HasEscapeCreatingWordShape(uint32_t keycode, bool creating_word_active, bool ui_less,
                                          bool client_supports_restore)
{
    return keycode == kVirtualKeyEscape &&
           HasRetreatBackspaceShape(creating_word_active, ui_less, client_supports_restore);
}

// Rime's ClearNonConfirmedComposition: with the option on, Esc drops only the
// unselected spelling and keeps the selected word on screen alone. With no
// spelling left (the word is already alone) the next Esc cancels everything.
// R mode is excluded: its temporary Japanese session is only restored by a
// full reset.
constexpr bool ShouldEscapeKeepSelectedWord(bool option_enabled, std::size_t raw_length, bool r_mode_active)
{
    return option_enabled && raw_length > 0 && !r_mode_active;
}

// Ctrl+Backspace inside a composition deletes one segmentation unit instead of
// one character. Only the bare Ctrl chord is the IME's: Shift, Alt and the
// Windows keys keep their host meaning (PRD R1).
constexpr bool IsSegmentBackspaceKey(uint32_t keycode, uint32_t modifiers_down)
{
    return keycode == kVirtualKeyBackspace && (modifiers_down & kKeyModifierMask) == kModifierControl;
}

// 光标驱动的组词重算（PRD R2/R10）：与 Ctrl+Backspace / Ctrl+方向的门控同一谓词族。
// 只有协商过 CompositionRestore 的非 UILess 客户端才把光标喂给会话做前缀重解；
// 未协商（旧 DLL 组合）、UILess 宿主、专用英文与特殊模式组合一律维持整串转换，
// 光标只是显示层插入点（现状）。
constexpr bool ShouldResegmentCompositionByCaret(bool client_supports_restore, bool ui_less, bool english_input_mode,
                                                 bool special_mode_composition_active) noexcept
{
    return client_supports_restore && !ui_less && !english_input_mode && !special_mode_composition_active;
}

// R4：光标量化后的前缀为空（raw 非空而前缀终点为 0）。此时候选必须为空、候选窗隐藏，
// 不得回退成「整串 raw 假候选」。caret 未设置（整串解码）时 prefix_end == raw_length，
// 恒为 false；raw 为空的组合同样恒为 false。
constexpr bool IsCaretPrefixEmpty(std::size_t prefix_end, std::size_t raw_length) noexcept
{
    return prefix_end == 0 && raw_length > 0;
}

// 候选页的身份是引擎按光标前缀解码出来的那一套候选（refresh_prefix_candidates 会把前缀
// 折成小写）。选词快照记下的候选绝对位置只在重建页前缀一致时才有意义：选词之后用户可能
// 用 Del、光标编辑或再次撤销改动了 raw，撤销重建出来的是另一套候选，把旧位置套上去会把
// 高亮落到不相干的项。调用方在应用记录位置前比较这两个归一化前缀。
inline std::string NormalizeCandidatePagePrefix(const std::string &raw_input_with_cases, std::size_t prefix_end)
{
    const std::size_t end = (std::min)(prefix_end, raw_input_with_cases.size());
    std::string prefix = raw_input_with_cases.substr(0, end);
    std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return prefix;
}

// 光标箭头键之后的候选发布决策（2026-09 真机回归修复）：光标移回串尾时引擎已按整串重算，
// 但 candidate_ui.items 仍是上一次前缀重解发布的页面。「只刷新页面」会继续用这批旧前缀
// 候选重建页面并参与空格/数字结算（ni'hao'ya 从 ni'hao 右移回串尾后只上屏「你好」+「ya」）。
// 门控启用时前缀中间与串尾一律从引擎重读重建页面；前缀为空仍收起候选窗（R4）；未启用
// （未协商/UILess/特殊模式）或 raw 为空维持只刷新页面的现状（R7/AC8 零差异）。
enum class CaretArrowCandidatePublish
{
    Hide,
    RebuildFromEngine,
    RefreshPageOnly
};

constexpr CaretArrowCandidatePublish ResolveCaretArrowCandidatePublish(bool caret_resegmentation,
                                                                       std::size_t prefix_end,
                                                                       std::size_t raw_length) noexcept
{
    if (IsCaretPrefixEmpty(prefix_end, raw_length))
    {
        return CaretArrowCandidatePublish::Hide;
    }
    if (caret_resegmentation && raw_length > 0)
    {
        return CaretArrowCandidatePublish::RebuildFromEngine;
    }
    return CaretArrowCandidatePublish::RefreshPageOnly;
}

// NeedToCreateWord 帧是否携带可选的第 4 字段（caret，contracts/windows_ipc.h）。该字段
// 的解析器是 #35 之后 DLL 才有的：旧 DLL 把第 2 个 '\t' 之后的整个尾部当
// display_preedit，未协商时追加 caret 会把 inline preedit 污染成形如「好ni'hao\t4」的
// 串（AC8：未协商组合必须收到与旧 Server 字节一致的 3 字段帧）。协商侧也只在光标
// 不在剩余 raw 末尾时携带——串尾造词流光标恒在末尾，省略字段即现状字节。
constexpr bool ShouldCreateWordFrameCarryCaret(bool client_supports_restore, std::size_t caret_position,
                                               std::size_t remaining_raw_size) noexcept
{
    return client_supports_restore && caret_position < remaining_raw_size;
}

// Ctrl+Left / Ctrl+Right move the caret by the same segmentation unit that
// Ctrl+Backspace deletes. They mirror IsSegmentBackspaceKey: only the bare Ctrl
// chord is the IME's, so Shift, Alt and the Windows keys keep their host
// meaning (PRD R1). The unit model itself is the engine's and stays Server-side.
constexpr bool IsSegmentCaretKey(uint32_t keycode, uint32_t modifiers_down)
{
    return (keycode == kVirtualKeyLeft || keycode == kVirtualKeyRight) &&
           (modifiers_down & kKeyModifierMask) == kModifierControl;
}

// A segment Backspace with nothing left before the caret deletes the last
// selected segment of the word being created: the accumulated word returns to
// its pre-selection state and the spelling that segment consumed is discarded
// rather than restored (PRD R3). Like the retraction above it needs a reply the
// client can apply, because TSF cannot mirror the deletion locally.
constexpr bool ShouldDropCreatingWordSegment(bool creating_word_active, bool ui_less, bool client_supports_restore,
                                             std::size_t caret_position, std::size_t selection_history_size)
{
    return creating_word_active && !ui_less && client_supports_restore && caret_position == 0 &&
           selection_history_size > 0;
}

// The offset a unit deletion starts from and a unit jump to the left lands on:
// the greatest boundary strictly before the caret, or `caret` itself when no
// unit boundary precedes it. Boundaries are raw offsets in ascending order. A
// result equal to `caret` means "no unit here": the caller then falls back to
// deleting / moving one character.
inline std::size_t PreviousSegmentBoundary(const std::vector<std::size_t> &boundaries, std::size_t caret)
{
    std::size_t result = caret;
    for (const std::size_t boundary : boundaries)
    {
        if (boundary >= caret)
        {
            break;
        }
        result = boundary;
    }
    return result;
}

// The offset a unit jump to the right lands on: the smallest boundary strictly
// after the caret, or `caret` itself when no unit boundary follows it. A result
// equal to `caret` means the caret already sits at the end of the last unit and
// the caller must not move it.
inline std::size_t NextSegmentBoundary(const std::vector<std::size_t> &boundaries, std::size_t caret)
{
    for (const std::size_t boundary : boundaries)
    {
        if (boundary > caret)
        {
            return boundary;
        }
    }
    return caret;
}

// After erasing [start, caret), the truncation point can sit next to a
// separator that no longer separates anything: "ni'hao" - "hao" leaves "ni'",
// "ni'hao'ma" - "hao" leaves "ni''ma". Drop exactly the one dangling
// separator so no empty segment or doubled delimiter survives the deletion.
inline void DropDanglingSegmentDelimiter(std::string &raw, std::size_t start)
{
    if (start < raw.size() && raw[start] == '\'' && (start == 0 || raw[start - 1] == '\''))
    {
        raw.erase(start, 1);
    }
    else if (start == raw.size() && start > 0 && raw[start - 1] == '\'')
    {
        raw.erase(start - 1, 1);
    }
}
} // namespace FanyImeIpc
