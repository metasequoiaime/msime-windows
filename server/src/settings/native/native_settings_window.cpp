#include "settings/native/native_settings_window.h"

#include "settings/native/native_page_help_widgets.h"
#include "settings/native/native_page_skin_common.h"
#include "settings/native/native_settings_controls.h"
#include "settings/native/native_settings_model.h"
#include "settings/native/native_settings_pages.h"
#include "settings/native/native_settings_theme.h"

#include "utils/common_utils.h"

#include "msimeui/Animation.h"
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
// Builds the pages the user has not opened yet so their settings are searchable from the first
// keystroke. One page per tick: a page builder is a few hundred control constructions, which is
// milliseconds, but sixteen of them in one go would be a visible hitch on a cold start.
constexpr UINT_PTR kIndexPageTimer = 0x4E57;
// Hover delay before a cut-off search suggestion shows its full text.
constexpr UINT_PTR kSearchTipTimer = 0x4E58;

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

// WinUI 3 control colors for the search box and its suggestions. The search box follows the
// Fluent AutoSuggestBox rather than the form controls ported from the web page, so it takes the
// system's token values (ControlFill*, ControlStroke*, SubtleFill*) instead of the CSS palette.
struct FluentColors
{
    D2D1_COLOR_F fillRest;
    D2D1_COLOR_F fillHover;
    D2D1_COLOR_F fillFocused;
    D2D1_COLOR_F stroke;
    // The darker bottom edge that gives a text box its sunken look.
    D2D1_COLOR_F strokeBottom;
    D2D1_COLOR_F subtleHover;
    D2D1_COLOR_F subtlePressed;
    D2D1_COLOR_F glyph;
    D2D1_COLOR_F flyoutFill;
    D2D1_COLOR_F flyoutStroke;
};

const FluentColors &Fluent(const Palette &p)
{
    static const FluentColors light = {
        Rgba(255, 255, 255, 0.70f), Rgba(249, 249, 249, 0.50f), Hex(0xFFFFFF),
        Rgba(0, 0, 0, 0.06f),       Rgba(0, 0, 0, 0.45f),       Rgba(0, 0, 0, 0.037f),
        Rgba(0, 0, 0, 0.024f),      Rgba(0, 0, 0, 0.62f),       Hex(0xF9F9F9),
        Rgba(0, 0, 0, 0.06f),
    };
    static const FluentColors dark = {
        Rgba(255, 255, 255, 0.06f), Rgba(255, 255, 255, 0.08f), Rgba(30, 30, 30, 0.70f),
        Rgba(255, 255, 255, 0.07f), Rgba(255, 255, 255, 0.54f), Rgba(255, 255, 255, 0.06f),
        Rgba(255, 255, 255, 0.04f), Rgba(255, 255, 255, 0.79f), Hex(0x2C2C2C),
        Rgba(255, 255, 255, 0.08f),
    };
    return p.light ? light : dark;
}

// Segoe Fluent Icons code points.
constexpr wchar_t kGlyphSearch[] = L"\xE721";
constexpr wchar_t kGlyphClear[] = L"\xE894";

void DrawGlyph(msimeui::DeviceResources &dr, const wchar_t *glyph, float size, const RectF &box,
               const D2D1_COLOR_F &color)
{
    TextStyle style;
    style.family = IconFontFamily();
    style.size = size;
    style.lineHeight = 1.0f;
    DrawTextInRect(dr, glyph, style, box, color, true);
}

// A TextBox that reports the pointer coming and going: the search field draws the chrome around
// it, and its hover look has to follow the pointer over the text too.
class HoverTextBox : public msimeui::TextBox
{
  public:
    using msimeui::TextBox::TextBox;

    void SetOnHoverChanged(std::function<void(bool hovered)> handler)
    {
        onHoverChanged_ = std::move(handler);
    }

    void OnMouseEnter() override
    {
        msimeui::TextBox::OnMouseEnter();
        if (onHoverChanged_)
            onHoverChanged_(true);
    }

    void OnMouseLeave() override
    {
        msimeui::TextBox::OnMouseLeave();
        if (onHoverChanged_)
            onHoverChanged_(false);
    }

    // Clicking into the box keeps its suggestion flyout open.
    bool KeepsPopupsOpenOnClick() const override
    {
        return true;
    }

  private:
    std::function<void(bool hovered)> onHoverChanged_;
};

// The Fluent AutoSuggestBox field: a 4px-rounded box with the sunken bottom edge, an accent
// underline while focused, the search glyph on the right and a clear button in front of it while
// there is text to clear.
class SearchField : public msimeui::Visual
{
  public:
    SearchField()
    {
        box_ = std::make_shared<HoverTextBox>(kHeight, L"搜索设置");
        box_->SetChromeVisible(false);
        box_->SetFontSize(kFontBody);
        box_->SetPlaceholderFontSize(kFontBody);
        box_->SetOnHoverChanged([this](bool hovered) {
            boxHovered_ = hovered;
            InvalidateVisual();
        });
        box_->SetOnTextChanged([this](const std::wstring &text) {
            InvalidateArrange();
            if (onChanged_)
                onChanged_(text);
        });
        box_->SetOnFocusChanged([this](bool focused) {
            InvalidateArrange();
            InvalidateVisual();
            if (onFocusChanged_)
                onFocusChanged_(focused);
        });
        box_->SetOnSubmit([this]() {
            if (onSubmit_)
                onSubmit_();
        });
        AdoptChild(box_);
    }

    void SetOnChanged(std::function<void(const std::wstring &)> handler)
    {
        onChanged_ = std::move(handler);
    }

    void SetOnSubmit(std::function<void()> handler)
    {
        onSubmit_ = std::move(handler);
    }

    void SetOnFocusChanged(std::function<void(bool focused)> handler)
    {
        onFocusChanged_ = std::move(handler);
    }

    std::wstring GetText() const
    {
        return box_->GetText();
    }

    // Clears the text and reports it, the same as the clear button.
    void Clear()
    {
        box_->SetText(L"");
        InvalidateArrange();
        if (onChanged_)
            onChanged_(std::wstring());
    }

    bool IsFocused() const
    {
        return box_->IsFocused();
    }

    void Focus()
    {
        if (window_)
            window_->FocusVisual(box_.get());
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        box_->MeasureInLayout({availableSize.width, kHeight});
        return {availableSize.width, kHeight};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        const float right = ClearVisible() ? ClearRect().x : GlyphRect().x;
        box_->ArrangeInLayout(
            {bounds_.x + kTextInset, bounds_.y, std::max(0.0f, right - bounds_.x - kTextInset), kHeight});
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        const FluentColors &c = Fluent(p);
        if (themedLight_ != (p.light ? 1 : 0))
        {
            themedLight_ = p.light ? 1 : 0;
            msimeui::TextBox::Appearance appearance;
            appearance.borderWidth = 0.0f;
            appearance.paddingX = 0.0f;
            appearance.paddingY = (kHeight - LineHeight(BodyStyle())) * 0.5f;
            appearance.fontFamily = UiFontFamily();
            appearance.text = p.text;
            appearance.placeholder = p.textMuted;
            appearance.selection = WithAlpha(p.accent, 0.45f);
            box_->SetAppearance(appearance);
        }

        const bool focused = box_->IsFocused();
        const bool hovered = boxHovered_ || selfHovered_;
        const D2D1_COLOR_F fill = focused ? c.fillFocused : hovered ? c.fillHover : c.fillRest;
        FillRounded(dr, bounds_, kRadius, fill);
        StrokeRounded(dr, bounds_, kRadius, c.stroke, CssBorder());
        // The bottom edge, inside the rounded outline so the corners curve up into it: one hairline
        // at rest, a two-pixel accent bar while focused.
        RenderBottomEdge(dr, focused ? p.accent : c.strokeBottom, focused ? CssBorder(2.0f) : CssBorder());

        box_->Render(dr);

        if (ClearVisible())
        {
            const RectF clear = ClearRect();
            if (clearPressed_ && clearHovered_)
                FillRounded(dr, clear, kRadius, c.subtlePressed);
            else if (clearHovered_)
                FillRounded(dr, clear, kRadius, c.subtleHover);
            DrawGlyph(dr, kGlyphClear, 10.0f, clear, c.glyph);
        }
        DrawGlyph(dr, kGlyphSearch, 12.0f, GlyphRect(), c.glyph);
    }

    void Attach(msimeui::Window *window) override
    {
        msimeui::Visual::Attach(window);
        box_->Attach(window);
    }

    msimeui::Visual *FindVisualAt(const PointF &point) override
    {
        if (!Contains(bounds_, point))
            return nullptr;
        if (box_->HitTest(point) && !(ClearVisible() && Contains(ClearRect(), point)))
            return box_.get();
        return this;
    }

    msimeui::Visual *FindFocusableAt(const PointF &point) override
    {
        if (!Contains(bounds_, point))
            return nullptr;
        // The clear button keeps whatever focus there is; anywhere else in the field focuses the
        // text, like clicking the padding of an <input>.
        if (ClearVisible() && Contains(ClearRect(), point))
            return nullptr;
        return box_->HitTest(point) ? box_.get() : nullptr;
    }

    msimeui::Visual *FindFirstFocusableDescendant() override
    {
        return box_.get();
    }

    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        if (!window_)
            return false;
        const PointF dip = window_->ClientPixelsToDips(point);
        if (ClearVisible() && Contains(ClearRect(), dip))
        {
            clearPressed_ = true;
            InvalidateVisual();
            return true;
        }
        Focus();
        return true;
    }

    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        if (!clearPressed_)
            return false;
        clearPressed_ = false;
        InvalidateVisual();
        if (window_ && ClearVisible() && Contains(ClearRect(), window_->ClientPixelsToDips(point)))
        {
            Clear();
            Focus();
        }
        return true;
    }

    bool OnMouseMove(const POINT &point, WPARAM) override
    {
        const bool over = window_ && ClearVisible() && Contains(ClearRect(), window_->ClientPixelsToDips(point));
        if (over != clearHovered_)
        {
            clearHovered_ = over;
            InvalidateVisual();
        }
        return true;
    }

    void OnMouseEnter() override
    {
        selfHovered_ = true;
        InvalidateVisual();
    }

    void OnMouseLeave() override
    {
        selfHovered_ = false;
        clearHovered_ = false;
        clearPressed_ = false;
        InvalidateVisual();
    }

    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, clearHovered_ ? IDC_ARROW : IDC_IBEAM);
    }

    bool KeepsPopupsOpenOnClick() const override
    {
        return true;
    }

  private:
    static constexpr float kHeight = 32.0f;
    static constexpr float kRadius = 4.0f;
    static constexpr float kTextInset = 11.0f;
    static constexpr float kGlyphWidth = 32.0f;
    static constexpr float kClearSize = 24.0f;

    bool ClearVisible() const
    {
        return box_->IsFocused() && !box_->GetText().empty();
    }

    RectF GlyphRect() const
    {
        return {bounds_.x + bounds_.width - kGlyphWidth, bounds_.y, kGlyphWidth, bounds_.height};
    }

    RectF ClearRect() const
    {
        const RectF glyph = GlyphRect();
        return {glyph.x - kClearSize + 4.0f, bounds_.y + (bounds_.height - kClearSize) * 0.5f, kClearSize, kClearSize};
    }

    void RenderBottomEdge(msimeui::DeviceResources &dr, const D2D1_COLOR_F &color, float thickness)
    {
        ID2D1RenderTarget *target = dr.GetRenderTarget();
        ID2D1SolidColorBrush *brush = target ? dr.GetSolidColorBrush(color) : nullptr;
        if (!brush)
            return;
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        target->GetFactory(factory.GetAddressOf());
        Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> shape;
        if (!factory || FAILED(factory->CreateRoundedRectangleGeometry(
                            D2D1::RoundedRect(ToD2D(bounds_), kRadius, kRadius), shape.GetAddressOf())))
            return;
        target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape.Get()), nullptr);
        const float bottom = bounds_.y + bounds_.height;
        target->FillRectangle(D2D1::RectF(bounds_.x, bottom - thickness, bounds_.x + bounds_.width, bottom), brush);
        target->PopLayer();
    }

    std::shared_ptr<HoverTextBox> box_;
    std::function<void(const std::wstring &)> onChanged_;
    std::function<void()> onSubmit_;
    std::function<void(bool focused)> onFocusChanged_;
    int themedLight_ = -1;
    bool boxHovered_ = false;
    bool selfHovered_ = false;
    bool clearHovered_ = false;
    bool clearPressed_ = false;
};

// One suggestion under the search box: the page icon, the row title, and the page it is on.
// Clicking it jumps to the row. The keyboard selection is marked like a Fluent list item, with a
// short accent pill on the left.
class SearchResultItem : public msimeui::Visual
{
  public:
    SearchResultItem(std::wstring title, std::wstring pageTitle, std::wstring icon, std::function<void()> onActivate)
        : onActivate_(std::move(onActivate)), title_(std::move(title)), page_(std::move(pageTitle)),
          icon_(std::move(icon))
    {
        SetMargin({0.0f, 1.0f, 0.0f, 1.0f});
    }

    void SetHighlighted(bool highlighted)
    {
        if (highlighted_ == highlighted)
            return;
        highlighted_ = highlighted;
        InvalidateVisual();
    }

    void SetOnHoverChanged(std::function<void(SearchResultItem *item, bool hovered)> handler)
    {
        onHoverChanged_ = std::move(handler);
    }

    // Whether either line is cut at the item's edge, so the full text needs a tooltip.
    bool IsTruncated() const
    {
        const float width = TextWidth();
        return MeasureTextWidth(title_, BodyStyle()) > width + 0.5f ||
               MeasureTextWidth(page_, PageStyle()) > width + 0.5f;
    }

    // The tooltip text: the full title, and the page name too when that is what got cut.
    std::wstring ToolTipText() const
    {
        if (MeasureTextWidth(page_, PageStyle()) > TextWidth() + 0.5f)
            return title_ + L"\n" + page_;
        return title_;
    }

    // Where the text starts, for lining the tooltip up with it.
    float TextX() const
    {
        return bounds_.x + 12.0f + 16.0f + 12.0f;
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        // Two stacked lines with 6px above and below, never shorter than a Fluent list item.
        return {availableSize.width, std::max(40.0f, LineHeight(BodyStyle()) + LineHeight(PageStyle()) + 12.0f)};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        const FluentColors &c = Fluent(p);
        if (pressed_ && hovered_)
            FillRounded(dr, bounds_, 4.0f, c.subtlePressed);
        else if (hovered_ || highlighted_)
            FillRounded(dr, bounds_, 4.0f, c.subtleHover);
        if (highlighted_)
            FillRounded(dr, {bounds_.x, bounds_.y + (bounds_.height - 16.0f) * 0.5f, 3.0f, 16.0f}, 1.5f, p.accent);

        const float iconX = bounds_.x + 12.0f;
        if (!icon_.empty())
            DrawSvg(dr, ThemedSvg(icon_, p.light), {iconX, bounds_.y + (bounds_.height - 16.0f) * 0.5f, 16.0f, 16.0f});
        const float textX = TextX();
        const float textWidth = TextWidth();
        // Clipped to the item: a long title is cut at the edge rather than drawn over the list.
        const float titleHeight = LineHeight(BodyStyle());
        const float pageHeight = LineHeight(PageStyle());
        const float blockY = bounds_.y + (bounds_.height - (titleHeight + pageHeight)) * 0.5f;
        if (fittedWidth_ != textWidth)
        {
            fittedWidth_ = textWidth;
            fittedTitle_ = Ellipsize(title_, BodyStyle(), textWidth);
            fittedPage_ = Ellipsize(page_, PageStyle(), textWidth);
        }
        DrawTextInRect(dr, fittedTitle_, BodyStyle(), {textX, blockY, textWidth, titleHeight}, p.text);
        DrawTextInRect(dr, fittedPage_, PageStyle(), {textX, blockY + titleHeight, textWidth, pageHeight}, p.textMuted);
    }

    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        pressed_ = window_ && HitTest(window_->ClientPixelsToDips(point));
        return pressed_;
    }

    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        if (!pressed_)
            return false;
        pressed_ = false;
        InvalidateVisual();
        if (window_ && HitTest(window_->ClientPixelsToDips(point)) && onActivate_)
        {
            // The handler switches pages, which drops this item out of the list it lives in.
            const auto handler = onActivate_;
            handler();
        }
        return true;
    }

    void OnMouseEnter() override
    {
        hovered_ = true;
        InvalidateVisual();
        if (onHoverChanged_)
        {
            const auto handler = onHoverChanged_;
            handler(this, true);
        }
    }

    void OnMouseLeave() override
    {
        hovered_ = false;
        pressed_ = false;
        InvalidateVisual();
        if (onHoverChanged_)
        {
            const auto handler = onHoverChanged_;
            handler(this, false);
        }
    }

    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, IDC_HAND);
    }

  private:
    static TextStyle PageStyle()
    {
        TextStyle style;
        style.size = kFontCaption;
        return style;
    }

    float TextWidth() const
    {
        return std::max(0.0f, bounds_.x + bounds_.width - 8.0f - TextX());
    }

    // `text` as it fits in `width`, ending in "…" when it does not (Fluent's character trimming).
    static std::wstring Ellipsize(const std::wstring &text, const TextStyle &style, float width)
    {
        if (MeasureTextWidth(text, style) <= width + 0.5f)
            return text;
        // Longest prefix that still fits with the ellipsis after it.
        size_t low = 0;
        size_t high = text.size();
        while (low < high)
        {
            const size_t mid = (low + high + 1) / 2;
            if (MeasureTextWidth(text.substr(0, mid) + L"\x2026", style) <= width + 0.5f)
                low = mid;
            else
                high = mid - 1;
        }
        // Never split a surrogate pair.
        if (low > 0 && IS_HIGH_SURROGATE(text[low - 1]))
            --low;
        return text.substr(0, low) + L"\x2026";
    }

    std::function<void()> onActivate_;
    std::function<void(SearchResultItem *item, bool hovered)> onHoverChanged_;
    // The lines as drawn at `fittedWidth_`, trimmed once per width rather than every frame.
    std::wstring fittedTitle_;
    std::wstring fittedPage_;
    float fittedWidth_ = -1.0f;
    std::wstring title_;
    std::wstring page_;
    std::wstring icon_;
    bool highlighted_ = false;
    bool hovered_ = false;
    bool pressed_ = false;
};

// The suggestion rows, scrolling once there are more than the popup has room for.
//
// It scrolls by itself rather than through msimeui::ScrollViewer: that one relayouts every popup
// whenever it arranges its content, which from inside a popup re-enters the popup's own layout.
class SearchResultsList : public msimeui::Visual
{
  public:
    explicit SearchResultsList(std::shared_ptr<Column> column) : column_(std::move(column))
    {
        AdoptChild(column_);
    }

    void SetMaxHeight(float height)
    {
        if (maxListHeight_ == height)
            return;
        maxListHeight_ = height;
        InvalidateMeasure();
    }

    // Scrolls just enough to show `rect` (window DIPs), for the row picked with the keyboard.
    void Reveal(const RectF &rect)
    {
        const float top = rect.y - bounds_.y + offset_;
        float offset = offset_;
        if (top < offset)
            offset = top;
        else if (top + rect.height > offset + bounds_.height)
            offset = top + rect.height - bounds_.height;
        ScrollTo(offset);
    }

    void Wheel(short delta)
    {
        // Three 16 DIP lines per notch, like the system's default wheel setting.
        ScrollTo(offset_ - static_cast<float>(delta) / WHEEL_DELTA * 48.0f);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        contentHeight_ = column_->MeasureInLayout({availableSize.width, 1000000.0f}).height;
        return {availableSize.width, std::min({contentHeight_, maxListHeight_, availableSize.height})};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        offset_ = std::clamp(offset_, 0.0f, MaxOffset());
        ArrangeContent();
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        dr.PushClip(bounds_);
        column_->Render(dr);
        dr.PopClip();
        if (MaxOffset() <= 0.0f)
            return;
        // A thin Fluent scroll indicator; the wheel and the arrow keys do the scrolling.
        const float track = bounds_.height - 4.0f;
        const float thumb = std::max(24.0f, track * bounds_.height / contentHeight_);
        const float y = bounds_.y + 2.0f + (track - thumb) * (offset_ / MaxOffset());
        FillRounded(dr, {bounds_.x + bounds_.width - 4.0f, y, 2.0f, thumb}, 1.0f, CurrentPalette().scrollbarThumb);
    }

    void Attach(msimeui::Window *window) override
    {
        msimeui::Visual::Attach(window);
        column_->Attach(window);
    }

    msimeui::Visual *FindVisualAt(const PointF &point) override
    {
        if (!Contains(bounds_, point))
            return nullptr;
        msimeui::Visual *hit = column_->FindVisualAt(point);
        return hit ? hit : this;
    }

    msimeui::Visual *FindFocusableAt(const PointF &point) override
    {
        // The rows are not focusable, so a click on one leaves the keyboard in the search box.
        return Contains(bounds_, point) ? column_->FindFocusableAt(point) : nullptr;
    }

  private:
    float MaxOffset() const
    {
        return std::max(0.0f, contentHeight_ - bounds_.height);
    }

    void ArrangeContent()
    {
        // Whole device pixels, so text does not resample at fractional offsets.
        const float scale = window_ ? window_->GetDpi() / 96.0f : 1.0f;
        const float offset = std::round(offset_ * scale) / scale;
        column_->ArrangeInLayout({bounds_.x, bounds_.y - offset, bounds_.width, contentHeight_});
    }

    void ScrollTo(float offset)
    {
        offset = std::clamp(offset, 0.0f, MaxOffset());
        if (offset == offset_)
            return;
        offset_ = offset;
        ArrangeContent();
        if (window_)
        {
            window_->RefreshHover();
            window_->Invalidate();
        }
    }

    std::shared_ptr<Column> column_;
    float contentHeight_ = 0.0f;
    float offset_ = 0.0f;
    float maxListHeight_ = 400.0f;
};

// The suggestion flyout: a layer of its own over the window, like an open dropdown menu, anchored
// under the search box and wider than the sidebar so titles have room.
class SearchPopup : public msimeui::Popup
{
  public:
    explicit SearchPopup(const std::shared_ptr<SearchResultsList> &list) : msimeui::Popup(list), list_(list.get())
    {
        const Palette &p = CurrentPalette();
        SetPadding(4.0f);
        SetPlacement(msimeui::PopupPlacement::BelowLeading);
        SetCornerRadius(8.0f);
        SetConstrainToViewport(true);
        SetMatchAnchorWidth(true);
        SetOffset(0.0f, 4.0f);
        SetBackgroundFill(Fluent(p).flyoutFill);
        SetBorderColor(p.dropdownMenuBorder);
        SetShadowEnabled(true);
        SetShadowPasses({{p.menuShadow[0].sigma, p.menuShadow[0].alpha, 0.0f, p.menuShadow[0].offsetY},
                         {p.menuShadow[1].sigma, p.menuShadow[1].alpha, 0.0f, p.menuShadow[1].offsetY}});
    }

    void SetAnchor(const RectF &anchor)
    {
        anchor_ = anchor;
        SetAnchorRect(anchor);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        // Measured at the width it is shown at, so the rows know where their text gets cut.
        const float width = std::min(std::max(kWidth, anchor_.width), availableSize.width);
        const SizeF size = msimeui::Popup::Measure({width, availableSize.height});
        return {width, size.height};
    }

    void LayoutOverlay(const SizeF &viewportSize) override
    {
        // As tall as the room below the box, so the list scrolls instead of being flipped above
        // the box or running off the window.
        const float below = viewportSize.height - (anchor_.y + anchor_.height + 4.0f) - 8.0f - 8.0f;
        list_->SetMaxHeight(std::clamp(below, 120.0f, 440.0f));
        msimeui::Popup::LayoutOverlay(viewportSize);
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        // Like an open dropdown menu, the flyout is a composited layer: grayscale text.
        ScopedTextAntialias grayscale(dr, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        msimeui::Popup::Render(dr);
    }

    bool OnMouseWheel(const POINT &point, short delta, WPARAM) override
    {
        if (!window_ || !HitTest(window_->ClientPixelsToDips(point)))
            return false;
        // Taken even at the end of the list, so the page under the flyout does not scroll.
        list_->Wheel(delta);
        return true;
    }

  private:
    static constexpr float kWidth = 320.0f;

    SearchResultsList *list_ = nullptr;
    RectF anchor_ = {};
};

// Fluent tooltip with the full text of a suggestion that does not fit. It sits in the popup layer
// above the flyout and takes no input, so the row under it stays clickable.
class SearchToolTip : public msimeui::Visual
{
  public:
    void Show(const std::wstring &text, const RectF &anchor)
    {
        TextStyle style;
        style.size = kFontCaption;
        style.wrap = true;
        text_.Set(text, style);
        anchor_ = anchor;
        fade_.Start(0.0f, 1.0f, 120.0, msimeui::Easing::Ease(), msimeui::NowMs());
        InvalidateMeasure();
        if (window_)
            window_->RequestAnimationFrame();
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        textWidth_ = std::max(0.0f, std::min(kMaxWidth, availableSize.width) - kPaddingX * 2.0f);
        const SizeF text = text_.Measure(textWidth_);
        textWidth_ = std::min(textWidth_, std::ceil(text.width));
        return {textWidth_ + kPaddingX * 2.0f, text.height + kPaddingY * 2.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void LayoutOverlay(const SizeF &viewportSize) override
    {
        const SizeF size = MeasureInLayout({std::max(0.0f, viewportSize.width - 16.0f), viewportSize.height});
        float x = std::clamp(anchor_.x, 8.0f, std::max(8.0f, viewportSize.width - size.width - 8.0f));
        float y = anchor_.y + anchor_.height + 4.0f;
        if (y + size.height > viewportSize.height - 8.0f)
            y = anchor_.y - size.height - 4.0f;
        ArrangeInLayout({x, std::max(8.0f, y), size.width, size.height});
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        const double now = window_ ? window_->GetFrameTime() : msimeui::NowMs();
        const float opacity = fade_.Value(now);
        if (fade_.IsRunning(now) && window_)
            window_->RequestAnimationFrame();
        PushOpacity(dr, opacity);
        FillRounded(dr, bounds_, 4.0f, p.tooltipBg);
        StrokeRounded(dr, bounds_, 4.0f, p.tooltipBorder, CssBorder());
        text_.Draw(dr, {bounds_.x + kPaddingX, bounds_.y + kPaddingY}, p.tooltipText, textWidth_);
        PopOpacity(dr);
    }

    bool HitTest(const PointF &) const override
    {
        return false;
    }

    msimeui::Visual *FindVisualAt(const PointF &) override
    {
        return nullptr;
    }

    msimeui::Visual *FindFocusableAt(const PointF &) override
    {
        return nullptr;
    }

  private:
    static constexpr float kMaxWidth = 320.0f;
    static constexpr float kPaddingX = 8.0f;
    static constexpr float kPaddingY = 5.0f;

    TextLayout text_;
    RectF anchor_ = {};
    float textWidth_ = 0.0f;
    msimeui::Tween fade_{0.0f};
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

// The search box at the top of the sidebar. Its suggestions open in a flyout over the window, in
// the popup layer an open dropdown menu uses, instead of pushing the navigation items down.
class SidebarSearch : public msimeui::Panel
{
  public:
    using ActivateHandler = std::function<void(const SettingsIndexEntry &entry)>;

    explicit SidebarSearch(ActivateHandler onActivate) : onActivate_(std::move(onActivate))
    {
        field_ = std::make_shared<SearchField>();
        field_->SetOnChanged([this](const std::wstring &text) {
            highlighted_ = -1;
            Search(text, true);
        });
        field_->SetOnFocusChanged([this](bool focused) {
            // Coming back to a box that still holds a query brings its suggestions back.
            if (focused && !open_ && !Query().empty())
                Search(Query(), true);
        });
        // Enter opens the suggestion picked with the arrow keys, or the best one, so the box is
        // usable without the mouse.
        field_->SetOnSubmit([this]() {
            if (!open_)
                Search(Query(), true);
            if (results_.empty())
                return;
            const size_t index = highlighted_ >= 0 ? static_cast<size_t>(highlighted_) : 0;
            Activate(results_[std::min(index, results_.size() - 1)]);
        });
        // margin: 2px 8px 8px, so the box lines up with the navigation items below it.
        field_->SetMargin({8.0f, 2.0f, 8.0f, 8.0f});
        tip_ = std::make_shared<SearchToolTip>();
        AddChild(field_);
    }

    // Rows collected so far, in page order. The window rebuilds this as pages get built.
    // Re-runs an open query against the new index.
    void SetIndex(const std::shared_ptr<const std::vector<SettingsIndexEntry>> &index)
    {
        index_ = index;
        if (open_ && !Query().empty())
            Search(Query(), false);
    }

    void Focus()
    {
        if (field_)
            field_->Focus();
    }

    // Keys the text box itself has no use for, routed here by the window while the box has focus:
    // Up / Down move through the suggestions, Escape closes them, then clears the query, then
    // leaves the box.
    bool HandleKey(WPARAM key)
    {
        if (!field_ || !field_->IsFocused())
            return false;
        if (key == VK_ESCAPE)
        {
            if (open_)
            {
                Close();
                return true;
            }
            if (Query().empty())
                return false;
            field_->Clear();
            return true;
        }
        if (key != VK_DOWN && key != VK_UP)
            return false;
        if (!open_)
        {
            if (Query().empty())
                return false;
            Search(Query(), true);
        }
        if (results_.empty())
            return true;
        const int count = static_cast<int>(results_.size());
        if (key == VK_DOWN)
            highlighted_ = highlighted_ < 0 ? 0 : (highlighted_ + 1) % count;
        else
            highlighted_ = highlighted_ <= 0 ? count - 1 : highlighted_ - 1;
        for (size_t i = 0; i < items_.size(); ++i)
            items_[i]->SetHighlighted(static_cast<int>(i) == highlighted_);
        if (list_ && static_cast<size_t>(highlighted_) < items_.size())
            list_->Reveal(items_[static_cast<size_t>(highlighted_)]->GetBounds());
        if (window_)
            window_->Invalidate();
        return true;
    }

    // The hover delay of a cut-off suggestion ran out: show its full text.
    void ShowPendingToolTip()
    {
        msimeui::Scene *scene = window_ ? window_->GetScene() : nullptr;
        if (window_)
            KillTimer(window_->GetHandle(), kSearchTipTimer);
        if (!scene || !open_ || !tipItem_)
            return;
        const RectF item = tipItem_->GetBounds();
        tip_->Show(tipItem_->ToolTipText(), {tipItem_->TextX(), item.y, item.width, item.height});
        // Re-added so it lands on top of the flyout.
        scene->RemovePopup(tip_.get(), false);
        scene->AddPopup(tip_);
        scene->RelayoutPopups();
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        // MeasureInLayout already accounts for the field's margin.
        return {availableSize.width, field_->MeasureInLayout(availableSize).height};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        field_->ArrangeInLayout(finalRect);
        if (popup_)
            popup_->SetAnchor(field_->GetBounds());
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        field_->Render(dr);
    }

  private:
    std::wstring Query() const
    {
        return field_ ? field_->GetText() : std::wstring();
    }

    static std::wstring PageIcon(const std::string &pageId)
    {
        for (const auto &page : Pages())
        {
            if (page.id == pageId)
                return page.icon;
        }
        return {};
    }

    bool SameResults(const std::vector<SettingsIndexEntry> &other) const
    {
        if (other.size() != results_.size())
            return false;
        for (size_t i = 0; i < other.size(); ++i)
        {
            if (other[i].pageId != results_[i].pageId || other[i].target.lock() != results_[i].target.lock())
                return false;
        }
        return true;
    }

    // Runs the query and shows its suggestions. `rebuild` false keeps an open flyout as it is when
    // the hits did not change, so indexing more pages in the background does not reset the
    // pointer's hover or the tooltip.
    void Search(const std::wstring &query, bool rebuild)
    {
        std::vector<SettingsIndexEntry> hits;
        if (index_ && !query.empty())
            hits = SearchSettings(*index_, query);
        if (!rebuild && open_ && SameResults(hits))
            return;
        results_ = std::move(hits);
        if (highlighted_ >= static_cast<int>(results_.size()))
            highlighted_ = -1;
        HideToolTip();
        tipItem_ = nullptr;
        if (query.empty())
        {
            Close();
            items_.clear();
            return;
        }
        msimeui::Scene *scene = window_ ? window_->GetScene() : nullptr;
        if (!scene)
            return;

        items_.clear();
        auto column = std::make_shared<Column>(0.0f);
        for (const SettingsIndexEntry &entry : results_)
        {
            auto item = std::make_shared<SearchResultItem>(entry.title, entry.pageTitle, PageIcon(entry.pageId),
                                                           [this, entry]() { Activate(entry); });
            item->SetHighlighted(static_cast<int>(items_.size()) == highlighted_);
            item->SetOnHoverChanged([this](SearchResultItem *hovered, bool entered) { OnItemHover(hovered, entered); });
            items_.push_back(item);
            column->Add(item);
        }
        if (results_.empty())
        {
            TextStyle style;
            style.size = kFontCaption;
            auto empty = std::make_shared<Label>(L"找不到相关设置", style, TextRole::Muted);
            empty->SetMargin({12.0f, 10.0f, 12.0f, 10.0f});
            column->Add(empty);
        }
        auto list = std::make_shared<SearchResultsList>(column);
        auto popup = std::make_shared<SearchPopup>(list);
        popup->SetAnchor(field_->GetBounds());

        const bool wasOpen = open_;
        if (wasOpen)
            scene->RemovePopup(popup_.get(), false);
        else
            // Only one flyout at a time: an open dropdown menu goes away.
            scene->ClearPopups(true);
        popup_ = popup;
        list_ = list.get();
        SearchPopup *raw = popup.get();
        scene->AddPopup(popup_, [this, raw]() {
            // Dismissed from outside: a click elsewhere, or the page switching.
            if (popup_.get() != raw)
                return;
            open_ = false;
            tipItem_ = nullptr;
            if (window_)
                KillTimer(window_->GetHandle(), kSearchTipTimer);
        });
        if (!wasOpen)
            popup_->BeginAppearAnimation(140.0, -3.0f, 0.985f);
        open_ = true;
        scene->RelayoutPopups();
        window_->Invalidate();
    }

    void Close()
    {
        HideToolTip();
        tipItem_ = nullptr;
        if (!open_)
            return;
        open_ = false;
        if (msimeui::Scene *scene = window_ ? window_->GetScene() : nullptr)
            scene->RemovePopup(popup_.get(), false);
        popup_.reset();
        list_ = nullptr;
        if (window_)
            window_->Invalidate();
    }

    // A pointer resting on a suggestion whose text is cut off gets the full text after the same
    // delay as the title bar's tooltips.
    void OnItemHover(SearchResultItem *item, bool hovered)
    {
        if (hovered)
        {
            HideToolTip();
            tipItem_ = item;
            if (window_ && item->IsTruncated())
                SetTimer(window_->GetHandle(), kSearchTipTimer, 500, nullptr);
            return;
        }
        if (tipItem_ == item)
        {
            tipItem_ = nullptr;
            HideToolTip();
        }
    }

    void HideToolTip()
    {
        if (!window_)
            return;
        KillTimer(window_->GetHandle(), kSearchTipTimer);
        if (msimeui::Scene *scene = window_->GetScene())
            scene->RemovePopup(tip_.get(), false);
    }

    void Activate(const SettingsIndexEntry &entry)
    {
        // Closing drops the flyout the clicked row lives in; keep what the handler needs.
        const SettingsIndexEntry target = entry;
        Close();
        if (onActivate_)
        {
            const auto handler = onActivate_;
            handler(target);
        }
    }

    std::shared_ptr<SearchField> field_;
    std::shared_ptr<SearchPopup> popup_;
    // The list inside `popup_`, owned by it.
    SearchResultsList *list_ = nullptr;
    std::shared_ptr<SearchToolTip> tip_;
    std::shared_ptr<const std::vector<SettingsIndexEntry>> index_;
    std::vector<SettingsIndexEntry> results_;
    // Kept here as well as in the flyout, so a row stays alive through its own click handler.
    std::vector<std::shared_ptr<SearchResultItem>> items_;
    // The suggestion under the pointer, waiting for its tooltip; one of `items_`.
    SearchResultItem *tipItem_ = nullptr;
    // Suggestion picked with the arrow keys; -1 while none is.
    int highlighted_ = -1;
    bool open_ = false;
    ActivateHandler onActivate_;
};

class Sidebar : public msimeui::Panel
{
  public:
    using SelectHandler = std::function<void(size_t index)>;

    Sidebar()
    {
        AddChild(std::make_shared<SidebarHeader>());
        search_ = std::make_shared<SidebarSearch>([this](const SettingsIndexEntry &entry) { Activate(entry); });
        AddChild(search_);
        const auto &pages = Pages();
        for (size_t i = 0; i < pages.size(); ++i)
        {
            auto item = std::make_shared<NavItem>(this, i, pages[i].title, pages[i].icon);
            items_.push_back(item);
            AddChild(item);
        }
    }

    SidebarSearch *Search()
    {
        return search_.get();
    }

    // Jumps to an arbitrary row, rather than a sidebar page: switches to the entry's page and asks
    // the window to scroll the row into view.
    void SetOnActivateEntry(std::function<void(const SettingsIndexEntry &entry)> handler)
    {
        onActivateEntry_ = std::move(handler);
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
        {
            const auto handler = onSelect_;
            handler(index);
        }
    }

    // A search hit: switch to its page, then hand the entry to the window so it can scroll.
    void Activate(const SettingsIndexEntry &entry)
    {
        if (onActivateEntry_)
            onActivateEntry_(entry);
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
    std::shared_ptr<SidebarSearch> search_;
    SelectHandler onSelect_;
    std::function<void(const SettingsIndexEntry &entry)> onActivateEntry_;
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
        {
            window_->DispatchImportedMessage(WM_MOUSELEAVE, 0, 0);
            window_->FocusVisual(nullptr);
        }
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

// What a search hit highlights: the SettingRow its title belongs to, or, for a title that heads a
// block of its own (a form field, a heading over a list of options), that block.
msimeui::Visual *HighlightTarget(msimeui::Visual &title)
{
    for (msimeui::Visual *visual = &title; visual; visual = visual->GetParentVisual())
    {
        if (dynamic_cast<SettingRow *>(visual))
            return visual;
    }
    msimeui::Visual *parent = title.GetParentVisual();
    return parent ? parent : &title;
}

// The accent pulse on the setting a search jumped to, like the highlight Windows Settings puts on
// the card a search opened. It is drawn over the page by the root rather than by the setting, so
// any target works, and it follows the target's bounds while the page scrolls.
class SearchHighlight : public msimeui::Visual
{
  public:
    // `title` keeps the target's page alive for the check; `target` is its row or block.
    void Start(const std::shared_ptr<msimeui::Visual> &title, msimeui::Visual *target)
    {
        title_ = title;
        target_ = target;
        reducedMotion_ = !SystemAnimationsEnabled();
        startMs_ = msimeui::NowMs();
        if (window_)
            window_->RequestAnimationFrame();
    }

    // The page it was on went away; its bounds no longer mean anything on screen.
    void Stop()
    {
        startMs_ = -1.0;
        target_ = nullptr;
        title_.reset();
    }

    void RenderOver(msimeui::DeviceResources &dr, const RectF &clip)
    {
        if (startMs_ < 0.0)
            return;
        const double now = window_ ? window_->GetFrameTime() : msimeui::NowMs();
        const double elapsed = std::max(0.0, now - startMs_);
        const double duration = reducedMotion_ ? kStillMs : kPulseMs * kPulses;
        if (elapsed >= duration || title_.expired())
        {
            startMs_ = -1.0;
            target_ = nullptr;
            return;
        }
        // Raised cosine per pulse: starts and ends at nothing, peaks halfway. With animations
        // turned off it holds still, then goes in one step.
        const float strength =
            reducedMotion_ ? 1.0f
                           : static_cast<float>(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * elapsed / kPulseMs));
        // Reaches into the section padding so the highlight frames the setting instead of
        // touching its text.
        const RectF bounds = target_->GetBounds();
        const RectF box = {bounds.x - 12.0f, bounds.y - 4.0f, bounds.width + 24.0f, bounds.height + 8.0f};
        const Palette &p = CurrentPalette();
        dr.PushClip(clip);
        FillRounded(dr, box, 6.0f, WithAlpha(p.accent, kFillAlpha * strength));
        StrokeRounded(dr, box, 6.0f, WithAlpha(p.accent, kStrokeAlpha * strength), 1.0f);
        dr.PopClip();
        if (window_)
            window_->RequestAnimationFrame();
    }

    SizeF Measure(const SizeF &) override
    {
        return {};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &) override
    {
    }

  private:
    static constexpr double kPulseMs = 700.0;
    static constexpr int kPulses = 2;
    static constexpr double kStillMs = 1200.0;
    // Over the content, so the fill stays faint enough to read the text through.
    static constexpr float kFillAlpha = 0.12f;
    static constexpr float kStrokeAlpha = 0.6f;

    std::weak_ptr<msimeui::Visual> title_;
    msimeui::Visual *target_ = nullptr;
    double startMs_ = -1.0;
    bool reducedMotion_ = false;
};

// ---------------------------------------------------------------------------------
// Root: title bar, sidebar, content and the overlays, laid out like `.main`.
// ---------------------------------------------------------------------------------
class RootPanel : public msimeui::Panel
{
  public:
    RootPanel(std::shared_ptr<TitleBar> titleBar, std::shared_ptr<msimeui::ScrollViewer> sidebar,
              std::shared_ptr<msimeui::ScrollViewer> content, std::shared_ptr<ModalLayer> modal,
              std::shared_ptr<ToastView> toast, std::shared_ptr<SplashView> splash,
              std::shared_ptr<SearchHighlight> highlight)
        : titleBar_(std::move(titleBar)), sidebar_(std::move(sidebar)), content_(std::move(content)),
          modal_(std::move(modal)), toast_(std::move(toast)), splash_(std::move(splash)),
          highlight_(std::move(highlight))
    {
        AddChild(titleBar_);
        AddChild(sidebar_);
        AddChild(content_);
        AddChild(toast_);
        AddChild(modal_);
        AddChild(splash_);
        // Not laid out: it draws over the content at the bounds of whatever it highlights.
        AddChild(highlight_);
        content_->SetVerticalAlignment(msimeui::VerticalAlignment::Stretch);
        sidebar_->SetVerticalAlignment(msimeui::VerticalAlignment::Stretch);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        const Columns c = Split(availableSize);
        titleBar_->MeasureInLayout({availableSize.width, kTitleBarHeight});
        sidebar_->MeasureInLayout({c.sidebarWidth, c.bodyHeight});
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
        sidebar_->ArrangeInLayout({finalRect.x, finalRect.y + kTitleBarHeight, c.sidebarWidth, c.bodyHeight});
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
        {
            content_->Render(dr);
            highlight_->RenderOver(dr, content_->GetBounds());
        }
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
    std::shared_ptr<SearchHighlight> highlight_;
};

bool IsCaptionButton(WPARAM hit)
{
    return hit == HTMINBUTTON || hit == HTMAXBUTTON || hit == HTCLOSE;
}

int TopFrameThickness(HWND hwnd)
{
    const UINT dpi = GetDpiForWindow(hwnd);
    return GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
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
        SetLayoutScale(static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f);
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
        highlight_ = std::make_shared<SearchHighlight>();
        root_ =
            std::make_shared<RootPanel>(titleBar_, sidebarScroll_, contentScroll_, modal_, toast_, splash_, highlight_);

        sidebar_->SetOnSelect([this](size_t index) { ShowPageIndex(index, false); });
        sidebar_->SetOnActivateEntry([this](const SettingsIndexEntry &entry) { ActivateSearchEntry(entry); });

        auto scene = std::make_unique<msimeui::Scene>();
        scene->SetRoot(root_);
        window_->SetScene(std::move(scene));

        // Hand the caption strip to the client area; the side and bottom frames stay.
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        model_.Post("configRequest");
        ShowPage(initialPage.empty() ? std::string("input") : initialPage);
        // Fill in the pages the user has not opened, one per tick, so search covers the whole
        // settings tree rather than only what has been visited this session.
        SetTimer(hwnd_, kIndexPageTimer, 1, nullptr);
        if (model_.HasConfig())
            splash_->Dismiss();
        else
            SetTimer(hwnd_, kSplashTimer, 1500, nullptr);
    }

    ~SettingsWindowImpl() override
    {
        skinpreview::SetImageLoadWindow(nullptr);
        KillTimer(hwnd_, kToolTipTimer);
        KillTimer(hwnd_, kSearchTipTimer);
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
        highlight_->Stop();
        window_->GetScene()->ClearPopups(true);
        modal_->Close();
        auto &page = pageCache_[index];
        if (!page)
            page = BuildPageAndIndex(index);
        pageHost_->SetChild(page);
        page->Attach(window_.get());
        // All pages share one scroller; a new page starts at the top.
        contentScroll_->SetScrollOffset(0.0f);
        window_->FocusVisual(nullptr);
        window_->Relayout();
        window_->RefreshHover();
    }

    // Builds a page under an index scope, so every row it creates is recorded, and hands it back
    // for the cache.
    //
    // Indexed pages stay in `pageCache_` rather than being thrown away after their rows are
    // collected. The index points at the page's rows, so a click has to scroll the very row that
    // is on screen; a page held only long enough to read its titles would hand back rows that no
    // longer exist. Holding one page per settings page costs what visiting them all costs, which
    // is what `pageCache_` already did, and a builder does not decode images or query the
    // dictionary on its own: those happen on first render and on user action.
    VisualPtr BuildPageAndIndex(size_t index)
    {
        const auto &pages = Pages();
        const std::string pageId = pages[index].id;
        const std::wstring pageTitle = pages[index].title;
        auto slice = std::make_shared<std::vector<SettingsIndexEntry>>();
        VisualPtr built;
        {
            SettingsIndexScope scope(pageId, [slice, pageId, pageTitle](const SettingsIndexScope::Row &row) {
                // A page that heads a block with the same words as the row under it ("联想模式"
                // over the 联想模式 row) would list the setting twice; the first one stays.
                for (SettingsIndexEntry &existing : *slice)
                {
                    if (existing.title != row.title)
                        continue;
                    if (existing.description.empty())
                        existing.description = row.description;
                    return;
                }
                slice->push_back({pageId, pageTitle, row.title, row.description, row.target});
            });
            built = pages[index].build(context_);
        }
        // A rebuild of this page replaces whatever it contributed before.
        pageEntries_.erase(std::remove_if(pageEntries_.begin(), pageEntries_.end(),
                                          [&pageId](const std::shared_ptr<std::vector<SettingsIndexEntry>> &entry) {
                                              return !entry->empty() && entry->front().pageId == pageId;
                                          }),
                           pageEntries_.end());
        pageEntries_.push_back(std::move(slice));
        RebuildSearchIndex();
        return built;
    }

    // Flattens per-page entry lists into the vector the search box reads.
    void RebuildSearchIndex()
    {
        auto flat = std::make_shared<std::vector<SettingsIndexEntry>>();
        for (const auto &pageEntries : pageEntries_)
        {
            for (const SettingsIndexEntry &entry : *pageEntries)
            {
                if (!entry.target.expired())
                    flat->push_back(entry);
            }
        }
        searchIndex_ = std::move(flat);
        if (sidebar_)
            sidebar_->Search()->SetIndex(searchIndex_);
    }

    // Builds the pages nobody has opened yet, one per timer tick, so every setting is findable
    // from the first keystroke. A page builder is a few hundred control constructions; sixteen of
    // them in one go would stall the first frame, so they are spread over ticks once the window is
    // already up and responsive.
    void IndexNextPage()
    {
        const auto &pages = Pages();
        while (nextIndexPage_ < pages.size())
        {
            const size_t index = nextIndexPage_++;
            // Built already: either it is on screen, or the indexer got to it earlier. Both are
            // in `pageCache_`, so membership is the only test needed and a page cannot be skipped
            // just because it happens to be the one showing.
            if (pageCache_.count(index) != 0)
                continue;
            pageCache_[index] = BuildPageAndIndex(index);
            return;
        }
        KillTimer(hwnd_, kIndexPageTimer);
    }

    // Switches to the entry's page, opens the collapsed group the row sits in, scrolls the row to
    // the middle of the view and flashes it.
    void ActivateSearchEntry(const SettingsIndexEntry &entry)
    {
        const auto &pages = Pages();
        for (size_t i = 0; i < pages.size(); ++i)
        {
            if (entry.pageId != pages[i].id)
                continue;
            // The sidebar follows, as if its item had been clicked.
            sidebar_->Select(i, current_ != static_cast<size_t>(-1));
            ShowPageIndex(i, false);
            auto title = entry.target.lock();
            if (!title)
                return;
            const bool shown = RevealInAncestors(*title);
            // Expanding a group moves everything under it: lay out again before reading where
            // the setting now is.
            window_->Relayout();
            if (!shown)
                return;
            msimeui::Visual *setting = HighlightTarget(*title);
            const RectF target = setting->GetBounds();
            const RectF view = contentScroll_->GetBounds();
            contentScroll_->SetScrollOffset(contentScroll_->GetScrollOffset() + (target.y + target.height * 0.5f) -
                                            (view.y + view.height * 0.5f));
            window_->Relayout();
            highlight_->Start(title, setting);
            window_->RefreshHover();
            return;
        }
    }

    void RebuildPages()
    {
        const size_t current = current_;
        pageCache_.clear();
        // Every cached page is gone, so its rows are too and the index has to be dropped with it.
        pageEntries_.clear();
        RebuildSearchIndex();
        current_ = static_cast<size_t>(-1);
        // Re-index from the start: the pages that were indexed before the rebuild no longer exist.
        nextIndexPage_ = 0;
        if (current < Pages().size())
            ShowPageIndex(current, true);
        SetTimer(hwnd_, kIndexPageTimer, 1, nullptr);
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
            if (wParam == kSearchTipTimer)
            {
                if (sidebar_)
                    sidebar_->Search()->ShowPendingToolTip();
                else
                    KillTimer(hwnd_, kSearchTipTimer);
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
            if (wParam == kIndexPageTimer)
            {
                IndexNextPage();
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
            // Ctrl+F focuses the settings search, so the box is reachable without the mouse and
            // without having to tab through the page that happens to be open.
            if (wParam == 'F' && (GetKeyState(VK_CONTROL) & 0x8000) && sidebar_)
            {
                sidebar_->Search()->Focus();
                result = 0;
                return true;
            }
            if (sidebar_ && sidebar_->Search()->HandleKey(wParam))
            {
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
    std::shared_ptr<SearchHighlight> highlight_;
    std::shared_ptr<ModalLayer> modal_;
    std::shared_ptr<ToastView> toast_;
    std::shared_ptr<SplashView> splash_;
    std::shared_ptr<RootPanel> root_;
    std::map<size_t, VisualPtr> pageCache_;
    // Per-page search entries, one list per built page, flattened into `searchIndex_`.
    std::vector<std::shared_ptr<std::vector<SettingsIndexEntry>>> pageEntries_;
    std::shared_ptr<const std::vector<SettingsIndexEntry>> searchIndex_;
    // Next page for the background indexer, which fills in the pages the user has not opened.
    size_t nextIndexPage_ = 0;
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
