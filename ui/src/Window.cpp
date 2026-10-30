#include "msimeui/Window.h"

#include "msimeui/Animation.h"
#include "msimeui/Application.h"
#include "msimeui/Theme.h"
#include "DebugLog.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <d2d1.h>
#include <dwmapi.h>
#include <windowsx.h>

namespace msimeui
{
namespace
{
POINT GetInitialWindowOrigin(int windowWidth, int windowHeight, WindowInitialPlacement placement, int bottomMargin)
{
    POINT origin = {CW_USEDEFAULT, CW_USEDEFAULT};

    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    const HMONITOR monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo))
    {
        return origin;
    }

    const RECT &workArea = monitorInfo.rcWork;
    const int workWidth = std::max(static_cast<int>(workArea.right - workArea.left), 0);
    const int workHeight = std::max(static_cast<int>(workArea.bottom - workArea.top), 0);
    const int clampedWidth = std::min(std::max(windowWidth, 0), workWidth);
    const int clampedHeight = std::min(std::max(windowHeight, 0), workHeight);

    origin.x = workArea.left + std::max((workWidth - clampedWidth) / 2, 0);
    if (placement == WindowInitialPlacement::BottomCenter)
    {
        const int margin = std::max(bottomMargin, 0);
        origin.y = workArea.bottom - clampedHeight - margin;
        if (origin.y < workArea.top)
            origin.y = workArea.top;
    }
    else
    {
        origin.y = workArea.top + std::max((workHeight - clampedHeight) / 2, 0);
    }
    return origin;
}
} // namespace

Window::Window(std::wstring className, std::wstring title, int width, int height)
    : className_(std::move(className)), title_(std::move(title)), width_(width), height_(height),
      instance_(GetModuleHandleW(nullptr))
{
}

Window::~Window() = default;

bool Window::Create()
{
    HICON largeIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                                    GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    HICON smallIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hIcon = largeIcon;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = className_.c_str();
    windowClass.hIconSm = smallIcon;
    RegisterClassExW(&windowClass);

    const POINT origin = GetInitialWindowOrigin(width_, height_, initialPlacement_, initialBottomMargin_);
    hwnd_ = CreateWindowExW(extendedWindowStyle_, className_.c_str(), title_.c_str(), windowStyle_, origin.x, origin.y,
                            width_, height_, nullptr, nullptr, instance_, this);
    if (hwnd_)
    {
        if (roundedCorners_)
        {
            const DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUND;
            DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
        }
        if (largeIcon)
        {
            SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
        }
        if (smallIcon)
        {
            SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
        }
        StartCaretTimer();
    }
    return hwnd_ != nullptr;
}

void Window::StartCaretTimer()
{
    if (hwnd_)
    {
        SetTimer(hwnd_, 1, GetCaretBlinkTime(), nullptr);
    }
}

void Window::SetQuitOnDestroy(bool quit)
{
    quitOnDestroy_ = quit;
}

void Window::SetBackgroundColor(const D2D1_COLOR_F &color)
{
    backgroundColor_ = color;
    hasBackgroundColor_ = true;
    Invalidate();
}

void Window::SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE mode)
{
    textAntialiasMode_ = mode;
    Invalidate();
}

void Window::SetCompositionRendering(bool enabled, bool opaque)
{
    deviceResources_.SetCompositionOpaque(opaque);
    if (composition_ == enabled)
    {
        return;
    }
    composition_ = enabled;
    deviceResources_.DiscardTarget();
    Invalidate();
}

void Window::RequestAnimationFrame()
{
    // Invalidating while painting would be validated again by EndPaint; OnPaint asks
    // for the next frame once it is done instead.
    animationRequested_ = true;
    if (!painting_ && hwnd_)
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

double Window::GetFrameTime() const
{
    return painting_ ? frameTimeMs_ : NowMs();
}

double Window::NextFrameTimeMs() const
{
    const double now = NowMs();
    if (!composition_)
    {
        return now;
    }
    // A composition swap chain shows the frame at the next vertical blank. Animating to
    // that time instead of to when painting happened to start keeps the steps between
    // frames even: the paint start wobbles by a few milliseconds after each DwmFlush,
    // and at scrolling speed that is several pixels of judder.
    DWM_TIMING_INFO timing = {};
    timing.cbSize = sizeof(timing);
    static const double frequency = [] {
        LARGE_INTEGER value = {};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &timing)) || timing.qpcRefreshPeriod == 0 || frequency <= 0.0)
    {
        return now;
    }
    const double period = static_cast<double>(timing.qpcRefreshPeriod) * 1000.0 / frequency;
    const double vblank = static_cast<double>(timing.qpcVBlank) * 1000.0 / frequency;
    // qpcVBlank is the vertical blank the compositor is working towards, which may lie a
    // period or two ahead; only its place on the refresh grid matters here.
    if (period < 2.0 || period > 100.0 || std::fabs(vblank - now) > 1000.0)
    {
        return now;
    }
    return vblank + std::ceil((now - vblank) / period) * period;
}

void Window::RefreshHover()
{
    if (!hwnd_)
    {
        return;
    }
    POINT screen = {};
    GetCursorPos(&screen);
    if (WindowFromPoint(screen) != hwnd_ || GetCapture() == hwnd_)
    {
        return;
    }
    POINT point = screen;
    ScreenToClient(hwnd_, &point);
    RECT client = {};
    GetClientRect(hwnd_, &client);
    if (!PtInRect(&client, point))
    {
        return;
    }
    UpdateHover(point);
    SetCursor(hoveredVisual_ ? hoveredVisual_->GetCursor() : LoadCursor(nullptr, IDC_ARROW));
}

void Window::UpdateHover(const POINT &point)
{
    Visual *hoverTarget = scene_ ? scene_->FindVisualAt(ClientPixelsToDips(point)) : nullptr;
    if (hoveredVisual_ == hoverTarget)
    {
        return;
    }
    if (hoveredVisual_)
    {
        hoveredVisual_->OnMouseLeave();
    }
    hoveredVisual_ = hoverTarget;
    if (hoveredVisual_)
    {
        hoveredVisual_->OnMouseEnter();
    }
}

LRESULT Window::OnLeftButtonDown(WPARAM wParam, LPARAM lParam)
{
    const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (!scene_)
    {
        return 0;
    }
    const PointF dipPoint = ClientPixelsToDips(point);
    Visual *target = scene_->FindVisualAt(dipPoint);
    scene_->DismissPopupsForClick(dipPoint, target);
    target = scene_->FindVisualAt(dipPoint);
    Visual *focusTarget = scene_->FindFocusableAt(dipPoint);
    Visual *dispatchTarget = focusTarget ? focusTarget : target;
    MSIMEUI_DEBUG_LOG("WM_LBUTTONDOWN point=(" << point.x << "," << point.y << ") target=" << target << " focusTarget="
                                               << focusTarget << " dispatchTarget=" << dispatchTarget);
    if (focusTarget && focusTarget->IsFocusable())
    {
        SetFocusedVisual(focusTarget);
        if (stealFocusOnClick_)
        {
            SetFocus(hwnd_);
        }
    }
    else if (!focusTarget)
    {
        // A non-focusable mouse target can retain the current keyboard focus (e.g. a searchable menu).
        // Only a click with no such target ends the previous control's edit session.
        SetFocusedVisual(nullptr);
    }
    capturedVisual_ = dispatchTarget;
    if (dispatchTarget && dispatchTarget->OnMouseDown(point, wParam))
    {
        SetCapture(hwnd_);
    }
    return 0;
}

bool Window::AdoptExistingHwnd(HWND hwnd)
{
    hwnd_ = hwnd;
    adoptedHwnd_ = hwnd != nullptr;
    return adoptedHwnd_;
}

LRESULT Window::DispatchImportedMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    return HandleMessage(message, wParam, lParam);
}

void Window::SetStealFocusOnClick(bool enabled)
{
    stealFocusOnClick_ = enabled;
}

int Window::Run(int nCmdShow)
{
    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

HWND Window::GetHandle() const
{
    return hwnd_;
}

HINSTANCE Window::GetInstance() const
{
    return instance_;
}

Scene *Window::GetScene() const
{
    return scene_.get();
}

DeviceResources &Window::GetDeviceResources()
{
    return deviceResources_;
}

void Window::SetDpiOverride(FLOAT dpi)
{
    const FLOAT normalized = dpi > 0.0f ? dpi : 0.0f;
    if (normalized == dpiOverride_)
    {
        return;
    }
    dpiOverride_ = normalized;
    // The presenter usually drives its own DeviceResources for composition,
    // but the HwndRenderTarget path (OnPaint) must not keep rendering at the
    // stale system DPI.
    deviceResources_.SetDpiOverride(dpi);
}

float Window::GetDpi() const
{
    if (dpiOverride_ > 0.0f)
    {
        return dpiOverride_;
    }
    return hwnd_ ? static_cast<float>(GetDpiForWindow(hwnd_)) : 96.0f;
}

PointF Window::ClientPixelsToDips(const POINT &point) const
{
    return ToDips(point, GetDpi());
}

SizeF Window::ClientPixelsToDips(const SIZE &size) const
{
    return ToDips(size, GetDpi());
}

RECT Window::DipsToClientPixels(const RectF &rect) const
{
    return ToRectPixels(rect, GetDpi());
}

void Window::Invalidate()
{
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Window::Invalidate(const RectF &rect)
{
    if (!hwnd_)
    {
        return;
    }

    if (rect.width <= 0.0f || rect.height <= 0.0f)
    {
        Invalidate();
        return;
    }

    RECT pixelRect = DipsToClientPixels(rect);
    InvalidateRect(hwnd_, &pixelRect, FALSE);
}

void Window::InvalidateMeasure()
{
    if (scene_)
    {
        scene_->InvalidateMeasure();
        return;
    }

    Invalidate();
}

void Window::InvalidateMeasure(Visual *source)
{
    if (scene_)
    {
        scene_->InvalidateMeasure(source);
        return;
    }

    Invalidate();
}

void Window::InvalidateArrange()
{
    if (scene_)
    {
        scene_->InvalidateArrange();
        return;
    }

    Invalidate();
}

void Window::InvalidateArrange(Visual *source)
{
    if (scene_)
    {
        scene_->InvalidateArrange(source);
        return;
    }

    Invalidate();
}

void Window::Relayout()
{
    if (!hwnd_ || !scene_)
    {
        return;
    }

    RECT rc = {};
    GetClientRect(hwnd_, &rc);
    const SIZE pixelSize = {rc.right, rc.bottom};
    scene_->InvalidateMeasure();
    scene_->EnsureLayout(ClientPixelsToDips(pixelSize));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Window::SetScene(std::unique_ptr<Scene> scene)
{
    // The cached targets point into the outgoing scene's visual tree, which is destroyed by the assignment below, so
    // they have to be dropped first or the next mouse or keyboard message dispatches through a dangling pointer.
    if (capturedVisual_ && hwnd_ && GetCapture() == hwnd_)
    {
        ReleaseCapture();
    }
    focusedVisual_ = nullptr;
    hoveredVisual_ = nullptr;
    capturedVisual_ = nullptr;

    scene_ = std::move(scene);
    if (scene_)
    {
        scene_->Attach(this);
        OnSize();
    }
}

LRESULT CALLBACK Window::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    Window *window = nullptr;
    if (message == WM_NCCREATE)
    {
        auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
        window = static_cast<Window *>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hwnd_ = hwnd;
    }
    else
    {
        window = reinterpret_cast<Window *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window)
    {
        return window->HandleMessage(message, wParam, lParam);
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Window::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_LBUTTONDOWN:
    // A class with CS_DBLCLKS turns every second click into this; it is still a press.
    case WM_LBUTTONDBLCLK:
        return OnLeftButtonDown(wParam, lParam);

    case WM_LBUTTONUP: {
        const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        Visual *target = capturedVisual_ ? capturedVisual_ : focusedVisual_;
        capturedVisual_ = nullptr;
        if (target)
        {
            target->OnMouseUp(point, wParam);
        }
        if (GetCapture() == hwnd_)
        {
            ReleaseCapture();
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        // Capture taken away mid-press (a modal dialog, Alt+Tab): end the press so the
        // visual does not stay in its pressed or dragging state.
        if (reinterpret_cast<HWND>(lParam) != hwnd_ && capturedVisual_)
        {
            Visual *target = capturedVisual_;
            capturedVisual_ = nullptr;
            POINT point = {};
            GetCursorPos(&point);
            ScreenToClient(hwnd_, &point);
            target->OnMouseUp(point, 0);
        }
        return 0;

    case WM_RBUTTONUP: {
        const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (scene_)
        {
            const PointF dipPoint = ClientPixelsToDips(point);
            Visual *target = scene_->FindVisualAt(dipPoint);
            scene_->DismissPopupsForClick(dipPoint, target);
            target = scene_->FindVisualAt(dipPoint);
            if (target && target->OnContextMenu(point, wParam))
            {
                return 0;
            }
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!mouseLeaveTracking_)
        {
            TRACKMOUSEEVENT tracking = {};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = hwnd_;
            if (TrackMouseEvent(&tracking))
            {
                mouseLeaveTracking_ = true;
            }
        }

        const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        // One hit test serves both the hover state and the cursor.
        UpdateHover(point);
        Visual *hoverTarget = hoveredVisual_;
        SetCursor(hoverTarget ? hoverTarget->GetCursor() : LoadCursor(nullptr, IDC_ARROW));

        if (capturedVisual_)
        {
            capturedVisual_->OnMouseMove(point, wParam);
        }
        else if (hoverTarget && hoverTarget->OnMouseMove(point, wParam))
        {
            return 0;
        }
        else if (focusedVisual_)
        {
            focusedVisual_->OnMouseMove(point, wParam);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        mouseLeaveTracking_ = false;
        if (hoveredVisual_)
        {
            hoveredVisual_->OnMouseLeave();
            hoveredVisual_ = nullptr;
        }
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return 0;

    case WM_MOUSEWHEEL:
        if (OnMouseWheel(wParam, lParam))
        {
            // Content moved under a still cursor.
            RefreshHover();
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (focusedVisual_ && focusedVisual_->OnKeyDown(wParam, lParam))
        {
            return 0;
        }
        if (wParam == VK_ESCAPE && focusedVisual_)
        {
            SetFocusedVisual(nullptr);
            return 0;
        }
        break;

    case WM_CHAR:
        if (focusedVisual_ && focusedVisual_->OnChar(static_cast<wchar_t>(wParam), lParam))
        {
            return 0;
        }
        break;

    case WM_SETFOCUS:
        if (focusedVisual_)
        {
            focusedVisual_->OnFocusChanged(true);
        }
        return 0;

    case WM_KILLFOCUS:
        if (focusedVisual_)
        {
            focusedVisual_->OnFocusChanged(false);
        }
        return 0;

    case WM_TIMER:
        if (wParam == kImageAnimationTimerId)
        {
            KillTimer(hwnd_, kImageAnimationTimerId);
            Invalidate();
            return 0;
        }
        if (focusedVisual_ && focusedVisual_->OnTimer(wParam))
        {
            return 0;
        }
        if (scene_ && scene_->OnTimer(wParam))
        {
            return 0;
        }
        break;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            POINT point = {};
            GetCursorPos(&point);
            ScreenToClient(hwnd_, &point);
            UpdateCursorForClientPoint(point);
            return TRUE;
        }
        break;

    case WM_NCHITTEST: {
        const LRESULT defaultHit = DefWindowProcW(hwnd_, message, wParam, lParam);
        if (defaultHit == HTCLIENT && dragRegionHeight_ > 0.0f)
        {
            POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &point);
            RECT client = {};
            GetClientRect(hwnd_, &client);
            const float dragHeightPixels = DipsToPixels(dragRegionHeight_, GetDpi());
            const float reservedRightPixels = DipsToPixels(58.0f, GetDpi());
            if (static_cast<float>(point.y) < dragHeightPixels &&
                static_cast<float>(point.x) < static_cast<float>(client.right) - reservedRightPixels)
            {
                return HTCAPTION;
            }
        }
        return defaultHit;
    }

    case WM_SIZE:
        OnSize();
        return 0;

    case WM_PAINT:
        OnPaint();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd_, 1);
        KillTimer(hwnd_, kImageAnimationTimerId);
        if (quitOnDestroy_)
        {
            PostQuitMessage(0);
        }
        return 0;

    default:
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void Window::OnPaint()
{
    PAINTSTRUCT paint = {};
    BeginPaint(hwnd_, &paint);

    painting_ = true;
    animationRequested_ = false;
    frameTimeMs_ = NextFrameTimeMs();
    if (composition_ ? deviceResources_.EnsureForComposition(hwnd_) : deviceResources_.EnsureForWindow(hwnd_))
    {
        ID2D1RenderTarget *target = deviceResources_.GetRenderTarget();
        deviceResources_.BeginFrame();
        target->BeginDraw();
        target->SetTextAntialiasMode(textAntialiasMode_);
        target->Clear(hasBackgroundColor_ ? backgroundColor_ : ThemeManager::GetCurrent().windowBackground);
        if (scene_)
        {
            RECT rc = {};
            GetClientRect(hwnd_, &rc);
            const SIZE pixelSize = {rc.right, rc.bottom};
            scene_->EnsureLayout(ClientPixelsToDips(pixelSize));
            scene_->Render(deviceResources_);
        }

        HRESULT hr = target->EndDraw();
        if (SUCCEEDED(hr) && composition_)
        {
            hr = deviceResources_.Present();
        }
        if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        {
            deviceResources_.DiscardTarget();
        }
        else if (FAILED(hr))
        {
            MSIMEUI_DEBUG_LOG("Window::OnPaint EndDraw failed hr=0x" << std::hex << static_cast<unsigned long>(hr));
        }
    }
    painting_ = false;

    EndPaint(hwnd_, &paint);
    // Animated images change at their own frame times, far apart compared with the display
    // rate, so they get a one-shot timer for the next change instead of a frame loop.
    const auto imageDeadline = deviceResources_.GetNextAnimationDeadline();
    // An adopted HWND belongs to a host that may not route WM_TIMER back here; it schedules
    // its own frames from the same deadline.
    if (imageDeadline && !animationRequested_ && !adoptedHwnd_)
    {
        const auto wait =
            std::chrono::ceil<std::chrono::milliseconds>(*imageDeadline - DeviceResources::AnimationClock::now());
        SetTimer(hwnd_, kImageAnimationTimerId, static_cast<UINT>((std::max)(wait.count(), 1LL)), nullptr);
    }
    else
    {
        KillTimer(hwnd_, kImageAnimationTimerId);
    }
    if (animationRequested_)
    {
        // The HWND target presents in step with the display; a composition swap chain
        // does not, so wait for the next composition to pace the animation at the
        // refresh rate instead of spinning.
        if (composition_)
        {
            DwmFlush();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Window::OnSize()
{
    RECT rc = {};
    GetClientRect(hwnd_, &rc);
    deviceResources_.Resize(static_cast<UINT>(rc.right), static_cast<UINT>(rc.bottom));

    if (scene_)
    {
        const SIZE pixelSize = {rc.right, rc.bottom};
        scene_->EnsureLayout(ClientPixelsToDips(pixelSize));
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool Window::OnMouseWheel(WPARAM wParam, LPARAM lParam)
{
    if (!scene_)
    {
        return false;
    }

    POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ScreenToClient(hwnd_, &point);
    return scene_->OnMouseWheel(point, GET_WHEEL_DELTA_WPARAM(wParam), GET_KEYSTATE_WPARAM(wParam));
}

bool Window::UpdateCursorForClientPoint(const POINT &point)
{
    if (!scene_)
    {
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return false;
    }

    const PointF dipPoint = ClientPixelsToDips(point);
    Visual *target = scene_->FindVisualAt(dipPoint);
    SetCursor(target ? target->GetCursor() : LoadCursor(nullptr, IDC_ARROW));
    return target != nullptr;
}

void Window::FocusVisual(Visual *visual)
{
    SetFocusedVisual(visual);
}

void Window::SetWindowStyle(DWORD style, DWORD extendedStyle)
{
    if (!hwnd_)
    {
        windowStyle_ = style;
        extendedWindowStyle_ = extendedStyle;
    }
}

void Window::SetDragRegionHeight(float height)
{
    dragRegionHeight_ = std::max(height, 0.0f);
}

void Window::SetRoundedCorners(bool enabled)
{
    roundedCorners_ = enabled;
}

void Window::SetInitialPlacement(WindowInitialPlacement placement, int bottomMargin)
{
    if (!hwnd_)
    {
        initialPlacement_ = placement;
        initialBottomMargin_ = bottomMargin;
    }
}

void Window::SetFocusedVisual(Visual *visual)
{
    if (focusedVisual_ == visual)
    {
        return;
    }

    MSIMEUI_DEBUG_LOG("SetFocusedVisual old=" << focusedVisual_ << " new=" << visual);

    if (focusedVisual_)
    {
        focusedVisual_->OnFocusChanged(false);
    }

    focusedVisual_ = visual;

    if (focusedVisual_ && GetFocus() == hwnd_)
    {
        focusedVisual_->OnFocusChanged(true);
    }
}
} // namespace msimeui
