#include "voice_result_card.h"
#include "mvi_utils.h"
#include "config/ime_config.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <dwmapi.h>

namespace
{
constexpr wchar_t kClassName[] = L"MviVoiceResultCardWindow";
constexpr UINT kModelChangedMessage = WM_APP + 190;
constexpr UINT kAnchorChangedMessage = WM_APP + 191;
constexpr UINT_PTR kAnimationTimerId = 1;
constexpr UINT kAnimationTickMs = 16;
constexpr UINT kCopyAutoCloseDelayMs = 1000;

// Proportions in DIP. The card keeps a calm centered result: a quiet reassurance line, a
// two-line preview, and one primary copy action. The full transcript stays in the model for
// Copy and can still be scrolled in place.
constexpr wchar_t kTitleText[] = L"转录已保留";
constexpr float kCardPreferredWidth = 390.0f;
constexpr float kHorizontalPadding = 30.0f;
constexpr float kTopPadding = 24.0f;
constexpr float kHeaderHeight = 26.0f;
constexpr float kHeaderIconSize = 13.0f;
constexpr float kHeaderGroupGap = 6.0f;
constexpr float kHeaderTitleFallbackWidth = 60.0f;
constexpr float kBodyGap = 14.0f;
constexpr float kBodyTop = kTopPadding + kHeaderHeight + kBodyGap; // 64 DIP
constexpr float kBodyFontSize = 14.5f;
constexpr float kBodyLineStep = 24.07f; // 1.66 line height at 14.5 px.
constexpr float kBodyMinHeight = kBodyLineStep;
constexpr float kBodyMaxHeight = 2.0f * kBodyLineStep; // 48.14 DIP
constexpr float kFooterGap = 24.0f;
constexpr float kButtonHeight = 36.0f;
constexpr float kFooterHeight = kButtonHeight;
constexpr float kBottomPadding = 24.0f;
constexpr float kChromeHeight = kBodyTop + kFooterGap + kFooterHeight + kBottomPadding; // 148 DIP
constexpr float kCornerRadius = 20.0f;
constexpr float kCloseTargetSize = 28.0f;
constexpr float kCloseEdgeInset = 12.0f;
constexpr float kCloseGlyphSize = 13.0f;
constexpr float kCloseGlyphInset = 2.0f;
constexpr float kCloseGlyphHalf = kCloseGlyphSize * 0.5f - kCloseGlyphInset;
constexpr float kButtonPreferredWidth = 188.0f;
constexpr float kButtonIconSpan = 15.0f;
constexpr float kButtonContentGap = 7.0f;
constexpr float kMaxLayoutHeight = 100000.0f;
constexpr ULONGLONG kEntranceDurationMs = 180;
constexpr float kEntranceLiftDip = 8.0f;

struct Palette
{
    D2D1_COLOR_F panel;
    D2D1_COLOR_F border;
    D2D1_COLOR_F text;
    D2D1_COLOR_F secondary;
    D2D1_COLOR_F header;
    D2D1_COLOR_F hover;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F primary;
    D2D1_COLOR_F primary_hover;
    D2D1_COLOR_F on_primary;
};

Palette PaletteFor(bool light)
{
    if (light)
    {
        return Palette{
            D2D1::ColorF(1.0f, 1.0f, 1.0f),       D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.09f),
            D2D1::ColorF(0.090f, 0.098f, 0.114f), D2D1::ColorF(0.380f, 0.404f, 0.443f),
            D2D1::ColorF(0.529f, 0.553f, 0.592f), D2D1::ColorF(0.945f, 0.949f, 0.957f),
            D2D1::ColorF(0.184f, 0.416f, 0.345f), D2D1::ColorF(0.106f, 0.114f, 0.129f),
            D2D1::ColorF(0.165f, 0.176f, 0.196f), D2D1::ColorF(1.0f, 1.0f, 1.0f),
        };
    }
    return Palette{
        D2D1::ColorF(0.098f, 0.106f, 0.125f), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f),
        D2D1::ColorF(0.945f, 0.949f, 0.957f), D2D1::ColorF(0.702f, 0.722f, 0.757f),
        D2D1::ColorF(0.529f, 0.557f, 0.600f), D2D1::ColorF(0.180f, 0.192f, 0.220f),
        D2D1::ColorF(0.612f, 0.804f, 0.741f), D2D1::ColorF(0.945f, 0.949f, 0.957f),
        D2D1::ColorF(0.886f, 0.898f, 0.914f), D2D1::ColorF(0.071f, 0.078f, 0.094f),
    };
}

bool SystemAnimationsEnabled()
{
    BOOL enabled = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0))
        return true;
    return enabled != FALSE;
}

template <typename T> void safe_release(T **obj)
{
    if (*obj)
    {
        (*obj)->Release();
        *obj = nullptr;
    }
}

// 剪贴板操作只在显式 Copy 点击（UI 线程）时发生。先备好完整的 UTF-16 内存（含结尾 NUL），
// 再打开剪贴板提交；分配、打开或提交任一步失败都保留卡片全文，并释放尚未转移所有权的内存。
bool CopyToClipboard(HWND hwnd, const std::wstring &text)
{
    if (text.empty())
        return false;

    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!handle)
        return false;

    wchar_t *dst = static_cast<wchar_t *>(GlobalLock(handle));
    if (!dst)
    {
        GlobalFree(handle);
        return false;
    }
    memcpy(dst, text.c_str(), bytes);
    GlobalUnlock(handle);

    bool copied = false;
    if (OpenClipboard(hwnd))
    {
        if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, handle) != nullptr)
            copied = true; // Ownership transferred to the system on success.
        CloseClipboard();
    }
    if (!copied)
        GlobalFree(handle); // The system never took ownership when any step failed.
    return copied;
}
} // namespace

VoiceResultCard::VoiceResultCard() = default;

VoiceResultCard::~VoiceResultCard()
{
    shutdown();
}

bool VoiceResultCard::init(HINSTANCE instance)
{
    if (hwnd_)
    {
        return true;
    }

    instance_ = instance;

    const HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_);
    if (FAILED(hr))
    {
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown **>(&write_factory_))) ||
        FAILED(write_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-cn",
                                                &title_text_format_)) ||
        FAILED(write_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, kBodyFontSize,
                                                L"zh-cn", &body_text_format_)) ||
        FAILED(write_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-cn",
                                                &button_text_format_)))
    {
        shutdown();
        return false;
    }
    title_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    title_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    body_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    body_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    body_text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    body_text_format_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, kBodyLineStep, 18.5f);
    button_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    button_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // Measure the fixed reassurance label once so the check mark and text can be centered as
    // one optical group instead of centering the text alone.
    IDWriteTextLayout *title_layout = nullptr;
    if (SUCCEEDED(write_factory_->CreateTextLayout(kTitleText, static_cast<UINT32>(wcslen(kTitleText)),
                                                   title_text_format_, 300.0f, kHeaderHeight, &title_layout)) &&
        title_layout)
    {
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(title_layout->GetMetrics(&metrics)) && metrics.width > 0.0f)
            title_label_width_dip_ = metrics.width;
        title_layout->Release();
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = VoiceResultCard::wnd_proc;
    wc.hInstance = instance_;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    wc.style = CS_HREDRAW | CS_VREDRAW;

    const ATOM atom = RegisterClassW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        shutdown();
        return false;
    }

    hwnd_ = CreateWindowExW(                                                 //
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, //
        kClassName,                                                          //
        L"",                                                                 //
        WS_POPUP,                                                            //
        0,                                                                   //
        0,                                                                   //
        static_cast<int>(std::lround(card_width_dip_)),                      //
        static_cast<int>(std::lround(card_height_dip_)),                     //
        nullptr,                                                             //
        nullptr,                                                             //
        instance_,                                                           //
        this);

    if (!hwnd_)
    {
        shutdown();
        return false;
    }

    post_hwnd_.store(hwnd_, std::memory_order_release);
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    MARGINS mar = {-1};
    DwmExtendFrameIntoClientArea(hwnd_, &mar);

    // Adopt any desired state that was updated before init.
    apply_desired_model();
    return true;
}

void VoiceResultCard::shutdown()
{
    stop_entrance_animation(false);
    cancel_copy_auto_close();
    post_hwnd_.store(nullptr, std::memory_order_release);
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    release_render_target();
    release_text_layout();
    safe_release(&button_text_format_);
    safe_release(&body_text_format_);
    safe_release(&title_text_format_);
    safe_release(&write_factory_);
    safe_release(&factory_);
}
void VoiceResultCard::set_anchor(const VoiceOverlayAnchor &anchor)
{
    {
        std::lock_guard<std::mutex> lock(anchor_mutex_);
        anchor_ = anchor;
    }
    if (HWND hwnd = post_hwnd_.load(std::memory_order_acquire))
        PostMessageW(hwnd, kAnchorChangedMessage, 0, 0);
}

void VoiceResultCard::reset(std::uint64_t session)
{
    if (model_.reset(session))
    {
        if (HWND hwnd = post_hwnd_.load(std::memory_order_acquire))
            PostMessageW(hwnd, kModelChangedMessage, 0, 0);
    }
}

void VoiceResultCard::show(std::uint64_t session, const std::wstring &text)
{
    if (model_.show(session, text))
    {
        if (HWND hwnd = post_hwnd_.load(std::memory_order_acquire))
            PostMessageW(hwnd, kModelChangedMessage, 0, 0);
    }
}

void VoiceResultCard::close()
{
    if (model_.close())
    {
        if (HWND hwnd = post_hwnd_.load(std::memory_order_acquire))
            PostMessageW(hwnd, kModelChangedMessage, 0, 0);
    }
}

LRESULT CALLBACK VoiceResultCard::wnd_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    VoiceResultCard *self = reinterpret_cast<VoiceResultCard *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE)
    {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        self = reinterpret_cast<VoiceResultCard *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    if (!self)
    {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    return self->handle_message(hwnd, message, wParam, lParam);
}

LRESULT VoiceResultCard::handle_message(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        return 0;
    case WM_SIZE:
        if (render_target_)
        {
            const UINT width = LOWORD(lParam);
            const UINT height = HIWORD(lParam);
            if (width > 0 && height > 0)
                render_target_->Resize(D2D1::SizeU(width, height));
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DPICHANGED:
        // Recompute from the captured anchor, never from whichever window happens to be foreground
        // during recognition. Fixtures without an explicit anchor keep the old fallback.
        stop_entrance_animation(true);
        refresh_anchor_for_display_change();
        update_window_bounds();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DISPLAYCHANGE:
        stop_entrance_animation(true);
        refresh_anchor_for_display_change();
        update_window_bounds();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
        // Theme/setting updates must show up on the card that is already on screen. A work-area
        // change (taskbar move/autohide) is a layout change too: refresh the saved monitor data,
        // never the foreground monitor. Other setting changes only re-read the theme.
        if (message == WM_SETTINGCHANGE && wParam == SPI_SETWORKAREA)
        {
            stop_entrance_animation(true);
            refresh_anchor_for_display_change();
            update_window_bounds();
        }
        // If accessibility settings just turned client-area animations off, finish an in-flight
        // entrance immediately.
        if (!animations_enabled())
            stop_entrance_animation(true);
        release_render_target();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR: {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd, &point);
        Region region{};
        if (hit_test(static_cast<float>(point.x) / scale_x_, static_cast<float>(point.y) / scale_y_, region))
        {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        Region region{};
        if (!hit_test(static_cast<float>(GET_X_LPARAM(lParam)) / scale_x_,
                      static_cast<float>(GET_Y_LPARAM(lParam)) / scale_y_, region))
            region = Region::None;
        if (region != hover_region_)
        {
            hover_region_ = region;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (!tracking_mouse_)
        {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            if (TrackMouseEvent(&track))
                tracking_mouse_ = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_mouse_ = false;
        if (hover_region_ != Region::None)
        {
            hover_region_ = Region::None;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        Region region{};
        if (hit_test(static_cast<float>(GET_X_LPARAM(lParam)) / scale_x_,
                     static_cast<float>(GET_Y_LPARAM(lParam)) / scale_y_, region))
        {
            pressed_region_ = region;
            region_pressed_ = true;
            SetCapture(hwnd);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP: {
        if (!region_pressed_)
            break;
        ReleaseCapture();
        region_pressed_ = false;
        Region region{};
        const bool released_on_same = hit_test(static_cast<float>(GET_X_LPARAM(lParam)) / scale_x_,
                                               static_cast<float>(GET_Y_LPARAM(lParam)) / scale_y_, region) &&
                                      region == pressed_region_;
        if (released_on_same && region == Region::Close)
            dismiss_shown_card();
        else if (released_on_same && region == Region::Copy)
            on_copy_button();
        return 0;
    }
    case WM_CAPTURECHANGED:
        region_pressed_ = false;
        return 0;
    case WM_MOUSEWHEEL: {
        if (!card_visible_ || !text_layout_)
            break;
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(text_layout_->GetMetrics(&metrics)) || metrics.height <= body_height_dip_)
            break;
        const float max_scroll = metrics.height - body_height_dip_;
        const float step = kBodyLineStep * static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        scroll_offset_ = (std::max)(0.0f, (std::min)(max_scroll, scroll_offset_ - step));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_RBUTTONUP:
        dismiss_shown_card();
        return 0;
    case kModelChangedMessage:
        apply_desired_model();
        return 0;
    case kAnchorChangedMessage:
        if (card_visible_)
        {
            stop_entrance_animation(true);
            update_window_bounds();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (wParam == kAnimationTimerId)
        {
            tick_entrance_animation();
            return 0;
        }
        if (copy_close_timer_id_ != 0 && wParam == copy_close_timer_id_)
        {
            on_copy_auto_close_timer();
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        draw();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kAnimationTimerId);
        cancel_copy_auto_close();
        animation_active_ = false;
        post_hwnd_.store(nullptr, std::memory_order_release);
        release_render_target();
        release_text_layout();
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
bool VoiceResultCard::ensure_render_target()
{
    if (render_target_)
    {
        return true;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);

    D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT>(rc.right - rc.left), static_cast<UINT>(rc.bottom - rc.top));
    const HRESULT hr = factory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                     D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED)),
        D2D1::HwndRenderTargetProperties(hwnd_, size), &render_target_);

    if (FAILED(hr))
    {
        return false;
    }

    ReloadImeConfigIfChanged();
    const Palette palette = PaletteFor(ResolveConfiguredTheme(GetConfiguredThemeVoice()) == "light");

    if (FAILED(render_target_->CreateSolidColorBrush(palette.panel, &bg_brush_)) ||
        FAILED(render_target_->CreateSolidColorBrush(palette.border, &border_brush_)) ||
        FAILED(render_target_->CreateSolidColorBrush(palette.text, &text_brush_)) ||
        FAILED(render_target_->CreateSolidColorBrush(palette.secondary, &secondary_text_brush_)) ||
        FAILED(render_target_->CreateSolidColorBrush(palette.header, &header_text_brush_)) ||
        FAILED(render_target_->CreateSolidColorBrush(palette.hover, &hover_fill_brush_)) ||
        FAILED(render_target_->CreateSolidColorBrush(palette.accent, &accent_brush_)) ||
        FAILED(render_target_->CreateSolidColorBrush(palette.primary, &primary_fill_brush_)) ||
        FAILED(render_target_->CreateSolidColorBrush(palette.primary_hover, &primary_hover_brush_)) ||
        FAILED(render_target_->CreateSolidColorBrush(palette.on_primary, &on_primary_brush_)))
    {
        release_render_target();
        return false;
    }

    update_dpi_scale(resolve_anchor());
    return true;
}

void VoiceResultCard::release_render_target()
{
    safe_release(&on_primary_brush_);
    safe_release(&primary_hover_brush_);
    safe_release(&primary_fill_brush_);
    safe_release(&accent_brush_);
    safe_release(&hover_fill_brush_);
    safe_release(&header_text_brush_);
    safe_release(&secondary_text_brush_);
    safe_release(&text_brush_);
    safe_release(&border_brush_);
    safe_release(&bg_brush_);
    safe_release(&render_target_);
}

void VoiceResultCard::release_text_layout()
{
    safe_release(&text_layout_);
    layout_source_text_.clear();
    layout_source_width_ = 0.0f;
}

bool VoiceResultCard::ensure_text_layout(const std::wstring &text, float width)
{
    if (text_layout_ && layout_source_text_ == text && layout_source_width_ == width)
        return true;
    release_text_layout();
    if (!write_factory_ || !body_text_format_ || text.empty())
        return false;

    if (FAILED(write_factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), body_text_format_,
                                                width, kMaxLayoutHeight, &text_layout_)))
        return false;
    layout_source_text_ = text;
    layout_source_width_ = width;
    // Changed text restarts from the top of the transcript; a width change re-wraps the layout.
    scroll_offset_ = 0.0f;
    return true;
}

VoiceOverlayAnchor VoiceResultCard::resolve_anchor() const
{
    {
        std::lock_guard<std::mutex> lock(anchor_mutex_);
        if (anchor_.valid)
            return anchor_;
    }
    return CaptureVoiceOverlayAnchor();
}

void VoiceResultCard::refresh_anchor_for_display_change()
{
    // Read-refresh-write stays under one lock. If StartRecording publishes a fresh anchor
    // concurrently it either happens before this refresh (and is refreshed too) or after it (and
    // replaces the result); an old display change can never clobber a newer recording anchor.
    std::lock_guard<std::mutex> lock(anchor_mutex_);
    if (anchor_.valid)
        anchor_ = RefreshVoiceOverlayAnchorForDisplayChange(anchor_);
}

void VoiceResultCard::update_dpi_scale(const VoiceOverlayAnchor &anchor)
{
    const UINT dpi = anchor.valid && anchor.dpi != 0 ? anchor.dpi : kVoiceOverlayDefaultDpi;
    dpi_ = dpi;
    scale_x_ = scale_y_ = static_cast<float>(dpi) / static_cast<float>(kVoiceOverlayDefaultDpi);
    if (render_target_)
        render_target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
}

void VoiceResultCard::update_card_geometry(const VoiceOverlayAnchor &anchor, float measured_body_height)
{
    const float work_width_dip = static_cast<float>(anchor.work.right - anchor.work.left) / (std::max)(1.0f, scale_x_);
    const float work_height_dip = static_cast<float>(anchor.work.bottom - anchor.work.top) / (std::max)(1.0f, scale_y_);

    card_width_dip_ = (std::max)(1.0f, (std::min)(kCardPreferredWidth, work_width_dip));

    const float available_body = (std::max)(0.0f, work_height_dip - kChromeHeight);
    const float max_body = (std::min)(kBodyMaxHeight, available_body);
    const float min_body = (std::min)(kBodyMinHeight, max_body);
    body_height_dip_ = (std::max)(min_body, (std::min)(measured_body_height, max_body));
    card_height_dip_ = kChromeHeight + body_height_dip_;
    if (work_height_dip > 0.0f)
        card_height_dip_ = (std::min)(card_height_dip_, work_height_dip);

    update_close_button_rect();
    update_copy_button_rect();
}

void VoiceResultCard::update_window_bounds()
{
    if (!hwnd_)
        return;

    const VoiceOverlayAnchor anchor = resolve_anchor();
    update_dpi_scale(anchor);

    const float work_width_dip = static_cast<float>(anchor.work.right - anchor.work.left) / (std::max)(1.0f, scale_x_);
    card_width_dip_ = (std::max)(1.0f, (std::min)(kCardPreferredWidth, work_width_dip));
    const float body_width = (std::max)(1.0f, card_width_dip_ - 2.0f * kHorizontalPadding);

    float measured_body_height = kBodyMinHeight;
    if (!displayed_text_.empty() && ensure_text_layout(displayed_text_, body_width))
    {
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(text_layout_->GetMetrics(&metrics)))
            measured_body_height = std::ceil(metrics.height);
    }

    update_card_geometry(anchor, measured_body_height);

    const int width = (std::max)(1, static_cast<int>(std::lround(card_width_dip_ * scale_x_)));
    const int height = (std::max)(1, static_cast<int>(std::lround(card_height_dip_ * scale_y_)));
    RECT target = VoiceOverlayBottomAnchoredRect(anchor, width, height);
    if (target.right <= target.left || target.bottom <= target.top)
        target = RECT{0, 0, width, height};
    target_window_rect_ = target;
    SetWindowPos(hwnd_, HWND_TOPMOST, target.left, target.top, target.right - target.left, target.bottom - target.top,
                 SWP_NOACTIVATE);
}

void VoiceResultCard::update_close_button_rect()
{
    const float right = card_width_dip_ - kCloseEdgeInset;
    close_button_rect_.right = right;
    close_button_rect_.left = right - kCloseTargetSize;
    close_button_rect_.top = kCloseEdgeInset;
    close_button_rect_.bottom = kCloseEdgeInset + kCloseTargetSize;
}

void VoiceResultCard::update_copy_button_rect()
{
    const wchar_t *label = copy_label();
    float text_width = 0.0f;
    if (write_factory_ && button_text_format_)
    {
        IDWriteTextLayout *layout = nullptr;
        if (SUCCEEDED(write_factory_->CreateTextLayout(label, static_cast<UINT32>(wcslen(label)), button_text_format_,
                                                       600.0f, kButtonHeight, &layout)) &&
            layout)
        {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics)))
                text_width = metrics.width;
            layout->Release();
        }
    }
    copy_label_width_dip_ = text_width;

    const float available_width = (std::max)(1.0f, card_width_dip_ - 2.0f * kHorizontalPadding);
    const float button_width = (std::min)(available_width, kButtonPreferredWidth);
    const float left = (card_width_dip_ - button_width) * 0.5f;
    const float footer_top = kBodyTop + body_height_dip_ + kFooterGap;
    const float top = (std::max)(0.0f, (std::min)(footer_top, card_height_dip_ - kButtonHeight));
    copy_button_rect_.left = left;
    copy_button_rect_.right = left + button_width;
    copy_button_rect_.top = top;
    copy_button_rect_.bottom = top + kButtonHeight;
}

void VoiceResultCard::apply_desired_model()
{
    if (!hwnd_)
        return;

    const VoiceResultCardModel::Snapshot snap = model_.snapshot();
    if (!snap.visible || snap.text.empty())
    {
        cancel_copy_auto_close();
        if (card_visible_)
            hide_card();
        else
            stop_entrance_animation(false);
        return;
    }

    const bool content_changed = displayed_session_ != snap.session || displayed_text_ != snap.text;
    if (content_changed)
    {
        // A new result replaces the old one: cancel an in-flight entrance and any pending
        // auto-close, drop any press/hover that started on the previous content, and re-read the
        // theme on the next paint.
        cancel_copy_auto_close();
        stop_entrance_animation(false);
        reset_press_state();
        reset_hover_state();
        release_render_target();
        release_text_layout();
        displayed_session_ = snap.session;
        displayed_text_ = snap.text;
        feedback_ = CopyFeedback::None;
        scroll_offset_ = 0.0f;
    }

    if (!card_visible_)
    {
        update_window_bounds();
        card_visible_ = true;
        SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        start_entrance_animation();
    }
    else if (content_changed)
    {
        update_window_bounds();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void VoiceResultCard::dismiss_shown_card()
{
    // A manual close cancels the copied-state dwell; only a fresh successful copy restarts it.
    cancel_copy_auto_close();
    // close_if_current is decided inside the model lock, so a click that arrived from the previous
    // card can never dismiss a newer generation that reset() already switched to.
    if (!displayed_text_.empty() && model_.close_if_current(displayed_session_, displayed_text_))
        hide_card();
}

void VoiceResultCard::on_copy_button()
{
    std::wstring text;
    if (!model_.read_current(displayed_session_, displayed_text_, text))
        return;

    if (CopyToClipboard(hwnd_, text))
    {
        feedback_ = CopyFeedback::Copied;
        schedule_copy_auto_close(displayed_session_, text);
    }
    else
    {
        // The latest attempt failed: keep the card for retry and cancel any earlier auto-close.
        cancel_copy_auto_close();
        feedback_ = CopyFeedback::Failed;
    }
    // Feedback changes the label; remeasure so the icon + label group stays centered in the
    // fixed-width pill. A failed copy stays clickable and the label becomes 重试复制.
    update_copy_button_rect();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void VoiceResultCard::schedule_copy_auto_close(std::uint64_t session, const std::wstring &text)
{
    cancel_copy_auto_close();
    if (!hwnd_)
        return;

    if (next_copy_close_timer_id_ == 0)
        next_copy_close_timer_id_ = kCopyAutoCloseTimerIdBase;
    const UINT_PTR timer_id = next_copy_close_timer_id_++;
    if (next_copy_close_timer_id_ == 0)
        next_copy_close_timer_id_ = kCopyAutoCloseTimerIdBase;

    if (SetTimer(hwnd_, timer_id, kCopyAutoCloseDelayMs, nullptr) == 0)
        return; // Keep the success card; the close button remains available.

    copy_close_timer_id_ = timer_id;
    copy_close_session_ = session;
    copy_close_text_ = text;
}

void VoiceResultCard::cancel_copy_auto_close()
{
    if (hwnd_ && copy_close_timer_id_ != 0)
        KillTimer(hwnd_, copy_close_timer_id_);
    copy_close_timer_id_ = 0;
    copy_close_session_ = 0;
    copy_close_text_.clear();
}

void VoiceResultCard::on_copy_auto_close_timer()
{
    if (!hwnd_ || copy_close_timer_id_ == 0)
        return;

    const UINT_PTR timer_id = copy_close_timer_id_;
    copy_close_timer_id_ = 0;
    KillTimer(hwnd_, timer_id);

    const std::uint64_t session = copy_close_session_;
    const std::wstring text = copy_close_text_;
    copy_close_session_ = 0;
    copy_close_text_.clear();

    // close_if_current checks the bound pair inside the model lock. A card replaced after the copy
    // stays visible; a stale timer can never close a newer generation.
    if (model_.close_if_current(session, text))
        hide_card();
}

bool VoiceResultCard::hit_test(float x_dip, float y_dip, Region &region) const
{
    region = Region::None;
    if (!card_visible_)
        return false;

    if (x_dip >= close_button_rect_.left && x_dip <= close_button_rect_.right && y_dip >= close_button_rect_.top &&
        y_dip <= close_button_rect_.bottom)
    {
        region = Region::Close;
        return true;
    }

    if (x_dip >= copy_button_rect_.left && x_dip <= copy_button_rect_.right && y_dip >= copy_button_rect_.top &&
        y_dip <= copy_button_rect_.bottom)
    {
        region = Region::Copy;
        return true;
    }
    return false;
}

const wchar_t *VoiceResultCard::copy_label() const
{
    switch (feedback_)
    {
    case CopyFeedback::Copied:
        return L"已复制";
    case CopyFeedback::Failed:
        return L"重试复制";
    case CopyFeedback::None:
    default:
        return L"复制全文";
    }
}

void VoiceResultCard::reset_press_state()
{
    if (region_pressed_ && GetCapture() == hwnd_)
        ReleaseCapture();
    pressed_region_ = Region::None;
    region_pressed_ = false;
}

void VoiceResultCard::reset_hover_state()
{
    hover_region_ = Region::None;
    tracking_mouse_ = false;
}

bool VoiceResultCard::animations_enabled() const
{
    return SystemAnimationsEnabled();
}

void VoiceResultCard::start_entrance_animation()
{
    if (!hwnd_ || !card_visible_)
        return;

    stop_entrance_animation(false);
    if (!animations_enabled() || target_window_rect_.right <= target_window_rect_.left ||
        target_window_rect_.bottom <= target_window_rect_.top)
    {
        stop_entrance_animation(true);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // The card is already laid out at its final bottom-anchored rectangle. The entrance starts one
    // restrained 8 DIP lower (still inside the 24 DIP bottom margin) and settles upward, so the
    // recognition strip is naturally replaced instead of a new panel appearing in another spot.
    const VoiceOverlayAnchor anchor = resolve_anchor();
    int lift = (std::max)(1, static_cast<int>(std::lround(kEntranceLiftDip * scale_y_)));
    if (anchor.valid)
    {
        // When the work area is too short for the normal margin, the anchored rect itself is
        // clamped to the bottom. Do not add the entrance offset below it: fade in place instead.
        const int available =
            (std::max)(0, static_cast<int>(anchor.work.bottom) - static_cast<int>(target_window_rect_.bottom));
        lift = (std::min)(lift, available);
    }
    animation_from_rect_ = target_window_rect_;
    animation_from_rect_.top += lift;
    animation_from_rect_.bottom += lift;
    animation_start_ms_ = GetTickCount64();
    animation_active_ = true;

    SetLayeredWindowAttributes(hwnd_, 0, 0, LWA_ALPHA);
    SetWindowPos(hwnd_, HWND_TOPMOST, animation_from_rect_.left, animation_from_rect_.top,
                 animation_from_rect_.right - animation_from_rect_.left,
                 animation_from_rect_.bottom - animation_from_rect_.top, SWP_NOACTIVATE);
    if (SetTimer(hwnd_, kAnimationTimerId, kAnimationTickMs, nullptr) == 0)
    {
        // A failed timer would otherwise leave the card stuck at alpha zero forever. Snap to the
        // final opaque position and let the normal paint path take over.
        animation_active_ = false;
        SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
        SetWindowPos(hwnd_, HWND_TOPMOST, target_window_rect_.left, target_window_rect_.top,
                     target_window_rect_.right - target_window_rect_.left,
                     target_window_rect_.bottom - target_window_rect_.top, SWP_NOACTIVATE);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void VoiceResultCard::stop_entrance_animation(bool snap_to_target)
{
    if (hwnd_)
        KillTimer(hwnd_, kAnimationTimerId);
    const bool was_active = animation_active_;
    animation_active_ = false;

    if (!hwnd_)
        return;

    // Every early stop leaves a fully opaque card. Without this, cancelling a fade halfway would
    // strand the window at partial alpha with no timer left to finish it.
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);

    if (snap_to_target && (was_active || target_window_rect_.right > target_window_rect_.left))
    {
        const int width = target_window_rect_.right - target_window_rect_.left;
        const int height = target_window_rect_.bottom - target_window_rect_.top;
        if (width > 0 && height > 0)
        {
            SetWindowPos(hwnd_, HWND_TOPMOST, target_window_rect_.left, target_window_rect_.top, width, height,
                         SWP_NOACTIVATE);
        }
    }
}

void VoiceResultCard::tick_entrance_animation()
{
    if (!animation_active_ || !hwnd_)
        return;

    const ULONGLONG elapsed = GetTickCount64() - animation_start_ms_;
    if (elapsed >= kEntranceDurationMs)
    {
        stop_entrance_animation(true);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    const float t = static_cast<float>(elapsed) / static_cast<float>(kEntranceDurationMs);
    const float remaining = 1.0f - t;
    const float eased = 1.0f - remaining * remaining * remaining; // cubic ease-out
    const int alpha = static_cast<int>(std::lround(eased * 255.0f));

    const int width = target_window_rect_.right - target_window_rect_.left;
    const int height = target_window_rect_.bottom - target_window_rect_.top;
    if (width <= 0 || height <= 0)
    {
        stop_entrance_animation(true);
        return;
    }

    const int left =
        static_cast<int>(std::lround(static_cast<float>(animation_from_rect_.left) +
                                     static_cast<float>(target_window_rect_.left - animation_from_rect_.left) * eased));
    const int top =
        static_cast<int>(std::lround(static_cast<float>(animation_from_rect_.top) +
                                     static_cast<float>(target_window_rect_.top - animation_from_rect_.top) * eased));

    SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(alpha), LWA_ALPHA);
    SetWindowPos(hwnd_, HWND_TOPMOST, left, top, width, height, SWP_NOACTIVATE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void VoiceResultCard::hide_card()
{
    reset_press_state();
    reset_hover_state();
    stop_entrance_animation(false);
    cancel_copy_auto_close();
    card_visible_ = false;
    feedback_ = CopyFeedback::None;
    scroll_offset_ = 0.0f;
    release_text_layout();
    release_render_target();
    displayed_session_ = 0;
    displayed_text_.clear();
    if (hwnd_)
    {
        SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void VoiceResultCard::draw_header_check(float center_x, float center_y)
{
    if (!render_target_ || !accent_brush_)
        return;

    // The check occupies a 13 DIP icon box but keeps a hairline weight, staying quieter than
    // the primary Copy action.
    const float half = kHeaderIconSize * 0.5f;
    render_target_->DrawLine(D2D1::Point2F(center_x + half * 0.66f, center_y - half * 0.50f),
                             D2D1::Point2F(center_x - half * 0.25f, center_y + half * 0.42f), accent_brush_, 1.3f);
    render_target_->DrawLine(D2D1::Point2F(center_x - half * 0.25f, center_y + half * 0.42f),
                             D2D1::Point2F(center_x - half * 0.66f, center_y), accent_brush_, 1.3f);
}

void VoiceResultCard::draw_close_icon(float center_x, float center_y)
{
    if (!render_target_)
        return;
    ID2D1SolidColorBrush *brush = hover_region_ == Region::Close ? text_brush_ : header_text_brush_;
    if (!brush)
        return;
    constexpr float half = kCloseGlyphHalf;
    render_target_->DrawLine(D2D1::Point2F(center_x - half, center_y - half),
                             D2D1::Point2F(center_x + half, center_y + half), brush, 1.35f);
    render_target_->DrawLine(D2D1::Point2F(center_x + half, center_y - half),
                             D2D1::Point2F(center_x - half, center_y + half), brush, 1.35f);
}

void VoiceResultCard::draw_copy_icon(float center_x, float center_y, CopyFeedback feedback)
{
    if (!render_target_ || !on_primary_brush_)
        return;
    ID2D1SolidColorBrush *brush = on_primary_brush_;

    if (feedback == CopyFeedback::Copied)
    {
        render_target_->DrawLine(D2D1::Point2F(center_x - 4.0f, center_y),
                                 D2D1::Point2F(center_x - 1.2f, center_y + 2.8f), brush, 1.7f);
        render_target_->DrawLine(D2D1::Point2F(center_x - 1.2f, center_y + 2.8f),
                                 D2D1::Point2F(center_x + 4.2f, center_y - 3.2f), brush, 1.7f);
        return;
    }
    if (feedback == CopyFeedback::Failed)
    {
        render_target_->DrawLine(D2D1::Point2F(center_x, center_y - 4.4f), D2D1::Point2F(center_x, center_y + 1.3f),
                                 brush, 1.6f);
        render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y + 3.8f), 0.9f, 0.9f), brush);
        return;
    }

    // Keep the rear sheet open behind the front sheet, matching the design's copy outline.
    // Coordinates and stroke width are DIP, scaled by the existing render target.
    constexpr float stroke = 1.125f;
    render_target_->DrawLine(D2D1::Point2F(center_x + 1.875f, center_y - 3.125f),
                             D2D1::Point2F(center_x + 1.875f, center_y - 5.625f), brush, stroke);
    render_target_->DrawLine(D2D1::Point2F(center_x + 1.875f, center_y - 5.625f),
                             D2D1::Point2F(center_x - 4.375f, center_y - 5.625f), brush, stroke);
    render_target_->DrawLine(D2D1::Point2F(center_x - 4.375f, center_y - 5.625f),
                             D2D1::Point2F(center_x - 5.625f, center_y - 4.375f), brush, stroke);
    render_target_->DrawLine(D2D1::Point2F(center_x - 5.625f, center_y - 4.375f),
                             D2D1::Point2F(center_x - 5.625f, center_y + 1.875f), brush, stroke);
    render_target_->DrawLine(D2D1::Point2F(center_x - 5.625f, center_y + 1.875f),
                             D2D1::Point2F(center_x - 3.125f, center_y + 1.875f), brush, stroke);
    const D2D1_ROUNDED_RECT front = D2D1::RoundedRect(
        D2D1::RectF(center_x - 1.875f, center_y - 1.875f, center_x + 5.625f, center_y + 5.625f), 1.25f, 1.25f);
    render_target_->DrawRoundedRectangle(front, brush, stroke);
}

void VoiceResultCard::draw()
{
    if (!ensure_render_target())
        return;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float w = static_cast<float>(rc.right - rc.left) / (std::max)(0.01f, scale_x_);
    const float h = static_cast<float>(rc.bottom - rc.top) / (std::max)(0.01f, scale_y_);
    if (w <= 0.0f || h <= 0.0f)
        return;

    render_target_->BeginDraw();
    render_target_->SetTransform(D2D1::Matrix3x2F::Identity());
    render_target_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    const D2D1_ROUNDED_RECT panel =
        D2D1::RoundedRect(D2D1::RectF(0.75f, 0.75f, w - 0.75f, h - 0.75f), kCornerRadius, kCornerRadius);
    render_target_->FillRoundedRectangle(panel, bg_brush_);
    render_target_->DrawRoundedRectangle(panel, border_brush_, 1.0f);

    // Header: the check and the muted label are one centered optical group. The close
    // affordance only gains a surface on hover, keeping the resting card calm.
    const float title_width = title_label_width_dip_ > 0.0f ? title_label_width_dip_ : kHeaderTitleFallbackWidth;
    const float header_group_width = kHeaderIconSize + kHeaderGroupGap + title_width;
    const float header_group_left = (w - header_group_width) * 0.5f;
    const float header_center_y = kTopPadding + kHeaderHeight * 0.5f;
    draw_header_check(header_group_left + kHeaderIconSize * 0.5f, header_center_y);
    if (title_text_format_ && header_text_brush_)
    {
        const float title_left = header_group_left + kHeaderIconSize + kHeaderGroupGap;
        render_target_->DrawTextW(
            kTitleText, static_cast<UINT32>(wcslen(kTitleText)), title_text_format_,
            D2D1::RectF(title_left, kTopPadding, title_left + title_width + 1.0f, kTopPadding + kHeaderHeight),
            header_text_brush_);
    }

    const float close_center_x = (close_button_rect_.left + close_button_rect_.right) * 0.5f;
    const float close_center_y = (close_button_rect_.top + close_button_rect_.bottom) * 0.5f;
    if (hover_region_ == Region::Close)
    {
        render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(close_center_x, close_center_y), 13.0f, 13.0f),
                                    hover_fill_brush_);
    }
    draw_close_icon(close_center_x, close_center_y);

    // Body: a secondary preview, clipped to at most two lines. The full transcript stays in the
    // layout and on the clipboard path; the wheel scrolls through it when it does not fit.
    const float body_bottom = kBodyTop + body_height_dip_;
    const float body_width = (std::max)(1.0f, w - 2.0f * kHorizontalPadding);
    if (!displayed_text_.empty())
    {
        render_target_->PushAxisAlignedClip(
            D2D1::RectF(kHorizontalPadding, kBodyTop, w - kHorizontalPadding, body_bottom),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (ensure_text_layout(displayed_text_, body_width))
        {
            render_target_->DrawTextLayout(D2D1::Point2F(kHorizontalPadding, kBodyTop - scroll_offset_), text_layout_,
                                           secondary_text_brush_);
        }
        render_target_->PopAxisAlignedClip();
    }

    // Footer: one centered primary pill. The icon and label are measured as a group so every
    // feedback label stays optically centered inside the same button.
    const bool copy_hover = hover_region_ == Region::Copy;
    const bool copy_pressed = region_pressed_ && pressed_region_ == Region::Copy;
    ID2D1SolidColorBrush *fill = (copy_hover || copy_pressed) ? primary_hover_brush_ : primary_fill_brush_;
    const D2D1_ROUNDED_RECT button = D2D1::RoundedRect(
        D2D1::RectF(copy_button_rect_.left, copy_button_rect_.top, copy_button_rect_.right, copy_button_rect_.bottom),
        kButtonHeight * 0.5f, kButtonHeight * 0.5f);
    if (fill)
        render_target_->FillRoundedRectangle(button, fill);

    const float button_center_x = (copy_button_rect_.left + copy_button_rect_.right) * 0.5f;
    const float group_width = kButtonIconSpan + kButtonContentGap + copy_label_width_dip_;
    const float group_left = button_center_x - group_width * 0.5f;
    const float icon_center_x = group_left + kButtonIconSpan * 0.5f;
    const float icon_center_y = (copy_button_rect_.top + copy_button_rect_.bottom) * 0.5f;
    draw_copy_icon(icon_center_x, icon_center_y, feedback_);

    if (button_text_format_ && on_primary_brush_)
    {
        const wchar_t *label = copy_label();
        const D2D1_RECT_F label_rect =
            D2D1::RectF(group_left + kButtonIconSpan + kButtonContentGap, copy_button_rect_.top,
                        group_left + group_width + 1.0f, copy_button_rect_.bottom);
        render_target_->DrawTextW(label, static_cast<UINT32>(wcslen(label)), button_text_format_, label_rect,
                                  on_primary_brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    const HRESULT hr = render_target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
        release_render_target();
}
