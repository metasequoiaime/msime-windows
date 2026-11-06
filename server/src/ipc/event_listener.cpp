#include "event_listener.h"
#include "engine/contracts/temporary_japanese_input.h"
#include <Windows.h>
#include <debugapi.h>
#include <ioapiset.h>
#include <string>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <utility>
#include <unordered_map>
#include "Ipc.h"
#include "ipc/ai_context_policy.h"
#include "ipc/candidate_text_policy.h"
#include "ipc/traditional_variant_expansion_policy.h"
#include "ipc/async_request_origin.h"
#include "ipc/candidate_ui_owner.h"
#include "defines/defines.h"
#include "ipc.h"
#include "defines/globals.h"
#include "utils/common_utils.h"
#include <boost/range/iterator_range_core.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string.hpp>
#include "global/globals.h"
#include "ipc/event_listener.h"
#include "window/caret_state_indicator.h"
#include "window/caret_state_indicator_policy.h"
#include "utils/ime_utils.h"
#include "cloud/cloud_ime.h"
#include "cloud/cloud_translation.h"
#include "ai/ai_assistant.h"
#include "english/english_ime.h"
#include "config/ime_config.h"
#include "engine/contracts/date_time_input.h"
#include "engine/contracts/uiless_page.h"
#include "engine/contracts/v_mode_input.h"
#include "conversion/chinese_converter.h"
#include "conversion/traditional_spelling_index.h"
#include "log/candidate_diag_log.h"
#include "ipc/event_listener_internal.h"
#include "ai/jev_candidate_policy.h"

using namespace event_listener_detail;

namespace event_listener_detail
{
bool g_quick_phrase_triggered = false;
bool g_radical_mode_triggered = false;
bool g_date_time_mode_triggered = false;
bool g_emoji_mode_triggered = false;
bool g_kaomoji_mode_triggered = false;
bool g_symbol_mode_triggered = false;
bool g_jianpin_mode_triggered = false;
bool g_y_mode_triggered = false;
bool g_r_mode_triggered = false;
std::shared_ptr<IInputSession> g_r_mode_original_session;
std::shared_ptr<IInputSession> g_language_cycle_parked_session;
bool g_english_input_mode = false;
} // namespace event_listener_detail

namespace
{
// Sticky UILess for the active Main-pipe client. When set, never raise the
// candidate HWND — hosts (games) draw via ITfUIElementSink instead.
bool g_activate_uiless = false;
bool g_session_uiless = false;
} // namespace

namespace
{
// 界面线程记下的鼠标操作所在页面代次，0 表示没有。不走任务队列：排在它前面的异步结果
// 必须在合并前就看到它，而且鼠标事件不能打断翻页任务的合并。
std::atomic<std::uint64_t> g_candidate_ui_interaction_generation{0};
} // namespace

namespace event_listener_detail
{
std::unordered_map<std::string, CandidateTranslationGloss> g_candidate_translation_glosses;
std::string g_candidate_translation_signature;

// 副候选框：Ctrl+Enter 在高亮候选有多条译义时，把候选框整个换成那几条译义，让空格/
// 数字键像选普通候选一样选一条上屏。输入串一个字都没动，所以退出这个子模式时把原来的
// items / 页码 / 高亮位原样放回去就行，不需要重新查词。
bool g_translation_candidates_active = false;
bool g_date_time_page_active = false;
std::string g_date_time_page_keyword;
std::vector<WordItem> g_translation_saved_items;
int g_translation_saved_page_index = 0;
int g_translation_saved_selected_index = 0;
bool g_translation_saved_selection_started = false;

std::string TranslationIdentity(const EnglishIme::TranslationQuery &query)
{
    return GetConfiguredTencentTmt().target_language + ":" +
           (query.direction == EnglishIme::TranslationDirection::EnglishToChinese ? "e:" : "z:") + query.key;
}

bool IsCandidateSubPageActive()
{
    return g_translation_candidates_active || g_date_time_page_active;
}

void LatchCandidateUiInteraction()
{
    const std::uint64_t generation = g_candidate_ui_interaction_generation.exchange(0, std::memory_order_acq_rel);
    // 只认用户操作时屏幕上那一页；之后已经换了一批候选，就不该冻结新的这批。
    if (generation != 0 && generation == Global::LoadCandidatePageSnapshot()->generation)
        Global::candidate_ui.begin_selection();
}

bool CanMergeAsyncCandidates()
{
    return !IsCandidateSubPageActive() && !Global::candidate_ui.selection_started;
}

bool IsUiLessMode()
{
    return g_activate_uiless || g_session_uiless;
}

void ApplyUiLessFromPacket(const FanyImeNamedpipeData &pipe_data)
{
    const bool wasUiLess = IsUiLessMode();
    if (pipe_data.event_type == FanyImePipeEventType::ClientActivated)
    {
        g_activate_uiless = (pipe_data.keycode != 0);
        g_session_uiless = g_activate_uiless;
    }
    else if (FanyImePipeEventType::IsRouteDeactivation(pipe_data.event_type))
    {
        g_activate_uiless = false;
        g_session_uiless = false;
    }
    else if (pipe_data.event_type == FanyImePipeEventType::KeyEvent ||
             pipe_data.event_type == FanyImePipeEventType::InputStateAction ||
             pipe_data.event_type == FanyImePipeEventType::ShowCandidateWnd ||
             pipe_data.event_type == FanyImePipeEventType::MoveCandidateWnd ||
             pipe_data.event_type == FanyImePipeEventType::HideCandidateWnd)
    {
        g_session_uiless = g_activate_uiless || ((pipe_data.modifiers_down & FanyImePipeFlags::UiLess) != 0);
    }

    if (!wasUiLess && IsUiLessMode())
    {
        g_candidate_translation_signature.clear();
        g_candidate_translation_glosses.clear();
        EnglishIme::ClearTranslations();
        // A prior non-UILess session may have left the candidate HWND visible;
        // hide it immediately when the host takes over drawing.
        ::is_global_wnd_cand_shown = false;
        Global::SetCandidateWindowRenderedVisible(false);
        if (::global_hwnd && IsWindow(::global_hwnd))
        {
            PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
        }
        // Same for a caret badge shown just before the host went UILess.
        if (const HWND caretState = ::global_hwnd_caret_state)
        {
            PostMessage(caretState, WM_HIDE_CARET_STATE, 0, 0);
        }
    }
}

// Dedicated English queries (English mode, Y mode) publish a placeholder page
// while the dictionary lookup is in flight: empty in English mode, the bare
// typed word in Y mode (see PrepareCandidateList). Painting it collapsed the
// card to one row, and ApplyEnglishCandidates grew it back a moment later — a
// height flash on every keystroke. The English worker answers every non-empty
// input (no match becomes the raw fallback) and that answer requests its own
// show, so the placeholder is never worth painting. Set by PrepareCandidateList
// for each dedicated query, cleared when its answer is applied.
bool g_dedicated_english_answer_pending = false;
} // namespace event_listener_detail

namespace
{
bool IsDedicatedEnglishAnswerPending()
{
    // ApplyEnglishCandidates refuses answers in these states, so nothing
    // would ever release the show.
    return g_dedicated_english_answer_pending && !GlobalIme::composition.creating_word.active &&
           !IsCandidateSubPageActive();
}
} // namespace

namespace event_listener_detail
{
void RequestShowCandidateWindow()
{
    if (IsUiLessMode() || !::global_hwnd)
    {
        CAND_DIAG_LOGF(L"show request skipped uiless={} hwnd_present={}", IsUiLessMode(), ::global_hwnd != nullptr);
        return;
    }
    if (IsDedicatedEnglishAnswerPending())
    {
        CAND_DIAG_LOGF(L"show request deferred to english query raw_units={}",
                       GlobalIme::composition.raw_input_with_cases.size());
        return;
    }
    bool expected = false;
    if (!g_candidate_show_msg_pending.compare_exchange_strong(expected, true))
    {
        CAND_DIAG_LOGF(L"show request coalesced raw_units={} candidate_count={}",
                       GlobalIme::composition.raw_input_with_cases.size(), Global::candidate_ui.items.size());
        return;
    }
    CAND_DIAG_LOGF(L"show request posted raw_units={} candidate_count={}",
                   GlobalIme::composition.raw_input_with_cases.size(), Global::candidate_ui.items.size());
    if (!PostMessage(::global_hwnd, WM_SHOW_MAIN_WINDOW, 0, 0))
    {
        g_candidate_show_msg_pending.store(false);
    }
}

// Drop every published candidate page and hide the candidate window without
// touching the composition session. The creating-word state can survive with no
// pinyin left after a segment Backspace, and that state must not be reset just
// because there is nothing left to offer as candidates.
void HideCandidateWindowAndDropItems()
{
    // Drop published candidates before a queued show can re-render an
    // empty-preedit + stale-candidate view.
    Global::CandidateString.clear();
    Global::ClearCandidatePageSnapshot();
    Global::candidate_ui.set_items({});
    // 候选窗收起时展开面板跟着关掉，别让下一次显示把它带回来。
    FanyNamedPipe::CloseCandidateExpansionPanel();
    // Clear the shown flag first so async callbacks refuse to resurrect the
    // window, then post the actual hide message.
    ::is_global_wnd_cand_shown = false;
    Global::SetCandidateWindowRenderedVisible(false);
    if (::global_hwnd && IsWindow(::global_hwnd))
    {
        PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
    }
}
} // namespace event_listener_detail

namespace event_listener_detail
{
bool IsQuickPhraseCompositionActive(const std::string &raw)
{
    return g_quick_phrase_triggered && !raw.empty() && raw.front() == 'K';
}

// U 模式：Shift+U 唤醒后整段组合都归它，和 K 模式一样只看唤醒标记与首字符。串里混进的撇号、
// 大写字母等照常进 raw（TSF 的按键缓冲同样收下它们，两侧才不会错位），只是查不出候选；
// 判据若要求其余全是 a-z，这种串就会掉回普通拼音路径。
bool IsRadicalCompositionActive(const std::string &raw)
{
    return FanyImeIpc::IsRadicalComposition(g_radical_mode_triggered, raw);
}

// T 后面是小写唤醒词，或者以数字开头的指定日期时间（形状见 engine/contracts/date_time_input.h）。后者
// 不再看其余字符：字母键照常进输入串，T2024a 仍算 T 模式，只是没有候选。
bool IsDateTimeCompositionActive(const std::string &raw)
{
    if (!g_date_time_mode_triggered || raw.empty() || raw.front() != 'T')
        return false;
    return FanyImeDateTimeInput::IsSpecificBody(raw.data() + 1, raw.size() - 1) ||
           std::all_of(raw.begin() + 1, raw.end(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; });
}

bool IsEmojiCompositionActive(const std::string &raw)
{
    return g_emoji_mode_triggered && !raw.empty() && raw.front() == 'E' &&
           std::all_of(raw.begin() + 1, raw.end(), [](unsigned char ch) {
               return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '\'';
           });
}

bool IsKaomojiCompositionActive(const std::string &raw)
{
    return g_kaomoji_mode_triggered && !raw.empty() && raw.front() == 'M' &&
           std::all_of(raw.begin() + 1, raw.end(), [](unsigned char ch) {
               return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '\'';
           });
}

// S 模式：符号编码只有字母（万象分组码与 codex 符号名），大写按小写查。
bool IsSymbolCompositionActive(const std::string &raw)
{
    return g_symbol_mode_triggered && !raw.empty() && raw.front() == 'S' &&
           std::all_of(raw.begin() + 1, raw.end(),
                       [](unsigned char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); });
}

bool IsJianpinCompositionActive(const std::string &raw)
{
    return g_jianpin_mode_triggered && !raw.empty() && raw.front() == 'J';
}

bool IsYModeCompositionActive(const std::string &raw)
{
    return g_y_mode_triggered && !raw.empty() && raw.front() == 'Y' &&
           std::all_of(raw.begin() + 1, raw.end(),
                       [](unsigned char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); });
}

bool IsYModeInput(const std::string &raw)
{
    return IsYModeCompositionActive(raw) && raw.size() > 1;
}

FanyImeVModeInput::Trigger CurrentVModeTrigger()
{
    return FanyImeVModeInput::TriggerFromPayload(FormatVModeWorkerPayload().front());
}

// V 模式没有单独的触发标记：V（全拼还有 v）后面紧跟数字或左括号就是，和 TSF 用同一条规则
// （engine/contracts/v_mode_input.h），所以小写 v 开头的 vip 仍是普通输入。
bool IsVModeCompositionActive(const std::string &raw)
{
    return FanyImeVModeInput::IsComposition(raw.data(), raw.size(), CurrentVModeTrigger());
}

void ClearSpecialModeTriggers()
{
    g_quick_phrase_triggered = false;
    g_radical_mode_triggered = false;
    g_date_time_mode_triggered = false;
    g_emoji_mode_triggered = false;
    g_kaomoji_mode_triggered = false;
    g_symbol_mode_triggered = false;
    g_jianpin_mode_triggered = false;
    g_y_mode_triggered = false;
    g_r_mode_triggered = false;
}

// True whenever a K/U/T/E/M/S/J/Y special-mode composition is in progress, even when the
// typed text is not yet a complete keyword/letter sequence. Such input must never
// be interpreted as normal pinyin.
bool IsSpecialModeCompositionActive(const std::string &raw)
{
    return IsQuickPhraseCompositionActive(raw) || IsRadicalCompositionActive(raw) || IsDateTimeCompositionActive(raw) ||
           IsEmojiCompositionActive(raw) || IsKaomojiCompositionActive(raw) || IsSymbolCompositionActive(raw) ||
           IsJianpinCompositionActive(raw) || IsYModeCompositionActive(raw) || IsVModeCompositionActive(raw);
}

bool IsWubiLiteralComposition(const std::string &raw)
{
    // T 模式按触发标记认，不按 IsDateTimeCompositionActive 的形状认：模式里敲了主体不收的键
    // （TdatE），组合只是不再出日期，里面的字母仍然不是要上屏的五笔码（tdat）。
    return (g_date_time_mode_triggered && !raw.empty() && raw.front() == 'T') || IsVModeCompositionActive(raw);
}
} // namespace event_listener_detail

namespace
{
std::mutex g_async_request_mutex;
uint64_t g_cloud_generation = 0;
uint64_t g_english_generation = 0;
uint64_t g_mixed_generation = 0;
uint64_t g_ai_generation = 0;
AsyncRequestOrigin g_cloud_request_origin;
AsyncRequestOrigin g_english_request_origin;
AsyncRequestOrigin g_mixed_request_origin;
AsyncRequestOrigin g_ai_request_origin;
std::string g_ai_context;
// g_ai_context 属于哪一次焦点会话。换了宿主进程（client_id）或焦点会话重新激活（activation_epoch）
// 就清空，免得在一个应用里打的字被带进另一个应用的 AI 请求。
uint64_t g_ai_context_client_id = 0;
uint64_t g_ai_context_activation_epoch = 0;
// 当前按键来自密码 / PIN 输入框（TSF 按 InputScope 判定）：不发 AI 请求，上屏也不进上下文。
bool g_ai_sensitive_field = false;
std::mutex g_status_snapshot_mutex;
int g_latest_status_snapshot = -1;
bool g_latest_english_input_mode = false;
} // namespace

namespace event_listener_detail
{
// Global CN/EN authority for input.ime_mode_scope = "global".
// -1 until first StatusSnapshot or lazy seed from default_ime_mode.
int g_authoritative_cn_mode = -1;
} // namespace event_listener_detail

namespace
{
// The toolbar is global but the mode is per TSF client, so an activation would
// otherwise keep displaying the outgoing client's mode until the incoming one
// happens to send its first snapshot. Entries are dropped when the client's
// main pipe unregisters.
std::unordered_map<uint64_t, int> g_client_status_snapshots;

HWND g_status_snapshot_window = nullptr;
std::mutex g_candidate_ui_owner_mutex;
FanyImeIpc::CandidateUiOwnerState g_candidate_ui_owner;
} // namespace

namespace event_listener_detail
{
void PublishCandidateUiOwner(uint64_t client_id, uint64_t activation_epoch)
{
    std::lock_guard lock(g_candidate_ui_owner_mutex);
    g_candidate_ui_owner.publish(client_id, activation_epoch);
}
} // namespace event_listener_detail

namespace
{
void ClearCandidateUiOwner()
{
    std::lock_guard lock(g_candidate_ui_owner_mutex);
    g_candidate_ui_owner.clear();
}
} // namespace

namespace event_listener_detail
{
FanyImeIpc::CandidateUiOwner SnapshotCandidateUiOwner()
{
    std::lock_guard lock(g_candidate_ui_owner_mutex);
    return g_candidate_ui_owner.snapshot();
}

bool CandidateUiOwnerIsCurrent(const FanyImeIpc::CandidateUiOwner &owner)
{
    std::lock_guard lock(g_candidate_ui_owner_mutex);
    return g_candidate_ui_owner.matches(owner);
}

void PublishStatusSnapshotValue(int packed_state)
{
    std::lock_guard lock(g_status_snapshot_mutex);
    g_latest_status_snapshot = packed_state;
    const bool has_window = g_status_snapshot_window && IsWindow(g_status_snapshot_window);
    if (has_window)
    {
        PostMessage(g_status_snapshot_window, UPDATE_FTB_STATUS, packed_state, 0);
    }
}
} // namespace event_listener_detail

namespace
{
void PublishEnglishInputModeValue(bool enabled)
{
    std::lock_guard lock(g_status_snapshot_mutex);
    g_latest_english_input_mode = enabled;
    const bool has_window = g_status_snapshot_window && IsWindow(g_status_snapshot_window);
    if (has_window)
    {
        PostMessage(g_status_snapshot_window, UPDATE_FTB_ENGLISH_INPUT_MODE, enabled ? 1 : 0, 0);
    }
}
} // namespace

namespace event_listener_detail
{
void SetEnglishInputMode(bool enabled)
{
    if (g_english_input_mode == enabled)
    {
        return;
    }
    g_english_input_mode = enabled;
    PublishEnglishInputModeValue(enabled);
    BroadcastToTsfWorkerThreadViaNamedpipe(FanyImeWorkerReplyType::TemporaryJapanesePolicyChanged,
                                           FanyNamedPipe::TemporaryJapanesePolicyPayload());
}

void RememberClientStatusSnapshot(uint64_t client_id, int packed_state)
{
    if (client_id == 0)
    {
        return;
    }
    std::lock_guard lock(g_status_snapshot_mutex);
    g_client_status_snapshots[client_id] = packed_state;
}

void ForgetClientStatusSnapshot(uint64_t client_id)
{
    std::lock_guard lock(g_status_snapshot_mutex);
    g_client_status_snapshots.erase(client_id);
}

// Returns -1 when this client has never reported a mode.
int RecallClientStatusSnapshot(uint64_t client_id)
{
    std::lock_guard lock(g_status_snapshot_mutex);
    const auto it = g_client_status_snapshots.find(client_id);
    return it == g_client_status_snapshots.end() ? -1 : it->second;
}

// Caret-badge events are presentation only. They never touch the toolbar or
// the status snapshot: StatusSnapshot remains the single source of mode state.
void PostCaretStateBadge(FanyImeUi::CaretStateBadge badge, int x, int y)
{
    const HWND hwnd = ::global_hwnd_caret_state;
    if (!hwnd || !FanyImeUi::IsUsableCaretAnchor(x, y))
        return;
    auto *request = new (std::nothrow) CaretStateIndicator::ShowRequest{std::move(badge), POINT{x, y}, IsUiLessMode()};
    if (request && !PostMessage(hwnd, WM_SHOW_CARET_STATE, 0, reinterpret_cast<LPARAM>(request)))
        delete request;
}

void UpdateCloudInput(const std::string &input, uint64_t client_id, uint64_t activation_epoch)
{
    std::lock_guard lock(g_async_request_mutex);
    const std::string effective_input = GetConfiguredCloudCandidatesEnabled() ? input : std::string{};
    const bool japanese = g_inputSession && g_inputSession->current_scheme_type() == SchemeType::JapaneseRomaji;
    CloudIme::OnInputChanged(effective_input, japanese);
    ++g_cloud_generation;
    g_cloud_request_origin = effective_input.empty()
                                 ? AsyncRequestOrigin{}
                                 : AsyncRequestOrigin{client_id, activation_epoch, g_cloud_generation, effective_input};
    if (!effective_input.empty() && g_inputSession)
        g_cloud_request_origin.engine_query = g_inputSession->online_query();
}

void UpdateEnglishInput(const std::string &input, uint64_t client_id, uint64_t activation_epoch, bool dedicated_mode)
{
    std::lock_guard lock(g_async_request_mutex);
    const size_t mixed_min_prefix =
        dedicated_mode ? size_t{1} : static_cast<size_t>(GetConfiguredEnglishMixedInputMinChars());
    EnglishIme::OnInputChanged(input, dedicated_mode, mixed_min_prefix);
    ++g_english_generation;
    g_english_request_origin = input.empty()
                                   ? AsyncRequestOrigin{}
                                   : AsyncRequestOrigin{client_id, activation_epoch, g_english_generation, input};
}

void UpdateMixedInput(MixedCandidates::Request request, uint64_t client_id, uint64_t activation_epoch)
{
    std::lock_guard lock(g_async_request_mutex);
    const std::string input = request.input;
    MixedCandidates::OnInputChanged(std::move(request));
    ++g_mixed_generation;
    g_mixed_request_origin = input.empty() ? AsyncRequestOrigin{}
                                           : AsyncRequestOrigin{client_id, activation_epoch, g_mixed_generation, input};
}
} // namespace event_listener_detail

namespace
{
std::vector<std::string> SplitPinyin(const std::string &segmentation)
{
    std::vector<std::string> result;
    boost::split(result, segmentation, boost::is_any_of("' "), boost::token_compress_on);
    result.erase(std::remove_if(result.begin(), result.end(), [](const std::string &item) { return item.empty(); }),
                 result.end());
    return result;
}
} // namespace

namespace event_listener_detail
{
void UpdateAiInput(const std::string &identity, uint64_t client_id, uint64_t activation_epoch)
{
    ClearJevSelection();
    std::lock_guard lock(g_async_request_mutex);
    const AiAssistantConfig config = GetConfiguredAiAssistant();
    const bool usable = config.enabled && g_inputSession &&
                        (g_inputSession->current_scheme_type() == SchemeType::Quanpin ||
                         g_inputSession->current_scheme_type() == SchemeType::Shuangpin) &&
                        g_inputSession->is_all_complete_pure_pinyin() && !g_inputSession->has_active_helpcode() &&
                        !identity.empty() && !g_ai_sensitive_field;
    (void)0;
    AiAssistant::Request request;
    if (usable)
    {
        request.pinyin_segments = SplitPinyin(g_inputSession->get_pinyin_segmentation());
        request.context = FanyImeIpc::StripCredentialLikeText(g_ai_context);
        request.identity = identity;
        request.config = config;
        if (config.provider == "jev")
            request.candidates = Jev::CandidatePool(g_inputSession->get_candidates(), request.pinyin_segments.size());
    }
    AiAssistant::OnInputChanged(std::move(request));
    ++g_ai_generation;
    g_ai_request_origin =
        usable ? AsyncRequestOrigin{client_id, activation_epoch, g_ai_generation, identity} : AsyncRequestOrigin{};
    if (usable)
        g_ai_request_origin.engine_query = g_inputSession->online_query();
}

AsyncRequestOrigin FindCloudRequestOrigin(const std::string &input, uint64_t generation)
{
    std::lock_guard lock(g_async_request_mutex);
    if (g_cloud_request_origin.matches(input, generation))
    {
        return g_cloud_request_origin;
    }
    return {};
}

AsyncRequestOrigin FindEnglishRequestOrigin(const std::string &input, uint64_t generation)
{
    std::lock_guard lock(g_async_request_mutex);
    if (g_english_request_origin.generation == generation && g_english_request_origin.input == input)
    {
        return g_english_request_origin;
    }
    return {};
}

AsyncRequestOrigin FindMixedRequestOrigin(const std::string &input, uint64_t generation)
{
    std::lock_guard lock(g_async_request_mutex);
    if (g_mixed_request_origin.generation == generation && g_mixed_request_origin.input == input)
    {
        return g_mixed_request_origin;
    }
    return {};
}

AsyncRequestOrigin FindAiRequestOrigin(const std::string &input, uint64_t generation)
{
    std::lock_guard lock(g_async_request_mutex);
    if (g_ai_request_origin.matches(input, generation))
        return g_ai_request_origin;
    return {};
}

namespace
{
bool ShouldConvertCandidateTextIn(const std::string &character_set, bool radical_candidate_list)
{
    const bool japanese_scheme = g_inputSession && g_inputSession->current_scheme_type() == SchemeType::JapaneseRomaji;
    return FanyImeIpc::ShouldConvertCandidateTextToTraditional(character_set == "traditional", japanese_scheme,
                                                               radical_candidate_list);
}

bool ShouldConvertCandidateText(bool radical_candidate_list)
{
    return ShouldConvertCandidateTextIn(GetConfiguredCharacterSet(), radical_candidate_list);
}

std::string ToTraditionalText(const std::string &text)
{
    return ChineseConverter::ToTraditional(text);
}

std::vector<std::string> TraditionalSpellingsOfText(const std::string &text)
{
    return FanyImeIpc::TraditionalSpellingsOf(text, ChineseConverter::OpenCcTraditionalSpellingIndex(),
                                              ChineseConverter::UserTraditionalRegion());
}
} // namespace

std::string CandidateTextForOutput(const std::string &text)
{
    if (!ShouldConvertCandidateText(/*radical_candidate_list=*/false))
        return text;
    return ToTraditionalText(text);
}

bool IsRadicalCandidateListActive()
{
    return g_inputSession &&
           FanyImeIpc::IsRadicalCandidateList(/*radical_mode_triggered=*/g_radical_mode_triggered,
                                              /*translation_page_active=*/g_translation_candidates_active,
                                              /*raw=*/g_inputSession->get_pinyin_sequence_with_cases());
}

const std::string &ShownCandidateCharacterSet()
{
    // 主列表按记下的字形展开过写法，显示与上屏要用同一个字形，所以读记下的值。
    // 译文页、日期页的项没有展开过写法，切换字形时也不换写法（见 RespellCandidatesForCharacterSetChange），
    // 读当前配置才能让这一页跟着切换后的字形显示。
    return IsCandidateSubPageActive() ? GetConfiguredCharacterSet() : FanyNamedPipe::CandidateListCharacterSet();
}

std::string CandidateOutputText(const WordItem &item)
{
    const bool convert = ShouldConvertCandidateTextIn(ShownCandidateCharacterSet(), IsRadicalCandidateListActive());
    return FanyImeIpc::CandidateOutputTextFor(item, convert, ToTraditionalText);
}

std::vector<WordItem> WithTraditionalVariantsFor(std::vector<WordItem> items, const std::string &character_set)
{
    if (!ShouldConvertCandidateTextIn(character_set, IsRadicalCandidateListActive()))
        return items;
    return FanyImeIpc::ExpandTraditionalVariants(std::move(items), ChineseConverter::OpenCcTraditionalSpellingIndex(),
                                                 ChineseConverter::UserTraditionalRegion());
}

std::vector<WordItem> WithTraditionalVariants(std::vector<WordItem> items)
{
    return WithTraditionalVariantsFor(std::move(items), FanyNamedPipe::CandidateListCharacterSet());
}

void ReorderCandidatesAsOneItemPerWord(std::vector<WordItem> &items,
                                       const std::function<void(std::vector<WordItem> &)> &reorder)
{
    items = FanyImeIpc::ReorderAsOneItemPerWord(std::move(items), reorder, WithTraditionalVariants);
}

std::string FirstSpellingOfText(const std::string &text)
{
    if (!ShouldConvertCandidateTextIn(ShownCandidateCharacterSet(), /*radical_candidate_list=*/false))
        return text;
    const auto spellings = TraditionalSpellingsOfText(text);
    return spellings.empty() ? text : spellings.front();
}

FanyImeIpc::ChosenText CreatingWordChosenText()
{
    const auto &creating_word = GlobalIme::composition.creating_word;
    // 选完上一段之后切过字形时，前缀仍按旧字形显示，整串取第一写法会把它改成新字形。
    const bool same_character_set = creating_word.first_segment_character_set.empty() ||
                                    creating_word.first_segment_character_set == ShownCandidateCharacterSet();
    return {creating_word.word, creating_word.output_text, creating_word.all_first_spellings && same_character_set};
}

FanyImeIpc::CandidateSpellingSettings CandidateListSpellingSettings()
{
    return {ChineseConverter::OpenCcTraditionalSpellingIndex(), ChineseConverter::UserTraditionalRegion(),
            FanyNamedPipe::CandidateListCharacterSet() == "traditional",
            g_inputSession ? g_inputSession->current_scheme_type() : SchemeType::Quanpin,
            IsRadicalCandidateListActive()};
}

WordItem WithFirstTraditionalSpelling(WordItem item)
{
    return FanyImeIpc::WithFirstTraditionalSpellingOf(std::move(item), CandidateListSpellingSettings());
}

void NoteAiContextKeySource(uint64_t client_id, uint64_t activation_epoch, bool sensitive_field)
{
    g_ai_sensitive_field = sensitive_field;
    if (client_id == g_ai_context_client_id && activation_epoch == g_ai_context_activation_epoch)
        return;
    g_ai_context_client_id = client_id;
    g_ai_context_activation_epoch = activation_epoch;
    if (g_ai_context.empty())
        return;
    g_ai_context.clear();
    if (g_inputSession)
        g_inputSession->set_rescoring_context(g_ai_context);
}

// 每次提交都把上屏文本追加进 AI 联想的上下文，并按 UTF-8 边界裁剪到 kAiContextMaxBytes。
// committed_word 是候选的内部文字，原样追加：上下文的读方按简体工作，不要传显示用的繁体。
// 造词过程中每选中一段都会各自调用一次，因此这里只追加本次提交的那一段。
void AppendAiContext(const std::string &committed_word)
{
    if (g_ai_sensitive_field)
        return;
    g_ai_context += committed_word;
    g_ai_context = FanyImeIpc::TrimAiContextFront(std::move(g_ai_context));
    // 同一份上屏历史也是神经整句重排的前文（青简那边叫 InputHistory）：下一次查询会随
    // QueryRequest 下发到词典层。词典层自己按 RerankOptions::context_chars 取末尾若干字。
    if (g_inputSession)
        g_inputSession->set_rescoring_context(g_ai_context);
}

void SyncShuangpinPreeditForms()
{
    auto &composition = GlobalIme::composition;
    composition.shuangpin_raw_segmentation.clear();
    composition.shuangpin_quanpin_segmentation.clear();
    composition.shuangpin_forms_source.clear();
    // 只有 segmented_pinyin 就是双拼会话自己的切分时才换成全拼：特殊模式、英文模式和 R 模式都会把它
    // 改写成按键原串，那些组合里的字母不是双拼编码。
    if (!g_inputSession || g_inputSession->current_scheme_type() != SchemeType::Shuangpin || g_english_input_mode ||
        g_r_mode_triggered || IsSpecialModeCompositionActive(composition.raw_input_with_cases) ||
        composition.segmented_pinyin != g_inputSession->get_pinyin_segmentation_with_cases())
    {
        return;
    }
    auto forms = g_inputSession->get_shuangpin_preedit_forms();
    if (forms.quanpin.empty())
    {
        return;
    }
    composition.shuangpin_raw_segmentation = std::move(forms.raw);
    composition.shuangpin_quanpin_segmentation = std::move(forms.quanpin);
    composition.shuangpin_forms_source = composition.segmented_pinyin;
}

std::wstring BuildCreateWordPipePayload(const std::string &remaining_raw_input_with_cases,
                                        const std::string &current_output_text)
{
    // remaining_raw \t committed_word \t display_preedit
    // display_preedit matches the candidate-window preedit (汉字 + 剩余分词).
    // Legacy TSF only reads the first two fields.
    const std::wstring remaining = string_to_wstring(remaining_raw_input_with_cases);
    const std::wstring word = string_to_wstring(current_output_text);
    const std::wstring preedit = BuildTsfPreedit(word, remaining_raw_input_with_cases);
    return remaining + L'\t' + word + L'\t' + preedit;
}
} // namespace event_listener_detail

namespace FanyNamedPipe
{
void NoteCandidateUiInteraction(uint64_t page_generation)
{
    if (page_generation != 0)
        g_candidate_ui_interaction_generation.store(page_generation, std::memory_order_release);
}

FanyImeTemporaryJapanese::Policy TemporaryJapanesePolicy()
{
    // Keep the underlying Chinese scheme available to TSF's queued language-cycle projection.
    const auto scheme = GetConfiguredInputScheme();
    const bool allowed =
        GetConfiguredRModeEnabled() && (scheme == SchemeType::Quanpin || scheme == SchemeType::Shuangpin);
    std::lock_guard lock(g_status_snapshot_mutex);
    return static_cast<FanyImeTemporaryJapanese::Policy>(
        (allowed ? FanyImeTemporaryJapanese::Allowed : 0) |
        (g_latest_english_input_mode ? FanyImeTemporaryJapanese::EnglishCandidates : 0));
}

std::wstring TemporaryJapanesePolicyPayload()
{
    return std::wstring(1, FanyImeTemporaryJapanese::ToPayload(TemporaryJapanesePolicy()));
}

void CancelCloudCandidateRequest()
{
    UpdateCloudInput("");
}

void RegisterStatusSnapshotWindow(HWND toolbar_window)
{
    std::lock_guard lock(g_status_snapshot_mutex);
    g_status_snapshot_window = toolbar_window;
    if (g_latest_status_snapshot >= 0 && g_status_snapshot_window && IsWindow(g_status_snapshot_window))
    {
        PostMessage(g_status_snapshot_window, UPDATE_FTB_STATUS, g_latest_status_snapshot, 0);
    }
    if (g_status_snapshot_window && IsWindow(g_status_snapshot_window))
    {
        PostMessage(g_status_snapshot_window, UPDATE_FTB_ENGLISH_INPUT_MODE, g_latest_english_input_mode ? 1 : 0, 0);
    }
}

bool SendCurrentDataToClient(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id)
{
    const UINT msg_type = Global::MsgTypeToTsf;
    FANY_IPC_LOGF(L"[msime]: [ipc] send-current-data: msg_type={}, text={}", msg_type,
                  ::Global::candidate_ui.selected_text);
    const ULONGLONG send_started_at_ms = GetTickCount64();
    const bool sent = SendToTsfClientViaNamedpipe(client_id, activation_epoch, msg_type, request_id,
                                                  ::Global::candidate_ui.selected_text);
    const ULONGLONG send_elapsed_ms = GetTickCount64() - send_started_at_ms;
    if (!sent || send_elapsed_ms >= 8)
    {
        DIAG_LOGF(L"[key-latency] side=server stage=reply-send request={} client={} epoch={} elapsed_ms={} sent={}",
                  request_id, client_id, activation_epoch, send_elapsed_ms, sent);
    }
    if (sent &&
        (msg_type == Global::DataFromServerMsgType::Normal ||
         msg_type == Global::DataFromServerMsgType::CommitExactText) &&
        IsPipeActivationCurrent(client_id, activation_epoch))
    {
        ClearState();
    }
    return sent;
}

namespace
{
std::wstring BuildUiLessPagePayload(const std::wstring &preedit)
{
    const std::wstring page = BuildUiLessCandidatePageW();
    ::WriteDataToSharedMemory(page, true);
    auto &ui = Global::candidate_ui;
    const int selection = ui.page_words.empty()
                              ? 0
                              : std::clamp(ui.selected_index_in_page, 0, static_cast<int>(ui.page_words.size()) - 1);
    return FanyImeUiLessPage::Format(preedit, page, selection,
                                     Global::candidate_page_generation.load(std::memory_order_acquire));
}
} // namespace

bool SendUiLessCompositionToClient(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id)
{
    std::wstring preedit;
    if (GlobalSettings::getTsfPreeditStyle() == GlobalSettings::TsfPreeditStyle::Pinyin)
    {
        preedit = GetTsfPreedit();
    }
    Global::MsgTypeToTsf = Global::DataFromServerMsgType::UiLessComposition;
    Global::candidate_ui.selected_text = BuildUiLessPagePayload(preedit);
    return SendCurrentDataToClient(client_id, activation_epoch, request_id);
}

void PushUiLessCandidatesToOwner()
{
    if (!IsUiLessMode())
    {
        return;
    }
    const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
    if (!owner || !IsPipeActivationCurrent(owner.client_id, owner.activation_epoch))
    {
        return;
    }
    // Preedit is left empty: the push only replaces the list, composition text belongs to the key path.
    SendToTsfWorkerThreadClientViaNamedpipe(owner.client_id, owner.activation_epoch,
                                            Global::DataFromServerMsgTypeToTsfWorkerThread::UiLessCandidatesChanged,
                                            BuildUiLessPagePayload(L""));
}

void ClearState()
{
    const auto r_mode_original_session = g_r_mode_original_session;
    ClearSpecialModeTriggers();
    ClearCandidateUiOwner();
    UpdateCloudInput("");
    UpdateEnglishInput("");
    g_dedicated_english_answer_pending = false;
    // The gloss cache survives the composition: the next one starts from the
    // same common words, and its first frame should already carry them.
    g_candidate_translation_signature.clear();
    g_translation_candidates_active = false;
    g_date_time_page_active = false;
    g_date_time_page_keyword.clear();
    g_translation_saved_items.clear();
    g_translation_saved_page_index = 0;
    g_translation_saved_selected_index = 0;
    g_translation_saved_selection_started = false;
    // 展开面板随组合一起结束：它挂在候选窗里，组合没了它也没有意义。
    CloseCandidateExpansionPanel();
    EnglishIme::ClearTranslations();
    CloudTranslation::Clear();
    UpdateMixedInput({});
    UpdateAiInput("");
    /* Clear dict engine state */
    g_inputSession->reset_state();
    if (r_mode_original_session)
    {
        g_inputSession = r_mode_original_session;
        g_r_mode_original_session.reset();
    }
    /* 造词的状态也要清理 */
    GlobalIme::composition.clear();
    HideCandidateWindowAndDropItems();
}
} // namespace FanyNamedPipe
