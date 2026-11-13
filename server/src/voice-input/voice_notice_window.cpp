#include "voice_notice_window.h"
#include "voice_overlay_layout.h"
#include "config/ime_config.h"
#include "settings/settings_launcher.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr wchar_t kClassName[] = L"MetasequoiaImeVoiceNoticeWindow";
// 字体写死雅黑 + zh-cn：不随系统界面语言回退，英文系统上中文也走正常的中文字形。
constexpr wchar_t kFontFamily[] = L"Microsoft YaHei UI";
constexpr wchar_t kLocale[] = L"zh-cn";

// 以下都是 DIP，按锚定显示器的 DPI 换算成像素。
constexpr float kPreferredWidth = 380.0f;
constexpr float kScreenInset = 16.0f;
constexpr float kPadding = 22.0f;
constexpr float kCornerRadius = 16.0f;
constexpr float kIconSize = 18.0f;
constexpr float kIconGap = 10.0f;
constexpr float kTitleHeight = 22.0f;
constexpr float kTitleFontSize = 14.5f;
constexpr float kBodyGap = 8.0f;
constexpr float kBodyFontSize = 13.5f;
constexpr float kBodyLineStep = 22.0f;
constexpr float kBodyBaseline = 17.5f;
constexpr float kFooterGap = 20.0f;
constexpr float kButtonHeight = 32.0f;
constexpr float kButtonMinWidth = 80.0f;
constexpr float kButtonTextPadding = 18.0f;
constexpr float kButtonGap = 8.0f;
constexpr float kButtonFontSize = 13.0f;
constexpr float kMaxLayoutHeight = 100000.0f;

struct Palette
{
    D2D1_COLOR_F panel;
    D2D1_COLOR_F border;
    D2D1_COLOR_F text;
    D2D1_COLOR_F secondary;
    D2D1_COLOR_F hover;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F error;
    D2D1_COLOR_F primary;
    D2D1_COLOR_F primary_hover;
    D2D1_COLOR_F on_primary;
};

// 与 VoiceResultCard 同一套色值，两个语音浮层放在一起看是一家的。
Palette PaletteFor(bool light)
{
    if (light)
    {
        return Palette{
            D2D1::ColorF(1.0f, 1.0f, 1.0f),       D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f),
            D2D1::ColorF(0.090f, 0.098f, 0.114f), D2D1::ColorF(0.380f, 0.404f, 0.443f),
            D2D1::ColorF(0.945f, 0.949f, 0.957f), D2D1::ColorF(0.184f, 0.416f, 0.345f),
            D2D1::ColorF(0.800f, 0.235f, 0.235f), D2D1::ColorF(0.106f, 0.114f, 0.129f),
            D2D1::ColorF(0.165f, 0.176f, 0.196f), D2D1::ColorF(1.0f, 1.0f, 1.0f),
        };
    }
    return Palette{
        D2D1::ColorF(0.098f, 0.106f, 0.125f), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f),
        D2D1::ColorF(0.945f, 0.949f, 0.957f), D2D1::ColorF(0.702f, 0.722f, 0.757f),
        D2D1::ColorF(0.180f, 0.192f, 0.220f), D2D1::ColorF(0.612f, 0.804f, 0.741f),
        D2D1::ColorF(0.957f, 0.471f, 0.471f), D2D1::ColorF(0.945f, 0.949f, 0.957f),
        D2D1::ColorF(0.886f, 0.898f, 0.914f), D2D1::ColorF(0.071f, 0.078f, 0.094f),
    };
}

// 屏幕上至多一个提示框；新提示建好后关掉旧的。
std::mutex g_current_mutex;
HWND g_current_notice = nullptr;

class NoticeWindow
{
  public:
    NoticeWindow(VoiceNoticeKind kind, std::wstring message, bool light)
        : kind_(kind), message_(std::move(message)), palette_(PaletteFor(light))
    {
    }

    ~NoticeWindow()
    {
        if (hwnd_)
            DestroyWindow(hwnd_);
    }

    NoticeWindow(const NoticeWindow &) = delete;
    NoticeWindow &operator=(const NoticeWindow &) = delete;

    bool Create()
    {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown **>(write_factory_.GetAddressOf()))) ||
            !CreateFormat(DWRITE_FONT_WEIGHT_SEMI_BOLD, kTitleFontSize, title_format_) ||
            !CreateFormat(DWRITE_FONT_WEIGHT_NORMAL, kBodyFontSize, body_format_) ||
            !CreateFormat(DWRITE_FONT_WEIGHT_NORMAL, kButtonFontSize, button_format_))
            return false;
        title_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        body_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        body_format_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, kBodyLineStep, kBodyBaseline);
        button_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        button_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        buttons_.push_back({Action::Dismiss, L"知道了", {}, kind_ == VoiceNoticeKind::Error});
        if (kind_ == VoiceNoticeKind::SettingsNeeded)
            buttons_.push_back({Action::OpenSettings, L"打开设置", {}, true});

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = &NoticeWindow::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;

        // 先按捕获到的显示器算好尺寸再建窗，直接落在目标位置，避免先在主屏建窗、再挪过去时
        // 触发一轮 WM_DPICHANGED。
        anchor_ = CaptureVoiceOverlayAnchor();
        const RECT target = ComputeLayout();
        // 不抢焦点：用户按下语音键时手里是别的程序的输入框，提示框只是浮在上面。
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, kClassName,
                                L"水杉输入法", WS_POPUP, target.left, target.top, target.right - target.left,
                                target.bottom - target.top, nullptr, nullptr, instance, this);
        if (!hwnd_)
            return false;
        SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
        MARGINS margins = {-1};
        DwmExtendFrameIntoClientArea(hwnd_, &margins);

        {
            std::lock_guard<std::mutex> lock(g_current_mutex);
            if (g_current_notice && g_current_notice != hwnd_)
                PostMessageW(g_current_notice, WM_CLOSE, 0, 0);
            g_current_notice = hwnd_;
        }
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        return true;
    }

    bool OpenSettingsRequested() const
    {
        return open_settings_;
    }

  private:
    enum class Action
    {
        None,
        Dismiss,
        OpenSettings,
    };

    struct ButtonSpec
    {
        Action action;
        const wchar_t *label;
        D2D1_RECT_F rect;
        bool primary;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto *self = reinterpret_cast<NoticeWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<NoticeWindow *>(reinterpret_cast<CREATESTRUCTW *>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(hwnd, message, wParam, lParam);
        return self->HandleMessage(hwnd, message, wParam, lParam);
    }

    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (target_)
                target_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            return 0;
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
            // 位置和缩放都以记下的那块显示器为准，不跟前台窗口走；显示器布局或缩放变了就重新排。
            anchor_ = RefreshVoiceOverlayAnchorForDisplayChange(anchor_);
            ApplyLayout();
            return 0;
        case WM_SETTINGCHANGE:
            if (wParam == SPI_SETWORKAREA)
            {
                anchor_ = RefreshVoiceOverlayAnchorForDisplayChange(anchor_);
                ApplyLayout();
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT)
            {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(hwnd, &point);
                SetCursor(LoadCursorW(nullptr, HitTest(point) != Action::None ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE: {
            const Action hover = HitTest(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (hover != hover_)
            {
                hover_ = hover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (!tracking_mouse_)
            {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                tracking_mouse_ = TrackMouseEvent(&track) != FALSE;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_mouse_ = false;
            if (hover_ != Action::None)
            {
                hover_ = Action::None;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
            pressed_ = HitTest(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (pressed_ != Action::None)
            {
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            const Action pressed = pressed_;
            pressed_ = Action::None;
            if (GetCapture() == hwnd)
                ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            if (pressed != Action::None && pressed == HitTest(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}))
            {
                open_settings_ = pressed == Action::OpenSettings;
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (pressed_ != Action::None)
            {
                pressed_ = Action::None;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_RBUTTONUP:
            DestroyWindow(hwnd);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: {
            {
                std::lock_guard<std::mutex> lock(g_current_mutex);
                if (g_current_notice == hwnd)
                    g_current_notice = nullptr;
            }
            target_.Reset();
            hwnd_ = nullptr;
            PostQuitMessage(0);
            return 0;
        }
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool CreateFormat(DWRITE_FONT_WEIGHT weight, float size, ComPtr<IDWriteTextFormat> &format)
    {
        return SUCCEEDED(write_factory_->CreateTextFormat(kFontFamily, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                          DWRITE_FONT_STRETCH_NORMAL, size, kLocale,
                                                          format.ReleaseAndGetAddressOf()));
    }

    float Scale() const
    {
        return anchor_.valid ? anchor_.scale() : static_cast<float>(GetDpiForSystem()) / kVoiceOverlayDefaultDpi;
    }

    float MeasureWidth(const wchar_t *text, IDWriteTextFormat *format) const
    {
        ComPtr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(write_factory_->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)), format, 1000.0f,
                                                    kButtonHeight, layout.GetAddressOf())) ||
            FAILED(layout->GetMetrics(&metrics)))
            return 0.0f;
        return metrics.widthIncludingTrailingWhitespace;
    }

    // 按当前锚定显示器重算 DIP 几何与正文排版，返回窗口在屏幕上的像素矩形。
    RECT ComputeLayout()
    {
        const float scale = (std::max)(0.5f, Scale());
        float work_width = kPreferredWidth + 2.0f * kScreenInset;
        float work_height = 10000.0f;
        if (anchor_.valid)
        {
            work_width = static_cast<float>(anchor_.work.right - anchor_.work.left) / scale;
            work_height = static_cast<float>(anchor_.work.bottom - anchor_.work.top) / scale;
        }
        width_ = (std::max)(160.0f, (std::min)(kPreferredWidth, work_width - 2.0f * kScreenInset));

        // 正文与标题文字左对齐，缩进在图标之后。
        const float text_left = kPadding + kIconSize + kIconGap;
        const float body_width = (std::max)(1.0f, width_ - text_left - kPadding);
        body_layout_.Reset();
        float body_height = kBodyLineStep;
        if (SUCCEEDED(write_factory_->CreateTextLayout(message_.c_str(), static_cast<UINT32>(message_.size()),
                                                       body_format_.Get(), body_width, kMaxLayoutHeight,
                                                       body_layout_.GetAddressOf())))
        {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(body_layout_->GetMetrics(&metrics)))
                body_height = (std::max)(kBodyLineStep, std::ceil(metrics.height));
        }
        body_top_ = kPadding + kTitleHeight + kBodyGap;
        const float footer_top = body_top_ + body_height + kFooterGap;
        height_ =
            (std::min)(footer_top + kButtonHeight + kPadding, (std::max)(120.0f, work_height - 2.0f * kScreenInset));

        // 按钮右对齐，主按钮在最右。
        const float button_top = height_ - kPadding - kButtonHeight;
        float right = width_ - kPadding;
        for (auto it = buttons_.rbegin(); it != buttons_.rend(); ++it)
        {
            const float w =
                (std::max)(kButtonMinWidth, MeasureWidth(it->label, button_format_.Get()) + 2.0f * kButtonTextPadding);
            it->rect = D2D1::RectF(right - w, button_top, right, button_top + kButtonHeight);
            right -= w + kButtonGap;
        }

        const int width_px = (std::max)(1, static_cast<int>(std::lround(width_ * scale)));
        const int height_px = (std::max)(1, static_cast<int>(std::lround(height_ * scale)));
        RECT rect = VoiceOverlayBottomAnchoredRect(anchor_, width_px, height_px);
        if (rect.right <= rect.left || rect.bottom <= rect.top)
        {
            // 拿不到显示器信息时退回主屏居中。
            const int screen_w = GetSystemMetrics(SM_CXSCREEN);
            const int screen_h = GetSystemMetrics(SM_CYSCREEN);
            rect.left = (screen_w - width_px) / 2;
            rect.top = (screen_h - height_px) / 2;
            rect.right = rect.left + width_px;
            rect.bottom = rect.top + height_px;
        }
        return rect;
    }

    void ApplyLayout()
    {
        if (!hwnd_)
            return;
        const RECT rect = ComputeLayout();
        SetWindowPos(hwnd_, HWND_TOPMOST, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOACTIVATE);
        if (target_)
        {
            const float dpi = Scale() * kVoiceOverlayDefaultDpi;
            target_->SetDpi(dpi, dpi);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    Action HitTest(POINT client) const
    {
        const float scale = (std::max)(0.01f, Scale());
        const float x = static_cast<float>(client.x) / scale;
        const float y = static_cast<float>(client.y) / scale;
        for (const ButtonSpec &button : buttons_)
        {
            if (x >= button.rect.left && x < button.rect.right && y >= button.rect.top && y < button.rect.bottom)
                return button.action;
        }
        return Action::None;
    }

    bool EnsureTarget()
    {
        if (target_)
            return true;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        if (FAILED(factory_->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                             D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                D2D1::HwndRenderTargetProperties(
                    hwnd_, D2D1::SizeU(static_cast<UINT>(rc.right - rc.left), static_cast<UINT>(rc.bottom - rc.top))),
                target_.GetAddressOf())))
            return false;
        const float dpi = Scale() * kVoiceOverlayDefaultDpi;
        target_->SetDpi(dpi, dpi);
        if (FAILED(target_->CreateSolidColorBrush(palette_.text, brush_.ReleaseAndGetAddressOf())))
        {
            target_.Reset();
            return false;
        }
        return true;
    }

    void Fill(const D2D1_ROUNDED_RECT &shape, const D2D1_COLOR_F &color)
    {
        brush_->SetColor(color);
        target_->FillRoundedRectangle(shape, brush_.Get());
    }

    void DrawIcon(float cx, float cy)
    {
        const bool error = kind_ == VoiceNoticeKind::Error;
        brush_->SetColor(error ? palette_.error : palette_.accent);
        const float r = kIconSize * 0.5f;
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush_.Get());
        // 白色的「!」或「i」：一竖加一点，错误时点在下，提示时点在上。
        brush_->SetColor(palette_.panel);
        const float stroke = 2.0f;
        const float dot_y = error ? cy + 4.25f : cy - 4.25f;
        const float bar_top = error ? cy - 5.0f : cy - 1.5f;
        const float bar_bottom = error ? cy + 1.5f : cy + 5.0f;
        target_->DrawLine(D2D1::Point2F(cx, bar_top), D2D1::Point2F(cx, bar_bottom), brush_.Get(), stroke);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, dot_y), 1.2f, 1.2f), brush_.Get());
    }

    void Paint()
    {
        if (!hwnd_ || !EnsureTarget())
            return;
        target_->BeginDraw();
        target_->SetTransform(D2D1::Matrix3x2F::Identity());
        target_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        const D2D1_ROUNDED_RECT panel =
            D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, width_ - 0.5f, height_ - 0.5f), kCornerRadius, kCornerRadius);
        Fill(panel, palette_.panel);
        brush_->SetColor(palette_.border);
        target_->DrawRoundedRectangle(panel, brush_.Get(), 1.0f);

        const float title_center = kPadding + kTitleHeight * 0.5f;
        DrawIcon(kPadding + kIconSize * 0.5f, title_center);
        const float text_left = kPadding + kIconSize + kIconGap;
        const wchar_t *title = kind_ == VoiceNoticeKind::Error ? L"语音输入出错了" : L"语音输入还没准备好";
        brush_->SetColor(palette_.text);
        target_->DrawTextW(title, static_cast<UINT32>(wcslen(title)), title_format_.Get(),
                           D2D1::RectF(text_left, kPadding, width_ - kPadding, kPadding + kTitleHeight), brush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        if (body_layout_)
        {
            const float footer_top = height_ - kPadding - kButtonHeight - kFooterGap;
            target_->PushAxisAlignedClip(D2D1::RectF(text_left, body_top_, width_ - kPadding, footer_top),
                                         D2D1_ANTIALIAS_MODE_ALIASED);
            brush_->SetColor(palette_.secondary);
            target_->DrawTextLayout(D2D1::Point2F(text_left, body_top_), body_layout_.Get(), brush_.Get());
            target_->PopAxisAlignedClip();
        }

        for (const ButtonSpec &button : buttons_)
        {
            const bool active = hover_ == button.action || pressed_ == button.action;
            const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(button.rect, kButtonHeight * 0.5f, kButtonHeight * 0.5f);
            if (button.primary)
            {
                Fill(shape, active ? palette_.primary_hover : palette_.primary);
                brush_->SetColor(palette_.on_primary);
            }
            else
            {
                if (active)
                    Fill(shape, palette_.hover);
                const D2D1_ROUNDED_RECT inset =
                    D2D1::RoundedRect(D2D1::RectF(button.rect.left + 0.5f, button.rect.top + 0.5f,
                                                  button.rect.right - 0.5f, button.rect.bottom - 0.5f),
                                      kButtonHeight * 0.5f - 0.5f, kButtonHeight * 0.5f - 0.5f);
                brush_->SetColor(palette_.border);
                target_->DrawRoundedRectangle(inset, brush_.Get(), 1.0f);
                brush_->SetColor(palette_.text);
            }
            target_->DrawTextW(button.label, static_cast<UINT32>(wcslen(button.label)), button_format_.Get(),
                               button.rect, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        if (target_->EndDraw() == D2DERR_RECREATE_TARGET)
            target_.Reset();
    }

    VoiceNoticeKind kind_;
    std::wstring message_;
    Palette palette_;
    HWND hwnd_ = nullptr;
    VoiceOverlayAnchor anchor_;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> write_factory_;
    ComPtr<IDWriteTextFormat> title_format_;
    ComPtr<IDWriteTextFormat> body_format_;
    ComPtr<IDWriteTextFormat> button_format_;
    ComPtr<IDWriteTextLayout> body_layout_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    std::vector<ButtonSpec> buttons_;
    float width_ = kPreferredWidth;
    float height_ = 160.0f;
    float body_top_ = 0.0f;
    Action hover_ = Action::None;
    Action pressed_ = Action::None;
    bool tracking_mouse_ = false;
    bool open_settings_ = false;
};
} // namespace

void ShowVoiceNoticeWindow(VoiceNoticeKind kind, std::wstring message)
{
    std::thread([kind, message = std::move(message)]() mutable {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        // 语音浮层都按 appearance.theme_voice 取明暗，这里一致。
        const bool light = ResolveConfiguredTheme(GetConfiguredThemeVoice()) == "light";
        bool open_settings = false;
        {
            NoticeWindow window(kind, std::move(message), light);
            if (window.Create())
            {
                MSG msg{};
                while (GetMessageW(&msg, nullptr, 0, 0) > 0)
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                open_settings = window.OpenSettingsRequested();
            }
        }
        if (open_settings)
            OpenSettingsVoiceApplication();
        if (SUCCEEDED(com))
            CoUninitialize();
    }).detach();
}
