// 候选窗宿主的窗口过程：显示 / 隐藏 / 移动消息与去重、隐藏宽限、DPI 变化、状态切换转发、
// 配置同步定时器，以及候选上的鼠标动作。
#include "window/ime_windows_internal.h"
#include "global/globals.h"
#include "config/ime_config.h"
#include "conversion/chinese_converter.h"
#include "ipc/ipc.h"
#include "ime_windows.h"
#include "window/candidate_presenter.h"
#include "window/floating_toolbar_presenter.h"
#include "window/caret_fallback.h"
#include "defines/defines.h"
#include "defines/globals.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <fmt/xchar.h>
#include "utils/window_utils.h"
#include "ipc/event_listener.h"
#include "utils/ime_utils.h"
#include "log/candidate_diag_log.h"
#include "voice-input/voice_input_service.h"

using namespace ime_windows_detail;

namespace
{
// WM_DPICHANGED must remasure even when the caret has not moved.
std::atomic<bool> g_candidate_force_layout{false};
// A WM_HIDE_MAIN_WINDOW is armed on TIMER_ID_CANDIDATE_HIDE_GRACE rather than
// applied straight away; a show inside the grace window cancels it.
bool g_candidate_hide_pending = false;
// Last frame actually painted, used to drop duplicate shows. Cleared on hide so
// the next session always paints.
std::wstring g_last_rendered_candidate_signature;
ULONGLONG g_last_rendered_candidate_tick = 0;

// Server-side caret lookup for compositions whose TSF anchor stays unresolved.
// The generation changes on every hide, so a lookup finishing after its
// composition ended is dropped.
const UINT WM_CANDIDATE_CARET_FALLBACK = RegisterWindowMessageW(L"WM_CANDIDATE_CARET_FALLBACK");
uint32_t g_caret_fallback_generation = 0;
bool g_caret_fallback_armed = false;
int g_caret_fallback_attempts = 0;
bool g_caret_fallback_valid = false;
POINT g_caret_fallback_anchor{};
// The card sits on the fallback anchor, so the first real TSF anchor must still
// be placed even though position locking would otherwise ignore it.
bool g_caret_fallback_placed = false;
// Last resort when no lookup found a caret: the card is pinned to the screen
// corner. Retries still run, and a caret found later replaces the corner.
bool g_caret_fallback_pinned = false;

void ArmCaretFallback(HWND hwnd)
{
    const bool resolved = g_caret_fallback_valid && !g_caret_fallback_pinned;
    if (Global::Point[1] != Global::INVALID_Y || resolved || g_caret_fallback_armed ||
        g_caret_fallback_attempts >= kCandidateCaretFallbackMaxAttempts)
    {
        return;
    }
    g_caret_fallback_armed = true;
    SetTimer(hwnd, TIMER_ID_CANDIDATE_CARET_FALLBACK, kCandidateCaretFallbackDelayMs, nullptr);
}

void UnpinCaretFallback()
{
    if (g_caret_fallback_pinned)
    {
        g_caret_fallback_pinned = false;
        CandidatePresenter::Instance().SetPinnedCardOrigin(nullptr);
    }
}

void ResetCaretFallback(HWND hwnd)
{
    KillTimer(hwnd, TIMER_ID_CANDIDATE_CARET_FALLBACK);
    ++g_caret_fallback_generation;
    g_caret_fallback_armed = false;
    g_caret_fallback_attempts = 0;
    g_caret_fallback_valid = false;
    g_caret_fallback_placed = false;
    UnpinCaretFallback();
}

// Puts the card's top-left at (2,2) on the foreground window's monitor.
void PinCandidateToScreenCorner(HWND hwnd)
{
    if (g_caret_fallback_pinned || !::is_global_wnd_cand_shown || Global::Point[1] != Global::INVALID_Y)
    {
        return;
    }
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (!GetMonitorInfoW(MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY), &monitorInfo))
    {
        return;
    }
    const POINT corner{monitorInfo.rcMonitor.left + kCandidateCornerFallbackInsetPx,
                       monitorInfo.rcMonitor.top + kCandidateCornerFallbackInsetPx};
    g_caret_fallback_valid = true;
    g_caret_fallback_pinned = true;
    g_caret_fallback_anchor = corner;
    CandidatePresenter::Instance().SetPinnedCardOrigin(&corner);
    CAND_DIAG_LOGF(L"caret-fallback pinned to screen corner ({},{})", corner.x, corner.y);
    FineTuneWindow(hwnd);
}

void NotePlacedCandidateCaret(POINT layoutCaret)
{
    g_last_placed_caret_x = layoutCaret.x;
    g_last_placed_caret_y = layoutCaret.y;
    g_caret_fallback_placed = g_caret_fallback_valid && Global::Point[1] == Global::INVALID_Y;
}
} // namespace

namespace ime_windows_detail
{
POINT GetCandidateLayoutCaret()
{
    POINT caret{Global::Point[0], Global::Point[1]};
    if (caret.y == Global::INVALID_Y)
    {
        return g_caret_fallback_valid ? g_caret_fallback_anchor : caret;
    }
    if (g_caret_fallback_valid)
    {
        // TSF caught up; its anchor is the accurate one.
        g_caret_fallback_valid = false;
        UnpinCaretFallback();
        CAND_DIAG_LOGF(L"caret-fallback superseded by tsf anchor ({},{})", caret.x, caret.y);
    }
    if (GetConfiguredCandidateWindowFollowCursor())
    {
        return caret;
    }
    if (!g_candidate_session_anchor_valid)
    {
        g_candidate_session_anchor = caret;
        g_candidate_session_anchor_valid = true;
        CAND_DIAG_LOGF(L"candidate-position anchor-captured caret=({},{})", caret.x, caret.y);
    }
    return g_candidate_session_anchor;
}

bool IsCandidateHostPaintedVisible(HWND hwnd)
{
    if (!hwnd || !::is_global_wnd_cand_shown || Global::Point[1] == Global::INVALID_Y)
    {
        return false;
    }
    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect) || windowRect.top == Global::INVALID_Y)
    {
        return false;
    }
    return !IsHostWindowCloaked(hwnd) && MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) != nullptr;
}

bool CandidateCaretNeedsStableFollow(HWND hwnd, POINT caret)
{
    if (g_last_placed_caret_x == Global::INVALID_Y || g_last_placed_caret_y == Global::INVALID_Y ||
        g_caret_fallback_placed)
    {
        return true;
    }

    POINT previous{g_last_placed_caret_x, g_last_placed_caret_y};
    if (MonitorFromPoint(previous, MONITOR_DEFAULTTONEAREST) != MonitorFromPoint(caret, MONITOR_DEFAULTTONEAREST))
    {
        return true;
    }

    // Following every horizontal glyph advance makes the card move and
    // re-layout continuously. Keep it anchored on the current text line; a real
    // line change or vertical scroll still follows after the settle timer.
    FLOAT scale = QueryHalfScreenDipLimitsForPoint(caret).scale;
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }
    const int lineChangeThresholdPx = (std::max)(1, static_cast<int>(std::lround(12.0 * static_cast<double>(scale))));
    const int visibleLineThresholdPx =
        (std::max)(lineChangeThresholdPx, static_cast<int>(std::lround(48.0 * static_cast<double>(scale))));
    const int thresholdPx = IsCandidateHostPaintedVisible(hwnd) ? visibleLineThresholdPx : lineChangeThresholdPx;
    return std::abs(caret.y - previous.y) >= thresholdPx;
}
} // namespace ime_windows_detail

// Re-lays out the candidate window at the current caret anchor.
int FineTuneWindow(HWND /*hwnd*/)
{
    const POINT layoutCaret = GetCandidateLayoutCaret();
    if (CandidatePresenter::Instance().IsBound())
    {
        CandidatePresenter::Instance().ShowFromGlobalState(layoutCaret);
        NotePlacedCandidateCaret(layoutCaret);
    }
    return 0;
}

LRESULT CALLBACK WndProcCandWindow(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_POWERBROADCAST || message == WM_DISPLAYCHANGE || message == WM_DWMCOMPOSITIONCHANGED ||
        message == WM_SETTINGCHANGE)
    {
        CAND_DIAG_LOGF(L"system window event message={:#x} wparam={:#x} lparam={:#x} {}", message,
                       static_cast<unsigned long long>(wParam), static_cast<unsigned long long>(lParam),
                       DescribeCandidateHostState());
    }

    if (IsSystemLightDarkToggle(message, lParam))
    {
        // A visible candidate window must flip immediately; a hidden one
        // re-resolves its theme on the next ShowFromGlobalState via the
        // ApplySkin fingerprint.
        if (::is_global_wnd_cand_shown && CandidatePresenter::Instance().IsBound())
        {
            CandidatePresenter::Instance().ShowFromGlobalState(GetCandidateLayoutCaret());
        }
    }

    if (message == WM_IMEACTIVATE)
    {
        g_is_ime_active = true;
        ApplyConfiguredFloatingToolbarVisibility(L"wm-imeactivate");
        return 0;
    }

    if (message == WM_IMEDEACTIVATE)
    {
        g_is_ime_active = false;
        ApplyConfiguredFloatingToolbarVisibility(L"wm-imedeactivate");
        return 0;
    }

    if (message == WM_SHOW_MAIN_WINDOW)
    {
        g_candidate_show_msg_pending.store(false);
        // A show arriving while a hide is still inside its grace window means the
        // hide belonged to an earlier keystroke of the same burst. Drop it, so the
        // window stays up and the user never sees the blink.
        if (g_candidate_hide_pending)
        {
            KillTimer(hwnd, TIMER_ID_CANDIDATE_HIDE_GRACE);
            g_candidate_hide_pending = false;
            CAND_DIAG_LOGF(L"hide cancelled by show within grace");
        }
        // This handler always renders the newest published state, never a payload
        // captured when the message was posted, so an older queued show can only
        // repaint what the newer one is about to repaint. Under load the worker
        // drains a backlog and posts several at once; painting every one of them
        // is what turns a queue stall into visible strobing.
        MSG supersedingShow{};
        if (PeekMessage(&supersedingShow, hwnd, WM_SHOW_MAIN_WINDOW, WM_SHOW_MAIN_WINDOW, PM_NOREMOVE))
        {
            CAND_DIAG_LOGF(L"candidate-frame path=superseded");
            return 0;
        }
        // Checked here rather than when the show is requested: a game can enter exclusive fullscreen
        // after the IME activated, and a single frame over it is enough to throw it out of the mode.
        // Hiding also clears the rendered-visible flag, so digit/space selection does not wait for a
        // paint that will never come.
        if (IsForegroundD3DExclusiveFullscreen())
        {
            CAND_DIAG_LOGF(L"candidate-frame path=exclusive-fullscreen shown={}", ::is_global_wnd_cand_shown);
            if (::is_global_wnd_cand_shown)
            {
                return WndProcCandWindow(hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
            }
            Global::SetCandidateWindowRenderedVisible(false);
            return 0;
        }
        const ULONGLONG updateStartedTick = GetTickCount64();
        ::ReadDataFromSharedMemory(0b1000000);
        // The worker does not wait for this posted message before rebuilding its candidate state, so render the page it
        // published rather than the live one.
        const Global::CandidatePageSnapshotPtr candidatePage = Global::LoadCandidatePageSnapshot();
        CAND_DIAG_LOGF(L"candidate-frame show tick={} preedit_units={} payload_units={} "
                       L"caret=({},{}) follow_cursor={} anchor_valid={} {}",
                       updateStartedTick,
                       GetConfiguredCandidateWindowPreeditStyle() == "empty" ? 0 : GetPreeditWithCaretMarker().size(),
                       candidatePage->candidate_string.size(), Global::Point[0], Global::Point[1],
                       GetConfiguredCandidateWindowFollowCursor(), g_candidate_session_anchor_valid,
                       DescribeCandidateHostState());
        EnsureSmallWindowsTopmost(L"show-candidate");
        const POINT layoutCaret = GetCandidateLayoutCaret();
        ArmCaretFallback(hwnd);
        const bool showPreedit = GetConfiguredCandidateWindowPreeditStyle() != "empty";
        const std::wstring preedit = GetPreeditWithCaretMarker();
        // Suppress a repaint that would reproduce the frame already on screen.
        // g_candidate_force_layout marks the cases (DPI / display change) where
        // the same content must still be re-laid out, so never dedup through it.
        const std::wstring signaturePreedit = showPreedit ? preedit : std::wstring{};
        const CandidateFramePageState pageState{candidatePage->selected_index_in_page, candidatePage->page_index,
                                                candidatePage->has_previous_page, candidatePage->has_next_page};
        const std::wstring frameSignature = CandidateFrameSignature(signaturePreedit, candidatePage->page_views,
                                                                    layoutCaret.x, layoutCaret.y, pageState);
        if (::is_global_wnd_cand_shown && !g_candidate_force_layout.load() &&
            frameSignature == g_last_rendered_candidate_signature &&
            updateStartedTick - g_last_rendered_candidate_tick < kCandidateShowDedupWindowMs)
        {
            // The frame already on screen is identical to the page just loaded, so the render echo is
            // honest even though nothing was repainted. Without it a digit/space selection would wait
            // out the full timeout on content that is already visible.
            Global::PublishRenderedCandidatePageGeneration(candidatePage->generation);
            CAND_DIAG_LOGF(L"candidate-frame path=dedup page_gen={}", candidatePage->generation);
            return 0;
        }
        g_last_rendered_candidate_signature = frameSignature;
        g_last_rendered_candidate_tick = updateStartedTick;
        if (CandidatePresenter::Instance().IsBound())
        {
            CandidatePresenter::Instance().ShowFromGlobalState(layoutCaret);
            NotePlacedCandidateCaret(layoutCaret);
            CAND_DIAG_LOGF(L"candidate-frame path=d2d elapsed_ms={}", GetTickCount64() - updateStartedTick);
        }
        return 0;
    }

    if (message == WM_HIDE_MAIN_WINDOW || (message == WM_TIMER && wParam == TIMER_ID_CANDIDATE_HIDE_GRACE))
    {
        // wParam != 0 marks a hide the worker delivered late, i.e. one that may
        // already be superseded by keystrokes queued behind it. Only those get the
        // grace; a hide arriving on time is a real commit or focus loss and is
        // applied straight away so typing keeps its snap.
        if (message == WM_HIDE_MAIN_WINDOW && wParam != 0)
        {
            // Repeat hides while one is already armed need no extra work — that is
            // where the redundant back-to-back hides get absorbed.
            if (!g_candidate_hide_pending)
            {
                g_candidate_hide_pending = true;
                SetTimer(hwnd, TIMER_ID_CANDIDATE_HIDE_GRACE, kCandidateHideGraceMs, nullptr);
            }
            CAND_DIAG_LOGF(L"hide message deferred grace_ms={} {}", kCandidateHideGraceMs,
                           DescribeCandidateHostState());
            return 0;
        }
        KillTimer(hwnd, TIMER_ID_CANDIDATE_HIDE_GRACE);
        g_candidate_hide_pending = false;
        CAND_DIAG_LOGF(L"hide message begin {}", DescribeCandidateHostState());
        ::is_global_wnd_cand_shown = false;
        Global::SetCandidateWindowRenderedVisible(false);
        g_last_rendered_candidate_signature.clear();
        KillTimer(hwnd, TIMER_ID_CANDIDATE_MOVE_SETTLE);
        g_candidate_session_anchor_valid = false;
        ResetCaretFallback(hwnd);
        ResetCandidatePlacementMemory();
        {
            SuppressCandidateDpiChange suppressDpi;
            if (CandidatePresenter::Instance().IsBound())
            {
                CandidatePresenter::Instance().Hide();
            }
            else
            {
                SetHostWindowCloaked(hwnd, true);
            }
        }
        CAND_DIAG_LOGF(L"hide message end {}", DescribeCandidateHostState());
        return 0;
    }

    if (message == WM_MOVE_CANDIDATE_WINDOW)
    {
        g_candidate_move_msg_pending.store(false);
        CAND_DIAG_LOGF(L"move message caret=({},{}) {}", Global::Point[0], Global::Point[1],
                       DescribeCandidateHostState());
        if (!::is_global_wnd_cand_shown)
        {
            return 0;
        }
        const bool forceLayout = g_candidate_force_layout.exchange(false);
        const POINT layoutCaret = GetCandidateLayoutCaret();
        ArmCaretFallback(hwnd);
        const bool sameCaret = g_last_placed_caret_x == layoutCaret.x && g_last_placed_caret_y == layoutCaret.y;
        // Position locking applies only after the first usable anchor has been
        // placed. A deferred show still needs its first valid MoveCandidate, and
        // so does a card that is only standing on the fallback anchor.
        const bool awaitingInitialPlacement = g_last_placed_caret_y == Global::INVALID_Y || g_caret_fallback_placed;
        if (!forceLayout && !GetConfiguredCandidateWindowFollowCursor() && !awaitingInitialPlacement)
        {
            CAND_DIAG_LOGF(L"candidate-position move-ignored locked_anchor=({},{}) reported=({},{})", layoutCaret.x,
                           layoutCaret.y, Global::Point[0], Global::Point[1]);
            return 0;
        }
        if (!forceLayout && !sameCaret && !CandidateCaretNeedsStableFollow(hwnd, layoutCaret))
        {
            CAND_DIAG_LOGF(L"candidate-position same-line-follow-suppressed anchor=({},{}) reported=({},{})",
                           g_last_placed_caret_x, g_last_placed_caret_y, layoutCaret.x, layoutCaret.y);
            return 0;
        }
        if (!forceLayout && sameCaret && IsCandidateHostPaintedVisible(hwnd))
        {
            CAND_DIAG_LOGF(L"move skipped same caret visible=true");
            return 0;
        }
        if (forceLayout)
        {
            KillTimer(hwnd, TIMER_ID_CANDIDATE_MOVE_SETTLE);
            FineTuneWindow(hwnd);
            return 0;
        }
        // Some hosts animate the composition rectangle and report a new caret
        // every few milliseconds. Lay out only the latest settled anchor.
        KillTimer(hwnd, TIMER_ID_CANDIDATE_MOVE_SETTLE);
        SetTimer(hwnd, TIMER_ID_CANDIDATE_MOVE_SETTLE, kCandidateMoveSettleMs, nullptr);
        return 0;
    }

    if (message == WM_DPICHANGED)
    {
        // Never apply the suggested rect or re-layout while we are inside our own
        // SetWindowPos: that path is what delivers this message synchronously on
        // focus-switch hide, and re-entering stalls the server UI thread.
        if (g_candidate_dpi_change_suppress_count > 0)
        {
            return 0;
        }

        // Visible candidate: defer the re-layout via the move message so it never
        // nests inside this handler.
        if (::is_global_wnd_cand_shown && Global::Point[1] != Global::INVALID_Y)
        {
            g_candidate_force_layout.store(true);
            PostMessage(hwnd, WM_MOVE_CANDIDATE_WINDOW, 0, 0);
        }
        // Hidden: keep the current host size. DefWindowProc must not run —
        // returning 0 without SetWindowPos is intentional.
        return 0;
    }

    if (message == WM_IMESWITCH)
    {
        const int cn = wParam != 0 ? 1 : 0;
        const std::string &punctuation_lock = GetConfiguredPunctuationLock();
        const int punc = punctuation_lock == "chinese" ? 1 : punctuation_lock == "english" ? 0 : cn;
        UpdateFtbCnEnAndPuncState(cn, punc);
        return 0;
    }

    if (message == WM_PUNCSWITCH)
    {
        if (wParam == 0) // 此时是中文标点状态
        {
            /* 更新 floating toolbar 的标点全角和半角状态为全角 */
            UpdateFtbPuncState(0);
        }
        else // 此时是英文标点状态
        {
            /* 更新 floating toolbar 的标点全角和半角状态为半角 */
            UpdateFtbPuncState(1);
        }
        return 0;
    }

    if (message == WM_DOUBLESINGLEBYTESWITCH)
    {
        if (wParam == 0) // 此时是半角状态
        {
            /* 更新 floating toolbar 的全角和半角状态为半角 */
            UpdateFtbDoubleSingleByteState(0);
        }
        else // 此时是全角状态
        {
            /* 更新 floating toolbar 的全角和半角状态为全角 */
            UpdateFtbDoubleSingleByteState(1);
        }
        return 0;
    }

    if (message == WM_REFRESH_CHARACTER_SET)
    {
        UpdateFtbCharacterSetState();
        FloatingToolbarPresenter::Instance().ApplyTheme();
        FanyNamedPipe::EnqueueRefreshCandidatePageTask();
        return 0;
    }

    if (message == WM_APPLY_IME_CONFIG)
    {
        if (wParam != 0)
        {
            ForceReloadConfiguredCandidateSkin();
            if (::is_global_wnd_cand_shown && CandidatePresenter::Instance().IsBound())
            {
                CandidatePresenter::Instance().ShowFromGlobalState(GetCandidateLayoutCaret());
            }
            return 0;
        }
        InvalidateImeConfigWriteTime();
        PostMessage(hwnd, WM_TIMER, TIMER_ID_CONFIG_SYNC, 0);
        return 0;
    }

    if (message == WM_APPLY_IME_INPUT_SCHEME)
    {
        InvalidateImeConfigWriteTime();
        ReloadImeConfigIfChanged();
        ApplyConfiguredInputScheme();
        return 0;
    }

    if (WM_CANDIDATE_CARET_FALLBACK != 0 && message == WM_CANDIDATE_CARET_FALLBACK)
    {
        CaretFallback::Result result;
        if (static_cast<uint32_t>(wParam) != g_caret_fallback_generation ||
            !CaretFallback::TakeResult(g_caret_fallback_generation, &result))
        {
            CAND_DIAG_LOGF(L"caret-fallback stale result generation={} current={}", static_cast<uint32_t>(wParam),
                           g_caret_fallback_generation);
            return 0;
        }
        CAND_DIAG_LOGF(L"caret-fallback result source={} anchor=({},{}) elapsed_ms={} attempt={} tsf=({},{}) trace:{}",
                       CaretFallback::SourceName(result.source), result.anchor.x, result.anchor.y, result.elapsedMs,
                       g_caret_fallback_attempts, Global::Point[0], Global::Point[1], result.trace);
        if (result.source == CaretFallback::Source::None)
        {
            // Let a later keystroke try again, up to the per-composition cap;
            // meanwhile show the card in the screen corner rather than not at all.
            g_caret_fallback_armed = false;
            PinCandidateToScreenCorner(hwnd);
            return 0;
        }
        if (!::is_global_wnd_cand_shown || Global::Point[1] != Global::INVALID_Y)
        {
            return 0;
        }
        UnpinCaretFallback();
        g_caret_fallback_valid = true;
        g_caret_fallback_anchor = result.anchor;
        FineTuneWindow(hwnd);
        return 0;
    }

    if (CandidatePresenter::Instance().HandleMessage(message, wParam, lParam))
    {
        return message == WM_ERASEBKGND ? 1 : 0;
    }

    switch (message)
    {
    case WM_TIMER: {
        if (wParam == TIMER_ID_CANDIDATE_CARET_FALLBACK)
        {
            KillTimer(hwnd, TIMER_ID_CANDIDATE_CARET_FALLBACK);
            if (!::is_global_wnd_cand_shown || Global::Point[1] != Global::INVALID_Y)
            {
                // TSF delivered an anchor in time; a later unresolved one may arm again.
                g_caret_fallback_armed = false;
                break;
            }
            const DWORD clientProcessId = static_cast<DWORD>(GetActivePipeClient().client_id >> 32);
            if (clientProcessId == 0)
            {
                CAND_DIAG_LOGF(L"caret-fallback skipped: no active client");
                PinCandidateToScreenCorner(hwnd);
                break;
            }
            ++g_caret_fallback_attempts;
            CAND_DIAG_LOGF(L"caret-fallback request generation={} attempt={} client_pid={}",
                           g_caret_fallback_generation, g_caret_fallback_attempts, clientProcessId);
            CaretFallback::Request(hwnd, WM_CANDIDATE_CARET_FALLBACK, g_caret_fallback_generation, clientProcessId);
            break;
        }
        if (wParam == TIMER_ID_CANDIDATE_MOVE_SETTLE)
        {
            KillTimer(hwnd, TIMER_ID_CANDIDATE_MOVE_SETTLE);
            if (!::is_global_wnd_cand_shown)
            {
                break;
            }
            const POINT layoutCaret = GetCandidateLayoutCaret();
            const bool sameCaret = g_last_placed_caret_x == layoutCaret.x && g_last_placed_caret_y == layoutCaret.y;
            if (!sameCaret && CandidateCaretNeedsStableFollow(hwnd, layoutCaret))
            {
                CAND_DIAG_LOGF(L"candidate-layout settled-move last=({},{}) caret=({},{})", g_last_placed_caret_x,
                               g_last_placed_caret_y, layoutCaret.x, layoutCaret.y);
                FineTuneWindow(hwnd);
            }
            break;
        }
        if (wParam == TIMER_ID_CONFIG_SYNC)
        {
            const SchemeType previous_input_scheme = GetConfiguredActiveInputScheme();
            const bool previous_trilingual_cycle = GetConfiguredTrilingualCycleEnabled();
            const std::string previous_shuangpin_schema = GetConfiguredShuangpinSchema();
            const std::string previous_character_set = GetConfiguredCharacterSet();
            const bool previous_translation_show_pos = GetConfiguredCandidateTranslationShowPos();
            const bool previous_translation_single_sense = GetConfiguredCandidateTranslationSingleSense();
            const std::string previous_layout = GetConfiguredCandidateWindowLayout();
            const std::string previous_candidate_skin = GetConfiguredCandidateSkin();
            const bool previous_fit_content = GetConfiguredCandidateWindowFitContent();
            const bool previous_single_line = GetConfiguredCandidateWindowSingleLine();
            const bool previous_floating_toolbar = GetConfiguredFloatingToolbarEnabled();
            const bool previous_caret_state_indicator = GetConfiguredCaretStateIndicatorEnabled();
            const FloatingToolbarItemsConfig previous_floating_toolbar_items = GetConfiguredFloatingToolbarItems();
            const double previous_floating_toolbar_scale = GetConfiguredFloatingToolbarScale();
            const int previous_floating_toolbar_font_size = GetConfiguredFloatingToolbarFontSize();
            const bool previous_floating_toolbar_auto_hide = GetConfiguredFloatingToolbarAutoHide();
            const int previous_floating_toolbar_auto_hide_delay = GetConfiguredFloatingToolbarAutoHideDelay();
            const bool previous_floating_toolbar_shadow = GetConfiguredFloatingToolbarShadow();
            const bool previous_cloud_candidates = GetConfiguredCloudCandidatesEnabled();
            const bool previous_comma_period = GetConfiguredPagingCommaPeriodEnabled();
            const bool previous_smart_punctuation = GetConfiguredSmartPunctuationEnabled();
            const bool previous_smart_punctuation_space_convert = GetConfiguredSmartPunctuationSpaceConvertEnabled();
            const bool previous_smart_punctuation_direct_digit = GetConfiguredSmartPunctuationDirectDigitEnabled();
            const bool previous_smart_punctuation_direct_letter = GetConfiguredSmartPunctuationDirectLetterEnabled();
            const bool previous_smart_punctuation_repeat_to_chinese =
                GetConfiguredSmartPunctuationRepeatToChineseEnabled();
            const bool previous_paired_punctuation = GetConfiguredPairedPunctuationEnabled();
            const std::wstring previous_enumeration_comma = FormatEnumerationCommaWorkerPayload();
            const std::wstring previous_horizontal_arrow_selection = FormatHorizontalArrowSelectionWorkerPayload();
            const std::string previous_punctuation_lock = GetConfiguredPunctuationLock();
            const bool previous_tsf_diagnostic_log = GetConfiguredTsfDiagnosticLogEnabled();
            const bool previous_statistics_enabled = GetConfiguredStatisticsEnabled();
            const bool previous_ai_assistant_enabled = GetConfiguredAiAssistant().enabled;
            const std::wstring previous_mid_sentence_helpcode = FormatMidSentenceHelpcodeWorkerPayload();
            const std::wstring previous_mid_sentence_helpcode_semicolon =
                FormatMidSentenceHelpcodeSemicolonWorkerPayload();
            const std::wstring previous_direct_helpcode = FormatDirectHelpcodeWorkerPayload();
            const std::wstring previous_direct_helpcode_end_trigger = FormatDirectHelpcodeEndTriggerWorkerPayload();
            const std::wstring previous_v_mode = FormatVModeWorkerPayload();
            const std::wstring previous_mid_sentence_helpcode_uppercase =
                FormatMidSentenceHelpcodeUppercaseWorkerPayload();
            const std::wstring previous_end_helpcode_triggers = FormatEndHelpcodeTriggersWorkerPayload();
            const std::wstring previous_paging_worker_payload = FormatPagingCommaPeriodWorkerPayload();
            const std::string previous_theme_mode = GetConfiguredThemeMode();
            const std::string previous_theme_cand = GetConfiguredThemeCand();
            const std::string previous_theme_ftb = GetConfiguredThemeFtb();
            const std::string previous_theme_menu = GetConfiguredThemeMenu();
            const std::string previous_font = GetConfiguredCandidateFont();
            const std::string previous_english_font = GetConfiguredCandidateEnglishFont();
            const std::string previous_default_font = GetConfiguredCandidateDefaultFont();
            const int previous_font_size = GetConfiguredCandidateFontSize();
            const int previous_preedit_font_size = GetConfiguredCandidateWindowPreeditFontSize();
            const std::string previous_cand_text_color = GetConfiguredCandidateTextColor();
            const auto previous_temporary_japanese_policy = FanyNamedPipe::TemporaryJapanesePolicy();
            const VoiceInputConfig previous_voice_input = GetConfiguredVoiceInput();
            if (ReloadImeConfigIfChanged())
            {
                // 下面入队的任务会按新字形换写法，切到繁体时先在后台载入转换字典。
                if (previous_character_set != GetConfiguredCharacterSet() &&
                    GetConfiguredCharacterSet() == "traditional")
                    ChineseConverter::PreloadInBackground();
                FanyNamedPipe::EnqueueApplyCandidatePageSizeTask();
                // ApplyConfiguredInputScheme broadcasts the policy with the rest of the mode state.
                if (previous_input_scheme != GetConfiguredActiveInputScheme())
                    ApplyConfiguredInputScheme();
                else if (previous_shuangpin_schema != GetConfiguredShuangpinSchema())
                    ApplyConfiguredShuangpinSchema();
                if (previous_input_scheme == GetConfiguredActiveInputScheme() &&
                    previous_temporary_japanese_policy != FanyNamedPipe::TemporaryJapanesePolicy())
                    BroadcastToTsfWorkerThreadViaNamedpipe(FanyImeWorkerReplyType::TemporaryJapanesePolicyChanged,
                                                           FanyNamedPipe::TemporaryJapanesePolicyPayload());
                if (previous_input_scheme == GetConfiguredActiveInputScheme() &&
                    previous_trilingual_cycle != GetConfiguredTrilingualCycleEnabled())
                    BroadcastTrilingualCycleState();
                if (previous_character_set != GetConfiguredCharacterSet())
                {
                    UpdateFtbCharacterSetState();
                }
                if (previous_character_set != GetConfiguredCharacterSet() ||
                    previous_translation_show_pos != GetConfiguredCandidateTranslationShowPos() ||
                    previous_translation_single_sense != GetConfiguredCandidateTranslationSingleSense())
                {
                    FanyNamedPipe::EnqueueRefreshCandidatePageTask();
                }
                // 候选窗由皮肤指纹在下一次显示时重新套用；常驻的工具栏跟着候选窗皮肤走，要立刻刷新。
                // 外部皮肤可能只声明了部分布局，换布局后皮肤是否生效会变，所以布局变了也要刷新。
                if (previous_layout != GetConfiguredCandidateWindowLayout() ||
                    previous_fit_content != GetConfiguredCandidateWindowFitContent())
                    ApplyConfiguredUiThemes();
                if (previous_candidate_skin != GetConfiguredCandidateSkin() ||
                    previous_theme_mode != GetConfiguredThemeMode() ||
                    previous_theme_cand != GetConfiguredThemeCand() || previous_theme_ftb != GetConfiguredThemeFtb() ||
                    previous_theme_menu != GetConfiguredThemeMenu())
                {
                    ApplyConfiguredUiThemes();
                }
                if (::is_global_wnd_cand_shown &&
                    (previous_layout != GetConfiguredCandidateWindowLayout() ||
                     previous_fit_content != GetConfiguredCandidateWindowFitContent() ||
                     previous_single_line != GetConfiguredCandidateWindowSingleLine() ||
                     previous_candidate_skin != GetConfiguredCandidateSkin() ||
                     previous_theme_mode != GetConfiguredThemeMode() ||
                     previous_theme_cand != GetConfiguredThemeCand() || previous_font != GetConfiguredCandidateFont() ||
                     previous_font_size != GetConfiguredCandidateFontSize() ||
                     previous_preedit_font_size != GetConfiguredCandidateWindowPreeditFontSize() ||
                     previous_cand_text_color != GetConfiguredCandidateTextColor()))
                {
                    if (CandidatePresenter::Instance().IsBound())
                    {
                        CandidatePresenter::Instance().ShowFromGlobalState(GetCandidateLayoutCaret());
                    }
                }
                if (previous_floating_toolbar != GetConfiguredFloatingToolbarEnabled())
                {
                    RestartFloatingToolbarAutoHide(L"config-sync");
                    SyncMenuFloatingToolbarToggle();
                }
                else if (previous_floating_toolbar_auto_hide != GetConfiguredFloatingToolbarAutoHide() ||
                         previous_floating_toolbar_auto_hide_delay != GetConfiguredFloatingToolbarAutoHideDelay())
                {
                    RestartFloatingToolbarAutoHide(L"config-sync-auto-hide");
                }
                if (previous_caret_state_indicator != GetConfiguredCaretStateIndicatorEnabled() &&
                    !GetConfiguredCaretStateIndicatorEnabled() && ::global_hwnd_caret_state)
                {
                    PostMessage(::global_hwnd_caret_state, WM_HIDE_CARET_STATE, 0, 0);
                }
                if (!FloatingToolbarItemsEqual(previous_floating_toolbar_items, GetConfiguredFloatingToolbarItems()))
                {
                    ApplyConfiguredFloatingToolbarItems();
                }
                else if (std::fabs(previous_floating_toolbar_scale - GetConfiguredFloatingToolbarScale()) > 0.001 ||
                         previous_floating_toolbar_font_size != GetConfiguredFloatingToolbarFontSize() ||
                         previous_floating_toolbar_shadow != GetConfiguredFloatingToolbarShadow())
                {
                    ApplyConfiguredFloatingToolbarSize();
                }
                if (previous_cloud_candidates && !GetConfiguredCloudCandidatesEnabled())
                    FanyNamedPipe::CancelCloudCandidateRequest();
                if (previous_comma_period != GetConfiguredPagingCommaPeriodEnabled() ||
                    previous_paging_worker_payload != FormatPagingCommaPeriodWorkerPayload())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::PagingCommaPeriodChanged,
                        FormatPagingCommaPeriodWorkerPayload());
                }
                if (previous_smart_punctuation != GetConfiguredSmartPunctuationEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationChanged,
                        GetConfiguredSmartPunctuationEnabled() ? L"1" : L"0");
                }
                if (previous_smart_punctuation_space_convert != GetConfiguredSmartPunctuationSpaceConvertEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationSpaceConvertChanged,
                        GetConfiguredSmartPunctuationSpaceConvertEnabled() ? L"1" : L"0");
                }
                if (previous_smart_punctuation_direct_digit != GetConfiguredSmartPunctuationDirectDigitEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationDirectDigitChanged,
                        GetConfiguredSmartPunctuationDirectDigitEnabled() ? L"1" : L"0");
                }
                if (previous_smart_punctuation_direct_letter != GetConfiguredSmartPunctuationDirectLetterEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationDirectLetterChanged,
                        GetConfiguredSmartPunctuationDirectLetterEnabled() ? L"1" : L"0");
                }
                if (previous_smart_punctuation_repeat_to_chinese !=
                    GetConfiguredSmartPunctuationRepeatToChineseEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationRepeatToChineseChanged,
                        GetConfiguredSmartPunctuationRepeatToChineseEnabled() ? L"1" : L"0");
                }
                if (previous_paired_punctuation != GetConfiguredPairedPunctuationEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::PairedPunctuationChanged,
                        GetConfiguredPairedPunctuationEnabled() ? L"1" : L"0");
                }
                if (previous_enumeration_comma != FormatEnumerationCommaWorkerPayload())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::EnumerationCommaKeysChanged,
                        FormatEnumerationCommaWorkerPayload());
                }
                // 开关和候选窗布局任一变化都可能改变生效状态。
                if (previous_horizontal_arrow_selection != FormatHorizontalArrowSelectionWorkerPayload())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::HorizontalArrowSelectionChanged,
                        FormatHorizontalArrowSelectionWorkerPayload());
                }
                if (previous_punctuation_lock != GetConfiguredPunctuationLock())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::PunctuationLockChanged,
                        FormatPunctuationLockWorkerPayload());
                    const std::string &punctuation_lock = GetConfiguredPunctuationLock();
                    if (punctuation_lock == "chinese")
                    {
                        UpdateFtbPuncState(1);
                    }
                    else if (punctuation_lock == "english")
                    {
                        UpdateFtbPuncState(0);
                    }
                }
                if (previous_tsf_diagnostic_log != GetConfiguredTsfDiagnosticLogEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::TsfDiagnosticLogChanged,
                        GetConfiguredTsfDiagnosticLogEnabled() ? L"1" : L"0");
                }
                if (previous_statistics_enabled != GetConfiguredStatisticsEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::StatisticsEnabledChanged,
                        GetConfiguredStatisticsEnabled() ? L"1" : L"0");
                }
                if (previous_ai_assistant_enabled != GetConfiguredAiAssistant().enabled)
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::AiContextUploadChanged,
                        GetConfiguredAiAssistant().enabled ? L"1" : L"0");
                }
                if (previous_mid_sentence_helpcode != FormatMidSentenceHelpcodeWorkerPayload())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::MidSentenceHelpcodeChanged,
                        FormatMidSentenceHelpcodeWorkerPayload());
                }
                if (previous_mid_sentence_helpcode_semicolon != FormatMidSentenceHelpcodeSemicolonWorkerPayload())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::MidSentenceHelpcodeSemicolonChanged,
                        FormatMidSentenceHelpcodeSemicolonWorkerPayload());
                }
                if (previous_direct_helpcode != FormatDirectHelpcodeWorkerPayload())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::DirectHelpcodeChanged,
                        FormatDirectHelpcodeWorkerPayload());
                }
                if (previous_direct_helpcode_end_trigger != FormatDirectHelpcodeEndTriggerWorkerPayload())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::DirectHelpcodeEndTriggerChanged,
                        FormatDirectHelpcodeEndTriggerWorkerPayload());
                }
                if (previous_v_mode != FormatVModeWorkerPayload())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(Global::DataFromServerMsgTypeToTsfWorkerThread::VModeChanged,
                                                           FormatVModeWorkerPayload());
                }
                if (previous_mid_sentence_helpcode_uppercase != FormatMidSentenceHelpcodeUppercaseWorkerPayload())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::MidSentenceHelpcodeUppercaseChanged,
                        FormatMidSentenceHelpcodeUppercaseWorkerPayload());
                }
                if (previous_end_helpcode_triggers != FormatEndHelpcodeTriggersWorkerPayload())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::EndHelpcodeTriggersChanged,
                        FormatEndHelpcodeTriggersWorkerPayload());
                }
                const VoiceInputConfig &voice_input = GetConfiguredVoiceInput();
                if (previous_voice_input.enabled != voice_input.enabled ||
                    previous_voice_input.hotkey_ralt != voice_input.hotkey_ralt ||
                    previous_voice_input.hotkey_ctrl_f9 != voice_input.hotkey_ctrl_f9 ||
                    previous_voice_input.hotkey_ctrl_win != voice_input.hotkey_ctrl_win ||
                    previous_voice_input.hotkey_rctrl_ralt != voice_input.hotkey_rctrl_ralt)
                {
                    VoiceInput::RefreshKeyboardHook();
                }
            }
            FanyNamedPipe::EnqueueEnsureInputSessionMatchesConfigTask();
        }
        break;
    }

    case WM_MOUSEACTIVATE:
        // Stop the window from being activated by mouse click
        return MA_NOACTIVATE;

    case WM_ACTIVATE: {
        if (LOWORD(wParam) != WA_INACTIVE)
        {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        break;
    }

    /* Clear dictionary buffer cache */
    case WM_CLS_DICT_CACHE: {
        FanyNamedPipe::EnqueueResetInputSessionCacheTask();
#ifdef FANY_DEBUG
        (void)0;
#endif
        break;
    }

    case WM_COMMIT_CANDIDATE: {
        int one_based = static_cast<int>(wParam);
#ifdef FANY_DEBUG
        (void)0;
#endif
        FanyNamedPipe::EnqueueCandidateUiAction(FanyNamedPipe::CandidateUiAction::Commit, one_based);

        break;
    }

    case WM_PIN_TO_TOP_CANDIDATE: {
        int one_based = static_cast<int>(wParam);
#ifdef FANY_DEBUG
        (void)0;
#endif
        FanyNamedPipe::EnqueueCandidateUiAction(FanyNamedPipe::CandidateUiAction::Pin, one_based);

        break;
    }

    case WM_DELETE_CANDIDATE: {
        int one_based = static_cast<int>(wParam);
#ifdef FANY_DEBUG
        (void)0;
#endif
        FanyNamedPipe::EnqueueCandidateUiAction(FanyNamedPipe::CandidateUiAction::Delete, one_based);

        break;
    }
    case WM_FIX_CANDIDATE_POSITION:
        FanyNamedPipe::EnqueueCandidateUiAction(FanyNamedPipe::CandidateUiAction::FixPosition, static_cast<int>(wParam),
                                                static_cast<int>(lParam));
        break;
    case WM_CLEAR_CANDIDATE_POSITION:
        FanyNamedPipe::EnqueueCandidateUiAction(FanyNamedPipe::CandidateUiAction::ClearPosition,
                                                static_cast<int>(wParam));
        break;

    case WM_PAGE_CANDIDATE:
        // The presenter funnels wheel paging through here, so this is the one
        // place the setting has to be honoured.
        if (!GetConfiguredPagingMouseWheelEnabled())
        {
            break;
        }
        FanyNamedPipe::EnqueueCandidateUiPaging(wParam == CANDIDATE_PAGE_NEXT
                                                    ? FanyNamedPipe::CandidateUiAction::PageDown
                                                    : FanyNamedPipe::CandidateUiAction::PageUp,
                                                static_cast<int>(lParam));
        break;

    case WM_PAGE_CANDIDATE_ARROW:
        // 翻页箭头是皮肤显式放出来的按钮，点了就翻，与滚轮开关无关。
        FanyNamedPipe::EnqueueCandidateUiPaging(wParam == CANDIDATE_PAGE_NEXT
                                                    ? FanyNamedPipe::CandidateUiAction::PageDown
                                                    : FanyNamedPipe::CandidateUiAction::PageUp,
                                                static_cast<int>(lParam));
        break;

    case WM_COMMIT_CANDIDATE_AT: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring *>(lParam));
        if (text)
        {
            FanyNamedPipe::EnqueueCandidateUiCommitAt(static_cast<int>(wParam), std::move(*text));
        }
        break;
    }

    case WM_EXPAND_CANDIDATES:
        FanyNamedPipe::EnqueueCandidateUiExpand();
        break;

    case WM_CLEAR_IME_ENGINE_CACHE: {
#ifdef FANY_DEBUG
        (void)0;
#endif
        /* 清除候选词缓存 */
        FanyNamedPipe::EnqueueResetInputSessionCacheTask();
        break;
    }

    case WM_DESTROY: {
        PostQuitMessage(0);
        break;
    }
    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }

    return 0;
}
