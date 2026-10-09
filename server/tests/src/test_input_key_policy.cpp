#include "ipc/input_key_policy.h"
#include "engine/contracts/trilingual_input.h"
#include "tests/includes/test_framework.h"

TEST_CASE(trilingual_shift_cycles_chinese_japanese_english_and_returns_to_chinese)
{
    using FanyImeTrilingualInput::Mode;
    using FanyImeTrilingualInput::Next;
    REQUIRE(Next(true, false) == Mode::Japanese);
    REQUIRE(Next(true, true) == Mode::English);
    // English can retain either native-language scheme; both return to Chinese.
    REQUIRE(Next(false, true) == Mode::Chinese);
    REQUIRE(Next(false, false) == Mode::Chinese);
}

TEST_CASE(trilingual_cycle_needs_an_explicit_request_not_a_plain_shift)
{
    using FanyImeTrilingualInput::EncodeCycleRequest;
    using FanyImeTrilingualInput::IsCycleRequest;
    // The binary toggle (Win held, fallback, setting not yet delivered) still
    // sends a plain VK_SHIFT; the Server must not turn it into a cycle.
    REQUIRE(!IsCycleRequest(0x10, 0));
    REQUIRE(!IsCycleRequest(0x10, FanyImeIpc::kModifierUiLess));
    REQUIRE(IsCycleRequest(0x10, EncodeCycleRequest(true, false)));
    REQUIRE(IsCycleRequest(0x10, EncodeCycleRequest(false, false) | FanyImeIpc::kModifierUiLess));
    REQUIRE(!IsCycleRequest('A', EncodeCycleRequest(true, false)));
    for (unsigned modifiers = 1; modifiers < 8; ++modifiers)
    {
        REQUIRE(!IsCycleRequest(0x10, EncodeCycleRequest(true, false) | modifiers));
        REQUIRE(!IsCycleRequest(0x09, EncodeCycleRequest(true, false) | modifiers));
    }
}

TEST_CASE(trilingual_cycle_starts_from_the_requesting_client_state)
{
    using FanyImeTrilingualInput::Destination;
    using FanyImeTrilingualInput::Mode;
    // ime_mode_scope=app: one host is English while another is Chinese. The
    // request's own state decides; the configured language does not.
    REQUIRE(Destination(false, false, true, true) == Mode::Chinese);
    REQUIRE(Destination(true, false, true, true) == Mode::Japanese);
    REQUIRE(Destination(true, true, true, false) == Mode::English);
    // Switch turned off while the request was queued: binary toggle reply.
    REQUIRE(Destination(true, false, false, false) == Mode::English);
    REQUIRE(Destination(false, true, false, false) == Mode::Chinese);
    REQUIRE(Destination(false, false, false, true) == Mode::Japanese);
}

TEST_CASE(pinyin_commit_requires_shift_enter)
{
    REQUIRE(FanyImeIpc::IsPinyinCommitKey(0x0D, 1));
    REQUIRE(FanyImeIpc::IsPinyinCommitKey(0x0D, 1 | FanyImeIpc::kModifierUiLess));
    for (unsigned modifiers = 0; modifiers < 8; ++modifiers)
        REQUIRE(FanyImeIpc::IsPinyinCommitKey(0x0D, modifiers) == (modifiers == 1));
    REQUIRE(!FanyImeIpc::IsPinyinCommitKey('A', 1));
}

TEST_CASE(pinyin_commit_converts_shuangpin_and_preserves_raw_fallback)
{
    using FanyImeIpc::EnteredPinyinText;
    REQUIRE_EQ(EnteredPinyinText("nihc", "ni'hao", true), std::string("nihao"));
    REQUIRE_EQ(EnteredPinyinText("ni'hc", "ni'hao'", true), std::string("nihao"));
    REQUIRE_EQ(EnteredPinyinText("vgh", "zheng'h", true), std::string("zhengh"));
    REQUIRE_EQ(EnteredPinyinText("nihc", "ni'hao", false), std::string("nihc"));
    REQUIRE_EQ(EnteredPinyinText("hello", "", true), std::string("hello"));
    REQUIRE_EQ(EnteredPinyinText("U4E00", "", false), std::string("U4E00"));
    REQUIRE_EQ(EnteredPinyinText("", "", true), std::string(""));
}

TEST_CASE(word_to_character_uses_only_the_selected_unmodified_key_pair)
{
    using FanyImeIpc::WordToCharacterDirection;
    REQUIRE_EQ(WordToCharacterDirection(0xDB, '[', 0, true, false), -1);
    REQUIRE_EQ(WordToCharacterDirection(0xDD, ']', 0, true, false), 1);
    REQUIRE_EQ(WordToCharacterDirection(0xBD, '-', 0, true, true), -1);
    REQUIRE_EQ(WordToCharacterDirection(0xBB, '=', 0, true, true), 1);
    REQUIRE_EQ(WordToCharacterDirection(0xDB, '[', 0, true, true), 0);
    REQUIRE_EQ(WordToCharacterDirection(0xBD, '-', 0, true, false), 0);
    REQUIRE_EQ(WordToCharacterDirection(0xBD, '-', 0, false, true), 0);
    REQUIRE_EQ(WordToCharacterDirection(0xDB, '[', 0, false, false), 0);
    // Unicode U+ entry and shifted punctuation must not select a character.
    REQUIRE_EQ(WordToCharacterDirection(0xBB, '+', 1, true, true), 0);
    REQUIRE_EQ(WordToCharacterDirection(0xBD, '_', 1, true, true), 0);
    REQUIRE_EQ(WordToCharacterDirection(0xDB, '{', 1, true, false), 0);
    for (unsigned modifiers = 1; modifiers < 8; ++modifiers)
        REQUIRE_EQ(WordToCharacterDirection(0xBB, '=', modifiers, true, true), 0);
    // Host-drawn candidates carry an unrelated UI-less flag.
    REQUIRE_EQ(WordToCharacterDirection(0xBB, '=', FanyImeIpc::kModifierUiLess, true, true), 1);
    REQUIRE_EQ(WordToCharacterDirection('A', '=', 0, true, true), 0);
}

TEST_CASE(shift_variants_are_backend_independent_composition_reset_keys)
{
    REQUIRE(FanyImeIpc::IsBackendIndependentCompositionResetKey(0x10));
    REQUIRE(FanyImeIpc::IsBackendIndependentCompositionResetKey(0x1B));
    REQUIRE(FanyImeIpc::IsBackendIndependentCompositionResetKey(0xA0));
    REQUIRE(FanyImeIpc::IsBackendIndependentCompositionResetKey(0xA1));

    REQUIRE(!FanyImeIpc::IsBackendIndependentCompositionResetKey(0));
    REQUIRE(!FanyImeIpc::IsBackendIndependentCompositionResetKey('A'));
    REQUIRE(!FanyImeIpc::IsBackendIndependentCompositionResetKey(0x0D));
    REQUIRE(!FanyImeIpc::IsBackendIndependentCompositionResetKey(0x11));
}

TEST_CASE(english_ime_status_requires_backend_independent_composition_reset)
{
    REQUIRE(FanyImeIpc::ShouldResetCompositionForImeMode(false));
    REQUIRE(!FanyImeIpc::ShouldResetCompositionForImeMode(true));
}

TEST_CASE(enter_english_learning_does_not_conflict_with_shift_letter_special_modes)
{
    REQUIRE(FanyImeIpc::ShouldLearnEnteredEnglishWord(false, false, true, false));
    REQUIRE(FanyImeIpc::ShouldLearnEnteredEnglishWord(true, false, true, true));
    // K/U/T/E/M/J/Y modes and the temporary R-mode session all use this path.
    REQUIRE(FanyImeIpc::ShouldLearnEnteredEnglishWord(false, true, true, true));
    REQUIRE(FanyImeIpc::ShouldLearnEnteredEnglishWord(false, true, false, true));
    REQUIRE(!FanyImeIpc::ShouldLearnEnteredEnglishWord(false, false, true, true));
    REQUIRE(!FanyImeIpc::ShouldLearnEnteredEnglishWord(false, false, false, false));
}

TEST_CASE(numpad_digits_are_normalized_to_candidate_digit_keys)
{
    REQUIRE(FanyImeIpc::NormalizeNumpadDigitKey(0x60) == '0');
    REQUIRE(FanyImeIpc::NormalizeNumpadDigitKey(0x61) == '1');
    REQUIRE(FanyImeIpc::NormalizeNumpadDigitKey(0x69) == '9');

    REQUIRE(FanyImeIpc::NormalizeNumpadDigitKey('1') == '1');
    REQUIRE(FanyImeIpc::NormalizeNumpadDigitKey(0x6A) == 0x6A);
}

TEST_CASE(english_mode_toggle_requires_ctrl_shift_e)
{
    REQUIRE(FanyImeIpc::IsEnglishModeToggleKey('E', 0b00000011u));
    REQUIRE(!FanyImeIpc::IsEnglishModeToggleKey('E', 0b00000111u));
    REQUIRE(!FanyImeIpc::IsEnglishModeToggleKey('E', 0b00000110u));
    REQUIRE(!FanyImeIpc::IsEnglishModeToggleKey('E', 0b00000001u));
    REQUIRE(!FanyImeIpc::IsEnglishModeToggleKey('A', 0b00000011u));
}

TEST_CASE(composition_reply_includes_microsoft_shuangpin_ing_key)
{
    REQUIRE(FanyImeIpc::ShouldSendCompositionReply(false, false, true, false, false, false));
    REQUIRE(FanyImeIpc::ShouldSendCompositionReply(true, false, false, false, false, false));
    REQUIRE(!FanyImeIpc::ShouldSendCompositionReply(false, false, false, false, false, false));
    // T 模式指定日期时间的数字、/ 和 : 进输入串，TSF 同样在等这一帧。
    REQUIRE(FanyImeIpc::ShouldSendCompositionReply(false, false, false, false, false, false, true));
}

TEST_CASE(backspace_retracts_the_last_selected_segment_before_deleting)
{
    using FanyImeIpc::ShouldRetreatCreatingWordSelection;
    // The Rime/WeChat-style default: a live word with an unlocked snapshot
    // retracts on the first Backspace, however much raw stays behind -- the
    // remaining length and the caret position no longer qualify it.
    REQUIRE(ShouldRetreatCreatingWordSelection(true, false, true, 4, 1, false));
    REQUIRE(ShouldRetreatCreatingWordSelection(true, false, true, 1, 3, false));
    // A spelling already emptied by a Ctrl+Backspace segment deletion still owns
    // its snapshots, so the plain Backspace retracts the selected segment.
    REQUIRE(ShouldRetreatCreatingWordSelection(true, false, true, 0, 1, false));
    // No active word, UILess host, old DLL, or no snapshot.
    REQUIRE(!ShouldRetreatCreatingWordSelection(false, false, true, 4, 1, false));
    REQUIRE(!ShouldRetreatCreatingWordSelection(true, true, true, 4, 1, false));
    REQUIRE(!ShouldRetreatCreatingWordSelection(true, false, false, 4, 1, false));
    REQUIRE(!ShouldRetreatCreatingWordSelection(true, false, true, 4, 0, false));
    // A character typed after the selection locks it (selected_before_editing):
    // Backspace keeps deleting the fresh input so it stays editable.
    REQUIRE(!ShouldRetreatCreatingWordSelection(true, false, true, 4, 1, true));
    // ...unless there is nothing left to delete: with an empty raw the key can
    // only mean retract, and discarding the selection would regress #35.
    REQUIRE(ShouldRetreatCreatingWordSelection(true, false, true, 0, 1, true));
}

TEST_CASE(retreat_backspace_shape_is_what_the_client_can_see)
{
    using FanyImeIpc::HasRetreatBackspaceShape;
    // The DLL arms its reply hold from its creating-word mirror alone -- it
    // cannot see the Server's snapshot history or edit lock -- so the shape
    // holds, and the Server owes it a frame in every outcome (retreat, locked
    // deletion, or a no-op behind an empty history), exactly while a word is
    // being created on a non-UILess negotiated client.
    REQUIRE(HasRetreatBackspaceShape(true, false, true));
    // No active word, UILess host, or unnegotiated client: the DLL never arms.
    REQUIRE(!HasRetreatBackspaceShape(false, false, true));
    REQUIRE(!HasRetreatBackspaceShape(true, true, true));
    REQUIRE(!HasRetreatBackspaceShape(true, false, false));
    // The shape alone never rewrites state: with no snapshot the same predicate
    // only owes the frame.
    REQUIRE(!FanyImeIpc::ShouldRetreatCreatingWordSelection(true, false, true, 4, 0, false));
}

TEST_CASE(retreat_backspace_reply_survives_the_key_that_ends_the_word)
{
    using FanyImeIpc::ShouldAnswerRetreatBackspace;
    // An ordinary deletion that keeps the word alive: the post-key shape alone
    // already owes the frame.
    REQUIRE(ShouldAnswerRetreatBackspace(false, false, true));
    // A retraction or a segment edit rewrites state and is answered regardless.
    REQUIRE(ShouldAnswerRetreatBackspace(true, false, false));
    // The key that deletes the last raw character clears the creating word, so
    // only the shape captured before the edit is left: the client armed its hold
    // from that state and must still get a frame, otherwise it burns its timeout.
    REQUIRE(ShouldAnswerRetreatBackspace(false, true, false));
    // No shape before or after, nothing restored: an ordinary Backspace outside
    // the creating word stays unanswered, as TSF mirrors it locally.
    REQUIRE(!ShouldAnswerRetreatBackspace(false, false, false));
}

TEST_CASE(deleting_the_last_raw_character_keeps_the_created_word)
{
    using FanyImeIpc::ShouldKeepCreatingWordAfterRawEmptied;
    // The reported regression: after picking 你好 from "nihaoya" and typing more,
    // deleting the remaining raw down to empty must leave the word on screen so
    // the next Backspace can retract the pick, not swallow the whole composition.
    REQUIRE(ShouldKeepCreatingWordAfterRawEmptied(true, false, true, 1));
    // Only a client that applies the keeping frame keeps the state: an old DLL or
    // a UILess host cancels its own composition locally.
    REQUIRE(!ShouldKeepCreatingWordAfterRawEmptied(true, true, true, 1));
    REQUIRE(!ShouldKeepCreatingWordAfterRawEmptied(true, false, false, 1));
    // A word being created without a snapshot has nothing to retract: the empty
    // raw keeps ending the composition (unchanged behavior).
    REQUIRE(!ShouldKeepCreatingWordAfterRawEmptied(true, false, true, 0));
    // No word being created: a plain deletion to empty ends the composition.
    REQUIRE(!ShouldKeepCreatingWordAfterRawEmptied(false, false, true, 3));
}

TEST_CASE(escape_inside_a_created_word_owes_the_client_a_frame)
{
    using FanyImeIpc::HasEscapeCreatingWordShape;
    constexpr uint32_t kEscape = 0x1B;
    // TSF holds for a reply whenever a word is being created, whatever the
    // option says, so the frame goes out in both outcomes.
    REQUIRE(HasEscapeCreatingWordShape(kEscape, true, false, true));
    REQUIRE(!HasEscapeCreatingWordShape(kEscape, false, false, true));
    REQUIRE(!HasEscapeCreatingWordShape(kEscape, true, true, true));
    REQUIRE(!HasEscapeCreatingWordShape(kEscape, true, false, false));
    REQUIRE(!HasEscapeCreatingWordShape(0x10, true, false, true));
}

TEST_CASE(escape_keeps_the_selected_word_only_while_spelling_remains)
{
    using FanyImeIpc::ShouldEscapeKeepSelectedWord;
    // 造句 + "deshihou": the first Esc drops the spelling and keeps 造句.
    REQUIRE(ShouldEscapeKeepSelectedWord(true, 8, false));
    // The word is already alone: the second Esc cancels everything.
    REQUIRE(!ShouldEscapeKeepSelectedWord(true, 0, false));
    // Option off: Esc always cancels the whole composition.
    REQUIRE(!ShouldEscapeKeepSelectedWord(false, 8, false));
    // R mode only unwinds its temporary session through a full reset.
    REQUIRE(!ShouldEscapeKeepSelectedWord(true, 8, true));
}

TEST_CASE(candidate_page_prefix_normalizes_the_decoded_prefix)
{
    using FanyImeIpc::NormalizeCandidatePagePrefix;
    // The engine decodes the lowercased caret prefix, so the page identity is
    // case-insensitive and clamped to the raw.
    REQUIRE_EQ(NormalizeCandidatePagePrefix("NiHaoYa", 2), std::string("ni"));
    // A caret at the end (or an unset caret) makes the whole string the prefix.
    REQUIRE_EQ(NormalizeCandidatePagePrefix("NiHaoYa", 7), std::string("nihaoya"));
    REQUIRE_EQ(NormalizeCandidatePagePrefix("NiHaoYa", 99), std::string("nihaoya"));
    // An empty prefix and an empty raw share the same page identity.
    REQUIRE_EQ(NormalizeCandidatePagePrefix("ni", 0), std::string());
    REQUIRE_EQ(NormalizeCandidatePagePrefix("", 0), std::string());
    // Shortening the suffix between the pick and the retraction changes the
    // prefix the page is rebuilt for: the recorded position must not be applied.
    REQUIRE(NormalizeCandidatePagePrefix("nihaoya", 7) != NormalizeCandidatePagePrefix("nihaoa", 6));
}

TEST_CASE(segment_backspace_is_ctrl_only)
{
    using FanyImeIpc::IsSegmentBackspaceKey;
    REQUIRE(IsSegmentBackspaceKey(FanyImeIpc::kVirtualKeyBackspace, FanyImeIpc::kModifierControl));
    // Shift, Alt, the Windows keys and any extra modifier keep the host meaning.
    REQUIRE(!IsSegmentBackspaceKey(FanyImeIpc::kVirtualKeyBackspace,
                                   FanyImeIpc::kModifierShift | FanyImeIpc::kModifierControl));
    REQUIRE(!IsSegmentBackspaceKey(FanyImeIpc::kVirtualKeyBackspace,
                                   FanyImeIpc::kModifierControl | FanyImeIpc::kModifierAlt));
    REQUIRE(!IsSegmentBackspaceKey(FanyImeIpc::kVirtualKeyBackspace, 0));
    REQUIRE(!IsSegmentBackspaceKey(FanyImeIpc::kVirtualKeyBackspace, FanyImeIpc::kModifierShift));
    REQUIRE(!IsSegmentBackspaceKey(FanyImeIpc::kVirtualKeyBackspace, FanyImeIpc::kModifierAlt));
    REQUIRE(!IsSegmentBackspaceKey(FanyImeIpc::kVirtualKeyBackspace, FanyImeIpc::kModifierUiLess));
    REQUIRE(!IsSegmentBackspaceKey('A', FanyImeIpc::kModifierControl));
}

TEST_CASE(segment_caret_move_is_ctrl_only)
{
    using FanyImeIpc::IsSegmentCaretKey;
    REQUIRE(IsSegmentCaretKey(FanyImeIpc::kVirtualKeyLeft, FanyImeIpc::kModifierControl));
    REQUIRE(IsSegmentCaretKey(FanyImeIpc::kVirtualKeyRight, FanyImeIpc::kModifierControl));
    // Shift, Alt, the Windows keys and any extra modifier keep the host meaning.
    for (const unsigned extra : {FanyImeIpc::kModifierShift, FanyImeIpc::kModifierAlt})
    {
        REQUIRE(!IsSegmentCaretKey(FanyImeIpc::kVirtualKeyLeft, FanyImeIpc::kModifierControl | extra));
        REQUIRE(!IsSegmentCaretKey(FanyImeIpc::kVirtualKeyRight, FanyImeIpc::kModifierControl | extra));
    }
    REQUIRE(!IsSegmentCaretKey(FanyImeIpc::kVirtualKeyLeft, 0));
    REQUIRE(!IsSegmentCaretKey(FanyImeIpc::kVirtualKeyRight, 0));
    REQUIRE(!IsSegmentCaretKey(FanyImeIpc::kVirtualKeyLeft, FanyImeIpc::kModifierUiLess));
    REQUIRE(!IsSegmentCaretKey(FanyImeIpc::kVirtualKeyBackspace, FanyImeIpc::kModifierControl));
    REQUIRE(!IsSegmentCaretKey('A', FanyImeIpc::kModifierControl));
}

TEST_CASE(segment_caret_boundaries_stop_at_the_unit_next_to_the_caret)
{
    using FanyImeIpc::NextSegmentBoundary;
    using FanyImeIpc::PreviousSegmentBoundary;
    const std::vector<std::size_t> boundaries = {0, 3, 7, 9};
    // Inside a unit, on its first offset, on its last offset and past the end:
    // left lands on the unit start, right on the unit end, and both directions
    // are idempotent at the raw ends.
    struct Case
    {
        std::size_t caret;
        std::size_t previous;
        std::size_t next;
    };
    const Case cases[] = {
        {8, 7, 9}, // inside the last unit
        {7, 3, 9}, // first offset of the last unit
        {9, 7, 9}, // raw end: both directions stay put
        {1, 0, 3}, // inside the first unit
        {0, 0, 3}, // raw start: left stays put
        {4, 3, 7}, // inside the middle unit
    };
    for (const Case &expected : cases)
    {
        REQUIRE_EQ(PreviousSegmentBoundary(boundaries, expected.caret), expected.previous);
        REQUIRE_EQ(NextSegmentBoundary(boundaries, expected.caret), expected.next);
    }
    // No unit model: both directions keep the caret where the caller had it and
    // the caller falls back to the single-character move.
    REQUIRE_EQ(PreviousSegmentBoundary({}, 4), std::size_t(4));
    REQUIRE_EQ(NextSegmentBoundary({}, 4), std::size_t(4));
    // An incomplete tail is a unit of its own, so the jump stops inside the raw.
    const std::vector<std::size_t> partial = {0, 2, 3};
    REQUIRE_EQ(PreviousSegmentBoundary(partial, 3), std::size_t(2));
    REQUIRE_EQ(NextSegmentBoundary(partial, 2), std::size_t(3));
}

TEST_CASE(segment_backspace_drops_a_selected_segment_only_at_the_head_of_the_raw)
{
    using FanyImeIpc::ShouldDropCreatingWordSegment;
    REQUIRE(ShouldDropCreatingWordSegment(true, false, true, 0, 1));
    // Nothing before the caret is not enough: the word must be active, the
    // client must be able to apply the restore reply, and a snapshot must exist.
    REQUIRE(!ShouldDropCreatingWordSegment(false, false, true, 0, 1));
    REQUIRE(!ShouldDropCreatingWordSegment(true, true, true, 0, 1));
    REQUIRE(!ShouldDropCreatingWordSegment(true, false, false, 0, 1));
    REQUIRE(!ShouldDropCreatingWordSegment(true, false, true, 0, 0));
    // Raw still in front of the caret: delete that unit instead of a segment.
    REQUIRE(!ShouldDropCreatingWordSegment(true, false, true, 1, 1));
}

TEST_CASE(previous_segment_boundary_stops_at_the_unit_before_the_caret)
{
    using FanyImeIpc::PreviousSegmentBoundary;
    const std::vector<std::size_t> boundaries = {0, 3, 7, 9};
    // End of the spelling deletes the last unit, a caret inside a unit deletes
    // only the part in front of it, and a caret on a boundary deletes the unit
    // before that boundary.
    REQUIRE_EQ(PreviousSegmentBoundary(boundaries, 9), std::size_t(7));
    REQUIRE_EQ(PreviousSegmentBoundary(boundaries, 8), std::size_t(7));
    REQUIRE_EQ(PreviousSegmentBoundary(boundaries, 7), std::size_t(3));
    REQUIRE_EQ(PreviousSegmentBoundary(boundaries, 4), std::size_t(3));
    REQUIRE_EQ(PreviousSegmentBoundary(boundaries, 3), std::size_t(0));
    REQUIRE_EQ(PreviousSegmentBoundary(boundaries, 1), std::size_t(0));
    // Nothing before the caret: the caller falls back to one character.
    REQUIRE_EQ(PreviousSegmentBoundary(boundaries, 0), std::size_t(0));
    REQUIRE_EQ(PreviousSegmentBoundary({}, 4), std::size_t(4));
}

TEST_CASE(segment_deletion_does_not_leave_a_dangling_delimiter)
{
    using FanyImeIpc::DropDanglingSegmentDelimiter;
    // "ni'hao" - "hao" leaves the delimiter of the deleted unit behind.
    std::string trailing = "ni'";
    DropDanglingSegmentDelimiter(trailing, trailing.size());
    REQUIRE_EQ(trailing, std::string("ni"));

    // "ni'hao'ma" - "hao" would otherwise leave two delimiters in a row.
    std::string doubled = "ni''ma";
    DropDanglingSegmentDelimiter(doubled, 3);
    REQUIRE_EQ(doubled, std::string("ni'ma"));

    // "ni'hao" - "ni'" leaves a leading delimiter, and an ordinary letter
    // boundary is left untouched.
    std::string leading = "'hao";
    DropDanglingSegmentDelimiter(leading, 0);
    REQUIRE_EQ(leading, std::string("hao"));
    std::string untouched = "ni'hao";
    DropDanglingSegmentDelimiter(untouched, 3);
    REQUIRE_EQ(untouched, std::string("ni'hao"));
}

TEST_CASE(composition_reply_includes_japanese_long_vowel_key)
{
    // 日语模式下 '-' 打长音符，必须回包刷新候选框。
    REQUIRE(FanyImeIpc::ShouldSendCompositionReply(false, false, false, false, false, true));
}

TEST_CASE(japanese_long_vowel_key_is_not_word_to_character_key)
{
    // 词转字用 -/= 时，日语模式的 '-' 已被长音符占用，不能再触发词转字。
    REQUIRE_EQ(FanyImeIpc::WordToCharacterDirection(0xBD, '-', 0, true, true), -1);
    REQUIRE_EQ(FanyImeIpc::WordToCharacterDirection(0xBD, '-', 0, false, true), 0);
}

TEST_CASE(temporary_r_mode_japanese_session_is_not_replaced_by_config_sync)
{
    REQUIRE(FanyImeIpc::InputSessionMatchesConfig(false, true, true));
    REQUIRE(FanyImeIpc::InputSessionMatchesConfig(true, false, false));
    REQUIRE(!FanyImeIpc::InputSessionMatchesConfig(false, false, true));
    REQUIRE(!FanyImeIpc::InputSessionMatchesConfig(false, true, false));
}

TEST_CASE(caret_resegmentation_requires_negotiated_non_uiless_pinyin_composition)
{
    using FanyImeIpc::ShouldResegmentCompositionByCaret;
    // R10/AC8：协商过 CompositionRestore 的非 UILess 客户端才启用前缀重算。
    REQUIRE(ShouldResegmentCompositionByCaret(true, false, false, false));
    // 未协商（旧 DLL 组合）：一切照旧。
    REQUIRE(!ShouldResegmentCompositionByCaret(false, false, false, false));
    // UILess 宿主：候选窗由宿主自绘，回退路径不得变坏。
    REQUIRE(!ShouldResegmentCompositionByCaret(true, true, false, false));
    // 专用英文模式：光标仍是显示层插入点。
    REQUIRE(!ShouldResegmentCompositionByCaret(true, false, true, false));
    // K/U/T/E/M/J/Y 等特殊模式组合：无单元模型语义，不重算。
    REQUIRE(!ShouldResegmentCompositionByCaret(true, false, false, true));
}

TEST_CASE(caret_prefix_empty_requires_non_empty_raw_and_zero_prefix)
{
    using FanyImeIpc::IsCaretPrefixEmpty;
    // R4：光标在串首（量化后前缀为空）：无候选，候选窗隐藏。
    REQUIRE(IsCaretPrefixEmpty(0, 14));
    // 前缀非空：不算空。
    REQUIRE(!IsCaretPrefixEmpty(2, 14));
    // 整串解码（caret 未设置）：prefix_end == 串长，永不判空。
    REQUIRE(!IsCaretPrefixEmpty(14, 14));
    // raw 为空的组合：不属于 R4（避免把空组合误判成「前缀为空」）。
    REQUIRE(!IsCaretPrefixEmpty(0, 0));
}

TEST_CASE(caret_arrow_candidate_publish_rebuilds_from_engine_at_both_prefix_and_tail)
{
    using FanyImeIpc::CaretArrowCandidatePublish;
    using FanyImeIpc::ResolveCaretArrowCandidatePublish;
    // R4：前缀为空（raw 非空）——收起候选窗，与门控无关。
    REQUIRE_EQ(ResolveCaretArrowCandidatePublish(true, 0, 14), CaretArrowCandidatePublish::Hide);
    REQUIRE_EQ(ResolveCaretArrowCandidatePublish(false, 0, 14), CaretArrowCandidatePublish::Hide);
    // 门控开 × 前缀中间：按前缀候选重建页面。
    REQUIRE_EQ(ResolveCaretArrowCandidatePublish(true, 2, 14), CaretArrowCandidatePublish::RebuildFromEngine);
    // 门控开 × 回到串尾：引擎已按整串重算，页面必须从引擎重读重建（真机回归修复点）。
    REQUIRE_EQ(ResolveCaretArrowCandidatePublish(true, 14, 14), CaretArrowCandidatePublish::RebuildFromEngine);
    // 门控关（未协商/UILess/专用英文/特殊模式）：光标从不进会话，维持只刷新页面（AC8）。
    REQUIRE_EQ(ResolveCaretArrowCandidatePublish(false, 2, 14), CaretArrowCandidatePublish::RefreshPageOnly);
    REQUIRE_EQ(ResolveCaretArrowCandidatePublish(false, 14, 14), CaretArrowCandidatePublish::RefreshPageOnly);
    // 门控开但 raw 为空（仅剩已选汉字的中间态）：没有候选内容可重建，保持只刷新。
    REQUIRE_EQ(ResolveCaretArrowCandidatePublish(true, 0, 0), CaretArrowCandidatePublish::RefreshPageOnly);
    REQUIRE_EQ(ResolveCaretArrowCandidatePublish(false, 0, 0), CaretArrowCandidatePublish::RefreshPageOnly);
}

TEST_CASE(create_word_frame_carries_caret_only_for_negotiated_mid_string_caret)
{
    using FanyImeIpc::ShouldCreateWordFrameCarryCaret;
    // R5/AC2：协商侧前缀选词结算后光标归后缀首（0），必须携带让 DLL 镜到后缀首。
    REQUIRE(ShouldCreateWordFrameCarryCaret(true, 0, 14));
    // 协商侧光标在剩余 raw 中间：同样携带。
    REQUIRE(ShouldCreateWordFrameCarryCaret(true, 3, 14));
    // 协商侧串尾造词流：光标恒在末尾，省略字段与现状字节一致。
    REQUIRE(!ShouldCreateWordFrameCarryCaret(true, 14, 14));
    // 空 raw：无位置可表达，也不带。
    REQUIRE(!ShouldCreateWordFrameCarryCaret(true, 0, 0));
    // AC8 回归锚：未协商（旧 DLL）时无条件回 plain 3 字段帧——改动前这里光标
    // 在中间会误追加第 4 字段，旧解析器把尾部当 display_preedit。
    REQUIRE(!ShouldCreateWordFrameCarryCaret(false, 0, 14));
    REQUIRE(!ShouldCreateWordFrameCarryCaret(false, 3, 14));
    REQUIRE(!ShouldCreateWordFrameCarryCaret(false, 14, 14));
}

TEST_CASE(wubi_unique_four_code_commit_follows_its_switch_and_guards_its_preconditions)
{
    using FanyImeIpc::ShouldAutoCommitCompleteWubiCode;
    // Switch on (the default): a unique complete four-letter code commits on the fourth key.
    REQUIRE(ShouldAutoCommitCompleteWubiCode(true, true, false));
    // The engine did not report a complete unique four-letter code.
    REQUIRE(!ShouldAutoCommitCompleteWubiCode(true, false, false));
    // A word is being created: the raw is a prefix, so the composition stays open.
    REQUIRE(!ShouldAutoCommitCompleteWubiCode(true, true, true));
    // Switch off: the code stays in the candidate window whatever the engine reports, so the
    // engine-side facts above can never drag the commit back on.
    REQUIRE(!ShouldAutoCommitCompleteWubiCode(false, true, false));
    REQUIRE(!ShouldAutoCommitCompleteWubiCode(false, false, false));
    REQUIRE(!ShouldAutoCommitCompleteWubiCode(false, true, true));
}

TEST_CASE(wubi_top_word_commit_follows_its_switch_and_guards_its_preconditions)
{
    using FanyImeIpc::ShouldCommitCompleteWubiCodeOnNextKey;
    // Arguments: top_commit_enabled, mixed_pinyin_enabled, four_code_is_complete, key_is_letter,
    // caret_at_end, creating_word_active.
    for (const bool mixed : {false, true})
    {
        REQUIRE(ShouldCommitCompleteWubiCodeOnNextKey(true, mixed, true, true, true, false));
        // Not a complete table-answered code: nothing to commit, the key belongs to the composition.
        REQUIRE(!ShouldCommitCompleteWubiCodeOnNextKey(true, mixed, false, true, true, false));
        // Not a letter key (Backspace, arrows, space): those edit or commit the code in place.
        REQUIRE(!ShouldCommitCompleteWubiCodeOnNextKey(true, mixed, true, false, true, false));
        // The caret is inside the code, so the user is editing it, not typing past it.
        REQUIRE(!ShouldCommitCompleteWubiCodeOnNextKey(true, mixed, true, true, false, false));
        // A word being created owns the raw as a prefix; committing it would end the word early.
        REQUIRE(!ShouldCommitCompleteWubiCodeOnNextKey(true, mixed, true, true, true, true));
    }
    // Switch off with mixed input on: the letter grows the composition into a mixed spelling, so
    // nothing is committed.
    REQUIRE(!ShouldCommitCompleteWubiCodeOnNextKey(false, true, true, true, true, false));
    REQUIRE(!ShouldCommitCompleteWubiCodeOnNextKey(false, true, true, true, true, true));
    // Switch off with mixed input off: the engine would clip the letter away while a raw-preedit
    // client already shows it, so the top commit still runs and the key is never lost.
    REQUIRE(ShouldCommitCompleteWubiCodeOnNextKey(false, false, true, true, true, false));
    // The fallback keeps every other guard.
    REQUIRE(!ShouldCommitCompleteWubiCodeOnNextKey(false, false, false, true, true, false));
    REQUIRE(!ShouldCommitCompleteWubiCodeOnNextKey(false, false, true, true, true, true));
}
