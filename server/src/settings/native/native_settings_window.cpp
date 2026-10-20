#include "settings/native/native_settings_window.h"

#include "settings/native/native_page_help_widgets.h"
#include "settings/native/native_page_skin_common.h"
#include "settings/native/native_settings_controls.h"
#include "settings/native/native_settings_model.h"
#include "settings/native/native_settings_pages.h"
#include "settings/native/native_settings_theme.h"

#include "utils/common_utils.h"

#include "msimeui/Animation.h"
#include "msimeui/Compat.h"
#include "msimeui/DeviceResources.h"
#include "msimeui/Layout.h"
#include "msimeui/Scene.h"
#include "msimeui/Window.h"

#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>

namespace native_settings
{
namespace
{
constexpr float kTitleBarHeight = 32.0f;    // --titlebar-height
constexpr float kSidebarWidth = 200.0f;     // --sidebar-width
constexpr float kScrollbarWidth = 8.0f;     // ::-webkit-scrollbar { width: 8px }
constexpr float kContentMaxWidth = 900.0f;  // #content-container > div { max-width: 900px }
constexpr float kWindowButtonWidth = 42.0f; // .window-button
constexpr float kWindowButtonGap = 2.0f;    // .window-controls { gap: 2px }
constexpr UINT_PTR kToolTipTimer = 0x4E53;
constexpr UINT_PTR kToastTimer = 0x4E54;
// Next frame of an animated skin image (APNG / GIF) in a preview. msimeui::Window does not
// time an adopted HWND's frames, so the window does it from the same deadline.
constexpr UINT_PTR kImageFrameTimer = 0x4E55;
// Lifts the launch splash if no configSnapshot arrives, so a failed config load still
// leaves a usable window.
constexpr UINT_PTR kSplashTimer = 0x4E56;

// 系统「显示动画」关闭时预览里的动图只显示首帧，与候选窗同一个开关；读不到时按开启处理。
bool SystemAnimationsEnabled()
{
    BOOL enabled = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0))
        return true;
    return enabled != FALSE;
}

msimeui::ScrollViewer::Appearance WebKitScrollbar(float reserved)
{
    const Palette &p = CurrentPalette();
    msimeui::ScrollViewer::Appearance appearance;
    appearance.reservedWidth = reserved;
    appearance.trackWidth = kScrollbarWidth;
    appearance.thumbWidth = kScrollbarWidth;
    appearance.cornerRadius = 2.0f;
    appearance.drawTrack = false;
    appearance.thumbMinLength = 24.0f;
    appearance.thumbColor = p.scrollbarThumb;
    appearance.thumbHoverColor = p.scrollbarThumbHover;
    appearance.hoverThumbOnly = true;
    // Chrome's 100px notch, gliding about as long as its wheel animation.
    appearance.wheelStep = 100.0f;
    appearance.smoothScrollMs = 260.0;
    return appearance;
}

const std::string &ThemedSvg(const std::wstring &relativePath, bool light)
{
    // Assets are white; the page turns them dark with `invert(1) brightness(b)`.
    struct Entry
    {
        std::string dark;
        std::string light;
    };
    static std::map<std::wstring, Entry> cache;
    auto it = cache.find(relativePath);
    if (it == cache.end())
    {
        Entry entry;
        entry.dark = LoadAssetSvg(relativePath);
        const float brightness = relativePath.rfind(L"sidebar/", 0) == 0 ? 0.25f : 0.2f;
        entry.light = InvertSvgColors(entry.dark, brightness);
        it = cache.emplace(relativePath, std::move(entry)).first;
    }
    return light ? it->second.light : it->second.dark;
}

// ---------------------------------------------------------------------------------
// Title bar: `.titlebar` with the window controls on the right.
// ---------------------------------------------------------------------------------
class TitleBar : public msimeui::Visual
{
  public:
    // Maximize is reported as a non-client button (HTMAXBUTTON brings up Snap Layouts);
    // its input arrives as non-client messages and the host forwards the state.
    LRESULT HitTestCaption(const PointF &point) const
    {
        if (!Contains(bounds_, point) || Contains(ButtonRect(HTMINBUTTON), point) ||
            Contains(ButtonRect(HTCLOSE), point))
            return HTCLIENT;
        return Contains(ButtonRect(HTMAXBUTTON), point) ? HTMAXBUTTON : HTCAPTION;
    }

    void SetNonClientButtonState(LRESULT hot, LRESULT pressed)
    {
        // The host only tracks maximize; leaving it must not clear a client button the
        // mouse already moved onto.
        if (hot == HTNOWHERE && pressed == HTNOWHERE && IsClientButton(hot_))
            return;
        UpdateButtons(hot, pressed);
    }

    void SetActive(bool active)
    {
        if (active_ == active)
            return;
        active_ = active;
        dim_.AnimateTo(active ? 1.0f : 0.52f, 140.0, msimeui::Easing::EaseOut(), msimeui::NowMs());
        if (window_)
            window_->RequestAnimationFrame();
    }

    void Reset()
    {
        hot_ = HTNOWHERE;
        pressed_ = HTNOWHERE;
        tooltip_ = HTNOWHERE;
        if (window_)
            KillTimer(window_->GetHandle(), kToolTipTimer);
        InvalidateVisual();
    }

    void ShowPendingToolTip()
    {
        if (window_)
            KillTimer(window_->GetHandle(), kToolTipTimer);
        if (hot_ == HTNOWHERE || pressed_ != HTNOWHERE)
            return;
        tooltip_ = hot_;
        tooltipFade_.Start(0.0f, 1.0f, 120.0, msimeui::Easing::Ease(), msimeui::NowMs());
        if (window_)
            window_->RequestAnimationFrame();
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        return {availableSize.width, kTitleBarHeight};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        const double now = window_ ? window_->GetFrameTime() : msimeui::NowMs();
        const float dim = dim_.Value(now);
        if (dim_.IsRunning(now) && window_)
            window_->RequestAnimationFrame();

        // .window-title: 12px, letter-spacing 0.2px, opacity 0.92.
        TextStyle title;
        title.size = 12.0f;
        title.letterSpacing = 0.2f;
        DrawTextInRect(dr, L"Settings", title,
                       {bounds_.x + 12.0f, bounds_.y, std::max(0.0f, ButtonRect(HTMINBUTTON).x - bounds_.x - 20.0f),
                        bounds_.height},
                       WithAlpha(p.text, 0.92f * dim));

        for (LRESULT button : {HTMINBUTTON, HTMAXBUTTON, HTCLOSE})
            RenderButton(dr, button, dim);
    }

    void RenderOverlay(msimeui::DeviceResources &dr)
    {
        if (tooltip_ == HTNOWHERE)
            return;
        const double now = window_ ? window_->GetFrameTime() : msimeui::NowMs();
        const float t = tooltipFade_.Value(now);
        if (tooltipFade_.IsRunning(now) && window_)
            window_->RequestAnimationFrame();
        if (t <= 0.0f && !tooltipFade_.IsRunning(now))
        {
            tooltip_ = HTNOWHERE;
            return;
        }
        const Palette &p = CurrentPalette();
        // [data-tooltip]::after: 10px text, padding 2px 4px, centered under the button,
        // 2px below it, sliding 2px further down as it fades in.
        TextStyle style;
        style.size = 10.0f;
        style.lineHeight = 1.2f;
        const std::wstring text = ToolTipText(tooltip_);
        const float width = MeasureTextWidth(text, style) + 8.0f + 2.0f;
        const float height = 12.0f + 4.0f + 2.0f;
        const RectF button = ButtonRect(tooltip_);
        float x = button.x + (button.width - width) * 0.5f;
        x = std::min(x, bounds_.x + bounds_.width - width - 1.0f);
        const RectF rect = {std::round(x), button.y + button.height + 2.0f + 2.0f * t, width, height};
        PushOpacity(dr, t);
        FillRounded(dr, rect, 2.0f, p.tooltipBg);
        StrokeRounded(dr, rect, 2.0f, p.tooltipBorder, 1.0f);
        DrawTextInRect(dr, text, style, rect, p.tooltipText, true);
        PopOpacity(dr);
    }

    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        const LRESULT button = ClientButtonAt(point);
        if (button == HTNOWHERE)
            return false;
        UpdateButtons(button, button);
        return true;
    }

    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        if (!IsClientButton(pressed_))
            return false;
        const LRESULT button = ClientButtonAt(point);
        const bool clicked = button == pressed_;
        // .window-controls-click-reset: the highlight goes away on click.
        UpdateButtons(clicked ? HTNOWHERE : button, HTNOWHERE);
        if (clicked && window_)
            PostMessageW(window_->GetHandle(), WM_SYSCOMMAND, button == HTMINBUTTON ? SC_MINIMIZE : SC_CLOSE, 0);
        return true;
    }

    bool OnMouseMove(const POINT &point, WPARAM) override
    {
        UpdateButtons(ClientButtonAt(point), IsClientButton(pressed_) ? pressed_ : HTNOWHERE);
        return true;
    }

    void OnMouseLeave() override
    {
        if (IsClientButton(hot_))
            UpdateButtons(HTNOWHERE, HTNOWHERE);
    }

  private:
    static bool IsClientButton(LRESULT button)
    {
        return button == HTMINBUTTON || button == HTCLOSE;
    }

    std::wstring ToolTipText(LRESULT button) const
    {
        switch (button)
        {
        case HTMINBUTTON:
            return L"Minimize";
        case HTMAXBUTTON:
            return window_ && IsZoomed(window_->GetHandle()) ? L"Restore" : L"Maximize";
        default:
            return L"Close";
        }
    }

    RectF ButtonRect(LRESULT button) const
    {
        const float slot = button == HTCLOSE ? 0.0f : button == HTMAXBUTTON ? 1.0f : 2.0f;
        const float right = bounds_.x + bounds_.width - slot * (kWindowButtonWidth + kWindowButtonGap);
        return {right - kWindowButtonWidth, bounds_.y, kWindowButtonWidth, bounds_.height};
    }

    LRESULT ClientButtonAt(const POINT &point) const
    {
        if (!window_)
            return HTNOWHERE;
        const PointF dip = window_->ClientPixelsToDips(point);
        for (LRESULT button : {HTMINBUTTON, HTCLOSE})
        {
            if (Contains(ButtonRect(button), dip))
                return button;
        }
        return HTNOWHERE;
    }

    void UpdateButtons(LRESULT hot, LRESULT pressed)
    {
        if (hot_ == hot && pressed_ == pressed)
            return;
        const bool hotChanged = hot_ != hot;
        hot_ = hot;
        pressed_ = pressed;
        if (hotChanged || pressed != HTNOWHERE)
        {
            if (tooltip_ != HTNOWHERE)
            {
                tooltip_ = HTNOWHERE;
                tooltipFade_.Snap(0.0f);
            }
            if (window_)
            {
                KillTimer(window_->GetHandle(), kToolTipTimer);
                // transition-delay: 500ms before the tooltip shows.
                if (hot != HTNOWHERE && pressed == HTNOWHERE)
                    SetTimer(window_->GetHandle(), kToolTipTimer, 500, nullptr);
            }
        }
        InvalidateVisual();
        if (window_)
            window_->Invalidate();
    }

    void RenderButton(msimeui::DeviceResources &dr, LRESULT button, float dim)
    {
        const Palette &p = CurrentPalette();
        const RectF rect = ButtonRect(button);
        const bool hot = hot_ == button;
        const bool pressed = pressed_ == button && hot;
        const bool close = button == HTCLOSE;
        if (pressed)
            FillRect(dr, rect, close ? p.closeActive : p.titlebarBtnActive);
        else if (hot)
            FillRect(dr, rect, close ? p.closeHover : p.titlebarBtnHover);

        std::wstring icon = L"close.svg";
        if (button == HTMINBUTTON)
            icon = L"minimize.svg";
        else if (button == HTMAXBUTTON)
            icon = window_ && IsZoomed(window_->GetHandle()) ? L"restore.svg" : L"maximize.svg";
        // The close icon stays white on its red hover even in the light theme.
        const bool lightIcon = p.light && !(close && hot);
        const std::string &markup = ThemedSvg(icon, lightIcon);
        // .window-icon { width: 9px; height: 10px; object-fit: contain }
        const bool flat = button == HTMINBUTTON;
        const SizeF size = flat ? SizeF{9.0f, 0.9f} : SizeF{9.0f, 9.0f};
        // Inactive windows mute the buttons, except the one under the pointer.
        const float opacity = hot ? 1.0f : dim;
        if (ID2D1Bitmap *bitmap = dr.GetSvgBitmap(markup, flat ? SizeF{9.0f, 1.0f} : size))
        {
            const float x = rect.x + (rect.width - size.width) * 0.5f;
            const float y = rect.y + (rect.height - (flat ? 1.0f : size.height)) * 0.5f;
            dr.GetRenderTarget()->DrawBitmap(bitmap, D2D1::RectF(x, y, x + size.width, y + (flat ? 1.0f : size.height)),
                                             opacity);
        }
    }

    LRESULT hot_ = HTNOWHERE;
    LRESULT pressed_ = HTNOWHERE;
    LRESULT tooltip_ = HTNOWHERE;
    msimeui::Tween tooltipFade_{0.0f};
    msimeui::Tween dim_{1.0f};
    bool active_ = true;
};

// ---------------------------------------------------------------------------------
// Sidebar: `.sidebar` with its header, items and the sliding `.active-indicator`.
// ---------------------------------------------------------------------------------
class Sidebar;

class NavItem : public msimeui::Visual
{
  public:
    NavItem(Sidebar *owner, size_t index, std::wstring title, std::wstring icon)
        : owner_(owner), index_(index), icon_(std::move(icon))
    {
        TextStyle style;
        style.size = kFontDefault;
        title_.Set(std::move(title), style);
        // margin: 2px 8px
        SetMargin({8.0f, 2.0f, 8.0f, 2.0f});
    }

    void SetSelected(bool selected)
    {
        if (selected_ == selected)
            return;
        selected_ = selected;
        InvalidateVisual();
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        // padding 6px 12px around a 24px icon and the 16px label.
        return {availableSize.width, std::max(24.0f, LineHeight(title_.Style())) + 12.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        if (hovered_ || pressed_)
            FillRounded(dr, bounds_, 5.0f, p.sidebarHover);
        else if (selected_)
            FillRounded(dr, bounds_, 5.0f, p.itemActiveHover);

        // .icon: 24px box nudged down 1.5px, the 22px image centered in it.
        const bool emphasized = hovered_ || selected_;
        const float iconX = bounds_.x + 12.0f + 1.0f;
        const float iconY = bounds_.y + (bounds_.height - 24.0f) * 0.5f + 1.5f + 1.0f;
        DrawSvg(dr, ThemedSvg(icon_, p.light), {iconX, iconY, 22.0f, 22.0f}, emphasized ? 1.0f : 0.9f);

        const float textX = bounds_.x + 12.0f + 24.0f + 15.0f;
        const SizeF text = title_.Measure(bounds_.width);
        const D2D1_COLOR_F color = selected_ && hovered_ ? p.text : p.sidebarItem;
        title_.Draw(dr, {textX, bounds_.y + (bounds_.height - text.height) * 0.5f}, color, bounds_.width);
    }

    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        pressed_ = window_ && HitTest(window_->ClientPixelsToDips(point));
        return pressed_;
    }

    bool OnMouseUp(const POINT &point, WPARAM) override;

    void OnMouseEnter() override
    {
        hovered_ = true;
        InvalidateVisual();
    }

    void OnMouseLeave() override
    {
        hovered_ = false;
        pressed_ = false;
        InvalidateVisual();
    }

  private:
    Sidebar *owner_ = nullptr;
    size_t index_ = 0;
    std::wstring icon_;
    TextLayout title_;
    bool selected_ = false;
    bool hovered_ = false;
    bool pressed_ = false;
};

class SidebarHeader : public msimeui::Visual
{
  public:
    SidebarHeader()
    {
        TextStyle style;
        style.size = 19.0f;
        style.weight = DWRITE_FONT_WEIGHT_MEDIUM;
        title_.Set(L"水杉 IME", style);
        // padding 0 12px 12px 12px; margin-bottom 8px
        SetMargin({0.0f, 0.0f, 0.0f, 8.0f});
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        return {availableSize.width, std::max(25.0f, LineHeight(title_.Style())) + 12.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        const float rowHeight = bounds_.height - 12.0f;
        // img 25x25, radius 6px, translateY(1px)
        const float logoY = bounds_.y + (rowHeight - 25.0f) * 0.5f + 1.0f;
        logo_.Arrange({bounds_.x + 12.0f, logoY, 25.0f, 25.0f});
        logo_.Render(dr);
        // gap 6px + margin-left 3px
        const SizeF text = title_.Measure(bounds_.width);
        title_.Draw(dr, {bounds_.x + 12.0f + 25.0f + 6.0f + 3.0f, bounds_.y + (rowHeight - text.height) * 0.5f}, p.text,
                    bounds_.width);
    }

  private:
    TextLayout title_;
    BitmapImage logo_{AssetPath(L"msime.png"), 25.0f, 25.0f, 6.0f};
};

class Sidebar : public msimeui::Panel
{
  public:
    using SelectHandler = std::function<void(size_t index)>;

    Sidebar()
    {
        AddChild(std::make_shared<SidebarHeader>());
        const auto &pages = Pages();
        for (size_t i = 0; i < pages.size(); ++i)
        {
            auto item = std::make_shared<NavItem>(this, i, pages[i].title, pages[i].icon);
            items_.push_back(item);
            AddChild(item);
        }
    }

    void SetOnSelect(SelectHandler handler)
    {
        onSelect_ = std::move(handler);
    }

    void Select(size_t index, bool animate)
    {
        if (index >= items_.size())
            return;
        for (size_t i = 0; i < items_.size(); ++i)
            items_[i]->SetSelected(i == index);
        selected_ = index;
        const bool reduceMotion = []() {
            BOOL enabled = TRUE;
            SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
            return !enabled;
        }();
        if (!hasLayout_)
        {
            pendingSnap_ = true;
        }
        else if (!positioned_ || !animate || reduceMotion)
        {
            // Snap now: an unchanged sidebar keeps its arrange cache, so Arrange may not
            // run again to take a pending snap.
            indicator_.Snap(TargetOffset(index));
            positioned_ = true;
            pendingSnap_ = false;
        }
        else
        {
            // sidebar.ts: the bar glides from where it is now; nearby items get a shorter
            // move so the strong ease-out does not linger at the end.
            const double now = msimeui::NowMs();
            const float from = indicator_.Value(now);
            const float to = TargetOffset(index);
            const double duration = std::round(std::min(280.0, std::max(140.0, 110.0 + std::fabs(to - from) * 0.9)));
            indicator_.Start(from, to, duration, msimeui::Easing{0.22f, 1.0f, 0.36f, 1.0f}, now);
        }
        if (window_)
            window_->RequestAnimationFrame();
        InvalidateVisual();
    }

    void Activate(size_t index)
    {
        if (index == selected_)
            return;
        Select(index, true);
        if (onSelect_)
            onSelect_(index);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        // padding 12px 0; 8px of the 200px are the stable scrollbar gutter.
        const float width = std::max(availableSize.width - kScrollbarWidth, 0.0f);
        float height = 12.0f;
        for (const auto &child : children_)
            height += child->MeasureInLayout({width, availableSize.height}).height;
        return {availableSize.width, height + 12.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        const float width = std::max(finalRect.width - kScrollbarWidth, 0.0f);
        float y = finalRect.y + 12.0f;
        for (const auto &child : children_)
        {
            const SizeF size = child->MeasureInLayout({width, finalRect.height});
            child->ArrangeInLayout({finalRect.x, y, width, size.height});
            y += size.height;
        }
        hasLayout_ = true;
        if (pendingSnap_ || !indicator_.IsRunning(msimeui::NowMs()))
        {
            // On resize the bar jumps to the active item (`animate: false`).
            indicator_.Snap(TargetOffset(selected_));
            positioned_ = true;
            pendingSnap_ = false;
        }
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        for (const auto &child : children_)
        {
            if (dr.IsVisible(Inflate(child->GetBounds(), 4.0f)))
                child->Render(dr);
        }
        if (!positioned_ || selected_ >= items_.size())
            return;
        const double now = window_ ? window_->GetFrameTime() : msimeui::NowMs();
        const float offset = indicator_.Value(now);
        if (indicator_.IsRunning(now) && window_)
            window_->RequestAnimationFrame();
        // .active-indicator: 4x22, fully rounded, 2px left of the item's edge.
        const RectF bar = {bounds_.x + 8.0f - 2.0f, bounds_.y + offset, 4.0f, 22.0f};
        ID2D1RenderTarget *target = dr.GetRenderTarget();
        if (ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(CurrentPalette().accent))
            target->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(bar), 2.0f, 2.0f), brush);
    }

  private:
    float TargetOffset(size_t index) const
    {
        if (index >= items_.size())
            return 0.0f;
        const RectF &item = items_[index]->GetBounds();
        return item.y - bounds_.y + (item.height - 22.0f) * 0.5f;
    }

    std::vector<std::shared_ptr<NavItem>> items_;
    SelectHandler onSelect_;
    size_t selected_ = 0;
    msimeui::Tween indicator_{0.0f};
    bool positioned_ = false;
    bool pendingSnap_ = true;
    bool hasLayout_ = false;
};

bool NavItem::OnMouseUp(const POINT &point, WPARAM)
{
    if (!pressed_)
        return false;
    pressed_ = false;
    InvalidateVisual();
    if (window_ && HitTest(window_->ClientPixelsToDips(point)))
        owner_->Activate(index_);
    return true;
}

// ---------------------------------------------------------------------------------
// Overlays: `.dict-toast` and `.dict-modal`.
// ---------------------------------------------------------------------------------
class ToastView : public msimeui::Visual
{
  public:
    void Show(std::wstring message, bool success)
    {
        message_.Set(std::move(message), [] {
            TextStyle style = BodyStyle();
            style.lineHeight = 1.45f;
            style.wrap = true;
            return style;
        }());
        success_ = success;
        visible_ = true;
        fade_.AnimateTo(1.0f, 200.0, msimeui::Easing::Ease(), msimeui::NowMs());
        if (window_)
        {
            SetTimer(window_->GetHandle(), kToastTimer, success ? 2600 : 4200, nullptr);
            window_->RequestAnimationFrame();
        }
    }

    void Hide()
    {
        if (!visible_)
            return;
        visible_ = false;
        fade_.AnimateTo(0.0f, 200.0, msimeui::Easing::Ease(), msimeui::NowMs());
        if (window_)
        {
            KillTimer(window_->GetHandle(), kToastTimer);
            window_->RequestAnimationFrame();
        }
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        return availableSize;
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    RectF ToastRect()
    {
        // grid 24px + 10px gap + message (180..), padding 12px 42px 12px 16px,
        // min-width 280, max-width 430, min-height 58.
        const float maxMessage = 430.0f - 16.0f - 42.0f - 24.0f - 10.0f - 2.0f;
        const SizeF text = message_.Measure(maxMessage);
        const float width =
            std::clamp(16.0f + 24.0f + 10.0f + std::max(text.width, 180.0f) + 42.0f + 2.0f, 280.0f, 430.0f);
        const float height = std::max(58.0f, text.height + 24.0f + 2.0f);
        const float centerX = bounds_.x + bounds_.width * 0.5f + kSidebarWidth * 0.5f;
        return {std::round(centerX - width * 0.5f), bounds_.y + bounds_.height - 41.0f - height, width, height};
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const double now = window_ ? window_->GetFrameTime() : msimeui::NowMs();
        const float t = fade_.Value(now);
        if (fade_.IsRunning(now) && window_)
            window_->RequestAnimationFrame();
        if (t <= 0.0f)
            return;
        const Palette &p = CurrentPalette();
        RectF rect = ToastRect();
        rect.y += 12.0f * (1.0f - t);
        PushOpacity(dr, t);
        // box-shadow: 0 8px 28px rgba(0, 0, 0, .18)
        msimeui::ShadowPass pass = {14.0f, 0.18f, 0.0f, 8.0f};
        shadow_.Render(dr.GetRenderTarget(), rect, 8.0f, &pass, 1);
        FillRounded(dr, rect, 8.0f, p.toastBg);
        StrokeRounded(dr, rect, 8.0f, p.toastBorder, 1.0f);
        const RectF icon = {rect.x + 1.0f + 16.0f + 1.0f, rect.y + (rect.height - 22.0f) * 0.5f, 22.0f, 22.0f};
        const D2D1_COLOR_F iconBg = success_ ? Rgba(84, 190, 119, 0.18f) : Rgba(230, 90, 90, 0.14f);
        const D2D1_COLOR_F iconFg =
            success_ ? (p.light ? Hex(0x0F7B3A) : Hex(0xB8F2CA)) : (p.light ? Hex(0xB42318) : Hex(0xFFC0C0));
        FillRounded(dr, icon, 11.0f, iconBg);
        if (success_)
        {
            DrawCheck(dr, {icon.x + 11.0f, icon.y + 11.0f - 1.0f}, iconFg);
        }
        else
        {
            TextStyle bang = BodyStyle();
            bang.weight = DWRITE_FONT_WEIGHT_BOLD;
            DrawTextInRect(dr, L"!", bang, icon, iconFg, true);
        }
        const float textX = rect.x + 1.0f + 16.0f + 24.0f + 10.0f;
        const float textWidth = rect.x + rect.width - 42.0f - 1.0f - textX;
        const SizeF text = message_.Measure(textWidth);
        message_.Draw(dr, {textX, rect.y + (rect.height - text.height) * 0.5f}, p.toastText, textWidth);
        // .dict-toast-close: 28px, two 12x1.5 bars crossed.
        const RectF close = CloseRect();
        if (closeHot_)
            FillRounded(dr, {close.x, close.y + 12.0f * (1.0f - t), close.width, close.height}, 4.0f, p.sidebarHover);
        const PointF c = {close.x + 14.0f, close.y + 14.0f + 12.0f * (1.0f - t)};
        const D2D1_COLOR_F cross = closeHot_ ? p.text : p.textMuted;
        DrawLine(dr, {c.x - 4.2f, c.y - 4.2f}, {c.x + 4.2f, c.y + 4.2f}, cross, 1.5f);
        DrawLine(dr, {c.x - 4.2f, c.y + 4.2f}, {c.x + 4.2f, c.y - 4.2f}, cross, 1.5f);
        PopOpacity(dr);
    }

    msimeui::Visual *FindVisualAt(const PointF &point) override
    {
        return visible_ && Contains(ToastRect(), point) ? this : nullptr;
    }

    msimeui::Visual *FindFocusableAt(const PointF &) override
    {
        return nullptr;
    }

    bool OnMouseMove(const POINT &point, WPARAM) override
    {
        const bool hot = window_ && Contains(CloseRect(), window_->ClientPixelsToDips(point));
        if (hot != closeHot_)
        {
            closeHot_ = hot;
            InvalidateVisual();
        }
        return true;
    }

    void OnMouseLeave() override
    {
        closeHot_ = false;
        InvalidateVisual();
    }

    bool OnMouseDown(const POINT &, WPARAM) override
    {
        return true;
    }

    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        if (window_ && Contains(CloseRect(), window_->ClientPixelsToDips(point)))
            Hide();
        return true;
    }

  private:
    RectF CloseRect()
    {
        const RectF rect = ToastRect();
        return {rect.x + rect.width - 2.0f - 28.0f - 1.0f, rect.y + 2.0f + 1.0f, 28.0f, 28.0f};
    }

    static void DrawCheck(msimeui::DeviceResources &dr, PointF center, const D2D1_COLOR_F &color)
    {
        // ::after: 6x11, right/bottom 2px borders, rotate(40deg) scale(.9)
        ID2D1RenderTarget *target = dr.GetRenderTarget();
        ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(color);
        if (!brush)
            return;
        D2D1_MATRIX_3X2_F previous;
        target->GetTransform(&previous);
        target->SetTransform(D2D1::Matrix3x2F::Scale(0.9f, 0.9f) * D2D1::Matrix3x2F::Rotation(40.0f) *
                             D2D1::Matrix3x2F::Translation(center.x, center.y) * previous);
        target->FillRectangle(D2D1::RectF(1.0f, -5.5f, 3.0f, 5.5f), brush);
        target->FillRectangle(D2D1::RectF(-3.0f, 3.5f, 3.0f, 5.5f), brush);
        target->SetTransform(previous);
    }

    TextLayout message_;
    bool success_ = true;
    bool visible_ = false;
    bool closeHot_ = false;
    msimeui::Tween fade_{0.0f};
    msimeui::ShadowCache shadow_;
};

// `.dict-dialog` frame: 24px padding, 12px radius, --card-bg-solid, strong border.
class DialogFrame : public msimeui::Container
{
  public:
    explicit DialogFrame(VisualPtr content) : msimeui::Container(std::move(content))
    {
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        // padding: 24px inside the 1px border. Assigned directly: SetPadding would
        // invalidate the measure pass that is running.
        const float padding = 24.0f + CssBorder();
        padding_ = {padding, padding, padding, padding};
        // The explicit width only clamps the result; measure the content at it too, or
        // wrapped text is measured one line short and overlaps what follows.
        SizeF available = availableSize;
        if (HasExplicitWidth())
            available.width = std::min(available.width, explicitWidth_);
        return msimeui::Container::Measure(available);
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        msimeui::ShadowPass pass = {30.0f, 0.28f, 0.0f, 18.0f};
        shadow_.Render(dr.GetRenderTarget(), bounds_, 12.0f, &pass, 1);
        FillRounded(dr, bounds_, 12.0f, p.cardBgSolid);
        StrokeRounded(dr, bounds_, 12.0f, p.borderStrong, 1.0f);
        msimeui::Container::Render(dr);
    }

  private:
    msimeui::ShadowCache shadow_;
};

class ModalLayer : public msimeui::Visual
{
  public:
    void Show(VisualPtr dialog, std::function<void()> onDismiss)
    {
        if (dialog_)
            ReleaseChild(dialog_);
        dialog_ = std::move(dialog);
        onDismiss_ = std::move(onDismiss);
        AdoptChild(dialog_);
        if (window_)
            dialog_->Attach(window_);
        InvalidateMeasure();
        if (window_)
        {
            window_->Invalidate();
            if (msimeui::Visual *focus = dialog_->FindFirstFocusableDescendant())
                window_->FocusVisual(focus);
        }
    }

    void Close()
    {
        if (!dialog_)
            return;
        if (window_)
            window_->FocusVisual(nullptr);
        ReleaseChild(dialog_);
        dialog_.reset();
        onDismiss_ = nullptr;
        InvalidateMeasure();
        if (window_)
            window_->Invalidate();
    }

    void Dismiss()
    {
        auto handler = onDismiss_;
        Close();
        if (handler)
            handler();
    }

    bool IsOpen() const
    {
        return dialog_ != nullptr;
    }

    void Attach(msimeui::Window *window) override
    {
        msimeui::Visual::Attach(window);
        if (dialog_)
            dialog_->Attach(window);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        if (dialog_)
            dialog_->MeasureInLayout({std::max(availableSize.width - kSidebarWidth, 0.0f), availableSize.height});
        return availableSize;
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        if (!dialog_)
            return;
        // padding-left: var(--sidebar-width); the dialog is centered in what remains.
        const RectF area = {finalRect.x + kSidebarWidth, finalRect.y, std::max(finalRect.width - kSidebarWidth, 0.0f),
                            finalRect.height};
        const SizeF size = dialog_->MeasureInLayout({area.width, area.height});
        dialog_->ArrangeInLayout({std::round(area.x + (area.width - size.width) * 0.5f),
                                  std::round(area.y + (area.height - size.height) * 0.5f), size.width, size.height});
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        if (!dialog_)
            return;
        FillRect(dr, bounds_, CurrentPalette().overlayBg);
        dialog_->Render(dr);
    }

    msimeui::Visual *FindVisualAt(const PointF &point) override
    {
        if (!dialog_ || !Contains(bounds_, point))
            return nullptr;
        if (msimeui::Visual *hit = dialog_->FindVisualAt(point))
            return hit;
        return this;
    }

    msimeui::Visual *FindFocusableAt(const PointF &point) override
    {
        return dialog_ ? dialog_->FindFocusableAt(point) : nullptr;
    }

    bool OnMouseDown(const POINT &, WPARAM) override
    {
        // mousedown on the overlay itself dismisses the dialog.
        Dismiss();
        return true;
    }

  private:
    VisualPtr dialog_;
    std::function<void()> onDismiss_;
};

// ---------------------------------------------------------------------------------
// Splash: the Settings app's launch screen, a large gear in the middle of the window,
// shown until the first configSnapshot has filled in the controls (otherwise their
// defaults show for a frame, e.g. 全拼 before the configured 双拼), then faded out.
// ---------------------------------------------------------------------------------
class SplashView : public msimeui::Visual
{
  public:
    // Covers the page (the title bar stays usable) until Dismiss; fully covered, nothing
    // under it needs drawing.
    bool Covers() const
    {
        return !dismissed_;
    }

    bool Visible() const
    {
        return !finished_;
    }

    // Off: a brief plain cover in the window color, no gear and no fade.
    void SetAnimated(bool animated)
    {
        animated_ = animated;
        if (window_)
            window_->Invalidate();
    }

    void Dismiss()
    {
        if (dismissed_)
            return;
        dismissed_ = true;
        dismissedMs_ = msimeui::NowMs();
        if (animated_ && !SystemAnimationsEnabled())
            finished_ = true;
        if (window_)
        {
            window_->RequestAnimationFrame();
            window_->Invalidate();
        }
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        return availableSize;
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        if (finished_)
            return;
        const double now = window_ ? window_->GetFrameTime() : msimeui::NowMs();
        if (shownMs_ <= 0.0)
            shownMs_ = now;
        float t = 0.0f;
        if (dismissed_)
        {
            // The cover stays at least its minimum even when the config is there at once,
            // so it reads as a launch screen rather than a flicker; the plain one then
            // goes in one step.
            const double fadeStart = std::max(dismissedMs_, shownMs_ + (animated_ ? kMinimumMs : kPlainMs));
            t = animated_ ? static_cast<float>(std::clamp((now - fadeStart) / kFadeMs, 0.0, 1.0))
                          : (now >= fadeStart ? 1.0f : 0.0f);
            if (t >= 1.0f)
            {
                finished_ = true;
                if (window_)
                    window_->RefreshHover();
                return;
            }
            if (window_)
                window_->RequestAnimationFrame();
        }
        const Palette &p = CurrentPalette();
        if (!animated_)
        {
            FillRect(dr, bounds_, p.chromeBg);
            return;
        }
        const float eased = msimeui::Easing::EaseOut()(t);
        const float opacity = 1.0f - eased;
        FillRect(dr, bounds_, WithAlpha(p.chromeBg, opacity));

        // U+E713 Settings; the gear grows a little as it fades, like the system's launch.
        TextStyle style;
        style.family = IconFontFamily();
        style.size = kGlyphSize;
        style.lineHeight = 1.0f;
        const float scale = 1.0f + 0.12f * eased;
        const PointF center = {bounds_.x + bounds_.width * 0.5f, bounds_.y + bounds_.height * 0.5f};
        ID2D1RenderTarget *target = dr.GetRenderTarget();
        D2D1_MATRIX_3X2_F previous;
        target->GetTransform(&previous);
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(center.x, center.y)) * previous);
        const float box = kGlyphSize * 2.0f;
        DrawTextInRect(dr, L"\xE713", style, {center.x - box * 0.5f, center.y - box * 0.5f, box, box},
                       WithAlpha(p.text, opacity), true);
        target->SetTransform(previous);
    }

    msimeui::Visual *FindVisualAt(const PointF &point) override
    {
        return Visible() && Contains(bounds_, point) ? this : nullptr;
    }

    msimeui::Visual *FindFocusableAt(const PointF &) override
    {
        return nullptr;
    }

    bool OnMouseDown(const POINT &, WPARAM) override
    {
        return Visible();
    }

    bool OnMouseWheel(const POINT &, short, WPARAM) override
    {
        return Visible();
    }

  private:
    static constexpr float kGlyphSize = 64.0f;
    static constexpr double kMinimumMs = 200.0;
    static constexpr double kPlainMs = 80.0;
    static constexpr double kFadeMs = 140.0;

    double shownMs_ = 0.0;
    double dismissedMs_ = 0.0;
    bool animated_ = true;
    bool dismissed_ = false;
    bool finished_ = false;
};

// ---------------------------------------------------------------------------------
// Root: title bar, sidebar, content and the overlays, laid out like `.main`.
// ---------------------------------------------------------------------------------
class RootPanel : public msimeui::Panel
{
  public:
    RootPanel(std::shared_ptr<TitleBar> titleBar, std::shared_ptr<msimeui::ScrollViewer> sidebar,
              std::shared_ptr<msimeui::ScrollViewer> content, std::shared_ptr<ModalLayer> modal,
              std::shared_ptr<ToastView> toast, std::shared_ptr<SplashView> splash)
        : titleBar_(std::move(titleBar)), sidebar_(std::move(sidebar)), content_(std::move(content)),
          modal_(std::move(modal)), toast_(std::move(toast)), splash_(std::move(splash))
    {
        AddChild(titleBar_);
        AddChild(sidebar_);
        AddChild(content_);
        AddChild(toast_);
        AddChild(modal_);
        AddChild(splash_);
        content_->SetVerticalAlignment(msimeui::VerticalAlignment::Stretch);
        sidebar_->SetVerticalAlignment(msimeui::VerticalAlignment::Stretch);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        const Columns c = Split(availableSize);
        titleBar_->MeasureInLayout({availableSize.width, kTitleBarHeight});
        sidebar_->MeasureInLayout({kSidebarWidth, c.bodyHeight});
        content_->MeasureInLayout({c.contentWidth, c.bodyHeight - 2.0f});
        modal_->MeasureInLayout({availableSize.width, c.bodyHeight});
        toast_->MeasureInLayout(availableSize);
        splash_->MeasureInLayout({availableSize.width, c.bodyHeight});
        return availableSize;
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        const Columns c = Split({finalRect.width, finalRect.height});
        titleBar_->ArrangeInLayout({finalRect.x, finalRect.y, finalRect.width, kTitleBarHeight});
        sidebar_->ArrangeInLayout({finalRect.x, finalRect.y + kTitleBarHeight, kSidebarWidth, c.bodyHeight});
        // #content-container { margin-top: 2px }
        content_->ArrangeInLayout({finalRect.x + c.sidebarWidth, finalRect.y + kTitleBarHeight + 2.0f, c.contentWidth,
                                   std::max(c.bodyHeight - 2.0f, 0.0f)});
        modal_->ArrangeInLayout({finalRect.x, finalRect.y + kTitleBarHeight, finalRect.width, c.bodyHeight});
        toast_->ArrangeInLayout(finalRect);
        splash_->ArrangeInLayout({finalRect.x, finalRect.y + kTitleBarHeight, finalRect.width, c.bodyHeight});
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        // The page content gets ClearType like in the browser, where it is painted into
        // the opaque root layer; the sidebar, title bar and overlays are composited
        // layers of their own there and get grayscale text.
        const bool covered = splash_->Covers();
        if (!covered)
            content_->Render(dr);
        ScopedTextAntialias grayscale(dr, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (!covered)
            sidebar_->Render(dr);
        titleBar_->Render(dr);
        toast_->Render(dr);
        modal_->Render(dr);
        splash_->Render(dr);
        titleBar_->RenderOverlay(dr);
    }

    msimeui::Visual *FindVisualAt(const PointF &point) override
    {
        if (point.y < kTitleBarHeight)
            return titleBar_->FindVisualAt(point);
        if (msimeui::Visual *hit = splash_->FindVisualAt(point))
            return hit;
        if (msimeui::Visual *hit = modal_->FindVisualAt(point))
            return hit;
        if (msimeui::Visual *hit = toast_->FindVisualAt(point))
            return hit;
        if (msimeui::Visual *hit = sidebar_->FindVisualAt(point))
            return hit;
        if (msimeui::Visual *hit = content_->FindVisualAt(point))
            return hit;
        return this;
    }

    msimeui::Visual *FindFocusableAt(const PointF &point) override
    {
        if (splash_->Visible())
            return nullptr;
        if (modal_->IsOpen())
            return point.y >= kTitleBarHeight ? modal_->FindFocusableAt(point) : nullptr;
        if (msimeui::Visual *hit = sidebar_->FindFocusableAt(point))
            return hit;
        return content_->FindFocusableAt(point);
    }

  private:
    struct Columns
    {
        float sidebarWidth;
        float contentWidth;
        float bodyHeight;
    };

    static Columns Split(const SizeF &size)
    {
        // #sidebar-container { flex: 1 } vs #content-container { flex: 4 }, the sidebar
        // never narrower than its 200px content.
        const float sidebar = std::max(kSidebarWidth, std::floor(size.width / 5.0f));
        return {sidebar, std::max(size.width - sidebar, 0.0f), std::max(size.height - kTitleBarHeight, 0.0f)};
    }

    std::shared_ptr<TitleBar> titleBar_;
    std::shared_ptr<msimeui::ScrollViewer> sidebar_;
    std::shared_ptr<msimeui::ScrollViewer> content_;
    std::shared_ptr<ModalLayer> modal_;
    std::shared_ptr<ToastView> toast_;
    std::shared_ptr<SplashView> splash_;
};

bool IsCaptionButton(WPARAM hit)
{
    return hit == HTMINBUTTON || hit == HTMAXBUTTON || hit == HTCLOSE;
}

int TopFrameThickness(HWND hwnd)
{
    const UINT dpi = msimeui::compat::GetWindowDpi(hwnd);
    return msimeui::compat::GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
           msimeui::compat::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}
} // namespace

// ---------------------------------------------------------------------------------
// SettingsWindowImpl
// ---------------------------------------------------------------------------------
class SettingsWindowImpl : public HostServices
{
  public:
    SettingsWindowImpl(HWND hwnd, SettingsWindow::Callbacks callbacks, const std::string &initialPage)
        : hwnd_(hwnd), callbacks_(std::move(callbacks)), model_([this](const std::string &message) {
              if (callbacks_.postToHost)
                  callbacks_.postToHost(message);
          }),
          context_(model_, *this)
    {
        window_ = std::make_unique<msimeui::Window>(L"MetasequoiaImeSettingsWindow", L"Metasequoia IME Settings", 0, 0);
        window_->AdoptExistingHwnd(hwnd_);
        window_->SetQuitOnDestroy(false);
        // The frame is extended into the client area for the custom caption; DWM does not
        // compose an HWND render target's output there, a composition swap chain it does.
        // Opaque, so text can use ClearType.
        window_->SetCompositionRendering(true, true);
        window_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        SetLayoutScale(static_cast<float>(msimeui::compat::GetWindowDpi(hwnd_)) / 96.0f);
        window_->SetBackgroundColor(CurrentPalette().chromeBg);
        window_->StartCaretTimer();
        window_->GetDeviceResources().SetAnimationsEnabled(SystemAnimationsEnabled());
        skinpreview::SetImageLoadWindow(hwnd_);

        titleBar_ = std::make_shared<TitleBar>();
        sidebar_ = std::make_shared<Sidebar>();
        sidebarScroll_ = std::make_shared<msimeui::ScrollViewer>(sidebar_);
        sidebarScroll_->SetAppearance(WebKitScrollbar(0.0f));
        pageHost_ = std::make_shared<msimeui::Container>();
        // #content-container: padding 12px 24px 12px 16px around the 900px column.
        auto column = std::make_shared<CenteredColumn>(pageHost_, kContentMaxWidth);
        column->SetPadding({16.0f, 12.0f, 24.0f, 12.0f});
        contentScroll_ = std::make_shared<msimeui::ScrollViewer>(column);
        contentScroll_->SetAppearance(WebKitScrollbar(kScrollbarWidth));
        modal_ = std::make_shared<ModalLayer>();
        toast_ = std::make_shared<ToastView>();
        splash_ = std::make_shared<SplashView>();
        root_ = std::make_shared<RootPanel>(titleBar_, sidebarScroll_, contentScroll_, modal_, toast_, splash_);

        sidebar_->SetOnSelect([this](size_t index) { ShowPageIndex(index, false); });

        auto scene = std::make_unique<msimeui::Scene>();
        scene->SetRoot(root_);
        window_->SetScene(std::move(scene));

        // Hand the caption strip to the client area; the side and bottom frames stay.
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        model_.Post("configRequest");
        ShowPage(initialPage.empty() ? std::string("input") : initialPage);
        if (model_.HasConfig())
            splash_->Dismiss();
        else
            SetTimer(hwnd_, kSplashTimer, 1500, nullptr);
    }

    ~SettingsWindowImpl() override
    {
        skinpreview::SetImageLoadWindow(nullptr);
        KillTimer(hwnd_, kToolTipTimer);
        KillTimer(hwnd_, kToastTimer);
        KillTimer(hwnd_, kImageFrameTimer);
        KillTimer(hwnd_, kSplashTimer);
    }

    // HostServices
    HWND Hwnd() const override
    {
        return hwnd_;
    }

    msimeui::Window *Window() const override
    {
        return window_.get();
    }

    void ShowToast(const std::wstring &message, bool success) override
    {
        toast_->Show(message, success);
        window_->Invalidate();
    }

    void Confirm(const std::wstring &message, std::function<void(bool confirmed)> done, const std::wstring &title,
                 const std::wstring &confirmText, const std::wstring &cancelText) override
    {
        auto column = std::make_shared<Column>(0.0f);
        TextStyle titleStyle;
        titleStyle.size = 20.0f;
        titleStyle.weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
        auto titleLabel = std::make_shared<Label>(title, titleStyle);
        titleLabel->SetMargin({0.0f, 0.0f, 0.0f, 20.0f});
        column->Add(titleLabel);
        TextStyle messageStyle = BodyStyle();
        messageStyle.lineHeight = 1.6f;
        messageStyle.wrap = true;
        column->Add(std::make_shared<Label>(message, messageStyle, TextRole::Secondary));
        auto actions = std::make_shared<Row>(10.0f);
        actions->SetJustify(Row::Justify::End);
        actions->SetMargin({0.0f, 24.0f, 0.0f, 0.0f});
        auto cancel = std::make_shared<Button>(cancelText, ButtonKind::Neutral);
        auto confirm = std::make_shared<Button>(confirmText, ButtonKind::Danger);
        auto shared = std::make_shared<std::function<void(bool)>>(std::move(done));
        cancel->SetOnClick([this, shared]() {
            CloseModal();
            if (*shared)
                (*shared)(false);
        });
        confirm->SetOnClick([this, shared]() {
            CloseModal();
            if (*shared)
                (*shared)(true);
        });
        actions->Add(cancel);
        actions->Add(confirm);
        column->Add(actions);
        auto frame = std::make_shared<DialogFrame>(column);
        // .dict-confirm-dialog { width: 360px }
        frame->SetWidth(360.0f);
        ShowModal(frame, [shared]() {
            if (*shared)
                (*shared)(false);
        });
        window_->FocusVisual(confirm.get());
    }

    void ShowModal(VisualPtr dialog, std::function<void()> onDismiss) override
    {
        window_->GetScene()->ClearPopups(true);
        auto frame = std::dynamic_pointer_cast<DialogFrame>(dialog);
        if (!frame)
        {
            frame = std::make_shared<DialogFrame>(dialog);
            frame->SetWidth(430.0f);
        }
        modal_->Show(frame, std::move(onDismiss));
    }

    void CloseModal() override
    {
        modal_->Close();
    }

    void NavigateTo(const std::string &pageId) override
    {
        ShowPage(pageId);
    }

    void ScrollIntoView(const msimeui::Visual &visual) override
    {
        contentScroll_->ScrollIntoView(visual.GetBounds());
    }

    // Window API
    void ShowPage(const std::string &pageId)
    {
        const auto &pages = Pages();
        for (size_t i = 0; i < pages.size(); ++i)
        {
            if (pageId == pages[i].id)
            {
                sidebar_->Select(i, current_ != static_cast<size_t>(-1));
                ShowPageIndex(i, true);
                return;
            }
        }
    }

    void ShowPageIndex(size_t index, bool fromHost)
    {
        (void)fromHost;
        const auto &pages = Pages();
        if (index >= pages.size() || index == current_)
            return;
        current_ = index;
        window_->GetScene()->ClearPopups(true);
        modal_->Close();
        auto &page = pageCache_[index];
        if (!page)
            page = pages[index].build(context_);
        pageHost_->SetChild(page);
        page->Attach(window_.get());
        // All pages share one scroller; a new page starts at the top.
        contentScroll_->SetScrollOffset(0.0f);
        window_->FocusVisual(nullptr);
        window_->Relayout();
        window_->RefreshHover();
    }

    void RebuildPages()
    {
        const size_t current = current_;
        pageCache_.clear();
        current_ = static_cast<size_t>(-1);
        if (current < Pages().size())
            ShowPageIndex(current, true);
    }

    void OnHostMessage(const std::wstring &message)
    {
        model_.Receive(wstring_to_string(message));
        if (model_.HasConfig() && splash_->Covers())
        {
            KillTimer(hwnd_, kSplashTimer);
            splash_->Dismiss();
        }
        window_->Invalidate();
    }

    void SetLightTheme(bool light)
    {
        if (light == CurrentPalette().light && themed_)
            return;
        themed_ = true;
        SetCurrentPalette(light);
        window_->SetBackgroundColor(CurrentPalette().chromeBg);
        sidebarScroll_->SetAppearance(WebKitScrollbar(0.0f));
        contentScroll_->SetAppearance(WebKitScrollbar(kScrollbarWidth));
        window_->Invalidate();
    }

    void SetLaunchAnimation(bool enabled)
    {
        splash_->SetAnimated(enabled);
    }

    void ResetTransientState()
    {
        titleBar_->Reset();
        caption_ = {};
    }

    bool HandleMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT &result)
    {
        if (HandleCaption(message, wParam, lParam, result))
            return true;
        if (message == BackgroundJobMessage())
        {
            CompleteBackgroundJob(wParam);
            result = 0;
            return true;
        }

        switch (message)
        {
        case WM_ERASEBKGND:
            // Until Direct2D has drawn a frame, fill with the theme's window color: the
            // class brush is white, and that is what a dark window would flash first.
            // Afterwards Direct2D covers every pixel and erasing would only flicker.
            if (!painted_)
            {
                const D2D1_COLOR_F c = CurrentPalette().chromeBg;
                RECT client{};
                GetClientRect(hwnd_, &client);
                if (HBRUSH brush = CreateSolidBrush(RGB(static_cast<BYTE>(c.r * 255.0f + 0.5f),
                                                        static_cast<BYTE>(c.g * 255.0f + 0.5f),
                                                        static_cast<BYTE>(c.b * 255.0f + 0.5f))))
                {
                    FillRect(reinterpret_cast<HDC>(wParam), &client, brush);
                    DeleteObject(brush);
                }
            }
            result = 1;
            return true;
        case WM_ACTIVATE:
            titleBar_->SetActive(LOWORD(wParam) != WA_INACTIVE);
            window_->Invalidate();
            return false;
        case WM_TIMER:
            if (wParam == kToolTipTimer)
            {
                titleBar_->ShowPendingToolTip();
                result = 0;
                return true;
            }
            if (wParam == kToastTimer)
            {
                toast_->Hide();
                result = 0;
                return true;
            }
            if (wParam == kSplashTimer)
            {
                KillTimer(hwnd_, kSplashTimer);
                splash_->Dismiss();
                result = 0;
                return true;
            }
            if (wParam == kImageFrameTimer)
            {
                KillTimer(hwnd_, kImageFrameTimer);
                window_->Invalidate();
                result = 0;
                return true;
            }
            if (wParam == 1)
            {
                result = window_->DispatchImportedMessage(message, wParam, lParam);
                return true;
            }
            return false;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE && modal_->IsOpen() && window_->GetScene())
            {
                modal_->Dismiss();
                result = 0;
                return true;
            }
            result = window_->DispatchImportedMessage(message, wParam, lParam);
            return true;
        case WM_DPICHANGED: {
            SetLayoutScale(static_cast<float>(HIWORD(wParam)) / 96.0f);
            // Border widths depend on the device pixel size and every page cached its
            // measurements with the old one: rebuild the pages.
            RebuildPages();
            const RECT *suggested = reinterpret_cast<const RECT *>(lParam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            window_->Relayout();
            result = 0;
            return true;
        }
        case WM_SIZE:
            // The host still lays out its own bits (splash) after this.
            window_->DispatchImportedMessage(message, wParam, lParam);
            return false;
        case WM_PAINT:
            result = window_->DispatchImportedMessage(message, wParam, lParam);
            painted_ = true;
            ScheduleImageFrame();
            return true;
        case WM_SETTINGCHANGE:
            if (wParam == SPI_SETCLIENTAREAANIMATION)
            {
                window_->GetDeviceResources().SetAnimationsEnabled(SystemAnimationsEnabled());
                window_->Invalidate();
            }
            return false;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONUP:
        case WM_MOUSEMOVE:
        case WM_MOUSELEAVE:
        case WM_MOUSEWHEEL:
        case WM_CHAR:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_CAPTURECHANGED:
            result = window_->DispatchImportedMessage(message, wParam, lParam);
            return true;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT)
            {
                result = window_->DispatchImportedMessage(message, wParam, lParam);
                return true;
            }
            return false;
        default:
            return false;
        }
    }

  private:
    struct CaptionState
    {
        LRESULT hot = HTNOWHERE;
        LRESULT pressed = HTNOWHERE;
        bool tracking = false;
    };

    // The frame just drawn asked for the next frame of an animated image: repaint then. An
    // animation off screen (another page, a hidden or minimized window) is not drawn and
    // so asks for nothing, which stops the timer by itself.
    void ScheduleImageFrame()
    {
        const auto deadline = window_->GetDeviceResources().GetNextAnimationDeadline();
        if (!deadline)
        {
            KillTimer(hwnd_, kImageFrameTimer);
            return;
        }
        const auto wait =
            std::chrono::ceil<std::chrono::milliseconds>(*deadline - msimeui::DeviceResources::AnimationClock::now());
        SetTimer(hwnd_, kImageFrameTimer, static_cast<UINT>((std::max)(wait.count(), 1LL)), nullptr);
    }

    void SetCaptionButtonState(LRESULT hot, LRESULT pressed)
    {
        if (caption_.hot == hot && caption_.pressed == pressed)
            return;
        caption_.hot = hot;
        caption_.pressed = pressed;
        titleBar_->SetNonClientButtonState(hot, pressed);
    }

    bool HandleCaption(UINT message, WPARAM wParam, LPARAM lParam, LRESULT &result)
    {
        switch (message)
        {
        case WM_NCCALCSIZE: {
            if (!wParam)
                return false;
            auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(lParam);
            const LONG top = params->rgrc[0].top;
            result = DefWindowProcW(hwnd_, message, wParam, lParam);
            // Keep the side and bottom resize borders; the caption strip becomes client.
            params->rgrc[0].top = top;
            if (IsZoomed(hwnd_))
                params->rgrc[0].top += TopFrameThickness(hwnd_);
            return true;
        }
        case WM_NCHITTEST: {
            result = DefWindowProcW(hwnd_, message, wParam, lParam);
            if (result != HTCLIENT)
                return true;
            POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &point);
            if (!IsZoomed(hwnd_) && point.y < TopFrameThickness(hwnd_))
            {
                result = HTTOP;
                return true;
            }
            result = titleBar_->HitTestCaption(window_->ClientPixelsToDips(point));
            // An open dialog keeps the caption usable but not the page under it.
            return true;
        }
        case WM_NCMOUSEMOVE:
            if (!caption_.tracking)
            {
                TRACKMOUSEEVENT tracking = {sizeof(tracking), TME_LEAVE | TME_NONCLIENT, hwnd_, 0};
                caption_.tracking = TrackMouseEvent(&tracking) != FALSE;
            }
            SetCaptionButtonState(IsCaptionButton(wParam) ? static_cast<LRESULT>(wParam) : HTNOWHERE,
                                  caption_.pressed == static_cast<LRESULT>(wParam) ? caption_.pressed : HTNOWHERE);
            return false;
        case WM_NCMOUSELEAVE:
            caption_.tracking = false;
            SetCaptionButtonState(HTNOWHERE, HTNOWHERE);
            return false;
        case WM_MOUSEMOVE:
            if (caption_.hot != HTNOWHERE || caption_.pressed != HTNOWHERE)
                SetCaptionButtonState(HTNOWHERE, HTNOWHERE);
            return false;
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
            if (!IsCaptionButton(wParam))
                return false;
            // Swallowed so DefWindowProc does not paint the classic caption buttons.
            SetCaptionButtonState(static_cast<LRESULT>(wParam), static_cast<LRESULT>(wParam));
            result = 0;
            return true;
        case WM_NCLBUTTONUP: {
            if (!IsCaptionButton(wParam))
                return false;
            const bool clicked = caption_.pressed == static_cast<LRESULT>(wParam);
            SetCaptionButtonState(clicked ? HTNOWHERE : static_cast<LRESULT>(wParam), HTNOWHERE);
            if (clicked)
            {
                const WPARAM command = wParam == HTMINBUTTON ? SC_MINIMIZE
                                       : wParam == HTCLOSE   ? SC_CLOSE
                                                             : (IsZoomed(hwnd_) ? SC_RESTORE : SC_MAXIMIZE);
                PostMessageW(hwnd_, WM_SYSCOMMAND, command, 0);
            }
            result = 0;
            return true;
        }
        default:
            return false;
        }
    }

    HWND hwnd_ = nullptr;
    // Direct2D has drawn at least one frame (see WM_ERASEBKGND).
    bool painted_ = false;
    SettingsWindow::Callbacks callbacks_;
    SettingsModel model_;
    PageContext context_;
    std::unique_ptr<msimeui::Window> window_;
    std::shared_ptr<TitleBar> titleBar_;
    std::shared_ptr<Sidebar> sidebar_;
    std::shared_ptr<msimeui::ScrollViewer> sidebarScroll_;
    std::shared_ptr<msimeui::Container> pageHost_;
    std::shared_ptr<msimeui::ScrollViewer> contentScroll_;
    std::shared_ptr<ModalLayer> modal_;
    std::shared_ptr<ToastView> toast_;
    std::shared_ptr<SplashView> splash_;
    std::shared_ptr<RootPanel> root_;
    std::map<size_t, VisualPtr> pageCache_;
    size_t current_ = static_cast<size_t>(-1);
    CaptionState caption_;
    bool themed_ = false;
};

SettingsWindow::SettingsWindow(HWND hwnd, Callbacks callbacks, const std::string &initialPage)
    : impl_(std::make_unique<SettingsWindowImpl>(hwnd, std::move(callbacks), initialPage))
{
}

SettingsWindow::~SettingsWindow() = default;

bool SettingsWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT &result)
{
    // An exception escaping a window procedure on x64 is swallowed by the system without a
    // trace and leaves the window half painted, so report it here instead.
    try
    {
        return impl_->HandleMessage(message, wParam, lParam, result);
    }
    catch (const std::exception &error)
    {
        OutputDebugStringA("native settings: ");
        OutputDebugStringA(error.what());
        OutputDebugStringA("\n");
        wchar_t trace[8]{};
        if (GetEnvironmentVariableW(L"METASEQUOIA_IME_SETTINGS_TRACE", trace, 8) > 0 && trace[0] == L'1')
        {
            wchar_t temp[MAX_PATH]{};
            GetTempPathW(MAX_PATH, temp);
            FILE *file = nullptr;
            if (_wfopen_s(&file, (std::wstring(temp) + L"metasequoia-settings-native.log").c_str(), L"a") == 0 && file)
            {
                fprintf(file, "message 0x%04X: %s\n", message, error.what());
                fclose(file);
            }
        }
        result = 0;
        return true;
    }
}

void SettingsWindow::OnHostMessage(const std::wstring &message)
{
    impl_->OnHostMessage(message);
}

void SettingsWindow::SetLightTheme(bool light)
{
    impl_->SetLightTheme(light);
}

void SettingsWindow::SetLaunchAnimation(bool enabled)
{
    impl_->SetLaunchAnimation(enabled);
}

void SettingsWindow::ShowPage(const std::string &pageId)
{
    impl_->ShowPage(pageId);
}

void SettingsWindow::ResetTransientState()
{
    impl_->ResetTransientState();
}
} // namespace native_settings
