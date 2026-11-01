// 按键处理：按键分类、组合编辑（光标/退格/插入）、译文副候选页、五笔顶字，以及 HandleImeKey 主流程。
#include "ipc/event_listener_internal.h"
#include <Windows.h>
#include <string>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>
#include "ipc.h"
#include "ipc/candidate_text_policy.h"
#include "ipc/candidate_translation_policy.h"
#include "ipc/input_key_policy.h"
#include "engine/contracts/date_time_input.h"
#include "engine/contracts/direct_helpcode.h"
#include "engine/contracts/ipc_negotiation.h"
#include "engine/contracts/mid_sentence_helpcode.h"
#include "engine/contracts/trilingual_input.h"
#include "engine/contracts/temporary_japanese_input.h"
#include "defines/defines.h"
#include "defines/globals.h"
#include "utils/common_utils.h"
#include "global/globals.h"
#include "window/caret_state_indicator_policy.h"
#include "utils/ime_utils.h"
#include "config/ime_config.h"
#include "conversion/chinese_converter.h"
#include "session/session_factory.h"
#include "mixed/date_time_candidates.h"
#include "log/candidate_diag_log.h"
#include "statistics/stats_candidate_capture.h"

using namespace event_listener_detail;

namespace
{
bool IsShiftLetterSpecialModeTriggered()
{
    return g_quick_phrase_triggered || g_radical_mode_triggered || g_date_time_mode_triggered ||
           g_emoji_mode_triggered || g_kaomoji_mode_triggered || g_symbol_mode_triggered || g_jianpin_mode_triggered ||
           g_y_mode_triggered || g_r_mode_triggered;
}

// 字母键写进 raw 的那个字母：键产生了字母就照收（保留大小写），否则取键位本身的小写字母。
// 插入与顶字重建共用这一条，preedit 才与用户敲键一致。
char CompositionLetter(UINT keycode, WCHAR wch)
{
    return (wch >= L'A' && wch <= L'Z') || (wch >= L'a' && wch <= L'z') ? static_cast<char>(wch)
                                                                        : static_cast<char>(keycode + ('a' - 'A'));
}

// Temporary R-mode policy of the client whose key is being handled. HandleImeKey
// resolves it once per key; the predicates below run several times per key.
FanyImeTemporaryJapanese::Policy g_key_temporary_japanese_policy = 0;

// Match TSF classification, including the negotiated temporary R-mode policy.
bool IsJapaneseInputMode()
{
    const auto &raw = GlobalIme::composition.raw_input_with_cases;
    return FanyImeTemporaryJapanese::IsMode(GetActiveInputMode() == "japanese", g_key_temporary_japanese_policy,
                                            raw.data(), raw.size());
}

// 日语模式下 '-' 不翻页，而是长音符（ー）的输入键。空编码时也要起头组合，
// 候选框第一项是长音符 ー、第二项是普通连字符 '-'（见日语候选提供者）。
bool IsJapaneseLongVowelKey(UINT keycode, WCHAR wch)
{
    return FanyImeIpc::ResolveJapaneseKeyRoute(IsJapaneseInputMode(), keycode, wch, Global::ModifiersDown,
                                               g_inputSession != nullptr)
        .long_vowel;
}

// 日语模式下 '-' '=' 一律不当翻页键用。
bool IsJapaneseDisabledPagingKey(UINT keycode)
{
    return FanyImeIpc::ResolveJapaneseKeyRoute(IsJapaneseInputMode(), keycode, Global::Wch, Global::ModifiersDown,
                                               g_inputSession != nullptr)
        .suppress_paging;
}

bool IsCommitWithHighlightedCandidatePunctuationInCandidateMode(UINT keycode, WCHAR wch)
{
    if (keycode == VK_TAB)
    {
        return false;
    }
    if ((keycode == VK_OEM_MINUS || keycode == VK_OEM_PLUS) && !IsJapaneseDisabledPagingKey(keycode))
    {
        return false;
    }
    // 日语模式下 '-' 走长音符输入，不能当作上屏标点。
    if (IsJapaneseLongVowelKey(keycode, wch))
    {
        return false;
    }
    const bool has_active_composition = g_inputSession != nullptr && !g_inputSession->get_pinyin_sequence().empty();
    if ((keycode == VK_OEM_COMMA || keycode == VK_OEM_PERIOD) && GetConfiguredPagingCommaPeriodEnabled() &&
        has_active_composition)
    {
        return false;
    }
    if ((keycode == VK_OEM_4 || keycode == VK_OEM_6) && GetConfiguredPagingBracketsEnabled() && has_active_composition)
    {
        return false;
    }

    static const std::unordered_set<WCHAR> kCommitWithHighlightedCandidatePunctuation = {
        L'`',  //
        L'!',  //
        L'@',  //
        L'#',  //
        L'$',  //
        L'%',  //
        L'^',  //
        L'&',  //
        L'*',  //
        L'-',  // Numpad arithmetic keys are not candidate paging keys.
        L'+',  //
        L'_',  // 日语模式禁用 -/= 翻页后，这两个字符退回标点上屏。
        L'=',  //
        L'(',  //
        L')',  //
        L'[',  //
        L']',  //
        L'\\', //
        L'/',  //
        L';',  //
        L':',  //
        L'\'', //
        L'"',  //
        L',',  //
        L'<',  //
        L'.',  //
        L'>',  //
        L'?'   //
    };
    return kCommitWithHighlightedCandidatePunctuation.find(wch) != kCommitWithHighlightedCandidatePunctuation.end();
}

bool IsManualPinyinSeparatorKey(UINT keycode, WCHAR wch)
{
    return keycode == VK_OEM_7 && wch == L'\'' && g_inputSession != nullptr &&
           g_inputSession->current_scheme_type() != SchemeType::Wubi && !g_inputSession->get_pinyin_sequence().empty();
}

bool IsMicrosoftShuangpinIngKeyAt(UINT keycode, WCHAR wch, const std::string &raw_input, size_t caret)
{
    if (keycode != VK_OEM_1 || wch != L';' || !IsConfiguredShuangpinSemicolonFinal() || g_inputSession == nullptr ||
        g_inputSession->current_scheme_type() != SchemeType::Shuangpin)
    {
        return false;
    }

    // 直接辅助码的辅码会打乱奇偶，改成只看前一个键是不是字母；TSF 按同一条规则吃键。
    if (GetConfiguredShuangpinDirectHelpcodeEnabled())
    {
        return FanyImeDirectHelpcode::AcceptsSemicolonFinalAt(raw_input.data(), raw_input.size(), caret);
    }
    // 句中辅助码或末尾辅助码的大写触发生效时大写段不算这一节的键，末尾段同样不算；都没有时就是原来「最后一个 '
    // 之后数奇偶」。TSF 同规则。
    return FanyImeMidSentenceHelpcode::AcceptsSemicolonFinalAt(
        raw_input.data(), raw_input.size(), caret,
        {IsConfiguredMidSentenceHelpcodeUppercaseTrigger(), IsConfiguredEndHelpcodeUppercaseTrigger()});
}

// 双拼直接辅助码四码后的 /：光标前是「两键音节 + 两位辅码」的形状时它是编码键而不是标点。TSF 按同一条
// 形状规则（FanyImeDirectHelpcode::AcceptsSlashAt）预判吃键，光标按 IsMidSentenceHelpcodeMarkerKey 同样的
// 方式算。
bool IsDirectHelpcodeSlashKey(UINT keycode, WCHAR wch, const std::string &raw_input)
{
    if (keycode != VK_OEM_2 || wch != L'/' || g_english_input_mode || g_inputSession == nullptr)
    {
        return false;
    }
    const auto &composition = GlobalIme::composition;
    const size_t caret = composition.raw_input_with_cases != raw_input && composition.caret_position == 0
                             ? raw_input.size()
                             : (std::min)(composition.caret_position, raw_input.size());
    return g_inputSession->accepts_direct_helpcode_slash(caret);
}

// Shift+T 指定日期时间的数字、/ 和 :：插入后 T 后面那一串还能接成某种日期时间形状时它是编码键，否则数字
// 仍是选词键、/ 和 : 仍是标点；Shift+数字的字符不是数字，始终选词。TSF 看不到配置，只按开头的 T 和同一条
// 形状规则（FanyImeDateTimeInput::AcceptsAt）预判吃键，所以这里也不看 T 模式开没开，两边的输入串才不会
// 分叉。小键盘数字到这里时已经归一成主键盘数字（NormalizeNumpadDigitKey），TSF 同样收它。光标按
// IsDirectHelpcodeSlashKey 同样的方式算。
bool IsDateTimeInputKey(UINT keycode, WCHAR wch, const std::string &raw_input)
{
    const bool digit = keycode >= '0' && keycode <= '9' && wch >= L'0' && wch <= L'9';
    const bool slash = keycode == VK_OEM_2 && wch == L'/';
    const bool colon = keycode == VK_OEM_1 && wch == L':';
    if ((!digit && !slash && !colon) || g_inputSession == nullptr)
    {
        return false;
    }
    const auto &composition = GlobalIme::composition;
    const size_t caret = composition.raw_input_with_cases != raw_input && composition.caret_position == 0
                             ? raw_input.size()
                             : (std::min)(composition.caret_position, raw_input.size());
    return FanyImeDateTimeInput::AcceptsAt(raw_input.data(), raw_input.size(), caret, static_cast<char>(wch));
}

// V 模式的数字和 . + - * / ( )：插入后仍是一段 V 模式输入时是编码键，不再选词、翻页或当标点。只看字符不看
// 键位，主键盘和小键盘一样；Shift+1 这类出 ! 的仍是选词键。TSF 按同一条规则
// （FanyImeVModeInput::AcceptsAt）和同一个 Trigger（VModeChanged）预判吃键。光标按 IsDirectHelpcodeSlashKey
// 同样的方式算。
bool IsVModeInputKey(WCHAR wch, const std::string &raw_input)
{
    if (wch > 0x7f || g_inputSession == nullptr)
    {
        return false;
    }
    const auto &composition = GlobalIme::composition;
    const size_t caret = composition.raw_input_with_cases != raw_input && composition.caret_position == 0
                             ? raw_input.size()
                             : (std::min)(composition.caret_position, raw_input.size());
    return FanyImeVModeInput::AcceptsAt(raw_input.data(), raw_input.size(), caret, static_cast<char>(wch),
                                        CurrentVModeTrigger());
}

bool IsMicrosoftShuangpinIngKey(UINT keycode, WCHAR wch, const std::string &raw_input)
{
    return IsMicrosoftShuangpinIngKeyAt(keycode, wch, raw_input,
                                        (std::min)(GlobalIme::composition.caret_position, raw_input.size()));
}

// 句中辅助码的触发键（反引号或分号，按设置里勾选的）：开关开着、双拼，且光标前这一节能接一段时，
// 它是编码键而不是标点，输入串里一律记成反引号。光标可以在句中（用箭头移回去补辅助码）。TSF 端按
// 同一条形状规则（FanyImeMidSentenceHelpcode::AcceptsMarkerAt）预判吃键，光标也按
// ApplyCompositionEditKey 插字时的同一个位置算。分号先让给 ing 韵母（IsMicrosoftShuangpinIngKey），
// 两侧判断顺序一致。
bool IsMidSentenceHelpcodeMarkerKey(UINT keycode, WCHAR wch, const std::string &raw_input)
{
    const bool backtick = keycode == VK_OEM_3 && wch == L'`';
    const bool semicolon = keycode == VK_OEM_1 && wch == L';';
    if ((!backtick && !semicolon) || !IsConfiguredMidSentenceHelpcodeTrigger(wch) || g_english_input_mode ||
        g_inputSession == nullptr)
    {
        return false;
    }
    const auto &composition = GlobalIme::composition;
    const size_t caret = composition.raw_input_with_cases != raw_input && composition.caret_position == 0
                             ? raw_input.size()
                             : (std::min)(composition.caret_position, raw_input.size());
    if (semicolon && IsMicrosoftShuangpinIngKeyAt(keycode, wch, raw_input, caret))
    {
        return false;
    }
    return g_inputSession->accepts_mid_sentence_helpcode_marker(caret);
}

// 双拼末尾辅助码（间接）的触发键（反引号或分号，按设置里勾选的、且没被句中辅助码占着的）：开关开着、
// 关着直接触发、双拼，且光标在输入串末尾、前面是一个个完整音节时，它是编码键而不是标点，输入串里一律
// 记成 FanyImeMidSentenceHelpcode::kEndMarker。TSF 按同一条形状规则（AcceptsEndMarkerAt）预判吃键；
// 分号同样先让给 ing 韵母。
bool IsEndHelpcodeMarkerKey(UINT keycode, WCHAR wch, const std::string &raw_input)
{
    const bool backtick = keycode == VK_OEM_3 && wch == L'`';
    const bool semicolon = keycode == VK_OEM_1 && wch == L';';
    if ((!backtick && !semicolon) || !IsConfiguredEndHelpcodeTrigger(wch) || g_english_input_mode ||
        g_inputSession == nullptr)
    {
        return false;
    }
    const auto &composition = GlobalIme::composition;
    const size_t caret = composition.raw_input_with_cases != raw_input && composition.caret_position == 0
                             ? raw_input.size()
                             : (std::min)(composition.caret_position, raw_input.size());
    if (semicolon && IsMicrosoftShuangpinIngKeyAt(keycode, wch, raw_input, caret))
    {
        return false;
    }
    return g_inputSession->accepts_end_helpcode_marker(caret);
}

// 直接辅助码下用引导键（末尾辅助码设置里勾了的反引号、分号）进入末尾辅助码：TSF 把输入末尾形状合适的引导键、
// 以及删掉它留下的末尾段开头的退格打上
// FanyImePipeFlags::DirectHelpcodeEndTriggerRewrite 交过来，并且一定会读这次的回复。是否这样处理以标记为准，
// 不再按配置和形状重算：配置刚改、TSF 还没收到新状态时，两边仍按同一个约定走。HandleImeKey 每键解析一次。
bool g_key_direct_end_trigger_request = false;

// 这颗引导键真的要改写输入串：TSF 打了标记，且直接辅助码的切分里每个音节都是「完整双拼 + 0~2 个辅码」，返回
// 去掉辅码后的纯双拼。改写不了时引导键照旧当标点（上屏高亮候选 + 标点），TSF 等的回帧就是那一帧。
std::optional<std::string> DirectHelpcodeEndTriggerPrefix(UINT keycode, WCHAR wch, const std::string &raw_input)
{
    const bool trigger_key = (keycode == VK_OEM_1 && wch == L';') || (keycode == VK_OEM_3 && wch == L'`');
    if (!g_key_direct_end_trigger_request || !trigger_key || g_english_input_mode || g_inputSession == nullptr ||
        IsUiLessMode() || IsJapaneseInputMode())
    {
        return std::nullopt;
    }
    return g_inputSession->direct_helpcode_end_trigger_prefix(raw_input);
}

bool IsSelectionKey(UINT keycode)
{
    if (keycode == VK_SPACE)
        return true;
    if (keycode >= '0' && keycode <= '9')
    {
        const std::string raw = g_inputSession ? g_inputSession->get_pinyin_sequence_with_cases() : std::string{};
        // T 模式指定日期时间：接得上的数字是编码键；V 模式里的数字也是。
        return !IsDateTimeInputKey(keycode, Global::Wch, raw) && !IsVModeInputKey(Global::Wch, raw);
    }
    return false;
}

bool IsPagingKey(UINT keycode)
{
    if (IsJapaneseDisabledPagingKey(keycode))
    {
        return false;
    }
    return keycode == VK_OEM_MINUS || keycode == VK_OEM_PLUS || keycode == VK_TAB || keycode == VK_PRIOR ||
           keycode == VK_NEXT || keycode == VK_LEFT || keycode == VK_RIGHT || keycode == VK_UP || keycode == VK_DOWN ||
           ((keycode == VK_OEM_COMMA || keycode == VK_OEM_PERIOD) && GetConfiguredPagingCommaPeriodEnabled()) ||
           ((keycode == VK_OEM_4 || keycode == VK_OEM_6) && GetConfiguredPagingBracketsEnabled());
}

bool IsCandidateNavigationKey(UINT keycode)
{
    if (IsJapaneseDisabledPagingKey(keycode))
    {
        return false;
    }
    return keycode == VK_OEM_MINUS || keycode == VK_OEM_PLUS || keycode == VK_OEM_COMMA || keycode == VK_OEM_PERIOD ||
           keycode == VK_OEM_4 || keycode == VK_OEM_6 || keycode == VK_TAB || keycode == VK_PRIOR ||
           keycode == VK_NEXT || keycode == VK_UP || keycode == VK_DOWN;
}

bool ApplyCompositionEditKey(UINT keycode, WCHAR wch, UINT modifiers_down, bool client_supports_restore,
                             bool &composition_restored)
{
    composition_restored = false;
    std::string raw = g_inputSession->get_pinyin_sequence_with_cases();
    auto &composition = GlobalIme::composition;
    // The R prefix makes display raw differ from engine raw even at a valid body-start caret.
    composition.caret_position = FanyImeIpc::ResolveCompositionCaret(
        composition.caret_position, composition.raw_input_with_cases, raw, g_r_mode_triggered && IsJapaneseInputMode());
    composition.caret_position = (std::min)(composition.caret_position, raw.size());

    // R2/R10：光标前缀重算总门控（与 Ctrl+Backspace / Ctrl+方向同一谓词族）。未协商、
    // UILess、专用英文、特殊模式组合一律维持整串转换，光标只是显示层插入点。
    const bool caret_resegmentation = FanyImeIpc::ShouldResegmentCompositionByCaret(
        client_supports_restore, IsUiLessMode(), g_english_input_mode, IsSpecialModeCompositionActive(raw));
    // 箭头与 Ctrl+方向路径不改 raw、没有 pending 序列，喂完光标重解一次即可。串尾
    // 也必须显式喂：引擎 caret_ 只在 set_pinyin_sequence 触发的 apply_pending_sequence
    // 里复位，箭头路径绕过它——串尾不喂 nullopt（与 set_caret(size) 在量化边界上等
    // 价）会残留上一次前缀激活的 caret_，候选停在旧前缀上、空格结算走错前缀路径（R7）。
    const auto resegment_by_caret = [&]() {
        if (!caret_resegmentation)
        {
            return;
        }
        g_inputSession->set_caret(composition.caret_position < raw.size()
                                      ? std::optional<std::size_t>(composition.caret_position)
                                      : std::nullopt);
        g_inputSession->recompute_candidates();
    };

    // Ctrl+Left / Ctrl+Right jump the caret by one segmentation unit instead of
    // one character, consuming the same engine boundaries Ctrl+Backspace
    // deletes. The Server owns the unit model, so it moves the authoritative
    // caret and answers with CompositionRestored; TSF only applies that caret.
    // Everything unnegotiated, UILess or unit-less keeps the single-character
    // move below, so both sides agree on when the jump happens.
    const bool segment_caret = FanyImeIpc::IsSegmentCaretKey(keycode, modifiers_down);
    const bool segment_caret_supported = segment_caret && client_supports_restore && !IsUiLessMode() &&
                                         !g_english_input_mode && !IsSpecialModeCompositionActive(raw);
    if (keycode == VK_LEFT || keycode == VK_RIGHT)
    {
        if (segment_caret_supported)
        {
            const std::vector<std::size_t> boundaries = g_inputSession->segment_raw_boundaries();
            if (!boundaries.empty())
            {
                composition.caret_position =
                    keycode == VK_LEFT ? FanyImeIpc::PreviousSegmentBoundary(boundaries, composition.caret_position)
                                       : FanyImeIpc::NextSegmentBoundary(boundaries, composition.caret_position);
                composition_restored = true;
                resegment_by_caret();
                return true;
            }
        }
        if (keycode == VK_LEFT)
        {
            if (composition.caret_position > 0)
            {
                --composition.caret_position;
            }
        }
        else if (composition.caret_position < raw.size())
        {
            ++composition.caret_position;
        }
        resegment_by_caret();
        return true;
    }

    // A spelling emptied by a segment Backspace below keeps the creating-word
    // state alive with the word alone (PRD R3), so the empty-raw cleanup has to
    // know this key produced that state on purpose.
    bool keep_creating_word_after_empty_raw = false;

    if (keycode == VK_BACK && g_key_direct_end_trigger_request && client_supports_restore && !IsUiLessMode() &&
        !FanyImeIpc::IsSegmentBackspaceKey(keycode, modifiers_down))
    {
        // 删的是进入末尾辅助码的那个引导键：前面还是当时整理出的样子就恢复整理前的原串（句中辅码重新生效），
        // 否则只删光标前的一个字符。两种结果都回 CompositionRestored，TSF 照着重建，自己不删。
        if (!composition.direct_end_trigger_original.empty() && raw == composition.direct_end_trigger_rewritten)
        {
            raw = composition.direct_end_trigger_original;
            composition.caret_position = raw.size();
        }
        else if (composition.caret_position > 0)
        {
            raw.erase(composition.caret_position - 1, 1);
            --composition.caret_position;
        }
        composition.clear_direct_end_trigger();
        composition_restored = true;
        keep_creating_word_after_empty_raw =
            raw.empty() && FanyImeIpc::ShouldKeepCreatingWordAfterRawEmptied(composition.creating_word.active,
                                                                             IsUiLessMode(), client_supports_restore,
                                                                             composition.selection_history.size());
    }
    else if (keycode == VK_BACK)
    {
        // Ctrl+Backspace deletes one segmentation unit (one character's pinyin)
        // instead of one character. The boundaries are the engine's, so TSF
        // cannot mirror the deletion: it rebuilds from the CompositionRestored
        // reply, and everything unnegotiated or unit-less falls back to the
        // ordinary single-character behavior right below.
        const bool segment_backspace = FanyImeIpc::IsSegmentBackspaceKey(keycode, modifiers_down);
        const bool segment_supported = segment_backspace && client_supports_restore && !IsUiLessMode() &&
                                       !g_english_input_mode && !IsSpecialModeCompositionActive(raw);
        if (segment_supported && FanyImeIpc::ShouldDropCreatingWordSegment(
                                     composition.creating_word.active, IsUiLessMode(), client_supports_restore,
                                     composition.caret_position, composition.selection_history.size()))
        {
            // R3: nothing is left before the caret, so the key removes the last
            // selected segment itself. Its spelling is discarded -- unlike the
            // retraction below the user asked to delete the segment, not to
            // edit its pinyin again -- and the raw stays empty.
            composition_restored = composition.drop_last_selection();
            keep_creating_word_after_empty_raw = composition_restored && composition.creating_word.active;
        }
        else if (segment_supported)
        {
            const std::vector<std::size_t> boundaries = g_inputSession->segment_raw_boundaries();
            const std::size_t start = FanyImeIpc::PreviousSegmentBoundary(boundaries, composition.caret_position);
            if (start < composition.caret_position)
            {
                raw.erase(start, composition.caret_position - start);
                composition.caret_position = start;
                FanyImeIpc::DropDanglingSegmentDelimiter(raw, start);
                composition_restored = true;
                // Emptying the raw does not end the word: the accumulated
                // segments stay on screen and the next Ctrl+Backspace drops one
                // of them (R3).
                keep_creating_word_after_empty_raw =
                    raw.empty() && FanyImeIpc::ShouldKeepCreatingWordAfterRawEmptied(
                                       composition.creating_word.active, IsUiLessMode(), client_supports_restore,
                                       composition.selection_history.size());
            }
        }

        if (!composition_restored &&
            FanyImeIpc::ShouldRetreatCreatingWordSelection(
                composition.creating_word.active, IsUiLessMode(), client_supports_restore, raw.size(),
                composition.selection_history.size(), composition.last_selection_raw_edited()))
        {
            // This Backspace must not also delete the character: the retraction
            // removes the segment and restores its raw spelling instead. The
            // restore is state only -- the engine sequence, its candidates and
            // the restored-caret prefix are applied exactly once by the tail
            // below, so nothing here may rebuild them; a helper that did cost
            // a second full candidate query on every retraction.
            composition_restored = composition.restore_last_selection();
            if (composition_restored)
            {
                // The retraction already replaced the raw, the word and the
                // caret; the local copy must follow it so the tail below
                // re-applies the restored sequence with its caret prefix
                // recompute instead of clobbering it with the stale raw.
                raw = composition.raw_input_with_cases;
            }
        }
        else if (!composition_restored && composition.caret_position > 0)
        {
            raw.erase(composition.caret_position - 1, 1);
            --composition.caret_position;
            // Deleting the last raw character must not take the accumulated word
            // down with it: the composition stays alive showing the selected
            // segments alone -- the same R3 state a segment Backspace produces --
            // and the reply below tells the client to keep composing instead of
            // cancelling. The next Backspace then retracts the newest selection
            // from that empty raw (the empty-raw override of the edit lock)
            // rather than discarding everything the user picked.
            keep_creating_word_after_empty_raw =
                raw.empty() && FanyImeIpc::ShouldKeepCreatingWordAfterRawEmptied(
                                   composition.creating_word.active, IsUiLessMode(), client_supports_restore,
                                   composition.selection_history.size());
        }
    }
    else if (keycode == VK_DELETE)
    {
        if (composition.caret_position < raw.size())
        {
            raw.erase(composition.caret_position, 1);
        }
    }
    else
    {
        char input = 0;
        if (keycode >= 'A' && keycode <= 'Z')
        {
            input = CompositionLetter(keycode, wch);
        }
        else if (keycode == VK_OEM_7 && wch == L'\'')
        {
            input = '\'';
        }
        else if (IsMicrosoftShuangpinIngKey(keycode, wch, raw))
        {
            input = ';';
        }
        else if (IsMidSentenceHelpcodeMarkerKey(keycode, wch, raw))
        {
            input = '`';
        }
        else if (IsEndHelpcodeMarkerKey(keycode, wch, raw))
        {
            input = FanyImeMidSentenceHelpcode::kEndMarker;
        }
        else if (const auto prefix =
                     client_supports_restore ? DirectHelpcodeEndTriggerPrefix(keycode, wch, raw) : std::nullopt)
        {
            // 直接辅助码下用引导键进入末尾辅助码：各音节上的辅码删掉，整串改写成「纯双拼 + 末尾段开头」，记下
            // 原串供删掉这个引导键时恢复。改写由 CompositionRestored 带给 TSF。
            composition.direct_end_trigger_original = raw;
            raw = *prefix + FanyImeMidSentenceHelpcode::kEndMarker;
            composition.direct_end_trigger_rewritten = raw;
            composition.caret_position = raw.size();
            composition.note_raw_inserted();
            composition_restored = true;
        }
        else if (IsDirectHelpcodeSlashKey(keycode, wch, raw))
        {
            input = '/';
        }
        else if (IsJapaneseLongVowelKey(keycode, wch))
        {
            input = '-';
        }
        else if (keycode == VK_OEM_MINUS && (wch == L'-' || wch == L'_'))
        {
            // Page-boundary minus and its Shift variant become literal input.
            input = static_cast<char>(wch);
        }
        else if (keycode == VK_OEM_PLUS && wch == L'+')
        {
            // Shift+= becomes a literal plus.
            input = static_cast<char>(wch);
        }
        else if (IsDateTimeInputKey(keycode, wch, raw) || IsVModeInputKey(wch, raw))
        {
            input = static_cast<char>(wch);
        }
        else
        {
            return false;
        }
        // U 模式里引号、分号、斜杠、大写字母这些键不能在这里吞掉：TSF 已经把它们当输入键记进了
        // 自己的按键缓冲，吞掉又不发 CompositionRestored 重同步，两侧的串和光标就会错位，之后的
        // 退格删的也不是同一个字符。照常插入，拆字查询对含非 a-z 的码返回空候选即可。
        if (input == '\'' && ((composition.caret_position > 0 && raw[composition.caret_position - 1] == '\'') ||
                              (composition.caret_position < raw.size() && raw[composition.caret_position] == '\'')))
        {
            return true;
        }
        // 0：上面已经整串改写过（直接辅助码下的 ;），没有要插入的字符。
        if (input != 0)
        {
            raw.insert(raw.begin() + static_cast<std::ptrdiff_t>(composition.caret_position), input);
            ++composition.caret_position;
            // Typing locks the newest selection (Rime's selected_before_editing):
            // Backspace must keep deleting these fresh characters instead of
            // retracting the selection out from under them. Caret moves never reach
            // here and deliberately do not lock.
            composition.note_raw_inserted();
        }
    }

    if (raw.empty() && !keep_creating_word_after_empty_raw)
    {
        // Without a kept state, TSF cancels the whole composition as soon as the
        // last remaining character is gone, so the accumulated word and the
        // snapshots a later Backspace could retract from must not survive here:
        // they would let a fresh pinyin composition retract a segment of the
        // previous one. Both Backspaces that legitimately empty the raw keep them
        // on purpose instead: the segment one through R3, the plain one because
        // its reply tells the client to keep composing with the word alone.
        composition.clear_creating_word();
        composition.selection_history.clear();
    }

    g_inputSession->set_pinyin_sequence(raw);
    g_inputSession->set_pinyin_sequence_with_cases(raw);
    // 五笔会把普通码的原样串裁成四码；T/V 模式的串得原样留着，形状判定才读得到，
    // 其中的字母也不能被当成码去查表、自动上屏或顶字（Tdate 归一出 tdat）。
    g_inputSession->set_wubi_literal_input(IsWubiLiteralComposition(raw));
    if (caret_resegmentation && composition.caret_position < raw.size())
    {
        // apply_pending_sequence() 会复位引擎光标：先让新 raw 生效，再喂光标做前缀
        // 重解（R2/R6）。caret 在串尾时不进这里，上一次 recompute 就是现状整串解码
        // （R7 零回归）。
        g_inputSession->recompute_candidates();
        g_inputSession->set_caret(composition.caret_position);
        g_inputSession->recompute_candidates();
    }
    else
    {
        g_inputSession->recompute_candidates();
    }
    composition.raw_input_with_cases = raw;
    return true;
}

// A literal character that would otherwise be swallowed as a candidate-navigation
// key becomes part of the raw spelling. The engine cannot split a spelling that
// carries it, so the candidate list collapses to the single fallback entry
// showing the typed string, and Space commits exactly that string.
//
// Only hosts that negotiated composition restore can apply the frame. Callers
// decide which keys qualify; this helper only checks that the append itself is
// meaningful.
bool AppendLiteralToComposition(WCHAR literal, uint64_t client_id, uint64_t activation_epoch, uint64_t request_id)
{
    const std::string raw = g_inputSession->get_pinyin_sequence_with_cases();
    if (raw.empty() || GlobalIme::composition.caret_position < raw.size() || g_english_input_mode || IsUiLessMode() ||
        IsJapaneseInputMode() || IsSpecialModeCompositionActive(raw) || IsCandidateSubPageActive() ||
        !ClientNegotiatedCompositionRestore(client_id))
    {
        return false;
    }

    // Reuse the ordinary edit-key path so the engine sequence, the literal-input
    // flag, wubi truncation and the caret rule all match a normal typed letter.
    const UINT keycode = literal == L'+' ? VK_OEM_PLUS : VK_OEM_MINUS;
    bool composition_restored = false;
    if (!ApplyCompositionEditKey(keycode, literal, Global::ModifiersDown, true, composition_restored))
    {
        return false;
    }
    if (composition_restored)
    {
        return false;
    }

    // PrepareCandidateList emits the same single fallback for an unsplittable
    // spelling, so later rebuilds keep the entry instead of dropping it right
    // before Space.
    FanyNamedPipe::PrepareCandidateList(client_id, activation_epoch);

    Global::MsgTypeToTsf = Global::DataFromServerMsgType::CompositionRestored;
    Global::candidate_ui.selected_text = BuildCreateWordPipePayload(g_inputSession->get_pinyin_sequence_with_cases(),
                                                                    GlobalIme::composition.creating_word.word);
    FanyNamedPipe::SendCurrentDataToClient(client_id, activation_epoch, request_id);
    return true;
}

// 按键延迟必须用高精度单调时钟。GetTickCount64 每个系统 tick 才前进一次（约 15.6 ms），
// 分辨不出 8 ms 阈值以下的差异。steady_clock 在 Windows 上走 QPC。
using KeyLatencyClock = std::chrono::steady_clock;
constexpr double kSlowStageThresholdMs = 8.0;

double ElapsedMilliseconds(KeyLatencyClock::time_point start, KeyLatencyClock::time_point end = KeyLatencyClock::now())
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// HandleImeKey 的分段计时打点：构造记起点，Stop() 或析构时耗时 >=8ms 才打一条
// stage=handle-<sub> 分段日志（阈值、字段集与 ScopedServerKeyLatency 一致）。部分调用点
// 落在 if 条件里，RAII 作用域包不住条件求值，那些点用 Stop() 显式收口；Stop() 只生效
// 一次，之后析构不再打点。红线：行内仅 stage 名/request_id/client/epoch/elapsed_ms，
// 不得出现按键、候选词、预编辑或提交文本。
struct ScopedKeyStage
{
    ScopedKeyStage(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id, const wchar_t *stage)
        : client_id_(client_id), activation_epoch_(activation_epoch), request_id_(request_id), stage_(stage)
    {
    }

    ~ScopedKeyStage()
    {
        Stop();
    }

    ScopedKeyStage(const ScopedKeyStage &) = delete;
    ScopedKeyStage &operator=(const ScopedKeyStage &) = delete;

    void Stop()
    {
        if (!stopped_)
        {
            stopped_ = true;
            const double elapsed_ms = ElapsedMilliseconds(started_at_);
            if (elapsed_ms >= kSlowStageThresholdMs)
            {
                DIAG_LOGF(L"[key-latency] side=server stage={} request={} client={} epoch={} elapsed_ms={:.3f}", stage_,
                          request_id_, client_id_, activation_epoch_, elapsed_ms);
            }
        }
    }

    uint64_t client_id_;
    uint64_t activation_epoch_;
    uint64_t request_id_;
    const wchar_t *stage_;
    KeyLatencyClock::time_point started_at_ = KeyLatencyClock::now();
    bool stopped_ = false;
};
} // namespace

namespace FanyNamedPipe
{
struct ScopedServerKeyLatency
{
    uint64_t client_id;
    uint64_t activation_epoch;
    uint64_t request_id;
    KeyLatencyClock::time_point started_at = KeyLatencyClock::now();

    ~ScopedServerKeyLatency()
    {
        const double elapsed_ms = ElapsedMilliseconds(started_at);
        if (elapsed_ms >= kSlowStageThresholdMs)
        {
            DIAG_LOGF(L"[key-latency] side=server stage=handle request={} client={} epoch={} elapsed_ms={:.3f}",
                      request_id, client_id, activation_epoch, elapsed_ms);
        }
    }
};

// 把候选框换回 Ctrl+Enter 或选中「📅日期」之前的那一屏。子页期间输入串一个字都没动，session
// 里的候选还是原来那批，所以这里直接把存下来的 items / 页码 / 高亮位放回去。子页期间切过字形时，
// 放回的列表还是按旧字形展开的写法，要按当前字形换写法，显示与上屏才跟得上切换。随后这颗按键
// 继续走它本来的流程，就像子页从来没出现过一样。
void ExitCandidateSubPage()
{
    if (!IsCandidateSubPageActive())
    {
        return;
    }
    g_translation_candidates_active = false;
    g_date_time_page_active = false;
    g_date_time_page_keyword.clear();
    auto &ui = Global::candidate_ui;
    ui.set_items(std::move(g_translation_saved_items));
    g_translation_saved_items.clear();
    ui.page_index = g_translation_saved_page_index;
    ui.selected_index_in_page = g_translation_saved_selected_index;
    ui.selection_started = g_translation_saved_selection_started;
    g_translation_saved_page_index = 0;
    g_translation_saved_selected_index = 0;
    g_translation_saved_selection_started = false;
    RefreshCandidatePageUi(false);
    if (CandidateListCharacterSet() != GetConfiguredCharacterSet())
        RespellCandidatesForCharacterSetChange();
}

// 混输里的「📅日期」入口：把候选框换成这组的全部格式（顺序与 Shift+T 模式同一份），空格/数字键
// 照常选一条上屏，其它键先换回原来那一屏（见 HandleImeKey）。
bool EnterDateTimeCandidatePage(const std::string &keyword)
{
    if (IsCandidateSubPageActive())
    {
        return false;
    }
    auto items = OrderedDateTimeCandidates(keyword);
    if (items.empty())
    {
        return false;
    }
    auto &ui = Global::candidate_ui;
    g_translation_saved_items = ui.items;
    g_translation_saved_page_index = ui.page_index;
    g_translation_saved_selected_index = ui.selected_index_in_page;
    g_translation_saved_selection_started = ui.selection_started;
    ui.set_items(std::move(items));
    g_date_time_page_active = true;
    g_date_time_page_keyword = keyword;
    RefreshCandidatePageUi(true);
    return true;
}

void RebuildDateTimeCandidatePage()
{
    if (!g_date_time_page_active)
    {
        return;
    }
    auto &ui = Global::candidate_ui;
    const int page_index = ui.page_index;
    const int selected_index = ui.selected_index_in_page;
    ui.set_items(OrderedDateTimeCandidates(g_date_time_page_keyword));
    if (page_index > 0 && page_index * ui.page_size < static_cast<int>(ui.items.size()))
    {
        ui.page_index = page_index;
    }
    ui.selected_index_in_page = selected_index;
    RefreshCandidatePageUi(true);
}

// Ctrl+Enter：上屏高亮候选右边的那条译文（副候选）。Ctrl+数字：上屏指定序号候选的那条译文
// （forced_index_in_page，0 起）。只有一条译义就直接上屏；有多条时把候选框整个换成这几条
// 译义，空格/数字键照常选一条上屏（见 ProcessSelectionKey 的译文分支）。
void HandleTranslationCommitKey(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id,
                                int forced_index_in_page)
{
    // 拿不到译文时回 NavigationIgnored：这颗键已经被 TSF 吃掉了，必须给一条回复，
    // 而且这条回复既不上屏也不给 wch 补标点。
    Global::MsgTypeToTsf = Global::DataFromServerMsgType::NavigationIgnored;
    const bool japanese = g_inputSession && g_inputSession->current_scheme_type() == SchemeType::JapaneseRomaji;
    if (IsCandidateSubPageActive() || IsUiLessMode() || japanese || !GetConfiguredCandidateTranslationsEnabled() ||
        Global::candidate_ui.items.empty())
    {
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
        return;
    }

    // 和数字/空格选词一样，先等画面追上已发布的那一页，否则取到的是用户没看见的那条译文。
    WaitForCandidateRenderSync(Global::Keycode);
    EnsureCandidatePageReady();

    auto &ui = Global::candidate_ui;
    const int highlighted_position = (std::max)(0, ui.selected_index_in_page);
    const int page_position = forced_index_in_page >= 0 ? forced_index_in_page : highlighted_position;
    const size_t index = static_cast<size_t>(page_position);
    if (index >= ui.page_glosses.size())
    {
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
        return;
    }
    const auto senses = FanyImeIpc::SplitTranslationGloss(wstring_to_string(ui.page_glosses[index]));
    if (senses.empty())
    {
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
        return;
    }

    if (senses.size() == 1)
    {
        Global::MsgTypeToTsf = Global::DataFromServerMsgType::CommitExactText;
        ui.selected_text = string_to_wstring(CandidateTextForOutput(senses.front()));
        // Normal/CommitExactText 的回复由 SendCurrentDataToClient 负责收尾（ClearState）。
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
        return;
    }

    g_translation_saved_items = ui.items;
    g_translation_saved_page_index = ui.page_index;
    g_translation_saved_selected_index = ui.selected_index_in_page;
    g_translation_saved_selection_started = ui.selection_started;
    std::vector<WordItem> translation_items;
    translation_items.reserve(senses.size());
    for (const auto &sense : senses)
    {
        translation_items.emplace_back(std::string{}, sense, 0, CandidateSource::Fallback);
    }
    ui.set_items(std::move(translation_items));
    g_translation_candidates_active = true;
    RefreshCandidatePageUi(true);
    SendCurrentDataToClient(client_id, activation_epoch, request_id);
}

// 真顶字与自动上屏的推送负载："<消费字符数>\t<上屏文本>"。TSF 拿这个数字裁自己的
// 组合缓冲，所以服务端看到的是四码、用户已抢敲第五个字母时，第五个字母不会被旧快照覆盖。
// 消费数就是五笔完整码的字母数（engine/schemes/wubi_scheme.h 的 kMaxCodeLength）。
constexpr std::size_t kWubiCompleteCodeLength = 4;
std::wstring BuildWubiCommitAndContinuePayload(const std::wstring &text)
{
    return std::to_wstring(kWubiCompleteCodeLength) + L"\t" + text;
}

// Bring the candidate page in step with the CompositionRestored frame just sent.
// TSF applies that payload without touching its candidate presenter, so the page
// on screen has to follow here -- and that covers every frame the reply branch
// sends, not just the retraction: the unit Backspace deletion shortens the raw
// and the Ctrl+arrow unit jump moves the caret prefix, and both would otherwise
// leave the pre-key page (and its selection) on screen.
//   - the raw is gone: nothing to show. The selected segments may still be on
//     screen (a segment Backspace emptied the raw but kept the word), where TSF
//     deliberately leaves its presenter alone because ending it would send
//     HideCandidateWnd and reset this very composition; and a Backspace that
//     ended the word must never fall through to a rebuild that would publish the
//     empty-input fallback candidate;
//   - the caret prefix is empty: hide too (R4, the same rule as the ShowCandidate
//     task and the caret-arrow path);
//   - otherwise rebuild from the engine -- the old page holds the pre-frame
//     items -- and re-highlight the restored pick.
void PublishRestoredCompositionCandidates(uint64_t client_id, uint64_t activation_epoch)
{
    // One-shot: consume it even when this outcome hides instead of rebuilding, so
    // a position recorded on a page that no longer exists cannot leak into a
    // later frame.
    const GlobalIme::RestoredSelectionHighlight restored_highlight =
        GlobalIme::composition.take_restored_selection_highlight();
    if (GlobalIme::composition.raw_input_with_cases.empty())
    {
        HideCandidateWindowAndDropItems();
        return;
    }
    if (FanyImeIpc::IsCaretPrefixEmpty(g_inputSession->prefix_end(),
                                       g_inputSession->get_pinyin_sequence_with_cases().size()))
    {
        HideCandidateWindowAndDropItems();
        return;
    }

    PrepareCandidateList(client_id, activation_epoch);
    if (restored_highlight.absolute_index >= 0)
    {
        // A retraction re-highlights the item the user had picked on the rebuilt
        // page: no frequency update ran during the creating word, so a page
        // rebuilt for the same prefix still holds the same items in the same
        // order. A page for another prefix (the suffix was edited between the
        // pick and the retraction) does not, and the recorded position would land
        // on an unrelated candidate -- apply it only after the prefixes match.
        const std::string rebuilt_page_prefix = FanyImeIpc::NormalizeCandidatePagePrefix(
            g_inputSession->get_pinyin_sequence_with_cases(), g_inputSession->prefix_end());
        auto &ui = Global::candidate_ui;
        if (rebuilt_page_prefix == restored_highlight.page_prefix && ui.item_total_count > 0 && ui.page_size > 0)
        {
            const int position = std::min(restored_highlight.absolute_index, ui.item_total_count - 1);
            ui.page_index = position / ui.page_size;
            ui.selected_index_in_page = position % ui.page_size;
            ui.begin_selection();
            RefreshCandidatePageUi(false);
        }
    }
    RequestShowCandidateWindow();
}

// Esc inside the creating-word shape (see HasEscapeCreatingWordShape). TSF is
// holding for a CompositionRestored frame, so it is sent in both outcomes:
//   - keep (ShouldEscapeKeepSelectedWord): drop only the unselected spelling
//     and leave the selected word alone on screen -- the R3 state a segment
//     Backspace already produces, except that the candidate window stays up
//     showing just the word as its preedit. The selection history stays, so a
//     later Backspace can still retract the word;
//   - otherwise reset everything like any other Esc; the empty payload makes
//     TSF cancel its composition.
void HandleCreatingWordEscape(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id,
                              const std::string &input_before_key)
{
    if (FanyImeIpc::ShouldEscapeKeepSelectedWord(GetConfiguredEscapeKeepsSelectedWord(), input_before_key.size(),
                                                 g_r_mode_triggered))
    {
        g_inputSession->set_pinyin_sequence("");
        g_inputSession->set_pinyin_sequence_with_cases("");
        g_inputSession->recompute_candidates();
        UpdateCloudInput("");
        UpdateEnglishInput("");
        UpdateMixedInput({});
        UpdateAiInput("");
        g_dedicated_english_answer_pending = false;
        ClearSpecialModeTriggers();
        auto &composition = GlobalIme::composition;
        composition.raw_input_with_cases.clear();
        composition.segmented_pinyin.clear();
        composition.caret_position = 0;
        composition.restored_selection_highlight = {};
        Global::MsgTypeToTsf = Global::DataFromServerMsgType::CompositionRestored;
        Global::candidate_ui.selected_text =
            BuildCreateWordPipePayload(std::string{}, composition.creating_word.prefix_text) + L"\t0";
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
        if (GetConfiguredCandidateWindowPreeditStyle() == "empty")
        {
            // The window draws no preedit, so an empty page would be a blank card.
            HideCandidateWindowAndDropItems();
            return;
        }
        // Keep the window up with the word alone as its preedit (Weasel style): an
        // empty page, so the stale candidates of the dropped spelling go away.
        Global::CandidateString.clear();
        Global::candidate_ui.set_items({});
        RefreshCandidatePageUi(true);
        return;
    }

    PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
    ClearState();
    Global::MsgTypeToTsf = Global::DataFromServerMsgType::CompositionRestored;
    Global::candidate_ui.selected_text = L"\t\t\t0";
    SendCurrentDataToClient(client_id, activation_epoch, request_id);
}

// Switches g_inputSession to the active language's scheme. The outgoing session
// is parked rather than dropped, so cycling back reuses it instead of building
// a new engine (and reopening msime.db) while the client waits for the reply.
void SwitchSessionForLanguageCycle()
{
    const SchemeType wanted = GetConfiguredActiveInputScheme();
    if (g_inputSession && g_inputSession->current_scheme_type() == wanted)
        return;
    std::shared_ptr<IInputSession> next;
    if (g_language_cycle_parked_session && g_language_cycle_parked_session->current_scheme_type() == wanted)
        next = std::move(g_language_cycle_parked_session);
    else
        next = CreateInputSessionFromConfig();
    g_language_cycle_parked_session = std::move(g_inputSession);
    g_inputSession = std::move(next);
}

void HandleTrilingualCycleKey(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id,
                              bool requester_ime_enabled, bool requester_japanese)
{
    using FanyImeTrilingualInput::Mode;
    // Cycle from the requester's own state. With ime_mode_scope=app the Server's
    // global CN/EN mode can describe another host. When the switch was turned
    // off while this request was queued, the active language is the configured
    // one again and the request is answered as the binary toggle.
    Mode destination =
        FanyImeTrilingualInput::Destination(requester_ime_enabled, requester_japanese,
                                            GetConfiguredTrilingualCycleEnabled(), GetActiveInputMode() == "japanese");

    // Match the existing language toggle: retain an already selected word
    // prefix, then commit the remaining raw spelling without choosing or
    // learning a candidate.
    const std::string raw = g_inputSession ? g_inputSession->get_pinyin_sequence_with_cases() : std::string{};
    const std::wstring commit_text =
        string_to_wstring(GlobalIme::composition.creating_word.prefix_text + (g_r_mode_triggered ? "R" + raw : raw));

    bool native_mode_changed = false;
    if (destination != Mode::English)
    {
        const std::string wanted_mode = destination == Mode::Japanese ? "japanese" : "chinese";
        if (wanted_mode != GetActiveInputMode())
        {
            native_mode_changed = SetActiveInputMode(wanted_mode);
            if (!native_mode_changed)
            {
                // The switch was turned off after Destination() read it. Never
                // claim a language the Server is not in.
                destination = GetActiveInputMode() == "japanese" ? Mode::Japanese : Mode::Chinese;
            }
        }
    }
    SetEnglishInputMode(false);
    ClearState();
    SwitchSessionForLanguageCycle();

    const bool next_ime_enabled = destination != Mode::English;
    g_authoritative_cn_mode = next_ime_enabled ? 1 : 0;
    const int remembered = RecallClientStatusSnapshot(client_id);
    const int packed_state = ((remembered >= 0 ? remembered : 0) & ~0x4) | (g_authoritative_cn_mode << 2);
    RememberClientStatusSnapshot(client_id, packed_state);
    PublishStatusSnapshotValue(packed_state);
    PostMessage(::global_hwnd_ftb, UPDATE_FTB_INPUT_MODE, GetActiveInputMode() == "japanese" ? 1 : 0, 0);
    // The originating client takes its native-language state from the main
    // reply, after committing. Other hosts still receive the global language.
    if (native_mode_changed)
        BroadcastConfiguredInputModeState(client_id);

    // The exact commit and destination share one correlated reply. TSF applies
    // them in one edit session before it drains the next key, so a fast Shift then
    // letter cannot be classified using the old language/compartment.
    Global::MsgTypeToTsf = Global::DataFromServerMsgType::TrilingualCycle;
    Global::candidate_ui.selected_text = FanyImeTrilingualInput::BuildPayload(destination, commit_text);
    SendCurrentDataToClient(client_id, activation_epoch, request_id);
}

// The toolbar has no requester state bits, so the clicked icon says whether the
// IME was open and the native language is the Server's active one, which is what
// that icon showed. TSF still owns OPENCLOSE: English and back go through the
// ordinary worker switches, Chinese <-> Japanese is a Server-side change only.
void HandleToolbarLanguageToggle(bool from_ime_enabled)
{
    using FanyImeTrilingualInput::Mode;
    const bool japanese = GetActiveInputMode() == "japanese";
    const Mode destination = FanyImeTrilingualInput::Destination(from_ime_enabled, japanese,
                                                                 GetConfiguredTrilingualCycleEnabled(), japanese);
    if (destination == Mode::English)
    {
        SendToTsfWorkerThreadViaNamedpipe(Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToEn, L"");
        return;
    }

    const std::string wanted_mode = destination == Mode::Japanese ? "japanese" : "chinese";
    if (wanted_mode != GetActiveInputMode() && SetActiveInputMode(wanted_mode))
    {
        SetEnglishInputMode(false);
        PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
        ClearState();
        SwitchSessionForLanguageCycle();
        PostMessage(::global_hwnd_ftb, UPDATE_FTB_INPUT_MODE, wanted_mode == "japanese" ? 1 : 0, 0);
        // Every client, the focused one included: no key reply carries this change.
        BroadcastConfiguredInputModeState();
    }
    if (!from_ime_enabled)
        SendToTsfWorkerThreadViaNamedpipe(Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToCn, L"");
}

/**
 * @brief
 *
 * 调频、造词也都在这里处理。
 *
 */
void HandleImeKey(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id)
{
    const ScopedServerKeyLatency latency{client_id, activation_epoch, request_id};
    /* 先清理一下状态 */
    Global::MsgTypeToTsf = Global::DataFromServerMsgType::Normal;
    ScopedKeyStage read_packet{client_id, activation_epoch, request_id, L"handle-read-packet"};
    ::ReadDataFromNamedPipe(0b000111);
    read_packet.Stop();

    // TSF classifies VK_NUMPAD0..9 as candidate digit keys. Keep the IPC
    // contract symmetric before any selection/composition predicates run.
    Global::Keycode = FanyImeIpc::NormalizeNumpadDigitKey(Global::Keycode);
    g_key_temporary_japanese_policy = ClientNegotiatedTemporaryJapanese(client_id)
                                          ? FanyNamedPipe::TemporaryJapanesePolicy()
                                          : FanyImeTemporaryJapanese::Policy{0};

    const bool sensitive_field = (Global::ModifiersDown & FanyImePipeFlags::SensitiveField) != 0;
    Global::ModifiersDown &= ~FanyImePipeFlags::SensitiveField;
    NoteAiContextKeySource(client_id, activation_epoch, sensitive_field);
    // 横排候选的左右键：TSF 只在这项生效、且协商过 CompositionRestore 时打这个标记，并且一定会读这次的回复。
    // 是否生效以标记为准而不是再查配置：配置刚改、TSF 还没收到新状态时，两边仍按同一个约定走。
    const bool horizontal_arrow_request = (Global::ModifiersDown & FanyImePipeFlags::HorizontalArrowSelection) != 0;
    Global::ModifiersDown &= ~FanyImePipeFlags::HorizontalArrowSelection;
    // 同理：直接辅助码下进入末尾辅助码的引导键和删掉它的退格，见 g_key_direct_end_trigger_request。
    g_key_direct_end_trigger_request = (Global::ModifiersDown & FanyImePipeFlags::DirectHelpcodeEndTriggerRewrite) != 0;
    Global::ModifiersDown &= ~FanyImePipeFlags::DirectHelpcodeEndTriggerRewrite;

    // MainPipeClientThread only enqueues this from a negotiated client with a known action.
    if (namedpipeData.event_type == FanyImePipeEventType::InputStateAction)
    {
        SetEnglishInputMode(!g_english_input_mode);
        ClearState();
        return;
    }

    // Only the explicit request bit cycles: a plain VK_SHIFT is still the binary
    // toggle, which the client also uses when it decides not to cycle.
    const UINT cycle_request = Global::ModifiersDown & FanyImeTrilingualInput::CycleRequestMask;
    Global::ModifiersDown &= ~FanyImeTrilingualInput::CycleRequestMask;
    if (FanyImeTrilingualInput::IsCycleRequest(Global::Keycode, Global::ModifiersDown | cycle_request) &&
        ClientNegotiatedTrilingualCycle(client_id))
    {
        HandleTrilingualCycleKey(client_id, activation_epoch, request_id,
                                 FanyImeTrilingualInput::CycleRequestImeEnabled(cycle_request),
                                 FanyImeTrilingualInput::CycleRequestJapanese(cycle_request));
        return;
    }

    if (FanyImeProtocol::IsCharacterSetShortcut(Global::Keycode, Global::ModifiersDown))
    {
        if (g_authoritative_cn_mode != 0 && GetConfiguredCharacterSetShortcutEnabled())
        {
            const std::string previous = GetConfiguredCharacterSet();
            const std::string next = previous == "traditional" ? "simplified" : "traditional";
            // The packet's point[] is this key's badge anchor, not the
            // candidate anchor: read it locally and leave Global::Point alone.
            // Legacy clients leave the struct-default point there instead.
            const bool changed = SetConfiguredCharacterSet(next) && GetConfiguredCharacterSet() != previous;
            if (changed && GetConfiguredCharacterSet() == "traditional")
                ChineseConverter::PreloadInBackground();
            if (changed && ClientNegotiatedCaretStateIndicator(client_id))
            {
                PostCaretStateBadge(FanyImeUi::SingleStateBadge(FanyImeUi::CaretStateKind::CharacterSet,
                                                                GetConfiguredCharacterSet() == "traditional"),
                                    namedpipeData.point[0], namedpipeData.point[1]);
            }
            if (changed)
                RespellCandidatesForCharacterSetChange();
        }
        return;
    }

    if (FanyImeIpc::IsEnglishModeToggleKey(Global::Keycode, Global::ModifiersDown) &&
        !ClientNegotiatedInputStateShortcuts(client_id))
    {
        SetEnglishInputMode(!g_english_input_mode);
        ClearState();
        return;
    }

    if (FanyImeIpc::IsTranslationCommitKey(Global::Keycode, Global::ModifiersDown))
    {
        HandleTranslationCommitKey(client_id, activation_epoch, request_id, -1);
        return;
    }
    // Ctrl+数字：上屏指定序号候选的译文（Ctrl+Enter 上屏当前高亮那条），两条 chord 落到同一个
    // 处理 branch。译文总开关（general.candidate_translations）关掉时 TSF 直接不吃它们，这里仍进
    // 同一个 branch 回 NavigationIgnored——不要改成先判总开关再放行，那样会落到下面的普通选词分支，
    // Ctrl+1 直接上屏第一个候选。
    if (FanyImeIpc::IsTranslationDigitKey(Global::Keycode, Global::ModifiersDown))
    {
        const uint32_t digit_position = FanyImeIpc::TranslationDigitPosition(Global::Keycode, Global::ModifiersDown);
        HandleTranslationCommitKey(client_id, activation_epoch, request_id, static_cast<int>(digit_position) - 1);
        return;
    }
    // 横排候选的左右键在这里决定是移高亮还是移光标（规则见 ShouldArrowMoveCandidateSelection）。要在下面 R 模式
    // 把光标换回引擎坐标之前判断：此刻光标和 raw_input_with_cases 都还是含 R 前缀的显示坐标，串尾的含义一致。
    const bool horizontal_arrow_key =
        horizontal_arrow_request && (Global::Keycode == VK_LEFT || Global::Keycode == VK_RIGHT) &&
        (Global::ModifiersDown & 0b00000111u) == 0 && !IsUiLessMode() && ClientNegotiatedCompositionRestore(client_id);
    bool arrow_moves_selection = false;
    if (horizontal_arrow_key)
    {
        const auto &composition = GlobalIme::composition;
        const auto &ui = Global::candidate_ui;
        arrow_moves_selection = FanyImeIpc::ShouldArrowMoveCandidateSelection(
            Global::Keycode, composition.caret_position >= composition.raw_input_with_cases.size(),
            ui.current_page_start() + ui.selected_index_in_page, ui.item_total_count);
    }

    // 译文页、日期页只认选词和翻页/移动高亮。其它任何键都先把候选框换回原来那一屏，然后照常处理，
    // 所以退格、字母、回车、标点在子页上的表现和没打开过它时完全一致。
    if (IsCandidateSubPageActive())
    {
        // Shift 放行是给 Shift+Tab 上一页留的；Ctrl/Alt 组合一律退出。
        const bool stays_on_sub_page =
            (Global::ModifiersDown & 0b00000110u) == 0 &&
            (IsSelectionKey(Global::Keycode) || IsCandidateNavigationKey(Global::Keycode) || arrow_moves_selection);
        if (!stays_on_sub_page)
        {
            ExitCandidateSubPage();
        }
    }

    if (g_r_mode_triggered && !GlobalIme::composition.raw_input_with_cases.empty() &&
        GlobalIme::composition.raw_input_with_cases.front() == 'R' && GlobalIme::composition.caret_position > 0)
    {
        // The published preedit has one extra display-only prefix. Normalize
        // the caret before every R-mode key, including paging and selection.
        --GlobalIme::composition.caret_position;
    }

    const std::string input_before_key =
        g_inputSession ? g_inputSession->get_pinyin_sequence_with_cases() : std::string{};
    // 顶字要的是「插入之前」的原始串长度与光标位置：ApplyCompositionEditKey 会把第五个字母插进
    // 本地 raw 并把光标推到 5，之后再问就分不清「用户又敲了一个字母」和「本来就停在别处」。引擎
    // 随后会把 raw 裁回四码，这个快照是唯一能区分两者的地方（raw_length_before_key == 4 且光标
    // 在末尾 = 用户正在往后打，不是回来改码）。
    const std::size_t raw_length_before_key = input_before_key.size();
    const std::size_t caret_before_key = GlobalIme::composition.caret_position;
    const bool shift_only = (Global::ModifiersDown & 0b00000111u) == 0b00000001u;
    const std::optional<SchemeType> scheme =
        g_inputSession ? std::optional<SchemeType>(g_inputSession->current_scheme_type()) : std::nullopt;
    const bool chinese_scheme = scheme == SchemeType::Quanpin || scheme == SchemeType::Shuangpin;
    const bool shuangpin_scheme = scheme == SchemeType::Shuangpin;
    // T 模式对五笔同样开放，但五笔不能并进 chinese_scheme：那个变量还给 K/U/E/M/S/J/Y/R 触发与
    // ShouldLearnEnteredEnglishWord 用，并进去会让五笔连带打开其它本地模式（R7）。
    const bool date_time_scheme = chinese_scheme || scheme == SchemeType::Wubi;
    const bool pinyin_commit =
        FanyImeIpc::IsPinyinCommitKey(Global::Keycode, Global::ModifiersDown) && !input_before_key.empty();
    const bool convert_shuangpin = pinyin_commit && g_inputSession->current_scheme_type() == SchemeType::Shuangpin &&
                                   !g_english_input_mode && !g_r_mode_triggered &&
                                   !IsSpecialModeCompositionActive(input_before_key);
    if (Global::Keycode == VK_RETURN && !input_before_key.empty() && !convert_shuangpin &&
        GetConfiguredEnterLearnsEnglishWord())
    {
        std::string english_word;
        // V 模式没有触发标记，按输入串认：回车上屏的 V123 不是要学的英文词。
        const bool shift_letter_special_mode =
            IsShiftLetterSpecialModeTriggered() || IsVModeCompositionActive(input_before_key);
        if (FanyImeIpc::ShouldLearnEnteredEnglishWord(g_english_input_mode, shift_letter_special_mode, chinese_scheme,
                                                      g_inputSession->is_all_complete_pure_pinyin()))
            english_word = g_r_mode_triggered ? "R" + input_before_key : input_before_key;
        EnqueueLearnEnteredEnglishWordTask(english_word);
    }
    if (pinyin_commit)
    {
        const std::string raw = g_r_mode_triggered ? "R" + input_before_key : input_before_key;
        const std::string text = FanyImeIpc::EnteredPinyinText(
            raw, convert_shuangpin ? g_inputSession->get_quanpin() : std::string{}, convert_shuangpin);
        Global::candidate_ui.selected_text = string_to_wstring(GlobalIme::composition.creating_word.prefix_text + text);
        Global::MsgTypeToTsf = Global::DataFromServerMsgType::CommitExactText;
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
        return;
    }
    if (chinese_scheme && !g_english_input_mode && GetConfiguredQuickPhraseEnabled() && input_before_key.empty() &&
        Global::Keycode == 'K' && Global::Wch == L'K' && shift_only)
        g_quick_phrase_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredRadicalModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'U' && Global::Wch == L'U' && shift_only)
        g_radical_mode_triggered = true;
    if (date_time_scheme && !g_english_input_mode && GetConfiguredDateTimeModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'T' && Global::Wch == L'T' && shift_only)
        g_date_time_mode_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredEmojiModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'E' && Global::Wch == L'E' && shift_only)
        g_emoji_mode_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredKaomojiModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'M' && Global::Wch == L'M' && shift_only)
        g_kaomoji_mode_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredSymbolModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'S' && Global::Wch == L'S' && shift_only)
        g_symbol_mode_triggered = true;
    // 超级简拼只留给双拼：全拼直接打声母就是简拼，Shift+J 在全拼下按普通大写 J 处理。
    if (shuangpin_scheme && !g_english_input_mode && GetConfiguredJianpinModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'J' && Global::Wch == L'J' && shift_only)
        g_jianpin_mode_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredYModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'Y' && Global::Wch == L'Y' && shift_only)
        g_y_mode_triggered = true;
    const bool r_mode_trigger_key = chinese_scheme && !g_english_input_mode && GetConfiguredRModeEnabled() &&
                                    input_before_key.empty() && Global::Keycode == 'R' && Global::Wch == L'R' &&
                                    shift_only;
    if (r_mode_trigger_key)
    {
        g_r_mode_original_session = g_inputSession;
        g_inputSession = CreateTemporaryJapaneseInputSession();
        g_r_mode_triggered = true;
    }

    if (FanyImeIpc::HasEscapeCreatingWordShape(Global::Keycode, GlobalIme::composition.creating_word.active,
                                               IsUiLessMode(), ClientNegotiatedCompositionRestore(client_id)))
    {
        HandleCreatingWordEscape(client_id, activation_epoch, request_id, input_before_key);
        return;
    }

    if (FanyImeIpc::IsBackendIndependentCompositionResetKey(Global::Keycode))
    {
        // TSF completes/cancels the composition locally. Keep every backend in
        // lockstep, invalidate async candidates, and do not manufacture a
        // reply for this locally consumed key.
        PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
        ClearState();
        return;
    }

    const bool is_paging_key = IsPagingKey(Global::Keycode);
    const bool is_manual_pinyin_separator = IsManualPinyinSeparatorKey(Global::Keycode, Global::Wch);
    const bool is_microsoft_shuangpin_ing_key =
        IsMicrosoftShuangpinIngKey(Global::Keycode, Global::Wch, input_before_key);
    // 句中辅助码与末尾辅助码（间接）的引导键在这之后的判断里待遇相同：只进输入串。
    const bool is_mid_sentence_helpcode_marker =
        IsMidSentenceHelpcodeMarkerKey(Global::Keycode, Global::Wch, input_before_key) ||
        IsEndHelpcodeMarkerKey(Global::Keycode, Global::Wch, input_before_key);
    const bool is_direct_helpcode_slash = IsDirectHelpcodeSlashKey(Global::Keycode, Global::Wch, input_before_key);
    // 直接辅助码下用引导键进入末尾辅助码：改写得了就当编辑键改写输入串；改写不了时照旧当标点，TSF 等的回帧
    // 就是标点那一帧（见 g_key_direct_end_trigger_request）。
    const bool is_direct_helpcode_end_trigger =
        ClientNegotiatedCompositionRestore(client_id) &&
        DirectHelpcodeEndTriggerPrefix(Global::Keycode, Global::Wch, input_before_key).has_value();
    const bool is_date_time_input_key = IsDateTimeInputKey(Global::Keycode, Global::Wch, input_before_key);
    // V 模式的数字和 . + - * / ( )：只进输入串，不选词、不翻页、不当标点。
    const bool is_v_mode_input_key = IsVModeInputKey(Global::Wch, input_before_key);
    // V 模式里数字键用来输入，Shift+数字选词。Shift+1 出的是 !，不排除就会被当成「上屏高亮候选
    // + 标点」；Shift+8/9/0 出 * ( ) 是编码键，上一行已经收下。
    const bool is_v_mode_shift_digit_selection = !is_v_mode_input_key && shift_only && Global::Keycode >= '1' &&
                                                 Global::Keycode <= '9' && IsVModeCompositionActive(input_before_key);
    // 日语模式下 '-' 是长音符输入键，既不翻页也不做词转字。
    const bool is_japanese_long_vowel = IsJapaneseLongVowelKey(Global::Keycode, Global::Wch);
    const int word_character_direction = FanyImeIpc::WordToCharacterDirection(
        Global::Keycode, Global::Wch, Global::ModifiersDown,
        GetConfiguredWordToCharacterEnabled() && !is_v_mode_input_key,
        GetConfiguredWordToCharacterKeys() == "minus_equal", is_japanese_long_vowel);
    const bool is_commit_with_highlighted_candidate_punctuation =
        word_character_direction != 0 ||
        (!is_manual_pinyin_separator && !is_microsoft_shuangpin_ing_key && !is_mid_sentence_helpcode_marker &&
         !is_direct_helpcode_slash && !is_direct_helpcode_end_trigger && !is_date_time_input_key &&
         !is_v_mode_input_key && !is_v_mode_shift_digit_selection &&
         IsCommitWithHighlightedCandidatePunctuationInCandidateMode(Global::Keycode, Global::Wch));
    const bool is_selection_key = IsSelectionKey(Global::Keycode);
    // 移高亮的左右键不碰组合，交给下面的候选导航分支。
    const bool is_composition_edit_key =
        ((Global::Keycode == VK_LEFT || Global::Keycode == VK_RIGHT) && !arrow_moves_selection) ||
        Global::Keycode == VK_BACK || Global::Keycode == VK_DELETE ||
        (Global::Keycode >= 'A' && Global::Keycode <= 'Z') || is_manual_pinyin_separator ||
        is_microsoft_shuangpin_ing_key || is_mid_sentence_helpcode_marker || is_direct_helpcode_slash ||
        is_direct_helpcode_end_trigger || is_japanese_long_vowel || is_date_time_input_key || is_v_mode_input_key;
    const bool should_forward_key_to_session = !is_commit_with_highlighted_candidate_punctuation && !is_selection_key &&
                                               !is_paging_key && !is_composition_edit_key;

    // Punctuation needs a synchronous highlighted-candidate response on the TSF pipe.
    // Reply before cloud-query and candidate recomputation work so the TSF-side
    // timeout sentinel keeps its original meaning instead of masking latency here.
    if (is_commit_with_highlighted_candidate_punctuation)
    {
        Global::MsgTypeToTsf = Global::DataFromServerMsgType::Normal;
        const bool has_active_composition = g_inputSession != nullptr && !g_inputSession->get_pinyin_sequence().empty();
        if (has_active_composition)
        {
            ScopedKeyStage ensure_punct{client_id, activation_epoch, request_id, L"handle-candidates"};
            EnsureCandidatePageReady();
            ensure_punct.Stop();
            auto &ui = Global::candidate_ui;
            ui.selected_text = FanyImeIpc::HighlightedCandidateText(ui.page_words, ui.selected_index_in_page);

            WordItem highlighted_item;
            const bool highlighted_resolved = ResolveCandidateItem(ui.selected_index_in_page + 1, highlighted_item);
            // 首词命中率：标点收尾带上屏候选时也是一次中文候选提交（用户决策 1：只有
            // 「纯标点收尾不带候选」才排除）。真正上屏的那条候选要么是高亮项，要么（高亮
            // 停在「📅日期」入口上时）是本页第一条；is_first 按实际提交的那条算。
            // 日语候选与 ProcessSelectionKey 的 japanese_candidate 门同款排除：日语 UI
            // 模式与 R 模式临时日语解析出的 WordItem scheme 都是 JapaneseRomaji，
            // 标点收尾把它们带出来的路径不经过 ProcessSelectionKey，必须在这里单独
            // 挡住（用户决策 3）。
            int committed_index_in_page = -2;
            WordItem committed_item;
            if (highlighted_resolved && metasequoia::local_modes::is_date_time_menu_item(highlighted_item))
            {
                // 「📅日期」入口不是要上屏的字：高亮停在它上面时，标点带出本页首个候选。
                ui.selected_text = FanyImeIpc::HighlightedCandidateText(ui.page_words, 0);
                // 计数门要看真正上屏的那条（本页第一条），不是高亮的日期入口（DateTime 会被排除）。
                if (ResolveCandidateItem(1, committed_item))
                {
                    committed_index_in_page = 0;
                }
            }
            else if (highlighted_resolved)
            {
                committed_index_in_page = ui.selected_index_in_page;
                committed_item = highlighted_item;
                if (word_character_direction != 0)
                {
                    const auto edge = word_character_direction < 0 ? FanyImeIpc::HanCharacterEdge::First
                                                                   : FanyImeIpc::HanCharacterEdge::Last;
                    const auto character = FanyImeIpc::ExtractHanCharacter(CandidateOutputText(highlighted_item), edge);
                    if (character)
                    {
                        Global::MsgTypeToTsf = Global::DataFromServerMsgType::CommitExactText;
                        ui.selected_text = string_to_wstring(*character);
                    }
                }
            }
            if (committed_index_in_page >= 0 && !g_english_input_mode &&
                committed_item.scheme != SchemeType::JapaneseRomaji &&
                MsimeStats::CountsTowardFirstWordSelection(committed_item))
            {
                // 造词途中按标点收尾时，前面各段也要都是首位才算命中（同 ProcessSelectionKey）。
                const auto &creating_word = GlobalIme::composition.creating_word;
                const bool prior_segments_first = !creating_word.active || creating_word.all_segments_first;
                MsimeStats::RecordCandidateSelection(prior_segments_first && ui.page_index == 0 &&
                                                     committed_index_in_page == 0);
            }
            SendCurrentDataToClient(client_id, activation_epoch, request_id);
        }
        else
        {
            ClearState();
        }
        return;
    }

    /* 先处理一下通用的按键，包括所有可能的按键，如普通的拼音字符按键、空格、Tab
     * 等等，然后再在下面处理其中的特殊的按键 */
    bool composition_restored = false;
    // The client arms its Backspace reply hold from its own creating-word mirror,
    // which mirrors the state before this key. Capture that shape here so the
    // reply below can still answer a Backspace that ends the word (the post-key
    // shape is gone by then, and the hold would otherwise burn its full timeout).
    bool retreat_backspace_shape_before_key = false;
    // 前缀重算让所有编辑键（含字母/Delete）都需要协商结果；段操作（Ctrl+Backspace /
    // Ctrl+方向）的键位与修饰键条件仍由各自的 chord 判定把守，这里放宽键位限制不影
    // 响它们。
    const bool client_supports_restore = is_composition_edit_key && ClientNegotiatedCompositionRestore(client_id);
    const bool r_mode_prefix_backspace = g_r_mode_triggered && Global::Keycode == VK_BACK && input_before_key.empty();
    if (r_mode_prefix_backspace)
    {
        ClearState();
    }
    else if (is_composition_edit_key && !r_mode_trigger_key)
    {
        retreat_backspace_shape_before_key =
            Global::Keycode == VK_BACK &&
            FanyImeIpc::HasRetreatBackspaceShape(GlobalIme::composition.creating_word.active, IsUiLessMode(),
                                                 client_supports_restore);
        ScopedKeyStage engine_edit{client_id, activation_epoch, request_id, L"handle-engine"};
        ApplyCompositionEditKey(Global::Keycode, Global::Wch, Global::ModifiersDown, client_supports_restore,
                                composition_restored);
        // 横排候选左右键移的是光标：TSF 不再自己移，等 CompositionRestored 里的光标（原地不动也要回这一帧）。
        if (horizontal_arrow_key)
        {
            composition_restored = true;
        }
    }
    else if (should_forward_key_to_session)
    {
        ScopedKeyStage engine_forward{client_id, activation_epoch, request_id, L"handle-engine"};
        g_inputSession->handle_key(Global::Keycode, Global::ModifiersDown, Global::Wch);
    }
    GlobalIme::composition.segmented_pinyin = g_inputSession->get_pinyin_segmentation_with_cases();
    GlobalIme::composition.raw_input_with_cases = g_inputSession->get_pinyin_sequence_with_cases();
    if (g_english_input_mode)
    {
        // English candidates are queried by the raw spelling. Do not expose
        // the Chinese pinyin session's syllable boundaries in the preedit.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (g_inputSession->get_pinyin_sequence_with_cases().empty() && !g_r_mode_triggered)
    {
        ClearSpecialModeTriggers();
    }
    if (!g_english_input_mode && g_r_mode_triggered)
    {
        // R is a visible mode prefix but is not part of the romaji sent to the
        // temporary Japanese engine. Keep both TSF and candidate-window preedit
        // aligned, including their caret coordinates.
        GlobalIme::composition.segmented_pinyin.insert(0, 1, 'R');
        GlobalIme::composition.raw_input_with_cases.insert(0, 1, 'R');
        ++GlobalIme::composition.caret_position;
    }
    if (!g_english_input_mode && IsRadicalCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed U+letters sequence.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsDateTimeCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsQuickPhraseCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed K-prefixed code.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsEmojiCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed E-prefixed code.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsKaomojiCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed M-prefixed code.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsSymbolCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed S-prefixed code.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsJianpinCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed J-prefixed code.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsYModeCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed Y-prefixed English.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsVModeCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed V-prefixed number or expression.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    SyncShuangpinPreeditForms();

    // 五笔四码唯一自动上屏：敲满四码且码表只给一个候选时，直接走与空格完全相同的提交路径，
    // 用户不必再按一次空格。判定只发生在字母键插入之后（上面的 ApplyCompositionEditKey）：
    // 退格、方向键、composition_restored 等路径都不会到这里，所以「打满第四键就上屏」只有
    // 这一个入口。开关默认开；关掉后组合停在四码上等用户选。两个开关作用在不同的按键上：
    // 本分支只看第四键（落键前 raw 是 3 码），下面的顶字分支只看第五键，第四键上不会互相影响。
    const bool letter_key = Global::Keycode >= 'A' && Global::Keycode <= 'Z';
    if (!g_english_input_mode && letter_key &&
        FanyImeIpc::ShouldAutoCommitCompleteWubiCode(GetConfiguredWubiFourCodeAutoCommit(),
                                                     g_inputSession->wubi_unique_four_code(),
                                                     GlobalIme::composition.creating_word.active))
    {
        // 候选页是异步发布的：此刻 ui.items / ui.page_words 可能还停在第 3 码那一拍，而提交
        // 路径读的正是这两份数据。先按当前组合同步重建一次，否则会把上一拍的候选上屏。
        // forced_index_in_page = 0 让结算不进入渲染等待（与鼠标点击同类），自动上屏的语义
        // 是「这个码只有一个候选」，必须显式取 0 而不是跟随页内选择。
        ScopedKeyStage prepare_auto_commit{client_id, activation_epoch, request_id, L"handle-candidates"};
        PrepareCandidateList(client_id, activation_epoch);
        prepare_auto_commit.Stop();
        Global::candidate_ui.select_first_on_page();
        ScopedKeyStage select_auto_commit{client_id, activation_epoch, request_id, L"handle-selection"};
        ProcessSelectionKey(VK_SPACE, client_id, activation_epoch, /*forced_index_in_page=*/0,
                            /*count_first_word=*/false);
        select_auto_commit.Stop();
        ScopedKeyStage push_auto_commit{client_id, activation_epoch, request_id, L"handle-worker-push"};
        if (Global::MsgTypeToTsf == Global::DataFromServerMsgType::Normal)
        {
            // 真上屏只能靠 worker 管道推送：字母键在默认 raw 预编辑样式下不读请求-回复管道，
            // 回一帧 Normal 既不会上屏，还会被 TSF 当成「不属于本次请求」的帧缓存起来，
            // 而本函数返回前 Server 已经清掉组合，两边就此分叉。推送携带消费的 4 个字符，
            // TSF 裁自己的缓冲；快打时用户已多敲的字母因此不会被旧快照覆盖。
            if (SendToTsfWorkerThreadClientViaNamedpipe(
                    client_id, activation_epoch,
                    Global::DataFromServerMsgTypeToTsfWorkerThread::CommitCandidateAndContinue,
                    BuildWubiCommitAndContinuePayload(Global::candidate_ui.selected_text)))
            {
                ClearState();
                // 推送同样会引来 HideCandidateWnd；用户若已抢敲下一个字母，那时服务端组合
                // 就是这个字母，不能被这次 Hide 清掉。
                NoteTopCommitPushed(client_id, activation_epoch);
            }
        }
        push_auto_commit.Stop();
        // UILess 与 pinyin 预编辑样式会为字母键等一帧回复（上限 50ms）：给它们一帧免得空等；
        // raw 样式不读回复，塞一帧反而变成死帧。
        if (IsUiLessMode() || GlobalSettings::getTsfPreeditStyle() == GlobalSettings::TsfPreeditStyle::Pinyin)
        {
            SendCurrentDataToClient(client_id, activation_epoch, request_id);
        }
        return;
    }

    // 真顶字：完整四码（不论是否唯一）之后再敲一个字母时，先上屏该码的首选候选，再把这个字母
    // 留作下一次组合的开头——用户已经在打下一个字，字母绝不能丢。这条路径有自己的开关，
    // 与上面的自动上屏开关互相独立。开关关闭只在混输开启时生效：字母并入混输串，两端一致。
    // 混输关闭时引擎会把码裁回四位，Server 丢掉这个字母而 raw 预编辑样式的 TSF 已经显示了它，
    // 两边就此分叉，所以这时仍走顶字。判定复用同一份引擎事实，
    // 但不要求唯一；上屏取候选 0（首选），不进入 30ms 渲染等待。
    if (!g_english_input_mode && letter_key && raw_length_before_key == kWubiCompleteCodeLength &&
        caret_before_key == raw_length_before_key &&
        FanyImeIpc::ShouldCommitCompleteWubiCodeOnNextKey(
            GetConfiguredWubiFifthCodeTopCommit(), GetConfiguredWubiMixedPinyin(),
            g_inputSession->wubi_four_code_is_complete(), /*key_is_letter=*/true, /*caret_at_end=*/true,
            GlobalIme::composition.creating_word.active))
    {
        ScopedKeyStage prepare_top_commit{client_id, activation_epoch, request_id, L"handle-candidates"};
        PrepareCandidateList(client_id, activation_epoch);
        prepare_top_commit.Stop();
        Global::candidate_ui.select_first_on_page();
        ScopedKeyStage select_top_commit{client_id, activation_epoch, request_id, L"handle-selection"};
        ProcessSelectionKey(VK_SPACE, client_id, activation_epoch, /*forced_index_in_page=*/0);
        select_top_commit.Stop();
        const std::wstring committed_text = Global::candidate_ui.selected_text;

        // 用刚敲下的这个字母重建服务端组合。ProcessSelectionKey 已经把引擎与组合清空，这里
        // 把字母写回去；引擎此刻的 raw 仍是被裁回的四码，所以必须显式设置而不是继续追加。
        // 大小写照 ApplyCompositionEditKey 的同一套规则取，保持 preedit 与用户敲键一致。
        const std::string next_raw(1, CompositionLetter(Global::Keycode, Global::Wch));
        GlobalIme::composition.clear_creating_word();
        GlobalIme::composition.selection_history.clear();
        g_inputSession->set_pinyin_sequence(next_raw);
        g_inputSession->set_pinyin_sequence_with_cases(next_raw);
        ScopedKeyStage recompute_top_commit{client_id, activation_epoch, request_id, L"handle-engine"};
        g_inputSession->recompute_candidates();
        recompute_top_commit.Stop();
        GlobalIme::composition.raw_input_with_cases = g_inputSession->get_pinyin_sequence_with_cases();
        GlobalIme::composition.segmented_pinyin = g_inputSession->get_pinyin_segmentation_with_cases();
        SyncShuangpinPreeditForms();
        GlobalIme::composition.caret_position = GlobalIme::composition.raw_input_with_cases.size();
        ScopedKeyStage prepare_rebuild_top_commit{client_id, activation_epoch, request_id, L"handle-candidates"};
        PrepareCandidateList(client_id, activation_epoch);
        prepare_rebuild_top_commit.Stop();
        // 组合被提交时 TSF 会送 HideCandidateWnd 把候选窗藏起来；顶字重建的新组合必须
        // 显式把窗口再请出来，否则后续整词的候选（xyyf 的统计）用户永远看不到。
        RequestShowCandidateWindow();

        // 推送与用户下一个按键是两条独立路径：TSF 裁的是它自己那一刻的缓冲，所以「服务端说
        // 消费 4 个、TSF 手里已经有 5 个」时，第 5 个自然留下来继续组词。服务端的组合也正好
        // 是同一批多出来的字母，两边都从同一条按键流派生，不会错位。不要 ClearState：重建的
        // 组合正是下一次按键要用的状态。
        ScopedKeyStage push_top_commit{client_id, activation_epoch, request_id, L"handle-worker-push"};
        if (Global::MsgTypeToTsf == Global::DataFromServerMsgType::Normal)
        {
            // 推送会让 DLL 结束旧组合，TSF 随之发来 HideCandidateWnd；标记本客户端的余码
            // 组合仍然存活，HideCandidate 处理器据此跳过 ClearState。推送失败就不会有这次 Hide，
            // 也就不记账。
            if (SendToTsfWorkerThreadClientViaNamedpipe(
                    client_id, activation_epoch,
                    Global::DataFromServerMsgTypeToTsfWorkerThread::CommitCandidateAndContinue,
                    BuildWubiCommitAndContinuePayload(committed_text)))
            {
                NoteTopCommitPushed(client_id, activation_epoch);
            }
        }
        push_top_commit.Stop();
        // UILess 与 pinyin 预编辑样式会为字母键等一帧回复（上限 50ms）。这里绝不能回 Normal
        // （SendCurrentDataToClient 会 ClearState，把刚重建的组合再清掉），只能回渲染帧。
        if (IsUiLessMode())
        {
            SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
        }
        else if (GlobalSettings::getTsfPreeditStyle() == GlobalSettings::TsfPreeditStyle::Pinyin)
        {
            Global::MsgTypeToTsf = Global::DataFromServerMsgType::Preedit;
            Global::candidate_ui.selected_text = GetTsfPreedit();
            SendCurrentDataToClient(client_id, activation_epoch, request_id);
        }
        return;
    }

    //
    // 先判断要不要触发云联想
    // 判断依据：
    //  - 拼音序列长度是偶数
    //  - 最后一个字符不是大写字母
    //
    // Paging / selection must not bump async generations or re-apply cached
    // cloud/AI results (that previously reset page_index and re-cached duplicates).
    const bool suppress_async_lookup = is_paging_key || is_selection_key;

    const auto cloud_query_state = g_inputSession->get_cloud_query_state();
    if (!g_english_input_mode && !suppress_async_lookup &&
        !IsSpecialModeCompositionActive(g_inputSession->get_pinyin_sequence_with_cases()) &&
        cloud_query_state.should_query)
    {
        ScopedKeyStage cloud_query{client_id, activation_epoch, request_id, L"handle-async-query"};
        UpdateCloudInput(cloud_query_state.query_text, client_id, activation_epoch);
    }

    const bool ai_eligible = !g_english_input_mode &&
                             !IsSpecialModeCompositionActive(g_inputSession->get_pinyin_sequence_with_cases()) &&
                             (g_inputSession->current_scheme_type() == SchemeType::Quanpin ||
                              g_inputSession->current_scheme_type() == SchemeType::Shuangpin) &&
                             g_inputSession->is_all_complete_pure_pinyin() && !g_inputSession->has_active_helpcode() &&
                             !GlobalIme::composition.creating_word.active;
    if (!suppress_async_lookup)
    {
        ScopedKeyStage ai_query{client_id, activation_epoch, request_id, L"handle-async-query"};
        UpdateAiInput(ai_eligible ? g_inputSession->get_pinyin_segmentation() : std::string{}, client_id,
                      activation_epoch);
    }

    // The shape the key leaves behind: a Backspace that keeps the creating word
    // alive still owes the client's hold a frame even when nothing was restored
    // (a plain deletion behind the edit lock, or a no-op).
    const bool retreat_backspace_shape_after_key =
        Global::Keycode == VK_BACK && FanyImeIpc::HasRetreatBackspaceShape(GlobalIme::composition.creating_word.active,
                                                                           IsUiLessMode(), client_supports_restore);

    //
    // 普通的拼音字符，发送 preedit 到 TSF 端
    //
    if (FanyImeIpc::ShouldSendCompositionReply(Global::Keycode >= 'A' && Global::Keycode <= 'Z',
                                               is_manual_pinyin_separator, is_microsoft_shuangpin_ing_key,
                                               is_japanese_long_vowel, is_date_time_input_key || is_v_mode_input_key))
    {
        if (IsUiLessMode())
        {
            ScopedKeyStage prepare_reply_uiless{client_id, activation_epoch, request_id, L"handle-candidates"};
            PrepareCandidateList(client_id, activation_epoch);
            prepare_reply_uiless.Stop();
            SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
        }
        else
        {
            if (GlobalSettings::getTsfPreeditStyle() == GlobalSettings::TsfPreeditStyle::Pinyin)
            {
                std::wstring preedit = GetTsfPreedit();
                Global::MsgTypeToTsf = Global::DataFromServerMsgType::Preedit;
                Global::candidate_ui.selected_text = preedit;
                SendCurrentDataToClient(client_id, activation_epoch, request_id);
            }
        }
    }
    else if (Global::Keycode == VK_BACK || Global::Keycode == VK_DELETE || composition_restored)
    {
        if (IsUiLessMode())
        {
            if (g_inputSession->get_pinyin_sequence().empty())
            {
                ClearState();
                Global::MsgTypeToTsf = Global::DataFromServerMsgType::UiLessComposition;
                Global::candidate_ui.selected_text = L"\t";
                SendCurrentDataToClient(client_id, activation_epoch, request_id);
            }
            else
            {
                ScopedKeyStage prepare_backspace_uiless{client_id, activation_epoch, request_id, L"handle-candidates"};
                PrepareCandidateList(client_id, activation_epoch);
                prepare_backspace_uiless.Stop();
                SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
            }
        }
        else if (FanyImeIpc::ShouldAnswerRetreatBackspace(composition_restored, retreat_backspace_shape_before_key,
                                                          retreat_backspace_shape_after_key))
        {
            // Unlike an ordinary deletion, the retraction and the unit caret
            // jump are not mirrored by TSF on its own: for a deletion TSF
            // rebuilds its keystroke buffer from this payload, and for a jump
            // it applies the caret field. It must therefore be sent in both
            // preedit styles, and the trailing caret field pins the
            // authoritative caret.
            //
            // Every Backspace the client may be holding for gets this frame --
            // retreat, plain character deletion behind the edit lock, or a
            // no-op behind an empty history: the DLL arms its hold from the
            // creating-word mirror it saw before the key (word_for_creating_word),
            // and without this frame the default raw preedit style would send no
            // reply at all, burning the hold's full 50 ms timeout. The pre-key
            // shape keeps that promise for the Backspace that deletes the last
            // raw character and ends the word: the post-key shape is gone by
            // then, while the client is still holding. The payload then
            // describes the state the key left behind -- restored, unchanged, or
            // one character shorter when the caret deletion in
            // ApplyCompositionEditKey ran -- which is what the hold applies in
            // every outcome. The candidate page follows it below.
            Global::MsgTypeToTsf = Global::DataFromServerMsgType::CompositionRestored;
            Global::candidate_ui.selected_text =
                BuildCreateWordPipePayload(GlobalIme::composition.raw_input_with_cases,
                                           GlobalIme::composition.creating_word.prefix_text) +
                L'\t' + std::to_wstring(GlobalIme::composition.caret_position);
            SendCurrentDataToClient(client_id, activation_epoch, request_id);
            ScopedKeyStage publish_restored{client_id, activation_epoch, request_id, L"handle-candidates"};
            PublishRestoredCompositionCandidates(client_id, activation_epoch);
        }
        else if (GlobalSettings::getTsfPreeditStyle() == GlobalSettings::TsfPreeditStyle::Pinyin)
        {
            if (!g_inputSession->get_pinyin_sequence().empty())
            {
                std::wstring preedit = GetTsfPreedit();
                Global::MsgTypeToTsf = Global::DataFromServerMsgType::Preedit;
                Global::candidate_ui.selected_text = preedit;
                SendCurrentDataToClient(client_id, activation_epoch, request_id);
            }
        }
    }
    else if (IsUiLessMode() && is_composition_edit_key && Global::Keycode != VK_LEFT && Global::Keycode != VK_RIGHT &&
             Global::Keycode != VK_BACK)
    {
        ScopedKeyStage prepare_edit_uiless{client_id, activation_epoch, request_id, L"handle-candidates"};
        PrepareCandidateList(client_id, activation_epoch);
        prepare_edit_uiless.Stop();
        SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
    }

    //
    // 在以下情况下，TSF 端会请求候选字符串
    //  - 空格，会上屏第一个候选项
    //  - 数字，会上屏相应序号对应的候选项
    //
    // 空格和数字键可能会触发造词，如果数字键上屏的汉字字符串所对应的拼音比实际的拼音要短的话，
    // 那么，就可能会触发造词事件，那么，就要适时改变候选框的状态
    //
    /* VK_SPACE, Digits (T-mode digits that extend a date/time are input) */
    if (Global::Keycode == VK_SPACE ||
        (!is_date_time_input_key && !is_v_mode_input_key && Global::Keycode > '0' && Global::Keycode <= '9'))
    {
        ScopedKeyStage select_space_digit{client_id, activation_epoch, request_id, L"handle-selection"};
        ProcessSelectionKey(Global::Keycode, client_id, activation_epoch);
        select_space_digit.Stop();
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
    }
    else if ((Global::Keycode == VK_LEFT || Global::Keycode == VK_RIGHT) && !arrow_moves_selection)
    {
        if (IsUiLessMode())
        {
            ScopedKeyStage prepare_arrow_uiless{client_id, activation_epoch, request_id, L"handle-candidates"};
            PrepareCandidateList(client_id, activation_epoch);
            prepare_arrow_uiless.Stop();
            SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
        }
        else
        {
            // R2/R4：前缀重算生效时光标移动会改变候选内容——前缀为空则收起候选窗，
            // 其余（含移回串尾）一律从引擎重读重建页面。移回串尾时引擎已按整串重算，
            // 但页面 items 还是旧前缀候选，只刷新页面会让那批旧候选参与结算（真机回归：
            // ni'hao'ya 右移回串尾后空格只上屏「你好」+「ya」）。整串解码（未启用或未协商）
            // 维持只刷新页面的现状（R7/AC8 零差异）。
            const std::string caret_raw = g_inputSession->get_pinyin_sequence_with_cases();
            const std::size_t prefix_end = g_inputSession->prefix_end();
            const bool caret_resegmentation = FanyImeIpc::ShouldResegmentCompositionByCaret(
                client_supports_restore, IsUiLessMode(), g_english_input_mode,
                IsSpecialModeCompositionActive(caret_raw));
            switch (FanyImeIpc::ResolveCaretArrowCandidatePublish(caret_resegmentation, prefix_end, caret_raw.size()))
            {
            case FanyImeIpc::CaretArrowCandidatePublish::Hide:
                HideCandidateWindowAndDropItems();
                break;
            case FanyImeIpc::CaretArrowCandidatePublish::RebuildFromEngine:
                // 窗口可能因之前的前缀为空状态被收起（单音节后缀从 caret=0 右移两次），
                // 必须显式请求显示；PrepareCandidateList 末尾自带 RefreshCandidatePageUi(false)。
                {
                    ScopedKeyStage prepare_arrow_rebuild{client_id, activation_epoch, request_id, L"handle-candidates"};
                    PrepareCandidateList(client_id, activation_epoch);
                }
                RequestShowCandidateWindow();
                break;
            case FanyImeIpc::CaretArrowCandidatePublish::RefreshPageOnly: {
                ScopedKeyStage refresh_arrow_page{client_id, activation_epoch, request_id, L"handle-candidates"};
                RefreshCandidatePageUi(true);
            }
            break;
            }
        }
    }
    else if ((IsCandidateNavigationKey(Global::Keycode) || arrow_moves_selection) && !is_v_mode_input_key)
    {
        auto &ui = Global::candidate_ui;
        UINT result = Global::DataFromServerMsgType::NavigationIgnored;
        bool refresh = false;

        const auto move_page = [&](int offset, UINT response_type) {
            result = response_type;
            // Keyboard paging keeps the in-page selection where it is; the wheel
            // path in WorkerThread is the one that restarts it at the top.
            ScopedKeyStage move_page_nav{client_id, activation_epoch, request_id, L"handle-candidates"};
            if (MoveCandidatePage(offset) != PageMoveResult::Unchanged)
            {
                refresh = true;
            }
        };
        const auto move_selection = [&](int offset, UINT response_type) {
            result = response_type;
            // 要在下面的扩展之前标记：扩展得按「已开始选词」只追加，不能重排已显示的候选。
            ui.begin_selection();
            if (offset > 0 && (ui.is_selection_at_last_candidate() ||
                               (ui.is_selection_at_current_page_end() && ui.is_next_page_partial_last_page())))
            {
                ScopedKeyStage expand_nav{client_id, activation_epoch, request_id, L"handle-candidates"};
                ExpandCandidatesKeepingPagePosition();
            }
            if (ui.move_selection(offset))
            {
                refresh = true;
            }
        };

        const bool shift_down = (Global::ModifiersDown & 0b00000001u) != 0;
        // Shift+- / Shift+= append the ASCII symbol to the raw spelling. The key
        // stays claimed by the IME, so the reply has to rebuild the composition;
        // MoveCandidatePage would otherwise swallow it as a page turn. Hosts that
        // cannot apply the frame fall through to the ordinary paging path.
        if (GetConfiguredPagingMinusEqualEnabled() &&
            FanyImeIpc::IsShiftedPagingSymbolKey(Global::Keycode, Global::Wch, Global::ModifiersDown) &&
            AppendLiteralToComposition(Global::Wch, client_id, activation_epoch, request_id))
        {
            return;
        }
        if (Global::Keycode == VK_OEM_MINUS && GetConfiguredPagingMinusEqualEnabled())
        {
            const PageMoveResult moved = MoveCandidatePage(-1);
            // The first page has no previous page, so the key becomes a literal
            // '-' in the raw spelling instead of a page turn that swallows it.
            // Ctrl and Alt combinations keep the old no-op paging behavior.
            if (moved == PageMoveResult::Unchanged && (Global::ModifiersDown & FanyImeIpc::kKeyModifierMask) == 0 &&
                AppendLiteralToComposition(L'-', client_id, activation_epoch, request_id))
            {
                return;
            }
            result = Global::DataFromServerMsgType::MovePagePrevious;
            refresh = moved != PageMoveResult::Unchanged;
        }
        else if (Global::Keycode == VK_OEM_PLUS && GetConfiguredPagingMinusEqualEnabled())
        {
            move_page(1, Global::DataFromServerMsgType::MovePageNext);
        }
        else if (Global::Keycode == VK_OEM_COMMA && GetConfiguredPagingCommaPeriodEnabled())
        {
            move_page(-1, Global::DataFromServerMsgType::MovePagePrevious);
        }
        else if (Global::Keycode == VK_OEM_PERIOD && GetConfiguredPagingCommaPeriodEnabled())
        {
            move_page(1, Global::DataFromServerMsgType::MovePageNext);
        }
        else if (Global::Keycode == VK_OEM_4 && GetConfiguredPagingBracketsEnabled())
        {
            move_page(-1, Global::DataFromServerMsgType::MovePagePrevious);
        }
        else if (Global::Keycode == VK_OEM_6 && GetConfiguredPagingBracketsEnabled())
        {
            move_page(1, Global::DataFromServerMsgType::MovePageNext);
        }
        else if (Global::Keycode == VK_TAB && GetConfiguredPagingTabEnabled())
        {
            move_page(shift_down ? -1 : 1, shift_down ? Global::DataFromServerMsgType::MovePagePrevious
                                                      : Global::DataFromServerMsgType::MovePageNext);
        }
        else if (Global::Keycode == VK_PRIOR && GetConfiguredPagingPageUpDownEnabled())
        {
            move_page(-1, Global::DataFromServerMsgType::MovePagePrevious);
        }
        else if (Global::Keycode == VK_NEXT && GetConfiguredPagingPageUpDownEnabled())
        {
            move_page(1, Global::DataFromServerMsgType::MovePageNext);
        }
        else if (arrow_moves_selection)
        {
            if (Global::Keycode == VK_LEFT)
            {
                move_selection(-1, Global::DataFromServerMsgType::MoveSelectionPrevious);
            }
            else
            {
                move_selection(1, Global::DataFromServerMsgType::MoveSelectionNext);
            }
        }
        else if (GetConfiguredCandidateArrowNavigationEnabled() &&
                 (Global::Keycode == VK_UP || Global::Keycode == VK_DOWN))
        {
            if (Global::Keycode == VK_UP)
            {
                move_selection(-1, Global::DataFromServerMsgType::MoveSelectionPrevious);
            }
            else
            {
                move_selection(1, Global::DataFromServerMsgType::MoveSelectionNext);
            }
        }

        if (IsUiLessMode())
        {
            if (refresh)
            {
                ScopedKeyStage refresh_nav_uiless{client_id, activation_epoch, request_id, L"handle-candidates"};
                RefreshCandidatePageUi(false);
            }
            else
            {
                ScopedKeyStage ensure_nav_uiless{client_id, activation_epoch, request_id, L"handle-candidates"};
                EnsureCandidatePageReady();
            }
            // Prefer selection index in page for host-drawn lists.
            if (!ui.page_words.empty())
            {
                ui.selected_index_in_page =
                    std::clamp(ui.selected_index_in_page, 0, static_cast<int>(ui.page_words.size()) - 1);
            }
            SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
        }
        else
        {
            Global::MsgTypeToTsf = result;
            SendCurrentDataToClient(client_id, activation_epoch, request_id);
            if (refresh)
            {
                ScopedKeyStage refresh_nav{client_id, activation_epoch, request_id, L"handle-candidates"};
                RefreshCandidatePageUi(true);
            }
        }
    }
}
} // namespace FanyNamedPipe
