// 候选窗 WebView：内容与测量区的脚本更新、悬停锁与粘性卡片脚本、候选槽位增量更新、
// 测量用 bounds，以及候选窗 controller 的创建与页面消息处理。
#include "webview2/windows_webview2_internal.h"
#include "engine/contracts/webview/validator.h"
#include "webview2/candidate_window_template.h"
#include "config/ime_config.h"
#include "defines/defines.h"
#include "defines/globals.h"
#include "global/globals.h"
#include "ipc/ipc.h"
#include "ipc/candidate_ui_action_policy.h"
#include "utils/common_utils.h"
#include "utils/ime_utils.h"
#include "utils/webview_utils.h"
#include "utils/window_utils.h"
#include "window/candidate_wheel_paging.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace windows_webview2_detail;

// Legacy floor kept for 100% DPI; high-DPI reserves are computed from DIPs below.
constexpr int candidateBoundExtraFloorPx = 1000;

static std::wstring EscapeForJsTemplateLiteral(const std::wstring &text)
{
    // Content is injected as an untagged JavaScript template literal. Kaomoji
    // commonly contain `\`, backticks (e.g. `( -'`-)`), and `${`; any of those
    // will terminate or invalidate the literal and the DOM update is dropped.
    std::wstring escaped;
    escaped.reserve(text.size() + 16);
    for (size_t index = 0; index < text.size(); ++index)
    {
        const wchar_t ch = text[index];
        if (ch == L'\\')
        {
            escaped += L"\\\\";
        }
        else if (ch == L'`')
        {
            escaped += L"\\`";
        }
        else if (ch == L'$' && index + 1 < text.size() && text[index + 1] == L'{')
        {
            escaped += L"\\${";
            ++index;
        }
        else
        {
            escaped += ch;
        }
    }
    return escaped;
}

namespace
{
constexpr UINT_PTR kCandidateHoverArmTimerId = 21;
constexpr int kCandidateHoverArmDistancePx = 2;

POINT g_candidate_hover_cursor{};
bool g_candidate_hover_armed = false;
ULONGLONG g_candidate_hover_disarm_tick = 0;

// Leftover wheel travel below one notch, carried across `candidateWheel`
// messages. The page reports deltas in WHEEL_DELTA units, so this is the same
// accumulator the D2D presenter keeps for WM_MOUSEWHEEL.
int g_candidate_wheel_accumulator = 0;

// CSS :hover is applied by Chromium when the HWND sits under a still
// cursor. That does not go through mousemove JS. Kill those paints until
// the physical screen cursor actually moves after the card is up.
constexpr wchar_t kInstallCandidateHoverLockScript[] = LR"(
(function () {
  if (!document.getElementById('msime-hover-lock-style')) {
    const style = document.createElement('style');
    style.id = 'msime-hover-lock-style';
    style.textContent = `
html:not(.msime-hover-armed) #realContainer .cand:not(.first):hover,
html:not(.msime-hover-armed) #realContainer.hover-active .cand:not(.first):hover {
  background-color: transparent !important;
  background-image: none !important;
  box-shadow: none !important;
  outline: none !important;
}
`;
    document.documentElement.appendChild(style);
  }
  document.documentElement.classList.remove('msime-hover-armed');
  const container = document.getElementById('realContainer');
  if (container) {
    container.classList.remove('hover-active');
  }
  if (!window.__msimeBlockSynthHover) {
    window.__msimeBlockSynthHover = true;
    document.addEventListener('mousemove', function (event) {
      if (!document.documentElement.classList.contains('msime-hover-armed')) {
        if (event.movementX !== 0 || event.movementY !== 0) {
          document.documentElement.classList.add('msime-hover-armed');
          const liveContainer = document.getElementById('realContainer');
          if (liveContainer) {
            liveContainer.classList.add('hover-active');
          }
          window.chrome.webview.postMessage(JSON.stringify({type:'candidatePointerArmed'}));
          return;
        }
        window.chrome.webview.postMessage(JSON.stringify({type:'candidatePointerMotion'}));
        event.stopImmediatePropagation();
      }
    }, true);
  }
})();
)";

constexpr wchar_t kRenderCandidateItemsScript[] = LR"MSIME_JS(
function RenderCandidateItems(root, items) {
  if (!root) return;
  root.querySelectorAll('.row-wrapper').forEach((wrapper, index) => {
    const slot = wrapper.querySelector('.cand-content');
    const item = items[index];
    if (!slot || !item) return;
    const label = document.createElement('span');
    label.textContent = item.text + item.annotation + item.badge;
    if (item.fixedPosition) label.style.color = '#379AD3';
    slot.replaceChildren(label);
    if (item.translation) {
      const translation = document.createElement('span');
      translation.className = 'cand-translation';
      translation.textContent = item.translation;
      slot.append(translation);
    }
  });
}
)MSIME_JS";

// Late additions to the page (translations, cloud/AI/English merges) resize the
// card between two frames of the same input, which reads as flicker even
// though each frame is complete. For one input (preedit + caret + page, the
// key) the card only grows, and shrinks back only when its natural size drops
// below kStickyShrinkRatio of the kept one (the page really got smaller). A new
// key — another keystroke, caret move or page turn — is a different page and
// starts from its natural size; carrying the old size over leaves blank rows.
// Runs in the same task as the content update, so the frame is laid out once
// with the final size; the native region measures the same box and follows. An
// empty update (hide) resets. Mirrors kStickyCardShrinkRatio in the D2D
// candidate presenter.
constexpr wchar_t kStickyCandidateCardScript[] = LR"(
window.MsimeStickyCandidateCard = function (reset, key) {
  const kStickyShrinkRatio = 0.7;
  const box = document.getElementById('realContainer');
  if (!box) return;
  const measure = document.getElementById('measureContainer');
  const boxes = measure ? [box, measure] : [box];
  boxes.forEach(function (b) { b.style.minWidth = ''; b.style.minHeight = ''; });
  if (reset) { window.__msimeStickyCard = null; return; }
  box.style.boxSizing = 'border-box';
  const rect = box.getBoundingClientRect();
  const previous = window.__msimeStickyCard;
  const kept = previous && previous.key === key ? previous : {width: 0, height: 0};
  const follow = function (keptValue, natural) {
    return natural >= keptValue || natural < keptValue * kStickyShrinkRatio ? natural : keptValue;
  };
  let width = follow(kept.width, rect.width);
  const maxWidth = Number.parseFloat(box.style.maxWidth);
  if (maxWidth > 0) width = Math.min(width, maxWidth);
  const height = follow(kept.height, rect.height);
  window.__msimeStickyCard = {key: key, width: width, height: height};
  if (width > rect.width + 0.5 || height > rect.height + 0.5) {
    boxes.forEach(function (b) {
      b.style.minWidth = width + 'px';
      b.style.minHeight = height + 'px';
    });
  }
};
)";

VOID CALLBACK CandidateHoverArmTimerProc(HWND, UINT, UINT_PTR, DWORD)
{
    MaybeArmCandidatePointerHover();
}
} // namespace

void DisarmCandidatePointerHover()
{
    g_candidate_hover_armed = false;
    g_candidate_hover_disarm_tick = GetTickCount64();
    if (!GetPhysicalCursorPos(&g_candidate_hover_cursor))
    {
        GetCursorPos(&g_candidate_hover_cursor);
    }
    CAND_WEBVIEW_TRACE_LOGF(L"candidate-hover disarm tick={} native_cursor=({},{}) shown={}",
                            g_candidate_hover_disarm_tick, g_candidate_hover_cursor.x, g_candidate_hover_cursor.y,
                            ::is_global_wnd_cand_shown);
    if (!::global_hwnd)
    {
        return;
    }
    if (::is_global_wnd_cand_shown)
    {
        SetTimer(::global_hwnd, kCandidateHoverArmTimerId, 32, CandidateHoverArmTimerProc);
    }
    else
    {
        KillTimer(::global_hwnd, kCandidateHoverArmTimerId);
    }
}

void MaybeArmCandidatePointerHover()
{
    if (g_candidate_hover_armed || !::is_global_wnd_cand_shown || !webviewCandWnd)
    {
        if (!::is_global_wnd_cand_shown && ::global_hwnd)
        {
            KillTimer(::global_hwnd, kCandidateHoverArmTimerId);
        }
        return;
    }
    POINT now{};
    if (!GetPhysicalCursorPos(&now))
    {
        GetCursorPos(&now);
    }
    const int dx = now.x - g_candidate_hover_cursor.x;
    const int dy = now.y - g_candidate_hover_cursor.y;
    if (dx * dx + dy * dy < kCandidateHoverArmDistancePx * kCandidateHoverArmDistancePx)
    {
        return;
    }
    g_candidate_hover_armed = true;
    CAND_WEBVIEW_TRACE_LOGF(L"candidate-hover arm tick={} baseline=({},{}) native_cursor=({},{}) delta=({},{})",
                            GetTickCount64(), g_candidate_hover_cursor.x, g_candidate_hover_cursor.y, now.x, now.y, dx,
                            dy);
    if (::global_hwnd)
    {
        KillTimer(::global_hwnd, kCandidateHoverArmTimerId);
    }
    webviewCandWnd->ExecuteScript(LR"(document.documentElement.classList.add('msime-hover-armed');
const c = document.getElementById('realContainer');
if (c) { c.classList.add('hover-active'); })",
                                  nullptr);
}

void ResetContainerHoverCandWnd(ComPtr<ICoreWebView2> webview)
{
    DisarmCandidatePointerHover();
    if (webview != nullptr)
    {
        webview->ExecuteScript(kInstallCandidateHoverLockScript, nullptr);
    }
}

constexpr wchar_t kEnsureApplyCandidateFrameScript[] = LR"(
window.ApplyCandidateFrame = function (payload) {
    const container = document.getElementById('realContainer');
    const parent = document.getElementById('realContainerParent');
    if (!container) return {width: 0, height: 0};
    if (payload.resetHover && window.ClearState) window.ClearState();
    if (parent && payload.applyMargins) {
      if (payload.marginTop != null) parent.style.marginTop = payload.marginTop + 'px';
      if (payload.marginLeft != null) parent.style.marginLeft = payload.marginLeft + 'px';
    }
    if (window.SetCandidatePreeditVisible) {
      window.SetCandidatePreeditVisible(payload.preeditVisible !== false);
    }
    const preedit = container.querySelector('.pinyin .text');
    if (preedit) {
      preedit.textContent = payload.preedit || '';
      if (window.SetPreeditCaret) window.SetPreeditCaret();
    }
    const items = Array.isArray(payload.items) ? payload.items : [];
    const wrappers = container.querySelectorAll('.row-wrapper');
    let lastVisible = -1;
    wrappers.forEach(function (wrapper, index) {
      if (index < items.length && String(items[index] || '')) lastVisible = index;
    });
    wrappers.forEach(function (wrapper, index) {
      const html = index < items.length ? String(items[index] || '') : '';
      wrapper.style.display = html ? '' : 'none';
      // Unused rows stay in the DOM, so :last-child cannot find the last shown row.
      wrapper.classList.toggle('last-visible', index === lastVisible);
      const cand = wrapper.querySelector('.cand');
      if (!cand) return;
      let slot = cand.querySelector('.cand-content');
      if (!slot) {
        const text = cand.querySelector('.text') || cand;
        slot = document.createElement('span');
        slot.className = 'cand-content';
        const num = text.querySelector('.num, .cand-no');
        if (num) {
          while (num.nextSibling) text.removeChild(num.nextSibling);
          text.appendChild(slot);
        } else {
          text.appendChild(slot);
        }
      }
      slot.innerHTML = html;
    });
    if (window.SetCandidateSelection) {
      window.SetCandidateSelection(payload.selected || 0);
    }
    void container.offsetWidth;
    const rect = container.getBoundingClientRect();
    // scrollWidth must be part of this: rect/offsetWidth report the clipped box
    // once .container{overflow-x:hidden} bites, so without it the native region
    // is built from a width narrower than what is actually painted.
    return {
      width: Math.max(rect.width, container.offsetWidth || 0, container.scrollWidth || 0) + 1,
      height: Math.max(rect.height, container.offsetHeight || 0) + 1,
      rw: rect.width, ow: container.offsetWidth || 0,
      sw: container.scrollWidth || 0, cw: container.clientWidth || 0
    };
};
// Do NOT force white-space:nowrap on .container/.row-wrapper here. The skin CSS
// wraps the inline-block candidates at --msime-max-width-dip; pinning them to a
// single line makes the card outgrow that cap, and .container's overflow-x:hidden
// then slices the last candidate mid-glyph instead of flowing it onto a new row.
)";

std::pair<double, double> g_last_candidate_slot_measured_size{};
bool g_candidate_slot_api_installed = false;

namespace
{
bool g_candidate_slot_update_inflight = false;
std::wstring g_candidate_slot_update_pending;
std::function<void()> g_candidate_slot_update_pending_complete;
bool g_candidate_slot_update_pending_content_only = false;

void SubmitCandidateSlotScript(ComPtr<ICoreWebView2> webview, const std::wstring &payload,
                               std::function<void()> onComplete, bool contentOnly);

void FlushPendingCandidateSlotUpdate(ComPtr<ICoreWebView2> webview)
{
    if (g_candidate_slot_update_pending.empty())
    {
        return;
    }
    std::wstring pending = std::move(g_candidate_slot_update_pending);
    std::function<void()> complete = std::move(g_candidate_slot_update_pending_complete);
    const bool pendingContentOnly = g_candidate_slot_update_pending_content_only;
    g_candidate_slot_update_pending.clear();
    g_candidate_slot_update_pending_complete = nullptr;
    g_candidate_slot_update_pending_content_only = false;
    SubmitCandidateSlotScript(webview, pending, std::move(complete), pendingContentOnly);
}

void SubmitCandidateSlotScript(ComPtr<ICoreWebView2> webview, const std::wstring &payload,
                               std::function<void()> onComplete, bool contentOnly)
{
    g_candidate_slot_update_inflight = true;
    if (!contentOnly)
    {
        DisarmCandidatePointerHover();
    }
    std::wstring script;
    script.reserve(payload.size() + 2048);
    if (!g_candidate_slot_api_installed)
    {
        script.append(kEnsureApplyCandidateFrameScript);
        g_candidate_slot_api_installed = true;
    }
    script.append(L"window.msimeCandidateDiagnostics = ");
    script.append(GetConfiguredDiagnosticLogEnabled() ? L"true;\n" : L"false;\n");
    script.append(L"window.ApplyCandidateFrame(");
    script.append(payload);
    script.append(L");");

    const HRESULT submitHr = webview->ExecuteScript(
        script.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>([webview, onComplete](HRESULT errorCode,
                                                                                   LPCWSTR result) -> HRESULT {
            if (FAILED(errorCode))
            {
                CAND_WEBVIEW_TRACE_LOGF(L"candidate-slot execute-failed hr={:#x}", static_cast<unsigned>(errorCode));
            }
            else if (result)
            {
                g_last_candidate_slot_measured_size = ParseDivSize(result);
                // Numbers only (no candidate text): shows painted vs reported width.
                CAND_WEBVIEW_TRACE_LOGF(L"candidate-slot measured_dip=({:.2f},{:.2f}) raw={}",
                                        g_last_candidate_slot_measured_size.first,
                                        g_last_candidate_slot_measured_size.second, result);
            }
            g_candidate_slot_update_inflight = false;
            if (!g_candidate_slot_update_pending.empty())
            {
                FlushPendingCandidateSlotUpdate(webview);
                return S_OK;
            }
            if (onComplete)
            {
                onComplete();
            }
            return S_OK;
        }).Get());
    if (FAILED(submitHr))
    {
        g_candidate_slot_update_inflight = false;
        CAND_WEBVIEW_TRACE_LOGF(L"candidate-slot submit-failed hr={:#x}", static_cast<unsigned>(submitHr));
        if (onComplete)
        {
            onComplete();
        }
    }
}

void UpdateCandidateSlotsWithJavaScript(ComPtr<ICoreWebView2> webview, const std::wstring &payload,
                                        std::function<void()> onComplete, bool contentOnly)
{
    if (!webview)
    {
        if (onComplete)
        {
            onComplete();
        }
        return;
    }
    if (g_candidate_slot_update_inflight)
    {
        g_candidate_slot_update_pending = payload;
        g_candidate_slot_update_pending_complete = std::move(onComplete);
        g_candidate_slot_update_pending_content_only = contentOnly;
        return;
    }
    SubmitCandidateSlotScript(webview, payload, std::move(onComplete), contentOnly);
}

} // namespace

std::pair<double, double> LastCandidateSlotMeasuredSize()
{
    return g_last_candidate_slot_measured_size;
}

void UpdateHtmlContentWithJavaScript(ComPtr<ICoreWebView2> webview, const std::wstring &newContent)
{
    UpdateHtmlContentWithJavaScript(webview, newContent, nullptr);
}

void UpdateHtmlContentWithJavaScript(ComPtr<ICoreWebView2> webview, const std::wstring &newContent,
                                     std::function<void()> onComplete, const CandidateWindowContent *content)
{
    if (!webview)
    {
        if (onComplete)
        {
            onComplete();
        }
        return;
    }

    const std::wstring escaped = EscapeForJsTemplateLiteral(newContent);

    std::wstring script;
    script.reserve(escaped.length() + 2200);

    const bool diagnosticsEnabled = GetConfiguredDiagnosticLogEnabled();
    script.append(L"window.msimeCandidateDiagnostics = ");
    script.append(diagnosticsEnabled ? L"true;\n" : L"false;\n");
    script.append(L"document.getElementById('realContainer').innerHTML = `");
    script.append(escaped);
    script.append(L"`;\n");
    if (content)
    {
        nlohmann::json candidateItems = nlohmann::json::array();
        for (const CandidateViewItem &item : content->page.page_views)
        {
            candidateItems.push_back({{"text", item.text},
                                      {"annotation", item.annotation},
                                      {"badge", item.badge},
                                      {"translation", item.translation},
                                      {"fixedPosition", item.fixed_position}});
        }
        script.append(kRenderCandidateItemsScript);
        script.append(L"RenderCandidateItems(document.getElementById('realContainer'), ");
        script.append(string_to_wstring(candidateItems.dump()));
        script.append(L");\n");
    }
    script.append(L"window.ClearState();\n");
    script.append(kInstallCandidateHoverLockScript);
    DisarmCandidatePointerHover();
    script.append(L"var el = document.getElementById('realContainerParent');\n");
    script.append(L"if (el) {\n");
    script.append(L"  el.style.marginTop = \"");
    script.append(std::to_wstring(Global::MarginTop));
    script.append(L"px\";\n");
    script.append(L"  el.style.marginLeft = \"");
    script.append(std::to_wstring(Global::MarginLeft));
    script.append(L"px\";\n");
    script.append(L"}\n");
    if (content)
    {
        script.append(L"if (window.SetCandidateSelection) { window.SetCandidateSelection(");
        script.append(std::to_wstring(content->page.selected_index_in_page));
        script.append(L"); }\n");
    }
    script.append(L"if (window.SetCandidatePreeditVisible) { window.SetCandidatePreeditVisible(");
    script.append(GetConfiguredCandidateWindowPreeditStyle() == "empty" ? L"false" : L"true");
    script.append(L"); }\n");
    script.append(L"if (window.SetPreeditCaret) { window.SetPreeditCaret(); }\n");
    if (content)
    {
        // 翻页箭头随模板一起被 innerHTML 换掉了，每帧重新标一次可用状态；皮肤关掉箭头时页面只是不显示它。
        script.append(L"if (window.SetCandidatePager) { window.SetCandidatePager(");
        script.append(content->page.has_previous_page ? L"true, " : L"false, ");
        script.append(content->page.has_next_page ? L"true" : L"false");
        script.append(L"); }\n");
    }
    script.append(kStickyCandidateCardScript);
    if (newContent.empty() || !content)
    {
        script.append(L"window.MsimeStickyCandidateCard(true);\n");
    }
    else
    {
        const nlohmann::json stickyKey =
            wstring_to_string(GetPreeditWithCaretMarker()) + "#" + std::to_string(content->page.page_index);
        script.append(L"window.MsimeStickyCandidateCard(false, ");
        script.append(string_to_wstring(stickyKey.dump()));
        script.append(L");\n");
    }
    script.append(L"if (window.CheckContentTruncation) { window.CheckContentTruncation(); }\n");
    if (diagnosticsEnabled)
    {
        script.append(LR"(
(function () {
  window.__msimeCandidateDomRevision = (window.__msimeCandidateDomRevision || 0) + 1;
  const revision = window.__msimeCandidateDomRevision;
  requestAnimationFrame(() => {
    window.chrome.webview.postMessage(JSON.stringify({type:'candidateFrameProbe',data:{
      revision:revision,stage:1,performanceMs:performance.now()
    }}));
    requestAnimationFrame(() => {
      window.chrome.webview.postMessage(JSON.stringify({type:'candidateFrameProbe',data:{
        revision:revision,stage:2,performanceMs:performance.now()
      }}));
    });
  });
})();
)");
    }

    if (!onComplete)
    {
        const HRESULT submitHr = webview->ExecuteScript(script.c_str(), nullptr);
        if (FAILED(submitHr))
        {
            CAND_WEBVIEW_TRACE_LOGF(L"candidate-script submit-failed hr={:#x} callback=false",
                                    static_cast<unsigned>(submitHr));
        }
        return;
    }

    const HRESULT submitHr = webview->ExecuteScript(
        script.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>([onComplete](HRESULT errorCode, LPCWSTR) -> HRESULT {
            if (FAILED(errorCode))
            {
                CAND_WEBVIEW_TRACE_LOGF(L"candidate-script execute-failed hr={:#x} callback=true",
                                        static_cast<unsigned>(errorCode));
            }
            onComplete();
            return S_OK;
        }).Get());
    if (FAILED(submitHr))
    {
        CAND_WEBVIEW_TRACE_LOGF(L"candidate-script submit-failed hr={:#x} callback=true",
                                static_cast<unsigned>(submitHr));
    }
}

void PrepareCandidateWebViewBoundsForMeasure(HWND hwnd)
{
    if (!webviewControllerCandWnd || !hwnd)
    {
        return;
    }
    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    // MarginLeft can push the card nearly a full max-width across the viewport
    // near a screen edge. Reserve that room in DIPs so 200% scaling still has
    // enough CSS pixels; a fixed 1000 physical-px pad is only ~500 DIP at 200%.
    FLOAT scale = GetWebViewRasterizationScale(hwnd);
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }
    const int extraRightDip = ::CANDIDATE_WINDOW_MAX_WIDTH_DIP + ::CANDIDATE_SHADOW_PAD_LEFT +
                              ::CANDIDATE_SHADOW_PAD_RIGHT + ::POP_UP_WND_WIDTH;
    const int extraBottomDip =
        ::CANDIDATE_WINDOW_HEIGHT + ::CANDIDATE_SHADOW_PAD_TOP + ::CANDIDATE_SHADOW_PAD_BOTTOM + ::POP_UP_WND_HEIGHT;
    const int extraRightPx = (std::max)(candidateBoundExtraFloorPx, static_cast<int>(std::ceil(extraRightDip * scale)));
    const int extraBottomPx =
        (std::max)(candidateBoundExtraFloorPx, static_cast<int>(std::ceil(extraBottomDip * scale)));
    bounds.right += extraRightPx;
    bounds.bottom += extraBottomPx;
    const HRESULT hr = webviewControllerCandWnd->put_Bounds(bounds);
    DIAG_LOGF(L"ui-webview-bounds phase=measure client_plus_reserve={}x{} extra_px=({},{}) scale={:.3f} hr={:#x}",
              bounds.right - bounds.left, bounds.bottom - bounds.top, extraRightPx, extraBottomPx,
              static_cast<double>(scale), static_cast<unsigned>(hr));
}

void SyncCandidateWebViewBoundsToHost(HWND hwnd)
{
    if (!webviewControllerCandWnd || !hwnd)
    {
        return;
    }
    // Keep the measure-time reserve instead of shrinking the viewport to the
    // host client. MarginLeft pushes the card towards the right of the viewport
    // near a screen edge, so an exactly-sized viewport leaves the card less
    // room than it was measured with and it wraps into a taller card that the
    // window region then clips.
    PrepareCandidateWebViewBoundsForMeasure(hwnd);
    const HRESULT hr = webviewControllerCandWnd->NotifyParentWindowPositionChanged();
    DIAG_LOGF(L"ui-webview-bounds phase=sync-parent notify_hr={:#x}", static_cast<unsigned>(hr));
}

void LogCandidateLayoutSnapshot(const wchar_t *stage)
{
    (void)stage;
}

//
//
// 候选窗口 webview
//
//

void UpdateMeasureContentWithJavaScript(ComPtr<ICoreWebView2> webview, const std::wstring &newContent,
                                        std::function<void()> onComplete)
{
    if (webview == nullptr)
    {
        if (onComplete)
        {
            onComplete();
        }
        return;
    }

    const std::wstring escaped = EscapeForJsTemplateLiteral(newContent);

    std::wstring script;
    script.reserve(escaped.length() + 256);

    script.append(L"document.getElementById('measureContainer').innerHTML = `");
    script.append(escaped);
    script.append(L"`;\n");
    script.append(L"if (window.SetCandidatePreeditVisible) { window.SetCandidatePreeditVisible(");
    script.append(GetConfiguredCandidateWindowPreeditStyle() == "empty" ? L"false" : L"true");
    script.append(L"); }\n");
    // 测量容器也要带上翻页箭头，量出的卡片尺寸才包含它；不带参数表示不改真实容器里的可用状态。
    script.append(L"if (window.SetCandidatePager) { window.SetCandidatePager(); }\n");

    if (!onComplete)
    {
        webview->ExecuteScript(script.c_str(), nullptr);
        return;
    }

    webview->ExecuteScript(
        script.c_str(), Callback<ICoreWebView2ExecuteScriptCompletedHandler>([onComplete](HRESULT, LPCWSTR) -> HRESULT {
                            onComplete();
                            return S_OK;
                        }).Get());
}

void UpdateMeasureContentWithJavaScript(ComPtr<ICoreWebView2> webview, const std::wstring &newContent)
{
    UpdateMeasureContentWithJavaScript(webview, newContent, nullptr);
}

void DisableMouseForAWhileWhenShownCandWnd(ComPtr<ICoreWebView2> webview)
{
    if (webview != nullptr)
    {
        std::wstring script = LR"(
if (window.mouseBlockTimeout) {
    clearTimeout(window.mouseBlockTimeout);
}

document.documentElement.style.pointerEvents = "none";

window.mouseBlockTimeout = setTimeout(() => {
    document.documentElement.style.pointerEvents = "auto";
    window.mouseBlockTimeout = null;
}, 500);
        )";
        webview->ExecuteScript(script.c_str(), nullptr);
    }
}

std::vector<std::wstring> CandidateTemplateSlots(const std::wstring &preedit, const Global::CandidatePageSnapshot &page,
                                                 bool measuring)
{
    std::wstring visiblePreedit = preedit;
    if (measuring)
    {
        visiblePreedit.erase(std::remove(visiblePreedit.begin(), visiblePreedit.end(), L'\uE000'),
                             visiblePreedit.end());
    }
    std::vector<std::wstring> slots;
    slots.reserve(page.page_views.size() + 1);
    slots.push_back(EscapeCandidateTemplateText(visiblePreedit));
    for (const CandidateViewItem &view : page.page_views)
    {
        slots.push_back(measuring ? string_to_wstring(CandidateViewHtml(view)) : L" ");
    }
    return slots;
}

void InflateCandWnd(const CandidateWindowContent &content, std::function<void()> onComplete)
{
    const std::wstring result =
        InflateCandidateTemplate(BodyStringCandWnd, CandidateTemplateSlots(content.preedit, content.page, false));
    UpdateHtmlContentWithJavaScript(webviewCandWnd, result, std::move(onComplete), &content);
}

void InflateMeasureDivCandWnd(const CandidateWindowContent &content, std::function<void()> onComplete)
{
    const std::wstring result =
        InflateCandidateTemplate(::MeasureStringCandWnd, CandidateTemplateSlots(content.preedit, content.page, true));
    UpdateMeasureContentWithJavaScript(webviewCandWnd, result, std::move(onComplete));
}

/**
 * @brief Handle candidate window webview2 controller creation
 *
 * @param hwnd
 * @param result
 * @param controller
 * @return HRESULT
 */
HRESULT OnControllerCreatedCandWnd(     //
    HWND hwnd,                          //
    HRESULT result,                     //
    ICoreWebView2Controller *controller //
)
{
    CAND_DIAG_LOGF(L"webview controller callback hr={:#x} controller_present={} hwnd_valid={}",
                   static_cast<unsigned>(result), controller != nullptr, IsWindow(hwnd) != FALSE);
    if (!controller || FAILED(result))
    {
        OnSmallWindowControllerSettled(FAILED(result) ? result : E_FAIL);
        return E_FAIL;
    }

    webviewControllerCandWnd = controller;
    const HRESULT getWebviewHr = webviewControllerCandWnd->get_CoreWebView2(webviewCandWnd.GetAddressOf());

    if (!webviewCandWnd)
    {
        CAND_DIAG_LOGF(L"webview get_CoreWebView2 failed hr={:#x}", static_cast<unsigned>(getWebviewHr));
        webviewControllerCandWnd.Reset();
        OnSmallWindowControllerSettled(FAILED(getWebviewHr) ? getWebviewHr : E_FAIL);
        return E_FAIL;
    }

    UpdateSmallWindowWebviewVisibility(hwnd, IsWindowVisible(hwnd) != FALSE);

    // Configure WebView settings
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webviewCandWnd->get_Settings(&settings)))
    {
        settings->put_IsScriptEnabled(TRUE);
        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        settings->put_IsWebMessageEnabled(TRUE);
        settings->put_AreHostObjectsAllowed(FALSE); // Since we only use WebMessages
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);

        // Try to disable browser accelerators (Ctrl+R, F5, etc.)
        ComPtr<ICoreWebView2Settings3> settings3;
        if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&settings3))))
        {
            settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
            // settings3->put_IsPinchZoomEnabled(FALSE); // Unsupported in this header version
        }

        // Try to disable autofill and password saving
        ComPtr<ICoreWebView2Settings5> settings5;
        if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&settings5))))
        {
            settings5->put_IsGeneralAutofillEnabled(FALSE);
            settings5->put_IsPasswordAutosaveEnabled(FALSE);
        }

        // Try to disable built-in error pages for a cleaner UI
        ComPtr<ICoreWebView2Settings6> settings6;
        if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&settings6))))
        {
            settings6->put_IsBuiltInErrorPageEnabled(FALSE);
        }
    }

    webviewControllerCandWnd->put_ZoomFactor(1.0);

    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&webviewController3CandWnd))))
    {
        // Let WebView2 track both monitor DPI and the user's accessibility text
        // scale. Every native clip/layout conversion reads this same value.
        const HRESULT detectHr = webviewController3CandWnd->put_ShouldDetectMonitorScaleChanges(TRUE);
        double rasterizationScale = 0.0;
        const HRESULT scaleHr = webviewController3CandWnd->get_RasterizationScale(&rasterizationScale);
        const HRESULT eventHr = webviewController3CandWnd->add_RasterizationScaleChanged(
            Callback<ICoreWebView2RasterizationScaleChangedEventHandler>([hwnd](ICoreWebView2Controller *sender,
                                                                                IUnknown *) -> HRESULT {
                if (!IsWindow(hwnd))
                {
                    return S_OK;
                }
                ComPtr<ICoreWebView2Controller3> controller3;
                double scale = 0.0;
                const HRESULT hr = sender ? sender->QueryInterface(IID_PPV_ARGS(&controller3)) : E_POINTER;
                const HRESULT scaleHr = SUCCEEDED(hr) ? controller3->get_RasterizationScale(&scale) : hr;
                DIAG_LOGF(L"candidate rasterization scale changed scale={:.4f} native_scale={:.4f} hr={:#x}", scale,
                          static_cast<double>(GetWindowScale(hwnd)), static_cast<unsigned>(scaleHr));
                InjectSurfaceViewportLimits(webviewCandWnd.Get(), hwnd);
                if (::is_global_wnd_cand_shown && IsCandidateWebviewReady())
                {
                    FineTuneWindow(hwnd);
                }
                return S_OK;
            }).Get(),
            &candidateRasterizationScaleChangedToken);
        candidateRasterizationScaleChangedRegistered = SUCCEEDED(eventHr);
        DIAG_LOGF(L"candidate rasterization scale initialized scale={:.4f} native_scale={:.4f} "
                  L"scale_hr={:#x} detect_hr={:#x} event_hr={:#x}",
                  rasterizationScale, static_cast<double>(GetWindowScale(hwnd)), static_cast<unsigned>(scaleHr),
                  static_cast<unsigned>(detectHr), static_cast<unsigned>(eventHr));
    }

    // Configure virtual host path
    if (SUCCEEDED(webviewCandWnd->QueryInterface(IID_PPV_ARGS(&webview3CandWnd))))
    {
        const auto contractsPath =
            std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"html" / L"webview2" / L"shared";
        webview3CandWnd->SetVirtualHostNameToFolderMapping(L"msime-contracts", contractsPath.wstring().c_str(),
                                                           COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);

        const std::wstring assetPath = fmt::format( //
            L"{}\\html\\webview2\\candwnd",         //
            CommonUtils::get_ime_data_path_w()      //
        );

        // Assets mapping
        webview3CandWnd->SetVirtualHostNameToFolderMapping(  //
            L"candwnd",                                      //
            assetPath.c_str(),                               //
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS //
        );                                                   //
        const std::wstring skinsPath = fmt::format(          //
            L"{}\\skins",                                    //
            CommonUtils::get_ime_data_path_w()               //
        );
        const HRESULT skinsMappingHr = webview3CandWnd->SetVirtualHostNameToFolderMapping(
            L"candidate-skins", skinsPath.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        (void)skinsMappingHr;
        (void)0;
    }

    // Set transparent background
    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&webviewController2CandWnd))))
    {
        COREWEBVIEW2_COLOR backgroundColor = {0, 0, 0, 0};
        webviewController2CandWnd->put_DefaultBackgroundColor(backgroundColor);
    }

    // Adjust to window size — keep the same DIP-based measure reserve used by
    // FineTune so the first layout is not constrained by a narrow HWND.
    PrepareCandidateWebViewBoundsForMeasure(hwnd);

    // Navigate to HTML
    if (HTMLStringCandWnd.empty())
    {
        PrepareHtmlForWnds();
    }
    HRESULT hr = webviewCandWnd->NavigateToString(HTMLStringCandWnd.c_str());
    CAND_DIAG_LOGF(L"webview NavigateToString hr={:#x} html_chars={} skin={}", static_cast<unsigned>(hr),
                   HTMLStringCandWnd.size(), string_to_wstring(preparedCandidateSkin));
    if (SUCCEEDED(hr))
    {
        loadedCandidateSkin = preparedCandidateSkin;
    }
    (void)0;
    if (FAILED(hr))
    {
        ShowErrorMessage(hwnd, L"Failed to navigate to string.");
    }

    /* Debug console */
    // webview->OpenDevToolsWindow();

    webviewCandWnd->add_WebMessageReceived(
        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [hwnd](ICoreWebView2 * /*sender*/, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                wil::unique_cotaskmem_string message;
                HRESULT hr = args->TryGetWebMessageAsString(&message);

                if (SUCCEEDED(hr) && message.get())
                {
                    std::wstring msg(message.get());
                    // 解析 msg，执行相应操作
                    try
                    {
                        json::value val = json::parse(wstring_to_string(msg));
                        if (!metasequoia::webview::Validate(val, "client", "candidate"))
                            return S_OK;
                        std::string type = json::value_to<std::string>(val.at("type"));
                        if (type == "candidateFrameProbe")
                        {
                            const auto &data = val.at("data");
                            const int revision = json::value_to<int>(data.at("revision"));
                            const int stage = json::value_to<int>(data.at("stage"));
                            const double performanceMs = json::value_to<double>(data.at("performanceMs"));
                            RECT rect{};
                            GetWindowRect(hwnd, &rect);
                            CAND_WEBVIEW_TRACE_LOGF(
                                L"candidate-browser-frame revision={} stage={} performance_ms={:.3f} native_tick={} "
                                L"hwnd_rect=({},{},{}x{}) shown={} window_visible={}",
                                revision, stage, performanceMs, GetTickCount64(), rect.left, rect.top,
                                rect.right - rect.left, rect.bottom - rect.top, ::is_global_wnd_cand_shown,
                                IsWindowVisible(hwnd) != FALSE);
                        }
                        else if (type == "candidatePointerArmed")
                        {
                            g_candidate_hover_armed = true;
                            if (::global_hwnd)
                            {
                                KillTimer(::global_hwnd, kCandidateHoverArmTimerId);
                            }
                            CAND_WEBVIEW_TRACE_LOGF(L"candidate-hover arm source=dom-motion tick={}", GetTickCount64());
                        }
                        else if (type == "candidatePointerMotion")
                        {
                            // A WebView mousemove can be synthesized when the
                            // candidate HWND moves under a stationary cursor.
                            // The native screen-coordinate comparison arms only
                            // when the physical pointer itself really moved.
                            MaybeArmCandidatePointerHover();
                        }
                        else if (type == "candidatePointerProbe")
                        {
                            const auto &data = val.at("data");
                            POINT cursor{};
                            GetCursorPos(&cursor);
                            RECT rect{};
                            GetWindowRect(hwnd, &rect);
                            CAND_WEBVIEW_TRACE_LOGF(
                                L"candidate-pointer probe={} event_screen=({},{}) event_client=({},{}) "
                                L"movement=({},{}) js_armed={} native_cursor=({},{}) baseline=({},{}) "
                                L"native_armed={} hwnd=({},{},{}x{}) tick={}",
                                json::value_to<int>(data.at("probe")), json::value_to<int>(data.at("screenX")),
                                json::value_to<int>(data.at("screenY")), json::value_to<int>(data.at("clientX")),
                                json::value_to<int>(data.at("clientY")), json::value_to<int>(data.at("movementX")),
                                json::value_to<int>(data.at("movementY")), json::value_to<bool>(data.at("armed")),
                                cursor.x, cursor.y, g_candidate_hover_cursor.x, g_candidate_hover_cursor.y,
                                g_candidate_hover_armed, rect.left, rect.top, rect.right - rect.left,
                                rect.bottom - rect.top, GetTickCount64());
                        }
                        else if (type == "delete")
                        {
                            int idx = json::value_to<int>(val.at("data"));
                            if (FanyImeIpc::IsValidCandidateUiOneBasedIndex(idx))
                            {
                                PostMessage(::global_hwnd, WM_DELETE_CANDIDATE, idx, 0);
                            }
                        }
                        else if (type == "pin")
                        {
                            int idx = json::value_to<int>(val.at("data"));
                            if (FanyImeIpc::IsValidCandidateUiOneBasedIndex(idx))
                            {
                                PostMessage(::global_hwnd, WM_PIN_TO_TOP_CANDIDATE, idx, 0);
                            }
                        }
                        else if (type == "fixPosition")
                        {
                            int idx = json::value_to<int>(val.at("data").at("index"));
                            int position = json::value_to<int>(val.at("data").at("position"));
                            if (FanyImeIpc::IsValidCandidateUiOneBasedIndex(idx) && position >= 1 && position <= 5)
                                PostMessage(::global_hwnd, WM_FIX_CANDIDATE_POSITION, idx, position);
                        }
                        else if (type == "clearPosition")
                        {
                            int idx = json::value_to<int>(val.at("data"));
                            if (FanyImeIpc::IsValidCandidateUiOneBasedIndex(idx))
                                PostMessage(::global_hwnd, WM_CLEAR_CANDIDATE_POSITION, idx, 0);
                        }
                        else if (type == "candidateWheel")
                        {
                            const int delta = json::value_to<int>(val.at("data"));
                            if (!::is_global_wnd_cand_shown)
                            {
                                // The page can post a wheel message that was in
                                // flight when the candidate window went away.
                                g_candidate_wheel_accumulator = 0;
                            }
                            else
                            {
                                const CandidateWheel::PagingSteps steps = CandidateWheel::ConsumeWheelDelta(
                                    g_candidate_wheel_accumulator, delta, WHEEL_DELTA);
                                // Same WM_PAGE_CANDIDATE route as the D2D window,
                                // which is also where the setting is checked.
                                if (steps.page_up > 0)
                                {
                                    PostMessage(::global_hwnd, WM_PAGE_CANDIDATE, CANDIDATE_PAGE_PREVIOUS,
                                                steps.page_up);
                                }
                                if (steps.page_down > 0)
                                {
                                    PostMessage(::global_hwnd, WM_PAGE_CANDIDATE, CANDIDATE_PAGE_NEXT, steps.page_down);
                                }
                            }
                        }
                        else if (type == "candidatePage")
                        {
                            // 翻页箭头，与 D2D 窗口走同一条 WM_PAGE_CANDIDATE_ARROW。
                            // 方向已由契约限定为 previous / next。
                            if (::is_global_wnd_cand_shown)
                            {
                                const bool next = json::value_to<std::string>(val.at("data")) == "next";
                                PostMessage(::global_hwnd, WM_PAGE_CANDIDATE_ARROW,
                                            next ? CANDIDATE_PAGE_NEXT : CANDIDATE_PAGE_PREVIOUS, 1);
                            }
                        }
                        else if (type == "contextMenuResize")
                        {
                            const auto &data = val.at("data");
                            const int width = json::value_to<int>(data.at("width"));
                            const int height = json::value_to<int>(data.at("height"));
                            int top_expansion = 0;
                            if (const auto *value = data.as_object().if_contains("topExpansion"))
                                top_expansion = (std::max)(0, json::value_to<int>(*value));
                            int left_expansion = 0;
                            if (const auto *value = data.as_object().if_contains("leftExpansion"))
                                left_expansion = (std::max)(0, json::value_to<int>(*value));
                            RECT window_rect{};
                            GetWindowRect(hwnd, &window_rect);
                            const FLOAT scale = GetWebViewRasterizationScale(hwnd);
                            const int current_width = static_cast<int>(window_rect.right - window_rect.left);
                            const int current_height = static_cast<int>(window_rect.bottom - window_rect.top);
                            const int top_expansion_px = static_cast<int>(std::ceil(top_expansion * scale));
                            const int left_expansion_px = static_cast<int>(std::ceil(left_expansion * scale));
                            SetWindowPos(hwnd, nullptr, window_rect.left - left_expansion_px,
                                         window_rect.top - top_expansion_px,
                                         (std::max)(current_width, static_cast<int>(std::ceil(width * scale))) +
                                             left_expansion_px,
                                         (std::max)(current_height, static_cast<int>(std::ceil(height * scale))) +
                                             top_expansion_px,
                                         SWP_NOZORDER | SWP_NOACTIVATE);
                            // FineTuneWindow clips the stable candidate host to
                            // the card. The context menu is allowed to use the
                            // temporarily expanded host and FineTuneWindow
                            // reapplies the tight region after it closes.
                            SetWindowRgn(hwnd, nullptr, TRUE);
                            RECT bounds{};
                            GetClientRect(hwnd, &bounds);
                            webviewControllerCandWnd->put_Bounds(bounds);
                            if (top_expansion > 0 || left_expansion > 0)
                            {
                                const std::wstring script = L"if(window.ApplyContextMenuTopExpansion)"
                                                            L"window.ApplyContextMenuTopExpansion(" +
                                                            std::to_wstring(top_expansion) +
                                                            L");if(window.ApplyContextMenuLeftExpansion)"
                                                            L"window.ApplyContextMenuLeftExpansion(" +
                                                            std::to_wstring(left_expansion) + L");";
                                webviewCandWnd->ExecuteScript(script.c_str(), nullptr);
                            }
                        }
                        else if (type == "contextMenuClosed")
                        {
                            const std::wstring preedit = GetConfiguredCandidateWindowPreeditStyle() == "empty"
                                                             ? std::wstring{}
                                                             : GetPreeditWithCaretMarker();
                            const Global::CandidatePageSnapshotPtr page = Global::LoadCandidatePageSnapshot();
                            CandidateWindowContent measurement{preedit, *page};
                            InflateMeasureDivCandWnd(measurement, [hwnd]() { FineTuneWindow(hwnd); });
                        }
                        else if (type == "candidate")
                        {
                            int idx = json::value_to<int>(val.at("data"));
                            if (FanyImeIpc::IsValidCandidateUiOneBasedIndex(idx))
                            {
                                PostMessage(::global_hwnd, WM_COMMIT_CANDIDATE, idx, 0);
                            }
                        }
                        else if (type == "contentTruncated")
                        {
                            // Candidate host is intentionally quarter-screen; the card is
                            // placed with CSS margins + SetWindowRgn. HTML truncation checks
                            // use clientWidth/Height, so margin+content past the right/bottom
                            // of the host looks "truncated" even when FineTune is correct.
                            // Never shrink the HWND or zero margins here — that parked the
                            // card at the host origin away from the caret.
                            if (::is_global_wnd_cand_shown)
                            {
                                const bool grew = HandleContentTruncatedMessage(hwnd, webviewCandWnd.Get(),
                                                                                webviewControllerCandWnd.Get(), val,
                                                                                g_last_content_truncation_cand_ms, 0);
                                if (grew)
                                {
                                    // Host grew beyond the previous region; drop the clip so
                                    // the next FineTune/clip-measure can reapply margins.
                                    SetWindowRgn(hwnd, nullptr, TRUE);
                                    CAND_DIAG_LOGF(L"candidate contentTruncated grew host; region cleared "
                                                   L"(margins kept) {}",
                                                   DescribeCandidateHostState());
                                }
                                else
                                {
                                    CAND_DIAG_LOGF(L"candidate contentTruncated ignored (no shrink) {}",
                                                   DescribeCandidateHostState());
                                }
                            }
                        }
                    }
                    catch (const std::exception &)
                    {
                        // A malformed message from the page must not tear down the host.
                        return S_OK;
                    }
                }
                return S_OK;
            })
            .Get(),
        nullptr);

    webviewCandWnd->add_NavigationCompleted(
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [hwnd](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                BOOL success = FALSE;
                COREWEBVIEW2_WEB_ERROR_STATUS errorStatus = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                const HRESULT successHr = args->get_IsSuccess(&success);
                const HRESULT statusHr = args->get_WebErrorStatus(&errorStatus);
                CAND_DIAG_LOGF(L"webview navigation completed success={} success_hr={:#x} status_hr={:#x} "
                               L"web_error={} logical_shown={}",
                               success != FALSE, static_cast<unsigned>(successHr), static_cast<unsigned>(statusHr),
                               static_cast<int>(errorStatus), ::is_global_wnd_cand_shown);
                if (success)
                {
                    g_candidate_slot_api_installed = false;
                    NotifySmallWindowNavigationReady(candidateNavigationReady, L"candidate");
                    ApplyConfiguredCandidateAppearance();
                    InjectSurfaceViewportLimits(webviewCandWnd.Get(), hwnd);
                    webviewCandWnd->ExecuteScript(kInstallCandidateHoverLockScript, nullptr);
                    DisarmCandidatePointerHover();
                }
                else
                {
                    (void)0;
                    if (::is_global_wnd_cand_shown)
                        SetCandidateHostCloaked(false);
                }
                if (success && ::is_global_wnd_cand_shown)
                {
                    const std::wstring preedit = GetConfiguredCandidateWindowPreeditStyle() == "empty"
                                                     ? std::wstring{}
                                                     : GetPreeditWithCaretMarker();
                    const Global::CandidatePageSnapshotPtr page = Global::LoadCandidatePageSnapshot();
                    CandidateWindowContent str{preedit, *page};
                    InflateMeasureDivCandWnd(str, [hwnd]() {
                        if (!::is_global_wnd_cand_shown)
                        {
                            return;
                        }
                        FineTuneWindow(hwnd);
                    });
                }
                return S_OK;
            })
            .Get(),
        nullptr);

    webviewCandWnd->add_ProcessFailed(Microsoft::WRL::Callback<ICoreWebView2ProcessFailedEventHandler>(
                                          [](ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *args) -> HRESULT {
                                              COREWEBVIEW2_PROCESS_FAILED_KIND kind =
                                                  COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                                              const HRESULT hr = args->get_ProcessFailedKind(&kind);
                                              CAND_DIAG_LOGF(L"webview process failed kind={} hr={:#x}",
                                                             static_cast<int>(kind), static_cast<unsigned>(hr));
                                              return S_OK;
                                          })
                                          .Get(),
                                      nullptr);

    OnSmallWindowControllerSettled(S_OK);
    return S_OK;
}

/**
 * @brief Handle candidate window webview2 environment creation
 *
 * @param hwnd
 * @param result
 * @param env
 * @return HRESULT
 */
HRESULT OnEnvironmentCreated(HWND hwnd, HRESULT result, ICoreWebView2Environment *env)
{
    (void)0;
    if (FAILED(result) || !env)
    {
        ShowErrorMessage(hwnd, L"Failed to create WebView2 environment.");
        return result;
    }

    // Create WebView2 controller
    const HRESULT hr = env->CreateCoreWebView2Controller(                                    //
        hwnd,                                                                                //
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>( //
            [hwnd](HRESULT result,                                                           //
                   ICoreWebView2Controller *controller) -> HRESULT {                         //
                return OnControllerCreatedCandWnd(hwnd, result, controller);                 //
            })                                                                               //
            .Get()                                                                           //
    );                                                                                       //
    (void)0;
    return hr;
}
