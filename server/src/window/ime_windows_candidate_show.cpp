// WebView2 候选窗的显示流程：贴近光标放置宿主、内容更新后按实际绘制尺寸重新锚定与裁剪、
// 槽位测量扩展裁剪，以及两遍测量的 FineTuneWindow。
#include "window/ime_windows_internal.h"
#include "global/globals.h"
#include "config/ime_config.h"
#include "ipc/ipc.h"
#include "window/candidate_presenter.h"
#include "defines/globals.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include "webview2/windows_webview2.h"
#include "utils/common_utils.h"
#include "utils/webview_utils.h"
#include "utils/window_utils.h"
#include "utils/ime_utils.h"
#include "log/candidate_diag_log.h"

using namespace ime_windows_detail;

namespace
{
void EndCandidateLayoutIfCurrent(uint64_t generation)
{
    if (generation == g_candidate_finetune_generation.load())
    {
        g_candidate_layout_inflight.store(false);
    }
}
} // namespace

namespace ime_windows_detail
{
void RefreshCandidateClipAfterPaint(HWND hwnd, uint64_t contentGeneration, ULONGLONG updateStartedTick)
{
    if (!hwnd || !::is_global_wnd_cand_shown || !webviewCandWnd ||
        contentGeneration != g_candidate_content_generation.load())
    {
        // This return used to be silent, which is why a region frozen across every
        // content-only update never showed up in the trace: clip-measure-drop only
        // covers the post-measure guard, so the whole refresh vanished without a
        // line. Never let the clip path fail quietly again.
        CAND_DIAG_LOGF(L"candidate-frame clip-refresh-skip content_gen={} current_gen={} shown={} webview={} hwnd={}",
                       contentGeneration, g_candidate_content_generation.load(), ::is_global_wnd_cand_shown,
                       webviewCandWnd != nullptr, hwnd != nullptr);
        return;
    }
    const HalfScreenDipLimits limits = QueryWebViewHalfScreenDipLimitsForHwnd(hwnd);
    const double wrapMaxDip =
        limits.maxWidthDip > 1.0 ? limits.maxWidthDip : static_cast<double>(::CANDIDATE_WINDOW_MAX_WIDTH_DIP);
    const double wrapMaxHeightDip =
        limits.maxHeightDip > 1.0 ? limits.maxHeightDip : static_cast<double>(::CANDIDATE_WINDOW_MAX_WIDTH_DIP);
    const ULONGLONG measureStartedTick = GetTickCount64();
    CAND_DIAG_LOGF(L"candidate-frame clip-measure-submit content_gen={} update_elapsed_ms={}", contentGeneration,
                   measureStartedTick - updateStartedTick);
    GetRealCandidateCardSize(
        webviewCandWnd,
        [hwnd, contentGeneration, updateStartedTick, measureStartedTick](std::pair<double, double> paintedSize) {
            const ULONGLONG completedTick = GetTickCount64();
            if (!::is_global_wnd_cand_shown || contentGeneration != g_candidate_content_generation.load() ||
                paintedSize.first <= 1.0 || paintedSize.second <= 1.0)
            {
                CAND_DIAG_LOGF(L"candidate-frame clip-measure-drop content_gen={} current_gen={} shown={} "
                               L"size_dip=({:.1f},{:.1f}) measure_ms={} total_ms={}",
                               contentGeneration, g_candidate_content_generation.load(), ::is_global_wnd_cand_shown,
                               paintedSize.first, paintedSize.second, completedTick - measureStartedTick,
                               completedTick - updateStartedTick);
                return;
            }
            const HalfScreenDipLimits clipLimits = QueryWebViewHalfScreenDipLimitsForHwnd(hwnd);
            paintedSize.first = ClampWidthDipToHalfScreen(paintedSize.first, clipLimits);
            paintedSize.second = ClampHeightDipToHalfScreen(paintedSize.second, clipLimits);
            const std::pair<double, double> decoratedSize = AddCandidateDecorationToSize(paintedSize);
            FLOAT scale = GetWebViewRasterizationScale(hwnd);
            if (scale <= 0.0f)
            {
                scale = clipLimits.scale > 0.0f ? clipLimits.scale : 1.0f;
            }

            // Content-only updates used to only refresh SetWindowRgn. When the
            // painted card grows wider near the right edge, that left the
            // opaque box hanging past the monitor. Re-anchor to the painted card
            // and re-clamp margins/host to the caret monitor, without a full
            // FineTune (which would reintroduce the show-time jump).
            RECT hostRect{};
            if (GetWindowRect(hwnd, &hostRect))
            {
                int hostX = hostRect.left;
                int hostY = hostRect.top;
                const int hostWidthPx = hostRect.right - hostRect.left;
                const int hostHeightPx = hostRect.bottom - hostRect.top;
                const POINT layoutCaret = GetCandidateLayoutCaret();
                const MonitorCoordinates coordinates = GetMonitorCoordinatesFromPoint(layoutCaret);
                const int marginLeftBefore = Global::MarginLeft;
                const int marginTopBefore = Global::MarginTop;

                ReanchorCandidateHostToPaintedCard(layoutCaret, paintedSize, coordinates, hostHeightPx, scale, hostX,
                                                   hostY);

                KeepCandidateCardInsideHostAndMonitor(hostX, hostY, hostWidthPx, hostHeightPx, decoratedSize.first,
                                                      decoratedSize.second, scale, coordinates, decoratedSize.first);
                const bool hostMoved = hostX != hostRect.left || hostY != hostRect.top;
                const bool marginMoved = Global::MarginLeft != marginLeftBefore || Global::MarginTop != marginTopBefore;
                if (hostMoved)
                {
                    SuppressCandidateDpiChange suppressDpi;
                    UINT flag = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE | SWP_SHOWWINDOW;
                    SetWindowPos(hwnd, nullptr, hostX, hostY, 0, 0, flag);
                    if (webviewControllerCandWnd)
                    {
                        webviewControllerCandWnd->NotifyParentWindowPositionChanged();
                    }
                }
                if (marginMoved && webviewCandWnd)
                {
                    MoveContainerBottom(webviewCandWnd, Global::MarginTop);
                }
                if (hostMoved || marginMoved)
                {
                    CAND_DIAG_LOGF(L"candidate-frame edge-reclamp content_gen={} host=({},{})->({},{}) "
                                   L"margin=({},{})->({},{}) size_dip=({:.1f},{:.1f})",
                                   contentGeneration, hostRect.left, hostRect.top, hostX, hostY, marginLeftBefore,
                                   marginTopBefore, Global::MarginLeft, Global::MarginTop, decoratedSize.first,
                                   decoratedSize.second);
                }
            }

            ClipCandidateWindowToContent(hwnd, decoratedSize, scale);
            // A content-only refresh changes the clip, not the anchor. Recording
            // the latest shared-memory caret here would make a pending move look
            // already applied even though the HWND/margins still use the old one.
            RememberCandidateClipSize(decoratedSize, scale);
            g_last_candidate_card_size = paintedSize;
            CAND_DIAG_LOGF(L"candidate-frame clip-measure-apply content_gen={} size_dip=({:.1f},{:.1f}) "
                           L"measure_ms={} total_ms={} {}",
                           contentGeneration, decoratedSize.first, decoratedSize.second,
                           completedTick - measureStartedTick, completedTick - updateStartedTick,
                           ::DescribeCandidateHostState());
        },
        wrapMaxDip, wrapMaxHeightDip);
}

void PlaceCandidateHostNearCaret(HWND hwnd, std::pair<double, double> requestedCardSize)
{
    if (!hwnd || Global::Point[1] == Global::INVALID_Y)
    {
        return;
    }

    POINT caretPt = GetCandidateLayoutCaret();
    const HalfScreenDipLimits halfLimits = QueryCandidateHalfScreenDipLimitsForPoint(hwnd, caretPt);
    FLOAT layoutScale = halfLimits.scale > 0.0f ? halfLimits.scale : 1.0f;
    const auto hostPx = ComputeQuarterScreenHostPixels(halfLimits.monitor);
    const int hostWidthPx = hostPx.first;
    const int hostHeightPx = hostPx.second;

    std::pair<double, double> cardSize{
        static_cast<double>(::CANDIDATE_WINDOW_WIDTH),
        static_cast<double>(::CANDIDATE_WINDOW_HEIGHT),
    };
    const bool lastClipPlausible =
        g_has_last_candidate_clip && g_last_candidate_clip_size.first > 1.0 &&
        g_last_candidate_clip_size.second > 1.0 &&
        (halfLimits.maxWidthDip <= 1.0 || g_last_candidate_clip_size.first < halfLimits.maxWidthDip * 0.8) &&
        (halfLimits.maxHeightDip <= 1.0 || g_last_candidate_clip_size.second < halfLimits.maxHeightDip * 0.8);
    if (requestedCardSize.first > 1.0 && requestedCardSize.second > 1.0)
    {
        cardSize = requestedCardSize;
    }
    else if (lastClipPlausible)
    {
        cardSize = g_last_candidate_clip_size;
        if (g_last_candidate_clip_scale > 0.0f)
        {
            layoutScale = g_last_candidate_clip_scale;
        }
    }
    cardSize.first = ClampWidthDipToHalfScreen(cardSize.first, halfLimits);
    cardSize.second = ClampHeightDipToHalfScreen(cardSize.second, halfLimits);

    auto properPos = std::make_shared<std::pair<int, int>>();
    // Flip against the actual card, not half the monitor. Passing maxWidthDip as
    // minWidthDip parks the card on the left edge whenever the caret is on the
    // right half of the screen (HTML then sits at margin 0 inside the host).
    AdjustCandidateWindowPosition(&caretPt, cardSize, properPos, layoutScale, cardSize.first);
    RememberCandidateFlip(properPos->second, caretPt.y);

    MonitorCoordinates coordinates = GetMonitorCoordinatesFromPoint(caretPt);
    int hostX = properPos->first -
                static_cast<int>(std::lround(::CANDIDATE_SHADOW_PAD_LEFT * static_cast<double>(layoutScale)));
    const int packingMarginTop = GetCandidatePackingMarginTopDip();
    int desiredOuterTopPx = GetCandidateOuterTopPx(properPos->second, packingMarginTop, layoutScale);
    int hostY = desiredOuterTopPx -
                static_cast<int>(std::lround(::CANDIDATE_SHADOW_PAD_TOP * static_cast<double>(layoutScale)));
    const int edgePadPx = static_cast<int>(std::lround(2.0 * static_cast<double>(layoutScale)));
    if (hostX + hostWidthPx > coordinates.right)
    {
        hostX = coordinates.right - hostWidthPx - edgePadPx;
    }
    if (hostX < coordinates.left)
    {
        hostX = coordinates.left + edgePadPx;
    }
    if (hostY + hostHeightPx > coordinates.bottom)
    {
        hostY = coordinates.bottom - hostHeightPx - edgePadPx;
    }
    if (hostY < coordinates.top)
    {
        hostY = coordinates.top + edgePadPx;
    }

    const int offsetXDip = static_cast<int>(std::lround((properPos->first - hostX) / static_cast<double>(layoutScale)));
    Global::MarginLeft = (std::max)(0, offsetXDip);
    Global::MarginTop = GetCandidateOuterMarginDip(desiredOuterTopPx, hostY, layoutScale);
    const int marginLeftBeforeKeep = Global::MarginLeft;
    const int marginTopBeforeKeep = Global::MarginTop;
    const int hostXBeforeKeep = hostX;
    const int hostYBeforeKeep = hostY;
    const std::pair<double, double> decoratedSize = AddCandidateDecorationToSize(cardSize);
    KeepCandidateCardInsideHostAndMonitor(hostX, hostY, hostWidthPx, hostHeightPx, decoratedSize.first,
                                          decoratedSize.second, layoutScale, coordinates, cardSize.first);
    if (hostX != hostXBeforeKeep || hostY != hostYBeforeKeep || Global::MarginLeft != marginLeftBeforeKeep ||
        Global::MarginTop != marginTopBeforeKeep)
    {
        CAND_DIAG_LOGF(L"candidate-place keep-adjusted host=({},{})->({},{}) margin=({},{})->({},{}) "
                       L"content_dip=({:.1f},{:.1f})",
                       hostXBeforeKeep, hostYBeforeKeep, hostX, hostY, marginLeftBeforeKeep, marginTopBeforeKeep,
                       Global::MarginLeft, Global::MarginTop, decoratedSize.first, decoratedSize.second);
    }

    UINT flag = SWP_SHOWWINDOW;
    RECT currentRect{};
    if (GetWindowRect(hwnd, &currentRect))
    {
        if ((currentRect.right - currentRect.left) == hostWidthPx &&
            (currentRect.bottom - currentRect.top) == hostHeightPx)
        {
            flag |= SWP_NOSIZE;
        }
        if (currentRect.left == hostX && currentRect.top == hostY)
        {
            flag |= SWP_NOMOVE;
        }
    }

    {
        SuppressCandidateDpiChange suppressDpi;
        SetWindowPos(hwnd, HWND_TOPMOST, hostX, hostY, hostWidthPx, hostHeightPx, flag);
    }
    if ((flag & SWP_NOSIZE) == 0)
    {
        SyncCandidateWebViewBoundsToHost(hwnd);
    }
    else if (webviewControllerCandWnd)
    {
        webviewControllerCandWnd->NotifyParentWindowPositionChanged();
    }
    // Push margins into the live DOM immediately. FineTune / Inflate may be
    // async; without this the card stays at margin 0 on the host's left edge.
    if (webviewCandWnd)
    {
        MoveContainerBottom(webviewCandWnd, Global::MarginTop);
    }
    g_last_placed_caret_x = caretPt.x;
    g_last_placed_caret_y = caretPt.y;
    LogCandidatePositionAudit(L"place-near-caret", POINT{Global::Point[0], Global::Point[1]}, caretPt,
                              POINT{properPos->first, properPos->second}, hostX, hostY, hostWidthPx, hostHeightPx,
                              layoutScale, decoratedSize.first, decoratedSize.second, coordinates, marginLeftBeforeKeep,
                              marginTopBeforeKeep);
}
} // namespace ime_windows_detail

namespace
{
void MaybeExpandCandidateClipFromSlotMeasure(HWND hwnd)
{
    if (!hwnd || !IsCandidateHostPaintedVisible(hwnd) || !g_has_last_candidate_clip)
    {
        return;
    }
    std::pair<double, double> paintedSize = LastCandidateSlotMeasuredSize();
    if (paintedSize.first <= 1.0 || paintedSize.second <= 1.0)
    {
        return;
    }
    const HalfScreenDipLimits clipLimits = QueryWebViewHalfScreenDipLimitsForHwnd(hwnd);
    paintedSize.first = ClampWidthDipToHalfScreen(paintedSize.first, clipLimits);
    paintedSize.second = ClampHeightDipToHalfScreen(paintedSize.second, clipLimits);
    if (clipLimits.maxWidthDip > 1.0 && paintedSize.first > clipLimits.maxWidthDip * 0.6)
    {
        return;
    }

    FLOAT slotScale = GetWebViewRasterizationScale(hwnd);
    if (slotScale <= 0.0f)
    {
        slotScale = clipLimits.scale > 0.0f ? clipLimits.scale : 1.0f;
    }
    // A slot measure reports a new painted height, which can change the flip
    // decision — re-derive the anchor from it instead of nudging MarginTop by
    // the height delta. See ReanchorCandidateHostToPaintedCard.
    RECT slotHostRect{};
    if (GetWindowRect(hwnd, &slotHostRect))
    {
        int hostX = slotHostRect.left;
        int hostY = slotHostRect.top;
        const int hostWidthPx = slotHostRect.right - slotHostRect.left;
        const int hostHeightPx = slotHostRect.bottom - slotHostRect.top;
        const POINT layoutCaret = GetCandidateLayoutCaret();
        const MonitorCoordinates coordinates = GetMonitorCoordinatesFromPoint(layoutCaret);
        const int marginLeftBefore = Global::MarginLeft;
        const int marginTopBefore = Global::MarginTop;
        const std::pair<double, double> slotDecoratedSize = AddCandidateDecorationToSize(paintedSize);

        ReanchorCandidateHostToPaintedCard(layoutCaret, paintedSize, coordinates, hostHeightPx, slotScale, hostX,
                                           hostY);
        KeepCandidateCardInsideHostAndMonitor(hostX, hostY, hostWidthPx, hostHeightPx, slotDecoratedSize.first,
                                              slotDecoratedSize.second, slotScale, coordinates,
                                              slotDecoratedSize.first);
        if (hostX != slotHostRect.left || hostY != slotHostRect.top)
        {
            SuppressCandidateDpiChange suppressDpi;
            SetWindowPos(hwnd, nullptr, hostX, hostY, 0, 0,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE | SWP_SHOWWINDOW);
            if (webviewControllerCandWnd)
            {
                webviewControllerCandWnd->NotifyParentWindowPositionChanged();
            }
        }
        if (Global::MarginLeft != marginLeftBefore || Global::MarginTop != marginTopBefore)
        {
            MoveContainerBottom(webviewCandWnd, Global::MarginTop);
        }
    }
    g_last_candidate_card_size = paintedSize;

    std::pair<double, double> decoratedSize = AddCandidateDecorationToSize(paintedSize);
    if (CandidateCardFitsClipEnvelope(decoratedSize))
    {
        return;
    }
    decoratedSize.first = (std::max)(decoratedSize.first, g_last_candidate_clip_size.first);
    decoratedSize.second = (std::max)(decoratedSize.second, g_last_candidate_clip_size.second);
    if (std::fabs(decoratedSize.first - g_last_candidate_clip_size.first) < 2.0 &&
        std::fabs(decoratedSize.second - g_last_candidate_clip_size.second) < 2.0 &&
        CandidateCardFitsClipEnvelope(decoratedSize))
    {
        return;
    }
    const FLOAT scale = slotScale;
    double extraTopDip = 0.0;
    if (g_candidate_placed_above_caret)
    {
        extraTopDip = (std::max)(0.0, static_cast<double>(Global::MarginTop) - g_clip_envelope_top_dip);
        extraTopDip = (std::max)(extraTopDip, EstimateVerticalPageHeightDip(paintedSize.second) - paintedSize.second);
    }
    ClipCandidateWindowToContent(hwnd, decoratedSize, scale, extraTopDip);
    RememberCandidateClipSize(decoratedSize, scale);
}

void FinishCandidateShowAfterSlots(HWND hwnd, uint64_t contentGeneration)
{
    if (!hwnd || !::is_global_wnd_cand_shown || contentGeneration != g_candidate_content_generation.load() ||
        !webviewCandWnd)
    {
        return;
    }

    if (IsCandidateHostPaintedVisible(hwnd))
    {
        return;
    }

    const HalfScreenDipLimits limits = QueryWebViewHalfScreenDipLimitsForHwnd(hwnd);
    const double wrapMaxDip =
        limits.maxWidthDip > 1.0 ? limits.maxWidthDip : static_cast<double>(::CANDIDATE_WINDOW_MAX_WIDTH_DIP);
    const double wrapMaxHeightDip =
        limits.maxHeightDip > 1.0 ? limits.maxHeightDip : static_cast<double>(::CANDIDATE_WINDOW_MAX_WIDTH_DIP);

    auto applyPaintedSize = [hwnd, contentGeneration](std::pair<double, double> paintedSize) {
        if (!::is_global_wnd_cand_shown || contentGeneration != g_candidate_content_generation.load())
        {
            return;
        }
        if (IsCandidateHostPaintedVisible(hwnd))
        {
            MaybeExpandCandidateClipFromSlotMeasure(hwnd);
            return;
        }
        if (paintedSize.first <= 1.0 || paintedSize.second <= 1.0)
        {
            SetHostWindowCloaked(hwnd, false);
            UpdateSmallWindowWebviewVisibility(hwnd, true);
            return;
        }
        const HalfScreenDipLimits clipLimits = QueryWebViewHalfScreenDipLimitsForHwnd(hwnd);
        paintedSize.first = ClampWidthDipToHalfScreen(paintedSize.first, clipLimits);
        paintedSize.second = ClampHeightDipToHalfScreen(paintedSize.second, clipLimits);
        std::pair<double, double> decoratedSize = AddCandidateDecorationToSize(paintedSize);
        decoratedSize.first += 8.0;
        decoratedSize.second += 6.0;
        decoratedSize.first = ClampWidthDipToHalfScreen(decoratedSize.first, clipLimits);
        decoratedSize.second = ClampHeightDipToHalfScreen(decoratedSize.second, clipLimits);
        FLOAT scale = GetWebViewRasterizationScale(hwnd);
        if (scale <= 0.0f)
        {
            scale = clipLimits.scale > 0.0f ? clipLimits.scale : 1.0f;
        }
        PlaceCandidateHostNearCaret(hwnd, paintedSize);
        const double extraTopDip =
            g_candidate_placed_above_caret
                ? (std::max)(0.0, EstimateVerticalPageHeightDip(paintedSize.second) - paintedSize.second)
                : (GetCandidateDecorationTopDip() > 0.0 ? 8.0 : 0.0);
        MoveContainerBottom(webviewCandWnd, Global::MarginTop,
                            [hwnd, decoratedSize, scale, extraTopDip, paintedSize]() {
                                if (!::is_global_wnd_cand_shown || IsCandidateHostPaintedVisible(hwnd))
                                {
                                    return;
                                }
                                ClipCandidateWindowToContent(hwnd, decoratedSize, scale, extraTopDip);
                                const POINT placedCaret = GetCandidateLayoutCaret();
                                RememberCandidateClip(decoratedSize, scale, placedCaret.x, placedCaret.y);
                                g_last_candidate_card_size = paintedSize;
                                SetHostWindowCloaked(hwnd, false);
                                UpdateSmallWindowWebviewVisibility(hwnd, true);
                            });
    };

    // Slot-script getBoundingClientRect can report the stretched WebView
    // viewport. FineTune's pass-2 measure forces fit-content; use that here.
    GetRealCandidateCardSize(webviewCandWnd, std::move(applyPaintedSize), wrapMaxDip, wrapMaxHeightDip);
}
} // namespace

int FineTuneWindow(HWND hwnd)
{
    const POINT layoutCaret = GetCandidateLayoutCaret();
    if (CandidatePresenter::Instance().IsBound())
    {
        CandidatePresenter::Instance().ShowFromGlobalState(layoutCaret);
        g_last_placed_caret_x = layoutCaret.x;
        g_last_placed_caret_y = layoutCaret.y;
        return 0;
    }

    UINT flag = SWP_SHOWWINDOW;

    int caretX = layoutCaret.x;
    int caretY = layoutCaret.y;
    POINT caretPt{caretX, caretY};
    // Size/clamp against the composition anchor's monitor DPI — not the
    // foreground HWND — so mixed-DPI extended screens stay consistent.
    FLOAT scale = QueryCandidateHalfScreenDipLimitsForPoint(hwnd, caretPt).scale;

    (void)0;
    if (!webviewCandWnd || !IsCandidateWebviewReady())
    {
        (void)0;
        LogSmallWindowReadyGate(L"fine-tune-no-webview");
        WEBVIEW_DIAG_LOGF(L"fine-tune skipped: no webview {}", DescribeCandidateHostState());
        return 0;
    }
    const uint64_t generation = ++g_candidate_finetune_generation;
    g_candidate_layout_inflight.store(true);
    CAND_DIAG_LOGF(L"candidate-frame fine-tune-begin layout_gen={} content_gen={} tick={} caret=({},{}) {}", generation,
                   g_candidate_content_generation.load(), GetTickCount64(), caretX, caretY,
                   DescribeCandidateHostState());
    WEBVIEW_DIAG_LOGF(L"fine-tune begin generation={} caret=({},{}) {}", generation, caretX, caretY,
                      DescribeCandidateHostState());
    // Wrap/scroll budget = stable host size (half-screen DIP). Width wraps;
    // height scrolls inside the card when fonts make the list taller than host.
    const HalfScreenDipLimits measureLimits = QueryCandidateHalfScreenDipLimitsForPoint(hwnd, caretPt);
    const double wrapMaxDip = measureLimits.maxWidthDip > 1.0 ? measureLimits.maxWidthDip
                                                              : static_cast<double>(::CANDIDATE_WINDOW_MAX_WIDTH_DIP);
    const double wrapMaxHeightDip = measureLimits.maxHeightDip > 1.0
                                        ? measureLimits.maxHeightDip
                                        : static_cast<double>(::CANDIDATE_WINDOW_MAX_WIDTH_DIP);
    HMONITOR caretMonitor = MonitorFromPoint(caretPt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO caretMonitorInfo{sizeof(caretMonitorInfo)};
    const bool hasMonitorInfo = caretMonitor && GetMonitorInfo(caretMonitor, &caretMonitorInfo);
    WEBVIEW_DIAG_LOGF(
        L"ui-layout generation={} config layout={} font={} cn_font_size={} preedit_font_size={} page_size={} "
        L"caret=({},{}) point_scale={:.3f} webview_scale={:.3f} hwnd_dpi={} system_dpi={} "
        L"monitor=({},{})-({},{}) "
        L"work=({},{})-({},{}) half_dip=({:.2f},{:.2f}) monitor_info={}",
        generation, string_to_wstring(GetConfiguredCandidateWindowLayout()),
        string_to_wstring(GetConfiguredCandidateFont()), GetConfiguredCandidateFontSize(),
        GetConfiguredCandidateWindowPreeditFontSize(), GetConfiguredCandidatePageSize(), caretX, caretY,
        static_cast<double>(scale), static_cast<double>(GetWebViewRasterizationScale(hwnd)), GetDpiForWindow(hwnd),
        GetDpiForSystem(), measureLimits.monitor.left, measureLimits.monitor.top, measureLimits.monitor.right,
        measureLimits.monitor.bottom, hasMonitorInfo ? caretMonitorInfo.rcWork.left : 0,
        hasMonitorInfo ? caretMonitorInfo.rcWork.top : 0, hasMonitorInfo ? caretMonitorInfo.rcWork.right : 0,
        hasMonitorInfo ? caretMonitorInfo.rcWork.bottom : 0, measureLimits.maxWidthDip, measureLimits.maxHeightDip,
        hasMonitorInfo);
    LogCandidateLayoutSnapshot(L"fine-tune-begin");
    // Give horizontal measure an unconstrained viewport before reading DOM size.
    PrepareCandidateWebViewBoundsForMeasure(hwnd);
    InjectSurfaceViewportLimits(::webviewCandWnd.Get(), hwnd);
    std::shared_ptr<std::pair<int, int>> properPos = std::make_shared<std::pair<int, int>>();
    GetContainerSizeCand(
        webviewCandWnd,
        [flag,       //
         scale,      //
         caretX,     //
         caretY,     //
         properPos,  //
         generation, //
         wrapMaxDip, //
         hwnd](std::pair<double, double> containerSize) {
            // Commit/ClearState may have hidden the window while this WebView2
            // measure callback was still pending — do not resurrect it.
            if (!::is_global_wnd_cand_shown || caretY == Global::INVALID_Y)
            {
                WEBVIEW_DIAG_LOGF(L"fine-tune generation={} discarded: hidden={} invalid_caret={}", generation,
                                  !::is_global_wnd_cand_shown, caretY == Global::INVALID_Y);
                EndCandidateLayoutIfCurrent(generation);
                return;
            }
            // A newer show/update already queued another FineTune — ignore this one.
            if (generation != g_candidate_finetune_generation.load())
            {
                WEBVIEW_DIAG_LOGF(L"fine-tune generation={} discarded: superseded_by={}", generation,
                                  g_candidate_finetune_generation.load());
                return;
            }

            POINT pt = {caretX, caretY};
            HalfScreenDipLimits halfLimits = QueryCandidateHalfScreenDipLimitsForPoint(hwnd, pt);
            auto capCandWidthDip = [&](double widthDip) { return ClampWidthDipToHalfScreen(widthDip, halfLimits); };
            auto capCandHeightDip = [&](double heightDip) { return ClampHeightDipToHalfScreen(heightDip, halfLimits); };

            const std::pair<double, double> measuredSize = containerSize;
            // Parentheses keep Windows min() macro from eating std::min.
            containerSize.first = capCandWidthDip(containerSize.first);
            containerSize.second = capCandHeightDip(containerSize.second);

            // properPos is the desired top-left of the opaque card (content), not
            // necessarily the host HWND — host stays at a stable quarter-screen size
            // while the card slides via MarginLeft/MarginTop.
            // Flip against the measured card, not half the monitor. Passing
            // maxWidthDip as minWidthDip parks the card on the left edge whenever
            // the caret is on the right half of the screen.
            AdjustCandidateWindowPosition(&pt, containerSize, properPos, halfLimits.scale, containerSize.first);
            RememberCandidateFlip(properPos->second, caretY);
            WEBVIEW_DIAG_LOGF(L"ui-layout generation={} pass=measure measured_dip=({:.2f},{:.2f}) "
                              L"capped_dip=({:.2f},{:.2f}) cap=({:.2f},{:.2f}) scale={:.3f} "
                              L"monitor=({},{})-({},{}) proper_pos=({},{}) packing_margin_top={}",
                              generation, measuredSize.first, measuredSize.second, containerSize.first,
                              containerSize.second, halfLimits.maxWidthDip, halfLimits.maxHeightDip,
                              static_cast<double>(halfLimits.scale), halfLimits.monitor.left, halfLimits.monitor.top,
                              halfLimits.monitor.right, halfLimits.monitor.bottom, properPos->first, properPos->second,
                              Global::MarginTop);

            const bool showPreedit = GetConfiguredCandidateWindowPreeditStyle() != "empty";
            const std::wstring preedit = GetPreeditWithCaretMarker();
            // This callback runs long after the worker posted the show request, so take the published page instead of
            // the vectors it may already be rebuilding.
            const Global::CandidatePageSnapshotPtr candidatePage = Global::LoadCandidatePageSnapshot();
            CandidateWindowContent str{preedit, *candidatePage, showPreedit};
            // Empty composition with no candidates means the session already ended.
            if (GlobalIme::composition.raw_input_with_cases.empty() && candidatePage->page_views.empty())
            {
                (void)0;
                EndCandidateLayoutIfCurrent(generation);
                return;
            }

            FLOAT layoutScale = scale;
            auto hostPx = ComputeQuarterScreenHostPixels(halfLimits.monitor);
            int hostWidthPx = hostPx.first;
            int hostHeightPx = hostPx.second;

            MonitorCoordinates coordinates = GetMonitorCoordinatesFromPoint(pt);
            int hostX = properPos->first -
                        static_cast<int>(std::lround(::CANDIDATE_SHADOW_PAD_LEFT * static_cast<double>(layoutScale)));
            const int packingMarginTop = GetCandidatePackingMarginTopDip();
            int desiredOuterTopPx = GetCandidateOuterTopPx(properPos->second, packingMarginTop, layoutScale);
            int hostY = desiredOuterTopPx -
                        static_cast<int>(std::lround(::CANDIDATE_SHADOW_PAD_TOP * static_cast<double>(layoutScale)));
            const int edgePadPx =
                static_cast<int>(std::lround(2.0 * static_cast<double>(layoutScale > 0 ? layoutScale : 1.0f)));
            // Right/bottom first, then re-clamp left/top so a host wider/taller than
            // the monitor cannot be pushed past the caret's screen edge.
            if (hostX + hostWidthPx > coordinates.right)
            {
                hostX = coordinates.right - hostWidthPx - edgePadPx;
            }
            if (hostX < coordinates.left)
            {
                hostX = coordinates.left + edgePadPx;
            }
            if (hostY + hostHeightPx > coordinates.bottom)
            {
                hostY = coordinates.bottom - hostHeightPx - edgePadPx;
            }
            if (hostY < coordinates.top)
            {
                hostY = coordinates.top + edgePadPx;
            }

            auto applyCardMargins = [&](FLOAT marginScale) {
                const int offsetXDip =
                    static_cast<int>(std::lround((properPos->first - hostX) / static_cast<double>(marginScale)));
                Global::MarginLeft = (std::max)(0, offsetXDip);
                Global::MarginTop = GetCandidateOuterMarginDip(desiredOuterTopPx, hostY, marginScale);
            };
            // Already on screen: first-pass size/flip is a guess. Applying it
            // moves the visible card, then pass 2 snaps it — that is the flash.
            const bool deferHostMove = IsCandidateHostPaintedVisible(hwnd);
            const std::pair<double, double> decoratedSize = AddCandidateDecorationToSize(containerSize);
            if (!deferHostMove)
            {
                applyCardMargins(layoutScale);
                KeepCandidateCardInsideHostAndMonitor(hostX, hostY, hostWidthPx, hostHeightPx, decoratedSize.first,
                                                      decoratedSize.second, layoutScale, coordinates,
                                                      decoratedSize.first);
            }

            int newWidth = hostWidthPx;
            int newHeight = hostHeightPx;
            UINT newFlag = flag;

            RECT currentRect{};
            if (GetWindowRect(hwnd, &currentRect))
            {
                const int curW = currentRect.right - currentRect.left;
                const int curH = currentRect.bottom - currentRect.top;
                if (curW == newWidth && curH == newHeight)
                {
                    newFlag |= SWP_NOSIZE;
                }
                // Prefer not to move the host when only the internal card offset changed.
                if (currentRect.left == hostX && currentRect.top == hostY)
                {
                    newFlag |= SWP_NOMOVE;
                }
            }
            CAND_DIAG_LOGF(L"candidate-layout host-pass layout_gen={} caret=({},{}) current=({},{},{}x{}) "
                           L"desired=({},{},{}x{}) defer_move={} flags={:#x} margin=({},{})",
                           generation, caretX, caretY, currentRect.left, currentRect.top,
                           currentRect.right - currentRect.left, currentRect.bottom - currentRect.top, hostX, hostY,
                           newWidth, newHeight, deferHostMove, newFlag, Global::MarginLeft, Global::MarginTop);
            LogCandidatePositionAudit(L"fine-tune-host-pass", POINT{Global::Point[0], Global::Point[1]},
                                      POINT{caretX, caretY}, POINT{properPos->first, properPos->second}, hostX, hostY,
                                      hostWidthPx, hostHeightPx, layoutScale, decoratedSize.first, decoratedSize.second,
                                      coordinates);

            if (!::is_global_wnd_cand_shown || generation != g_candidate_finetune_generation.load())
            {
                EndCandidateLayoutIfCurrent(generation);
                return;
            }
            RaiseCandidateHostForShow(L"fine-tune");
            // Geometry size is stable (quarter-screen). Do not cloak — card motion
            // is CSS margin inside the already-placed transparent host.
            BOOL positioned = FALSE;
            if (!deferHostMove)
            {
                SuppressCandidateDpiChange suppressDpi;
                SetLastError(0);
                positioned = SetWindowPos( //
                    hwnd,                  //
                    HWND_TOPMOST,          //
                    hostX,                 //
                    hostY,                 //
                    newWidth,              //
                    newHeight,             //
                    newFlag                //
                );
            }
            const DWORD positionError = positioned ? ERROR_SUCCESS : GetLastError();
            WEBVIEW_DIAG_LOGF(L"ui-layout generation={} pass=host-place desired=({},{},{}x{}) flags={:#x} "
                              L"result={} gle={} point_scale={:.3f} margin=({},{})",
                              generation, hostX, hostY, newWidth, newHeight, newFlag, positioned != FALSE,
                              positionError, static_cast<double>(layoutScale), Global::MarginLeft, Global::MarginTop);

            // After the host lands on the caret's monitor, WebView2 may update
            // its rasterization scale. Re-sync physical size + CSS margins so
            // MarginLeft*rasterScale matches painting and SetWindowRgn.
            FLOAT hwndScale = GetWebViewRasterizationScale(hwnd);
            if (!deferHostMove && hwndScale > 0.0f && std::fabs(hwndScale - layoutScale) > 0.001f)
            {
                const FLOAT oldLayoutScale = layoutScale;
                layoutScale = hwndScale;
                halfLimits = QueryWebViewHalfScreenDipLimitsForHwnd(hwnd);
                hostPx = ComputeQuarterScreenHostPixels(halfLimits.monitor);
                hostWidthPx = hostPx.first;
                hostHeightPx = hostPx.second;

                AdjustCandidateWindowPosition(&pt, containerSize, properPos, layoutScale, containerSize.first);
                RememberCandidateFlip(properPos->second, caretY);
                const int resyncPackingMarginTop = GetCandidatePackingMarginTopDip();
                hostX = properPos->first -
                        static_cast<int>(std::lround(::CANDIDATE_SHADOW_PAD_LEFT * static_cast<double>(layoutScale)));
                desiredOuterTopPx = GetCandidateOuterTopPx(properPos->second, resyncPackingMarginTop, layoutScale);
                hostY = desiredOuterTopPx -
                        static_cast<int>(std::lround(::CANDIDATE_SHADOW_PAD_TOP * static_cast<double>(layoutScale)));
                const int resyncEdgePadPx =
                    static_cast<int>(std::lround(2.0 * static_cast<double>(layoutScale > 0 ? layoutScale : 1.0f)));
                if (hostX + hostWidthPx > coordinates.right)
                {
                    hostX = coordinates.right - hostWidthPx - resyncEdgePadPx;
                }
                if (hostX < coordinates.left)
                {
                    hostX = coordinates.left + resyncEdgePadPx;
                }
                if (hostY + hostHeightPx > coordinates.bottom)
                {
                    hostY = coordinates.bottom - hostHeightPx - resyncEdgePadPx;
                }
                if (hostY < coordinates.top)
                {
                    hostY = coordinates.top + resyncEdgePadPx;
                }

                const int offsetXDip =
                    static_cast<int>(std::lround((properPos->first - hostX) / static_cast<double>(layoutScale)));
                Global::MarginLeft = (std::max)(0, offsetXDip);
                Global::MarginTop = GetCandidateOuterMarginDip(desiredOuterTopPx, hostY, layoutScale);
                KeepCandidateCardInsideHostAndMonitor(hostX, hostY, hostWidthPx, hostHeightPx, decoratedSize.first,
                                                      decoratedSize.second, layoutScale, coordinates,
                                                      decoratedSize.first);

                if (!::is_global_wnd_cand_shown || generation != g_candidate_finetune_generation.load())
                {
                    EndCandidateLayoutIfCurrent(generation);
                    return;
                }
                {
                    SuppressCandidateDpiChange suppressDpi;
                    SetLastError(0);
                    const BOOL resyncResult =
                        SetWindowPos(hwnd, HWND_TOPMOST, hostX, hostY, hostWidthPx, hostHeightPx, flag);
                    WEBVIEW_DIAG_LOGF(L"ui-layout generation={} pass=dpi-resync scale={:.3f}->{:.3f} "
                                      L"desired=({},{},{}x{}) result={} gle={} half_dip=({:.2f},{:.2f}) margin=({},{})",
                                      generation, static_cast<double>(oldLayoutScale), static_cast<double>(layoutScale),
                                      hostX, hostY, hostWidthPx, hostHeightPx, resyncResult != FALSE,
                                      resyncResult ? ERROR_SUCCESS : GetLastError(), halfLimits.maxWidthDip,
                                      halfLimits.maxHeightDip, Global::MarginLeft, Global::MarginTop);
                }
                newWidth = hostWidthPx;
                newHeight = hostHeightPx;
                newFlag = flag;
            }

            if ((newFlag & SWP_NOSIZE) == 0)
            {
                SyncCandidateWebViewBoundsToHost(hwnd);
            }
            else if (webviewControllerCandWnd)
            {
                webviewControllerCandWnd->NotifyParentWindowPositionChanged();
            }
            InjectSurfaceViewportLimits(::webviewCandWnd.Get(), hwnd);

            InflateCandWnd(str, [hwnd, positioned, generation, containerSize, layoutScale, caretX, caretY,
                                 packingMarginTop, deferHostMove, pageGeneration = candidatePage->generation]() {
                if (!::is_global_wnd_cand_shown || generation != g_candidate_finetune_generation.load())
                {
                    EndCandidateLayoutIfCurrent(generation);
                    return;
                }
                // The DOM update for this page has completed; echo the captured generation so a
                // selection that arrives while the layout pass is still running settles against the
                // page that is about to become visible.
                Global::PublishRenderedCandidatePageGeneration(pageGeneration);

                // Stay cloaked through pass 1. First-pass size is routinely taller
                // than the painted card (log: 289dip then 201dip). Uncloaking with
                // that provisional region is the flash at the wrong clip.

                auto finalizeClip = [hwnd, generation, layoutScale, caretX,
                                     caretY](std::pair<double, double> finalSize) {
                    if (!::is_global_wnd_cand_shown || generation != g_candidate_finetune_generation.load())
                    {
                        EndCandidateLayoutIfCurrent(generation);
                        return;
                    }
                    const POINT targetCaret{caretX, caretY};
                    const HalfScreenDipLimits clipLimits = QueryCandidateHalfScreenDipLimitsForPoint(hwnd, targetCaret);
                    finalSize.first = ClampWidthDipToHalfScreen(finalSize.first, clipLimits);
                    finalSize.second = ClampHeightDipToHalfScreen(finalSize.second, clipLimits);
                    const std::pair<double, double> decoratedFinalSize = AddCandidateDecorationToSize(finalSize);
                    ClipCandidateWindowToContent(hwnd, decoratedFinalSize, layoutScale);
                    WEBVIEW_DIAG_LOGF(L"cand-clip scale={:.3f} margin=({},{}) size=({:.1f},{:.1f})",
                                      static_cast<double>(layoutScale), Global::MarginLeft, Global::MarginTop,
                                      decoratedFinalSize.first, decoratedFinalSize.second);
                    SetHostWindowCloaked(hwnd, false);
                    UpdateSmallWindowWebviewVisibility(hwnd, true);
                    RememberCandidateClip(decoratedFinalSize, layoutScale, caretX, caretY);
                    EndCandidateLayoutIfCurrent(generation);
                    RECT actualRect{};
                    GetWindowRect(hwnd, &actualRect);
                    LogCandidatePositionAudit(
                        L"fine-tune-final", POINT{Global::Point[0], Global::Point[1]}, POINT{caretX, caretY},
                        POINT{actualRect.left + static_cast<LONG>(std::lround(Global::MarginLeft * layoutScale)),
                              actualRect.top + static_cast<LONG>(std::lround(Global::MarginTop * layoutScale))},
                        actualRect.left, actualRect.top, actualRect.right - actualRect.left,
                        actualRect.bottom - actualRect.top, layoutScale, decoratedFinalSize.first,
                        decoratedFinalSize.second,
                        QueryCandidateHalfScreenDipLimitsForPoint(hwnd, POINT{caretX, caretY}).monitor);
                    CAND_DIAG_LOGF(L"candidate-frame fine-tune-complete layout_gen={} content_gen={} tick={} "
                                   L"size_dip=({:.1f},{:.1f}) rect=({},{},{}x{}) {}",
                                   generation, g_candidate_content_generation.load(), GetTickCount64(), finalSize.first,
                                   finalSize.second, actualRect.left, actualRect.top,
                                   actualRect.right - actualRect.left, actualRect.bottom - actualRect.top,
                                   DescribeCandidateHostState());
                    WEBVIEW_DIAG_LOGF(L"fine-tune generation={} completed size_dip=({:.1f},{:.1f}) "
                                      L"margin=({},{}) rect=({},{},{}x{}) {}",
                                      generation, finalSize.first, finalSize.second, Global::MarginLeft,
                                      Global::MarginTop, actualRect.left, actualRect.top,
                                      actualRect.right - actualRect.left, actualRect.bottom - actualRect.top,
                                      DescribeCandidateHostState());
                    LogCandidateLayoutSnapshot(L"fine-tune-final");
                };

                // Pass 2: remeasure the painted card after margins. Keep the stable
                // quarter-screen host; only re-clamp position / margins / region.
                const POINT targetCaret{caretX, caretY};
                const HalfScreenDipLimits pass2LimitsForWrap =
                    QueryCandidateHalfScreenDipLimitsForPoint(hwnd, targetCaret);
                const double pass2WrapMaxDip = pass2LimitsForWrap.maxWidthDip > 1.0
                                                   ? pass2LimitsForWrap.maxWidthDip
                                                   : static_cast<double>(::CANDIDATE_WINDOW_MAX_WIDTH_DIP);
                const double pass2WrapMaxHeightDip = pass2LimitsForWrap.maxHeightDip > 1.0
                                                         ? pass2LimitsForWrap.maxHeightDip
                                                         : static_cast<double>(::CANDIDATE_WINDOW_MAX_WIDTH_DIP);
                GetRealCandidateCardSize(
                    webviewCandWnd,
                    [hwnd, generation, layoutScale, caretX, caretY, packingMarginTop, containerSize,
                     finalizeClip](std::pair<double, double> paintedSize) {
                        if (!::is_global_wnd_cand_shown || generation != g_candidate_finetune_generation.load())
                        {
                            EndCandidateLayoutIfCurrent(generation);
                            return;
                        }
                        if (paintedSize.first <= 1.0 || paintedSize.second <= 1.0)
                        {
                            finalizeClip(containerSize);
                            return;
                        }
                        const std::pair<double, double> rawPaintedSize = paintedSize;
                        POINT pt = {caretX, caretY};
                        // During a visible move, pass 1 deliberately leaves the
                        // stable host where it is to avoid a provisional flash.
                        // Therefore HWND-based limits still describe the old
                        // monitor here. Keep every painted-pass calculation on
                        // the caret monitor; otherwise a right-edge move can use
                        // the old monitor's origin/scale and erase MarginLeft.
                        HalfScreenDipLimits pass2Limits = QueryCandidateHalfScreenDipLimitsForPoint(hwnd, pt);
                        paintedSize.first = ClampWidthDipToHalfScreen(paintedSize.first, pass2Limits);
                        paintedSize.second = ClampHeightDipToHalfScreen(paintedSize.second, pass2Limits);
                        WEBVIEW_DIAG_LOGF(L"ui-layout generation={} pass=painted measured_dip=({:.2f},{:.2f}) "
                                          L"capped_dip=({:.2f},{:.2f}) first_pass=({:.2f},{:.2f}) "
                                          L"cap=({:.2f},{:.2f}) hwnd_scale={:.3f}",
                                          generation, rawPaintedSize.first, rawPaintedSize.second, paintedSize.first,
                                          paintedSize.second, containerSize.first, containerSize.second,
                                          pass2Limits.maxWidthDip, pass2Limits.maxHeightDip,
                                          static_cast<double>(pass2Limits.scale));

                        FLOAT pass2Scale = pass2Limits.scale > 0.0f ? pass2Limits.scale : layoutScale;
                        auto properPos = std::make_shared<std::pair<int, int>>();
                        AdjustCandidateWindowPosition(&pt, paintedSize, properPos, pass2Scale);
                        RememberCandidateFlip(properPos->second, caretY);
                        const int packingTop = GetCandidatePackingMarginTopDip();
                        const std::pair<double, double> decoratedPaintedSize =
                            AddCandidateDecorationToSize(paintedSize);

                        const auto pass2Host = ComputeQuarterScreenHostPixels(pass2Limits.monitor);
                        const int hostWidthPx = pass2Host.first;
                        const int hostHeightPx = pass2Host.second;

                        MonitorCoordinates coordinates = GetMonitorCoordinatesFromPoint(pt);
                        int hostX = properPos->first - static_cast<int>(std::lround(::CANDIDATE_SHADOW_PAD_LEFT *
                                                                                    static_cast<double>(pass2Scale)));
                        const int desiredOuterTopPx = GetCandidateOuterTopPx(properPos->second, packingTop, pass2Scale);
                        int hostY =
                            desiredOuterTopPx -
                            static_cast<int>(std::lround(::CANDIDATE_SHADOW_PAD_TOP * static_cast<double>(pass2Scale)));
                        const int edgePadPx = static_cast<int>(
                            std::lround(2.0 * static_cast<double>(pass2Scale > 0 ? pass2Scale : 1.0f)));
                        if (hostX + hostWidthPx > coordinates.right)
                        {
                            hostX = coordinates.right - hostWidthPx - edgePadPx;
                        }
                        if (hostX < coordinates.left)
                        {
                            hostX = coordinates.left + edgePadPx;
                        }
                        if (hostY + hostHeightPx > coordinates.bottom)
                        {
                            hostY = coordinates.bottom - hostHeightPx - edgePadPx;
                        }
                        if (hostY < coordinates.top)
                        {
                            hostY = coordinates.top + edgePadPx;
                        }

                        const int offsetXDip =
                            static_cast<int>(std::lround((properPos->first - hostX) / static_cast<double>(pass2Scale)));
                        Global::MarginLeft = (std::max)(0, offsetXDip);
                        Global::MarginTop = GetCandidateOuterMarginDip(desiredOuterTopPx, hostY, pass2Scale);
                        KeepCandidateCardInsideHostAndMonitor(hostX, hostY, hostWidthPx, hostHeightPx,
                                                              decoratedPaintedSize.first, decoratedPaintedSize.second,
                                                              pass2Scale, coordinates);

                        if (!::is_global_wnd_cand_shown || generation != g_candidate_finetune_generation.load())
                        {
                            EndCandidateLayoutIfCurrent(generation);
                            return;
                        }
                        {
                            SuppressCandidateDpiChange suppressDpi;
                            // Prefer SWP_NOSIZE when already on the quarter-screen size so
                            // only the clamped top-left moves with the card.
                            UINT pass2Flag = SWP_SHOWWINDOW;
                            RECT cur{};
                            if (GetWindowRect(hwnd, &cur) && (cur.right - cur.left) == hostWidthPx &&
                                (cur.bottom - cur.top) == hostHeightPx)
                            {
                                pass2Flag |= SWP_NOSIZE;
                            }
                            const BOOL pass2Result =
                                SetWindowPos(hwnd, HWND_TOPMOST, hostX, hostY, hostWidthPx, hostHeightPx, pass2Flag);
                            CAND_DIAG_LOGF(L"candidate-layout painted-pass layout_gen={} caret=({},{}) "
                                           L"current=({},{},{}x{}) desired=({},{},{}x{}) flags={:#x} "
                                           L"result={} gle={} margin=({},{}) size_dip=({:.1f},{:.1f})",
                                           generation, caretX, caretY, cur.left, cur.top, cur.right - cur.left,
                                           cur.bottom - cur.top, hostX, hostY, hostWidthPx, hostHeightPx, pass2Flag,
                                           pass2Result != FALSE, pass2Result ? ERROR_SUCCESS : GetLastError(),
                                           Global::MarginLeft, Global::MarginTop, paintedSize.first,
                                           paintedSize.second);
                            LogCandidatePositionAudit(L"fine-tune-painted-pass",
                                                      POINT{Global::Point[0], Global::Point[1]}, POINT{caretX, caretY},
                                                      POINT{properPos->first, properPos->second}, hostX, hostY,
                                                      hostWidthPx, hostHeightPx, pass2Scale, decoratedPaintedSize.first,
                                                      decoratedPaintedSize.second, coordinates);
                        }
                        SyncCandidateWebViewBoundsToHost(hwnd);
                        InjectSurfaceViewportLimits(::webviewCandWnd.Get(), hwnd);

                        // Re-apply margins into the live DOM before clipping.
                        std::wstring marginScript =
                            L"(function(){var el=document.getElementById('realContainerParent');"
                            L"if(el){el.style.marginTop='" +
                            std::to_wstring(Global::MarginTop) + L"px';el.style.marginLeft='" +
                            std::to_wstring(Global::MarginLeft) + L"px';}})();";
                        if (webviewCandWnd)
                        {
                            webviewCandWnd->ExecuteScript(
                                marginScript.c_str(),
                                Callback<ICoreWebView2ExecuteScriptCompletedHandler>([finalizeClip, paintedSize](
                                                                                         HRESULT, LPCWSTR) -> HRESULT {
                                    finalizeClip(paintedSize);
                                    return S_OK;
                                }).Get());
                        }
                        else
                        {
                            finalizeClip(paintedSize);
                        }
                        (void)packingMarginTop;
                    },
                    pass2WrapMaxDip, pass2WrapMaxHeightDip);
            });
        },
        wrapMaxDip, wrapMaxHeightDip);
    return 0;
}
