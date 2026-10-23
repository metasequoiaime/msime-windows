#include "wave_overlay.h"
#include "mvi_utils.h"
#include "config/ime_config.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <algorithm>
#include <cmath>
#include <dwmapi.h>
#include <windowsx.h>

namespace
{
constexpr wchar_t kClassName[] = L"MviWaveOverlayWindow";
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerMs = 16;
constexpr UINT kTranscriptChangedMessage = WM_APP + 186;
constexpr UINT kAnchorChangedMessage = WM_APP + 192;
constexpr UINT kVisibilityChangedMessage = WM_APP + 193;
constexpr int kCompactWidth = 78;
constexpr int kCompactHeight = 32;
constexpr int kProcessingWidth = 112;
constexpr int kProcessingHeight = 40;
constexpr int kActionWidth = 142;
constexpr int kActionHeight = 40;
constexpr float kActionCenterInset = 18.0f;
constexpr float kActionRadius = 12.0f;
constexpr int kTranscriptWidth = kVoiceOverlayPreferredWidthDip;
constexpr int kTranscriptHeight = 112;
constexpr float kTranscriptHorizontalPadding = 14.0f;
constexpr float kTranscriptTextTop = 33.0f;
constexpr float kTranscriptTextBottom = 8.0f;
constexpr UINT32 kMaxTranscriptLines = 3;
constexpr float kPanelOpacity = 0.90f;
constexpr int kBarCount = 12;
constexpr float kDotRadius = 1.15f;
constexpr float kMaxHalfHeight = 14.0f;

template <typename T> void safe_release(T **obj)
{
    if (*obj)
    {
        (*obj)->Release();
        *obj = nullptr;
    }
}
} // namespace

WaveOverlay::WaveOverlay()
{
    // Deterministic-but-irregular profile per bar:
    // each bar has different amplitude/frequency/phase so movement looks natural.
    for (int i = 0; i < kBarCount; ++i)
    {
        const float fi = static_cast<float>(i);
        amplitudes_[i] = 0.55f + 0.45f * std::fabs(std::sin(0.73f * fi + 0.19f));
        phases_[i] = 0.41f * fi + 0.37f * std::sin(0.29f * fi + 1.11f);
        freqs_[i] = 4.2f + std::fmod(1.7f * fi, 3.6f);
    }
}

WaveOverlay::~WaveOverlay()
{
    shutdown();
}

bool WaveOverlay::init(HINSTANCE instance, std::function<void(Action)> action_handler)
{
    if (hwnd_)
    {
        return true;
    }

    instance_ = instance;
    action_handler_ = std::move(action_handler);

    const HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_);
    if (FAILED(hr))
    {
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown **>(&write_factory_))) ||
        FAILED(write_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"zh-cn",
                                                &text_format_)) ||
        FAILED(write_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"zh-cn",
                                                &processing_text_format_)))
    {
        shutdown();
        return false;
    }
    text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    text_format_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 22.0f, 18.0f);
    processing_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    processing_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WaveOverlay::wnd_proc;
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
        120,                                                                 //
        120,                                                                 //
        kCompactWidth,                                                       //
        kCompactHeight,                                                      //
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

    // 设置窗口透明
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    MARGINS mar = {-1};
    DwmExtendFrameIntoClientArea(hwnd_, &mar);

    return true;
}

void WaveOverlay::shutdown()
{
    post_hwnd_.store(nullptr, std::memory_order_release);
    visibility_requested_.store(false);
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    release_render_target();
    release_text_layout();
    safe_release(&processing_text_format_);
    safe_release(&text_format_);
    safe_release(&write_factory_);
    safe_release(&factory_);
}

void WaveOverlay::show()
{
    visibility_requested_.store(true);
    if (HWND hwnd = post_hwnd_.load(std::memory_order_acquire))
        PostMessageW(hwnd, kVisibilityChangedMessage, 0, 0);
}

void WaveOverlay::hide()
{
    visibility_requested_.store(false);
    if (HWND hwnd = post_hwnd_.load(std::memory_order_acquire))
        PostMessageW(hwnd, kVisibilityChangedMessage, 0, 0);
}

void WaveOverlay::apply_visibility()
{
    if (hwnd_)
    {
        // UI-thread only: these window calls can synchronously dispatch messages. Background
        // callers may hold the voice send mutex, which is also needed by UI cancel callbacks.
        if (!visibility_requested_.load())
        {
            ShowWindow(hwnd_, SW_HIDE);
            return;
        }
        ReloadImeConfigIfChanged();
        const bool light_theme = ResolveConfiguredTheme(GetConfiguredThemeVoice()) == "light";
        if (light_theme != light_theme_)
        {
            light_theme_ = light_theme;
            release_render_target();
        }
        // 每次显示时根据当前显示器重新定位
        update_window_bounds();

        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd_);
    }
}

void WaveOverlay::set_anchor(const VoiceOverlayAnchor &anchor)
{
    {
        std::lock_guard<std::mutex> lock(anchor_mutex_);
        anchor_ = anchor;
    }
    if (HWND hwnd = post_hwnd_.load(std::memory_order_acquire))
        PostMessageW(hwnd, kAnchorChangedMessage, 0, 0);
}

void WaveOverlay::set_listening(bool listening)
{
    listening_.store(listening);
}

void WaveOverlay::set_input_level(float level)
{
    const float clamped = std::max(0.0f, std::min(1.0f, level));
    input_level_.store(clamped);
}

void WaveOverlay::set_transcript(const std::wstring &text)
{
    {
        std::lock_guard<std::mutex> lock(transcript_mutex_);
        if (transcript_ == text)
            return;
        transcript_ = text;
    }
    if (HWND hwnd = post_hwnd_.load(std::memory_order_acquire); hwnd && show_transcript_.load())
        PostMessageW(hwnd, kTranscriptChangedMessage, 0, 0);
}

void WaveOverlay::set_show_transcript(bool show)
{
    const bool previous = show_transcript_.exchange(show);
    if (previous == show)
        return;
    if (!show)
    {
        std::lock_guard<std::mutex> lock(transcript_mutex_);
        transcript_.clear();
    }
    if (HWND hwnd = post_hwnd_.load(std::memory_order_acquire))
        PostMessageW(hwnd, kTranscriptChangedMessage, 0, 0);
}

void WaveOverlay::set_compact_status(CompactStatus status)
{
    const CompactStatus previous = compact_status_.exchange(status);
    if (previous == status)
        return;
    if (HWND hwnd = post_hwnd_.load(std::memory_order_acquire))
        PostMessageW(hwnd, kTranscriptChangedMessage, 0, 0);
}

void WaveOverlay::set_actions_visible(bool visible)
{
    const bool previous = actions_visible_.exchange(visible);
    if (previous == visible)
        return;
    if (HWND hwnd = post_hwnd_.load(std::memory_order_acquire))
        PostMessageW(hwnd, kTranscriptChangedMessage, 0, 0);
}

LRESULT CALLBACK WaveOverlay::wnd_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    WaveOverlay *self = reinterpret_cast<WaveOverlay *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE)
    {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        self = reinterpret_cast<WaveOverlay *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    if (!self)
    {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    return self->handle_message(hwnd, message, wParam, lParam);
}

LRESULT WaveOverlay::handle_message(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        SetTimer(hwnd, kTimerId, kTimerMs, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == kTimerId)
        {
            update_wave_levels();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_SIZE:
        if (render_target_)
        {
            render_target_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        update_dpi_scale(resolve_anchor());
        return 0;
    case WM_DPICHANGED:
        // A saved anchor is refreshed only for system layout changes. Ordinary foreground changes
        // during recognition never reach this path.
        refresh_anchor_for_display_change();
        update_window_bounds();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DISPLAYCHANGE:
        // Re-resolve the saved monitor after a display/DPI layout change, then keep growing from
        // the same saved monitor. Foreground focus is deliberately not consulted.
        refresh_anchor_for_display_change();
        update_window_bounds();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SETTINGCHANGE:
        // Taskbar position/autohide changes resize rcWork without necessarily changing the monitor
        // or DPI. Refresh the saved anchor for that layout change only.
        if (wParam == SPI_SETWORKAREA)
        {
            refresh_anchor_for_display_change();
            update_window_bounds();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR: {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd, &point);
        Action action{};
        if (hit_test_action(static_cast<float>(point.x) / scale_x_, static_cast<float>(point.y) / scale_y_, action))
        {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        Action action{};
        if (hit_test_action(static_cast<float>(GET_X_LPARAM(lParam)) / scale_x_,
                            static_cast<float>(GET_Y_LPARAM(lParam)) / scale_y_, action))
        {
            pressed_action_ = action;
            action_pressed_ = true;
            SetCapture(hwnd);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP: {
        if (!action_pressed_)
            break;
        ReleaseCapture();
        action_pressed_ = false;
        Action action{};
        if (hit_test_action(static_cast<float>(GET_X_LPARAM(lParam)) / scale_x_,
                            static_cast<float>(GET_Y_LPARAM(lParam)) / scale_y_, action) &&
            action == pressed_action_ && action_handler_)
            action_handler_(action);
        return 0;
    }
    case WM_CAPTURECHANGED:
        action_pressed_ = false;
        return 0;
    case WM_RBUTTONUP:
        // 右键一律当作取消。这个窗口是 WS_POPUP + WS_EX_NOACTIVATE，右键落在它
        // 上面本来就会被吃掉，前台应用的右键菜单也弹不出来；把它定义成取消，
        // 可以保证在任何状态下（按钮被隐藏时同样有效）都有一个鼠标退出通道。
        if (action_handler_)
            action_handler_(Action::Cancel);
        return 0;
    case kVisibilityChangedMessage:
        apply_visibility();
        return 0;
    case kAnchorChangedMessage:
        update_window_bounds();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case kTranscriptChangedMessage:
        if (!actions_visible_.load())
            action_pressed_ = false;
        release_text_layout();
        update_window_bounds();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        draw();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kTimerId);
        post_hwnd_.store(nullptr, std::memory_order_release);
        release_render_target();
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool WaveOverlay::ensure_render_target()
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

    const D2D1_COLOR_F background = light_theme_ ? D2D1::ColorF(0.98f, 0.98f, 0.99f, kPanelOpacity)
                                                 : D2D1::ColorF(0.07f, 0.08f, 0.10f, kPanelOpacity);
    const D2D1_COLOR_F bars = light_theme_ ? D2D1::ColorF(0.35f, 0.18f, 0.42f) : D2D1::ColorF(D2D1::ColorF::White);
    const D2D1_COLOR_F border =
        light_theme_ ? D2D1::ColorF(0.20f, 0.20f, 0.24f, 0.22f) : D2D1::ColorF(0.90f, 0.93f, 1.0f, 0.20f);
    const D2D1_COLOR_F action_background =
        light_theme_ ? D2D1::ColorF(0.86f, 0.87f, 0.89f) : D2D1::ColorF(0.18f, 0.19f, 0.21f);

    if (FAILED(render_target_->CreateSolidColorBrush(background, &bg_brush_)))
    {
        release_render_target();
        return false;
    }

    if (FAILED(render_target_->CreateSolidColorBrush(bars, &bar_brush_)))
    {
        release_render_target();
        return false;
    }

    if (FAILED(render_target_->CreateSolidColorBrush(border, &border_brush_)))
    {
        release_render_target();
        return false;
    }

    if (FAILED(render_target_->CreateSolidColorBrush(action_background, &action_bg_brush_)))
    {
        release_render_target();
        return false;
    }

    update_dpi_scale(resolve_anchor());
    return true;
}

void WaveOverlay::release_render_target()
{
    safe_release(&bar_brush_);
    safe_release(&bg_brush_);
    safe_release(&border_brush_);
    safe_release(&action_bg_brush_);
    safe_release(&render_target_);
}

void WaveOverlay::release_text_layout()
{
    safe_release(&text_layout_);
    layout_source_transcript_.clear();
}

bool WaveOverlay::ensure_text_layout(const std::wstring &transcript, float width, float height)
{
    if (text_layout_ && layout_source_transcript_ == transcript)
        return true;
    release_text_layout();
    if (!write_factory_ || !text_format_ || transcript.empty())
        return false;

    const auto line_count_for = [&](const std::wstring &candidate) {
        IDWriteTextLayout *layout = nullptr;
        UINT32 line_count = 0;
        if (SUCCEEDED(write_factory_->CreateTextLayout(candidate.c_str(), static_cast<UINT32>(candidate.size()),
                                                       text_format_, width, 4096.0f, &layout)))
        {
            layout->GetLineMetrics(nullptr, 0, &line_count);
            layout->Release();
        }
        return line_count;
    };

    std::wstring visible = transcript;
    if (line_count_for(visible) > kMaxTranscriptLines)
    {
        size_t low = 0;
        size_t high = transcript.size();
        while (low < high)
        {
            const size_t middle = low + (high - low) / 2;
            const std::wstring candidate = L"…" + transcript.substr(middle);
            if (line_count_for(candidate) <= kMaxTranscriptLines)
                high = middle;
            else
                low = middle + 1;
        }

        // Do not split a UTF-16 surrogate pair when keeping the newest text.
        if (low < transcript.size() && LOW_SURROGATE_START <= transcript[low] && transcript[low] <= LOW_SURROGATE_END)
            ++low;
        visible = L"…" + transcript.substr(low);
    }

    if (FAILED(write_factory_->CreateTextLayout(visible.c_str(), static_cast<UINT32>(visible.size()), text_format_,
                                                width, height, &text_layout_)))
        return false;
    layout_source_transcript_ = transcript;
    return true;
}

VoiceOverlayAnchor WaveOverlay::resolve_anchor() const
{
    {
        std::lock_guard<std::mutex> lock(anchor_mutex_);
        if (anchor_.valid)
            return anchor_;
    }
    return CaptureVoiceOverlayAnchor();
}

void WaveOverlay::refresh_anchor_for_display_change()
{
    std::lock_guard<std::mutex> lock(anchor_mutex_);
    if (anchor_.valid)
        anchor_ = RefreshVoiceOverlayAnchorForDisplayChange(anchor_);
}

void WaveOverlay::update_dpi_scale(const VoiceOverlayAnchor &anchor)
{
    if (!hwnd_)
        return;

    // The anchor captured at recording start owns the scale for this session. Only a fixture that
    // never called set_anchor falls back to the foreground monitor.
    const float scale = anchor.valid ? anchor.scale() : mvi_utils::GetForegroundMonitorScale();
    scale_x_ = scale;
    scale_y_ = scale;
    dpi_ = static_cast<UINT>(std::lround(scale * USER_DEFAULT_SCREEN_DPI));

    if (render_target_)
        render_target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
}

void WaveOverlay::update_window_bounds()
{
    if (!hwnd_)
        return;
    // 每次重新定位都按当前目标显示器刷新缩放，使 show()、状态切换、分辨率/DPI 变化
    // 走到的尺寸计算始终基于最新的显示器 DPI（与候选框每次显示都重算的行为一致）。
    const VoiceOverlayAnchor anchor = resolve_anchor();
    update_dpi_scale(anchor);
    bool has_transcript = false;
    if (show_transcript_.load())
    {
        std::lock_guard<std::mutex> lock(transcript_mutex_);
        has_transcript = !transcript_.empty();
    }
    const bool has_compact_status = compact_status_.load() != CompactStatus::None;
    const bool has_actions = actions_visible_.load();
    const int logical_width =
        has_actions ? kActionWidth
                    : (has_compact_status ? kProcessingWidth : (has_transcript ? kTranscriptWidth : kCompactWidth));
    const int logical_height =
        has_actions ? kActionHeight
                    : (has_compact_status ? kProcessingHeight : (has_transcript ? kTranscriptHeight : kCompactHeight));
    const int width = static_cast<int>(std::lround(logical_width * scale_x_));
    const int height = static_cast<int>(std::lround(logical_height * scale_y_));
    // Preserve the recording bar's existing full-monitor centering and 10 physical-pixel
    // work-area bottom gap. Only the monitor/DPI source is pinned to recording start.
    mvi_utils::MonitorMetrics metrics;
    if (anchor.valid)
    {
        metrics.full = anchor.full;
        metrics.work = anchor.work;
    }
    else
    {
        metrics = mvi_utils::GetForegroundMonitorMetrics();
    }
    const int x = (metrics.full.right + metrics.full.left) / 2 - width / 2;
    const int y = metrics.work.bottom - height - 10;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
}

bool WaveOverlay::hit_test_action(float x, float y, Action &action) const
{
    if (!actions_visible_.load() || !hwnd_)
        return false;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float width = static_cast<float>(rc.right - rc.left) / scale_x_;
    const float height = static_cast<float>(rc.bottom - rc.top) / scale_y_;
    const float center_y = height * 0.5f;
    const auto inside = [&](float center_x) {
        const float dx = x - center_x;
        const float dy = y - center_y;
        return dx * dx + dy * dy <= kActionRadius * kActionRadius;
    };
    if (inside(kActionCenterInset))
    {
        action = Action::Cancel;
        return true;
    }
    if (inside(width - kActionCenterInset))
    {
        action = Action::Confirm;
        return true;
    }
    return false;
}

void WaveOverlay::update_wave_levels()
{
    const bool listening = listening_.load();
    const float level = listening ? input_level_.load() : 0.0f;
    const float t = static_cast<float>(GetTickCount64() * 0.001);

    for (int i = 0; i < kBarCount; ++i)
    {
        const float p = phases_[i];
        const float f = freqs_[i];
        const float center = 0.5f * static_cast<float>(kBarCount - 1);
        const float dist = std::abs(static_cast<float>(i) - center) / std::max(1.0f, center);
        const float center_boost = 1.0f + 0.75f * (1.0f - dist); // stronger expansion near center when speaking

        // Multi-harmonic motion. Keep irregularity, but avoid excessive high-frequency jitter.
        const float n1 = 0.5f + 0.5f * std::sin(t * f + p);
        const float n2 = 0.5f + 0.5f * std::sin(t * (0.57f * f) + 1.7f * p + 0.9f);
        const float n3 = 0.5f + 0.5f * std::sin(t * (1.23f * f) + 0.6f * p + 2.1f);
        const float irregular = std::min(1.0f, 0.60f * n1 + 0.30f * n2 + 0.10f * n3);

        // When signal is weak, floor also drops so bars settle back to dots smoothly.
        const float floor = level * (0.06f + 0.12f * n3);
        float target = level * amplitudes_[i] * center_boost * std::max(floor, irregular);

        // Add transient punch on rising edge so start feels more energetic.
        if (target > levels_[i])
        {
            const float transient = 0.22f * level * (1.0f - levels_[i]);
            target = std::min(1.0f, target + transient);
        }

        const float attack = 0.52f;
        const float decay = listening ? 0.07f : 0.045f;
        const float smooth = target > levels_[i] ? attack : decay;
        levels_[i] = levels_[i] * (1.0f - smooth) + target * smooth;
    }
}

void WaveOverlay::draw()
{
    if (!ensure_render_target())
    {
        return;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);

    const float w = static_cast<float>(rc.right - rc.left) / scale_x_;
    const float h = static_cast<float>(rc.bottom - rc.top) / scale_y_;
    std::wstring transcript;
    const bool show_transcript = show_transcript_.load();
    if (show_transcript)
    {
        std::lock_guard<std::mutex> lock(transcript_mutex_);
        transcript = transcript_;
    }
    const CompactStatus compact_status = compact_status_.load();
    const bool has_compact_status = compact_status != CompactStatus::None;
    const bool has_actions = actions_visible_.load();
    const bool has_transcript = !has_compact_status && show_transcript && !transcript.empty();
    const float content_left = has_actions ? 35.0f : 0.0f;
    const float content_right = has_actions ? w - 35.0f : w;
    const float content_width = content_right - content_left;
    const float wave_width =
        has_transcript ? (std::min)(78.0f, w) : (has_actions ? (std::min)(78.0f, content_width) : w);
    const float wave_left = has_transcript ? (w - wave_width) * 0.5f : 0.0f;
    const float centered_wave_left = has_actions ? content_left + (content_width - wave_width) * 0.5f : wave_left;
    const float center_y = has_transcript ? 18.0f : h * 0.5f;
    const float side_margin = kDotRadius + 0.25f;
    const float track_width = std::max(1.0f, wave_width - 2.0f * side_margin);
    const float base_step = (kBarCount > 1) ? (track_width / static_cast<float>(kBarCount - 1)) : 0.0f;
    const float step = base_step * 0.75f; // 减小一点间距，让波形更紧凑一些
    const float used_width = step * static_cast<float>(kBarCount - 1);
    const float start_x = centered_wave_left + (wave_width - used_width) * 0.5f;
    const float dot_radius = kDotRadius;
    const float bar_width = std::max(dot_radius * 2.0f, base_step * 0.26f);

    render_target_->BeginDraw();
    render_target_->SetTransform(D2D1::Matrix3x2F::Identity());
    render_target_->Clear(D2D1::ColorF(0, 0, 0, 0));

    const float corner = (has_compact_status || has_actions) ? h * 0.5f : (has_transcript ? 16.0f : 10.0f);
    const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), corner, corner);
    render_target_->FillRoundedRectangle(panel, bg_brush_);
    render_target_->DrawRoundedRectangle(panel, border_brush_, 1.0f);

    if (has_compact_status)
    {
        const wchar_t *label = compact_status == CompactStatus::Recognizing ? L"识别中..." : L"处理中...";
        render_target_->DrawTextW(label, static_cast<UINT32>(wcslen(label)), processing_text_format_,
                                  D2D1::RectF(content_left, 0.0f, content_right, h), bar_brush_);
    }
    else
    {
        for (int i = 0; i < kBarCount; ++i)
        {
            const float x = start_x + i * step;
            const float display_level = has_transcript ? (std::min)(1.0f, levels_[i] * 1.35f) : levels_[i];
            const float max_half_height = has_transcript ? 14.0f : kMaxHalfHeight;
            const float half = dot_radius + display_level * (max_half_height - dot_radius);
            if (display_level < 0.06f)
            {
                render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, center_y), dot_radius, dot_radius),
                                            bar_brush_);
            }
            else
            {
                const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
                    D2D1::RectF(x - bar_width * 0.5f, center_y - half, x + bar_width * 0.5f, center_y + half),
                    bar_width * 0.5f, bar_width * 0.5f);
                render_target_->FillRoundedRectangle(rr, bar_brush_);
            }
        }
    }

    if (has_transcript && text_format_)
    {
        const float text_width = w - 2.0f * kTranscriptHorizontalPadding;
        const float text_height = h - kTranscriptTextTop - kTranscriptTextBottom;
        if (ensure_text_layout(transcript, text_width, text_height))
        {
            render_target_->DrawTextLayout(D2D1::Point2F(kTranscriptHorizontalPadding, kTranscriptTextTop),
                                           text_layout_, bar_brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    if (has_actions)
    {
        const float button_y = h * 0.5f;
        const float left_x = kActionCenterInset;
        const float right_x = w - kActionCenterInset;
        render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(left_x, button_y), kActionRadius, kActionRadius),
                                    action_bg_brush_);
        render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(right_x, button_y), kActionRadius, kActionRadius),
                                    action_bg_brush_);

        constexpr float x_half = 3.0f;
        render_target_->DrawLine(D2D1::Point2F(left_x - x_half, button_y - x_half),
                                 D2D1::Point2F(left_x + x_half, button_y + x_half), bar_brush_, 1.7f);
        render_target_->DrawLine(D2D1::Point2F(left_x + x_half, button_y - x_half),
                                 D2D1::Point2F(left_x - x_half, button_y + x_half), bar_brush_, 1.7f);
        render_target_->DrawLine(D2D1::Point2F(right_x - 3.8f, button_y),
                                 D2D1::Point2F(right_x - 0.8f, button_y + 3.0f), bar_brush_, 1.7f);
        render_target_->DrawLine(D2D1::Point2F(right_x - 0.8f, button_y + 3.0f),
                                 D2D1::Point2F(right_x + 4.2f, button_y - 3.5f), bar_brush_, 1.7f);
    }

    const HRESULT hr = render_target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        release_render_target();
    }
}
