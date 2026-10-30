#include "window/floating_toolbar_presenter.h"

#include "config/ime_config.h"
#include "conversion/chinese_converter.h"
#include "defines/globals.h"
#include "global/globals.h"
#include "ipc/event_listener.h"
#include "ipc/ipc.h"
#include "log/ftb_diag_log.h"
#include "settings/settings_launcher.h"
#include "skin/candidate_skin_catalog.h"
#include "utils/common_utils.h"
#include "utils/window_utils.h"
#include "window/small_windows.h"
#include "window/floating_toolbar_image_layout.h"
#include "window/floating_toolbar_skin.h"

#include "msimeui/Controls.h"
#include "msimeui/DeviceResources.h"
#include "msimeui/Fonts.h"
#include "msimeui/ImageMask.h"
#include "msimeui/Layout.h"
#include "msimeui/Scene.h"
#include "msimeui/Window.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <d2d1.h>
#include <dwrite.h>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <windowsx.h>
#include <wrl/client.h>

using msimeui::PointF;
using msimeui::RectF;
using msimeui::SizeF;

namespace
{
D2D1_COLOR_F ColorFromRgb(UINT rgb, float alpha = 1.0f)
{
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}

// Same reserve as the candidate card shadow: about 2σ of the shadow below,
// biased to the lower right like the shadow is.
constexpr float kShadowPadLeft = 16.0f;
constexpr float kShadowPadTop = 14.0f;
constexpr float kShadowPadRight = 32.0f;
constexpr float kShadowPadBottom = 34.0f;
constexpr float kToolbarStrokeWidth = 1.4f;
// D2D centres the stroke on the card edge, so half of it lies outside the card.
// Without the shadow margin the host still needs that half plus its antialiased
// fringe, or the window edge clips the border.
constexpr float kNoShadowPad = 1.0f;
static_assert(kNoShadowPad >= kToolbarStrokeWidth * 0.5f);
constexpr float kToolbarGlyphFontSizeFactor = 0.82f;
constexpr float kToolbarUnderlinedTextFontSizeFactor = 0.58f;
// 工具栏 HWND 上的动图帧定时器。宿主自己的定时器编号见 ime_windows_internal.h（2–14），这里避开它们。
constexpr UINT_PTR kToolbarAnimationTimerId = 0x0A12;
// Every toolbar icon carries a text fallback: "Segoe Fluent Icons" ships with
// Windows 11 only, and the Windows 10 build of "Segoe MDL2 Assets" may predate
// some of the IME-specific codepoints below. DirectWrite substitutes a font
// silently rather than failing, so without this the button renders as a blank
// box (issue #232).
struct ToolbarIcon
{
    wchar_t codepoint;
    const wchar_t *fallbackText;
};

constexpr ToolbarIcon kIconJa = {0xE7DE, L"日"};
constexpr ToolbarIcon kIconCn = {0xE982, L"中"};
constexpr ToolbarIcon kIconEn = {0xE983, L"英"};
constexpr ToolbarIcon kIconHalfWidth = {0xEC46, L"半"};
constexpr ToolbarIcon kIconFullWidth = {0xF138, L"全"};
constexpr ToolbarIcon kIconPuncEn = {0xF110, L","};
constexpr ToolbarIcon kIconPuncCn = {0xF111, L"。"};
constexpr ToolbarIcon kIconSimplified = {0xE88D, L"简"};
constexpr ToolbarIcon kIconTraditional = {0xE88C, L"繁"};
constexpr ToolbarIcon kIconEmoji = {0xE76E, L"表"};
constexpr ToolbarIcon kIconKeyboard = {0xE765, L"键"};
constexpr ToolbarIcon kIconSettings = {0xE713, L"设"};

float ToolbarUserScale()
{
    const float scale = static_cast<float>(GetConfiguredFloatingToolbarScale());
    const float fontFactor = static_cast<float>(GetConfiguredFloatingToolbarFontSize()) / 24.0f;
    const float combined = (scale > 0.0f ? scale : 1.0f) * (fontFactor > 0.0f ? fontFactor : 1.0f);
    return combined > 0.05f ? combined : 1.0f;
}

class ToolbarIconButton : public msimeui::Visual
{
  public:
    using ClickHandler = std::function<void()>;

    ToolbarIconButton(wchar_t glyph, std::wstring glyphFamily, std::wstring text, float width, float height)
        : glyph_(glyph), glyphFamily_(std::move(glyphFamily)), text_(std::move(text)), width_(width), height_(height),
          size_((std::min)(width, height)), glyphSize_(size_ * kToolbarGlyphFontSizeFactor)
    {
        SetWidth(width_);
        SetHeight(height_);
    }

    // 皮肤单独指定的图标字号，不跟按钮框走。
    void SetGlyphSize(float size)
    {
        glyphSize_ = size;
        InvalidateVisual();
    }

    // 皮肤给的按钮图，铺在按钮框里（等比）；空路径画回矢量图标。
    void SetImage(const std::wstring &path)
    {
        image_.reset();
        if (!path.empty())
        {
            image_ = std::make_shared<msimeui::Image>(path);
            image_->SetStretch(msimeui::ImageStretch::Uniform);
        }
        InvalidateVisual();
    }

    // 皮肤给的按钮底图（如圆底），拉满按钮框，画在按钮图或图标下面；空路径不画。
    void SetBackground(const std::wstring &path)
    {
        background_.reset();
        if (!path.empty())
        {
            background_ = std::make_shared<msimeui::Image>(path);
            background_->SetStretch(msimeui::ImageStretch::Fill);
        }
        InvalidateVisual();
    }

    void SetOnClick(ClickHandler handler)
    {
        onClick_ = std::move(handler);
    }

    void SetColors(const D2D1_COLOR_F &glyph, const D2D1_COLOR_F &hover, float hoverRadius)
    {
        glyphColor_ = glyph;
        hoverFill_ = hover;
        hoverRadius_ = hoverRadius;
        InvalidateVisual();
    }

    void SetUnderline(bool underline)
    {
        underline_ = underline;
        InvalidateVisual();
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        (void)availableSize;
        return {width_, height_};
    }

    void Arrange(const msimeui::RectF &finalRect) override
    {
        bounds_ = finalRect;
        for (const auto &layer : {background_, image_})
        {
            if (layer)
            {
                layer->Arrange(finalRect);
            }
        }
    }

    void Render(msimeui::DeviceResources &deviceResources) override
    {
        ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
        if (!target)
        {
            return;
        }
        if (background_)
        {
            background_->Render(deviceResources);
        }
        if (image_)
        {
            // 贴图按钮的悬停底色盖在图上：图通常不透明，垫在下面就看不见了。
            image_->Render(deviceResources);
            FillHover(deviceResources, target);
            return;
        }
        FillHover(deviceResources, target);

        IDWriteFactory *factory = deviceResources.GetDWriteFactory();
        ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(glyphColor_);
        if (!factory || !brush)
        {
            return;
        }

        const bool useGlyph = glyph_ != 0 && !glyphFamily_.empty();
        const std::wstring family = useGlyph ? glyphFamily_ : std::wstring(msimeui::UiFontFamily());
        // 带下划线的 "En" 比图标字形小一号，比例跟着图标字号走。
        const float fontSize =
            underline_ ? glyphSize_ * (kToolbarUnderlinedTextFontSizeFactor / kToolbarGlyphFontSizeFactor) : glyphSize_;
        IDWriteTextFormat *format =
            deviceResources.GetTextFormat(family, fontSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER,
                                          DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!format)
        {
            return;
        }

        wchar_t glyphText[2] = {glyph_, L'\0'};
        const wchar_t *drawText = useGlyph ? glyphText : text_.c_str();
        const UINT32 length = useGlyph ? 1u : static_cast<UINT32>(text_.size());
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (FAILED(factory->CreateTextLayout(drawText, length, format, bounds_.width, bounds_.height, &layout)))
        {
            return;
        }
        target->DrawTextLayout(D2D1::Point2F(bounds_.x, bounds_.y), layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);

        if (underline_ && bounds_.width > 4.0f)
        {
            const float inset = bounds_.width * 0.08f;
            const float y = bounds_.y + bounds_.height - std::max(2.0f, size_ * 0.12f);
            target->DrawLine(D2D1::Point2F(bounds_.x + inset, y), D2D1::Point2F(bounds_.x + bounds_.width - inset, y),
                             brush, std::max(1.0f, size_ * 0.06f));
        }
    }

    bool HitTest(const msimeui::PointF &point) const override
    {
        return point.x >= bounds_.x && point.x < bounds_.x + bounds_.width && point.y >= bounds_.y &&
               point.y < bounds_.y + bounds_.height;
    }

    bool OnMouseDown(const POINT &point, WPARAM keyState) override
    {
        (void)keyState;
        if (!window_)
        {
            return false;
        }
        pressed_ = HitTest(window_->ClientPixelsToDips(point));
        InvalidateVisual();
        return pressed_;
    }

    bool OnMouseUp(const POINT &point, WPARAM keyState) override
    {
        (void)keyState;
        if (!window_ || !pressed_)
        {
            return false;
        }
        const bool shouldClick = HitTest(window_->ClientPixelsToDips(point));
        pressed_ = false;
        InvalidateVisual();
        if (shouldClick && onClick_)
        {
            // A handler may tear down the visual that owns onClick_ while it runs.
            const auto handler = onClick_;
            handler();
        }
        return true;
    }

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
    void FillHover(msimeui::DeviceResources &deviceResources, ID2D1RenderTarget *target)
    {
        if (!hovered_ && !pressed_)
        {
            return;
        }
        if (ID2D1SolidColorBrush *hover = deviceResources.GetSolidColorBrush(hoverFill_))
        {
            const float radius = std::min(hoverRadius_, size_ * 0.5f);
            const auto rounded = D2D1::RoundedRect(
                D2D1::RectF(bounds_.x, bounds_.y, bounds_.x + bounds_.width, bounds_.y + bounds_.height), radius,
                radius);
            target->FillRoundedRectangle(rounded, hover);
        }
    }

    wchar_t glyph_ = 0;
    std::wstring glyphFamily_;
    std::wstring text_;
    float width_ = 24.0f;
    float height_ = 24.0f;
    // 按钮框的短边；悬停底色的圆角与下划线粗细按它算。
    float size_ = 24.0f;
    float glyphSize_ = 24.0f * kToolbarGlyphFontSizeFactor;
    bool underline_ = false;
    bool hovered_ = false;
    bool pressed_ = false;
    D2D1_COLOR_F glyphColor_ = D2D1::ColorF(0xFFFFFF);
    D2D1_COLOR_F hoverFill_ = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f);
    float hoverRadius_ = 6.0f;
    ClickHandler onClick_;
    std::shared_ptr<msimeui::Image> image_;
    std::shared_ptr<msimeui::Image> background_;
};

class ToolbarDragHandle : public msimeui::Visual
{
  public:
    ToolbarDragHandle(float width, float slotHeight, float barWidth, float barHeight, D2D1_COLOR_F color)
        : width_(width), slotHeight_(slotHeight), barWidth_(barWidth), barHeight_(barHeight), color_(color)
    {
        SetWidth(width_);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        (void)availableSize;
        return {width_, slotHeight_};
    }

    void Arrange(const msimeui::RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &deviceResources) override
    {
        ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
        ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(color_);
        if (!target || !brush)
        {
            return;
        }
        const float x = bounds_.x + (bounds_.width - barWidth_) * 0.5f;
        const float y = bounds_.y + (bounds_.height - barHeight_) * 0.5f;
        const float radius = std::max(1.0f, barWidth_ * 0.8f);
        const auto rounded = D2D1::RoundedRect(D2D1::RectF(x, y, x + barWidth_, y + barHeight_), radius, radius);
        target->FillRoundedRectangle(rounded, brush);
    }

    bool OnMouseDown(const POINT &point, WPARAM keyState) override
    {
        (void)point;
        (void)keyState;
        return false;
    }

    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, IDC_SIZEALL);
    }

  private:
    float width_ = 10.0f;
    float slotHeight_ = 35.0f;
    float barWidth_ = 2.5f;
    float barHeight_ = 14.0f;
    D2D1_COLOR_F color_ = ColorFromRgb(0x8E8CD8);
};

class ToolbarDivider : public msimeui::Visual
{
  public:
    ToolbarDivider(float width, float height, D2D1_COLOR_F color) : width_(width), height_(height), color_(color)
    {
        SetWidth(width_);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        (void)availableSize;
        return {width_, height_};
    }

    void Arrange(const msimeui::RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &deviceResources) override
    {
        ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
        ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(color_);
        if (!target || !brush || bounds_.height <= 0.0f)
        {
            return;
        }
        target->FillRectangle(D2D1::RectF(bounds_.x, bounds_.y, bounds_.x + bounds_.width, bounds_.y + bounds_.height),
                              brush);
    }

  private:
    float width_ = 1.2f;
    float height_ = 35.0f;
    D2D1_COLOR_F color_ = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f);
};

void SendWorker(UINT type)
{
    SendToTsfWorkerThreadViaNamedpipe(type, L"");
}

// 工具栏上的一个按钮：皮肤里的按钮键与当前状态名（见 CandidateSkinCatalog::ToolbarButtonStates），
// 以及没有贴图时画的图标。条形模式按顺序排成一行，图片模式按皮肤给的坐标摆。
struct ToolbarButtonSpec
{
    std::string key;
    std::string state;
    // 图标字体里的字形；为 0 时画 text。
    wchar_t codepoint = 0;
    std::wstring text;
    bool underline = false;
    ToolbarIconButton::ClickHandler click;
};

ToolbarButtonSpec GlyphSpec(std::string key, std::string state, const ToolbarIcon &icon,
                            ToolbarIconButton::ClickHandler click)
{
    return {std::move(key), std::move(state), icon.codepoint, icon.fallbackText, false, std::move(click)};
}

ToolbarButtonSpec TextSpec(std::string key, std::string state, std::wstring text, bool underline,
                           ToolbarIconButton::ClickHandler click)
{
    return {std::move(key), std::move(state), 0, std::move(text), underline, std::move(click)};
}

// 图标字体缺字时退回文字（见 ToolbarIcon）。
std::shared_ptr<ToolbarIconButton> MakeToolbarButton(const ToolbarButtonSpec &spec, float width, float height)
{
    if (spec.codepoint != 0)
    {
        const msimeui::IconGlyph resolved = msimeui::ResolveIconGlyph(spec.codepoint);
        if (resolved.family)
        {
            return std::make_shared<ToolbarIconButton>(resolved.codepoint, resolved.family, L"", width, height);
        }
    }
    auto button = std::make_shared<ToolbarIconButton>(0, L"", spec.text, width, height);
    button->SetUnderline(spec.underline);
    return button;
}
} // namespace

struct FloatingToolbarPresenter::Impl
{
    msimeui::DeviceResources resources;
    std::unique_ptr<msimeui::Window> window;
    std::shared_ptr<msimeui::Visual> root;
    std::shared_ptr<msimeui::Card> card;
    std::shared_ptr<msimeui::Container> frame;
    std::shared_ptr<msimeui::Visual> dragLimit;
    RectF captionRect = {};
    D2D1_COLOR_F fill = ColorFromRgb(0x1A1A1A);
    D2D1_COLOR_F border = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f);
    D2D1_COLOR_F glyph = ColorFromRgb(0xFFFFFF);
    D2D1_COLOR_F hover = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f);
    D2D1_COLOR_F divider = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f);
    D2D1_COLOR_F handle = ColorFromRgb(0x8E8CD8);
    float radius = 8.0f;
    float iconRadius = 6.0f;
    bool light = false;
    bool shadow = false;
    float cardHeightDip = 0.0f;
    float opacity = 1.0f;
    int cnEn = 1;
    int doubleSingleByte = 0;
    int punctuation = 1;
    int englishInputMode = 0;
    int capsLock = 0;
    int japaneseInputMode = 0;
    // 皮肤的图片模式（[toolbar] mode = "image"）：一整张背景图，按钮按皮肤的坐标摆在上面，
    // 不画底色、描边、阴影、把手与分隔线，窗口裁成背景图不透明部分的形状。
    bool imageMode = false;
    std::wstring packageRoot;
    std::wstring imagePath;
    float imageWidthDip = 0.0f;
    float imageHeightDip = 0.0f;
    // 皮肤清单，按钮怎么摆由 LayoutToolbarImageButtons 按它算。
    CandidateSkinCatalog::Package imagePackage;
    // 图片模式下摆出来的按钮：落在它们上面的点不算拖动区。
    std::vector<std::shared_ptr<ToolbarIconButton>> imageButtons;
    // v2 皮肤的动图才播放，与候选窗一致。
    bool skinAnimates = false;
    // 换了皮肤图或强制重载过，图片缓存与形状都要重读。
    std::wstring imageKey;
    // 背景图不透明部分的区域（窗口像素），以及它是按哪张图、多大算的；解码整张动图不便宜，只在这几项变了时重算。
    HRGN imageShape = nullptr;
    std::wstring shapePath;
    int shapeWidthPx = 0;
    int shapeHeightPx = 0;
    // 窗口形状：imageShape 再并上每个按钮的框。
    HRGN shape = nullptr;

    ~Impl()
    {
        for (HRGN region : {imageShape, shape})
        {
            if (region)
            {
                DeleteObject(region);
            }
        }
    }
};

FloatingToolbarPresenter::FloatingToolbarPresenter() = default;

FloatingToolbarPresenter &FloatingToolbarPresenter::Instance()
{
    static FloatingToolbarPresenter instance;
    return instance;
}

bool FloatingToolbarPresenter::IsBound() const
{
    return bound_;
}

bool FloatingToolbarPresenter::Bind(HWND hwnd)
{
    hwnd_ = hwnd;
    if (!hwnd)
    {
        bound_ = false;
        impl_.reset();
        return false;
    }
    impl_ = std::make_unique<Impl>();
    impl_->window = std::make_unique<msimeui::Window>(L"metasequoiaime_windows", L"ftb", 1, 1);
    impl_->window->AdoptExistingHwnd(hwnd);
    impl_->window->SetStealFocusOnClick(false);
    bound_ = impl_->resources.EnsureForComposition(hwnd);
    impl_->japaneseInputMode = GetActiveInputMode() == "japanese" ? 1 : 0;
    ApplyTheme();
    FTB_DIAG_LOGF(L"ftb-d2d bind hwnd={:#x} composition={}", reinterpret_cast<uintptr_t>(hwnd), bound_ ? 1 : 0);
    return bound_;
}

void FloatingToolbarPresenter::RebuildScene()
{
    if (!impl_ || !impl_->window)
    {
        return;
    }

    const float s = ToolbarUserScale();
    const float barHeight = 35.0f * s;
    const float iconSize = 26.0f * s;
    const float gap = 5.0f * s;
    const float padLeft = 8.0f * s;
    const float padRight = 4.0f * s;
    const float radius = impl_->radius * s;
    const float iconRadius = impl_->iconRadius * s;

    // 当前要显示的按钮，按工具栏上的顺序：中/英状态总在，其余跟设置页的开关走。
    std::vector<ToolbarButtonSpec> specs;
    if (impl_->capsLock == 1)
    {
        specs.push_back(TextSpec("status", "caps", L"A", false, [this]() {
            if (impl_->englishInputMode == 1)
            {
                FanyNamedPipe::EnqueueExitEnglishInputModeTask();
            }
            else
            {
                FanyNamedPipe::EnqueueToolbarLanguageToggleTask(impl_->cnEn == 1);
            }
        }));
    }
    else if (impl_->cnEn != 1)
    {
        specs.push_back(
            GlyphSpec("status", "en", kIconEn, []() { FanyNamedPipe::EnqueueToolbarLanguageToggleTask(false); }));
    }
    else if (impl_->englishInputMode == 1)
    {
        specs.push_back(TextSpec("status", "en-candidate", L"En", true,
                                 []() { FanyNamedPipe::EnqueueExitEnglishInputModeTask(); }));
    }
    else if (impl_->japaneseInputMode == 1)
    {
        specs.push_back(
            GlyphSpec("status", "ja", kIconJa, []() { FanyNamedPipe::EnqueueToolbarLanguageToggleTask(true); }));
    }
    else
    {
        specs.push_back(
            GlyphSpec("status", "cn", kIconCn, []() { FanyNamedPipe::EnqueueToolbarLanguageToggleTask(true); }));
    }

    const FloatingToolbarItemsConfig &items = GetConfiguredFloatingToolbarItems();
    if (items.fullwidth)
    {
        if (impl_->doubleSingleByte == 1)
        {
            specs.push_back(GlyphSpec("fullwidth", "full", kIconFullWidth, []() {
                SendWorker(Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToHalfwidth);
            }));
        }
        else
        {
            specs.push_back(GlyphSpec("fullwidth", "half", kIconHalfWidth, []() {
                SendWorker(Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToFullwidth);
            }));
        }
    }
    if (items.punctuation)
    {
        if (impl_->punctuation == 1)
        {
            specs.push_back(GlyphSpec("punctuation", "cn", kIconPuncCn, []() {
                SendWorker(Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToPuncEn);
            }));
        }
        else
        {
            specs.push_back(GlyphSpec("punctuation", "en", kIconPuncEn, []() {
                SendWorker(Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToPuncCn);
            }));
        }
    }
    if (items.character_set)
    {
        const bool traditional = GetConfiguredCharacterSet() == "traditional";
        specs.push_back(GlyphSpec("character_set", traditional ? "traditional" : "simplified",
                                  traditional ? kIconTraditional : kIconSimplified, []() {
                                      const std::string next =
                                          GetConfiguredCharacterSet() == "traditional" ? "simplified" : "traditional";
                                      if (!SetConfiguredCharacterSet(next))
                                          return;
                                      if (next == "traditional")
                                          ChineseConverter::PreloadInBackground();
                                      // 组字途中切换时，候选列表由任务线程按新字形换写法。
                                      FanyNamedPipe::EnqueueRefreshCandidatePageTask();
                                  }));
    }
    if (items.emoji)
    {
        specs.push_back(GlyphSpec("emoji", "", kIconEmoji, []() { OpenEmojiPanelApplication(); }));
    }
    if (items.screen_keyboard)
    {
        specs.push_back(GlyphSpec("screen_keyboard", "", kIconKeyboard, []() { OpenKeyboardPanelApplication(); }));
    }
    if (items.settings)
    {
        specs.push_back(GlyphSpec("settings", "", kIconSettings, []() { OpenSettingsApplication(); }));
    }

    impl_->imageButtons.clear();
    if (impl_->imageMode)
    {
        // 背景图铺满，按钮按 LayoutToolbarImageButtons 摆在上面（与设置页的预览同一份规则）。窗口按背景图与
        // 按钮合起来的范围开，按钮排到图外也不会被裁掉。
        auto root = std::make_shared<msimeui::OverlayPanel>();
        auto background = std::make_shared<msimeui::Image>(impl_->imagePath);
        background->SetStretch(msimeui::ImageStretch::Fill);
        background->SetWidth(impl_->imageWidthDip * s);
        background->SetHeight(impl_->imageHeightDip * s);
        root->AddChildAt(background, 0.0f, 0.0f);
        std::vector<ToolbarImageButtonRequest> shown;
        shown.reserve(specs.size());
        for (const ToolbarButtonSpec &spec : specs)
        {
            shown.push_back({spec.key, spec.state});
        }
        for (const ToolbarImageButtonSlot &slot : LayoutToolbarImageButtons(impl_->imagePackage, shown))
        {
            const auto spec = std::find_if(specs.begin(), specs.end(), [&](const ToolbarButtonSpec &candidate) {
                return candidate.key == slot.key;
            });
            auto button =
                MakeToolbarButton(*spec, static_cast<float>(slot.widthDip) * s, static_cast<float>(slot.heightDip) * s);
            button->SetColors(impl_->glyph, impl_->hover, iconRadius);
            button->SetOnClick(std::move(spec->click));
            if (slot.iconSizeDip > 0.0)
            {
                button->SetGlyphSize(static_cast<float>(slot.iconSizeDip) * s);
            }
            if (!slot.background.empty())
            {
                button->SetBackground(impl_->packageRoot + string_to_wstring(slot.background));
            }
            if (!slot.image.empty())
            {
                button->SetImage(impl_->packageRoot + string_to_wstring(slot.image));
            }
            root->AddChildAt(button, static_cast<float>(slot.xDip) * s, static_cast<float>(slot.yDip) * s);
            impl_->imageButtons.push_back(button);
        }
        impl_->card.reset();
        impl_->frame.reset();
        impl_->dragLimit.reset();
        impl_->shadow = false;
        impl_->root = root;
        auto scene = std::make_unique<msimeui::Scene>();
        scene->SetRoot(impl_->root);
        impl_->window->SetScene(std::move(scene));
        return;
    }

    auto row = std::make_shared<msimeui::HorizontalStackPanel>(gap);
    row->SetVerticalContentAlignment(msimeui::VerticalAlignment::Center);
    row->SetPadding({0.0f, 0.0f, padRight, 0.0f});
    row->SetHeight(barHeight);

    auto handle =
        std::make_shared<ToolbarDragHandle>(padLeft + 10.0f * s, barHeight, 2.5f * s, 14.0f * s, impl_->handle);
    auto divider = std::make_shared<ToolbarDivider>(1.2f * s, barHeight, impl_->divider);
    auto leading = std::make_shared<msimeui::HorizontalStackPanel>(2.0f * s);
    leading->SetVerticalContentAlignment(msimeui::VerticalAlignment::Center);
    leading->AddChild(handle);
    leading->AddChild(divider);
    impl_->dragLimit = divider;
    auto icons = std::make_shared<msimeui::HorizontalStackPanel>(gap);
    icons->SetVerticalContentAlignment(msimeui::VerticalAlignment::Center);

    for (ToolbarButtonSpec &spec : specs)
    {
        auto button = MakeToolbarButton(spec, iconSize, iconSize);
        button->SetColors(impl_->glyph, impl_->hover, iconRadius);
        button->SetOnClick(std::move(spec.click));
        icons->AddChild(button);
    }

    row->AddChild(leading);
    row->AddChild(icons);

    msimeui::Brush brush;
    brush.fill = impl_->fill;
    brush.stroke = impl_->border;
    brush.strokeWidth = kToolbarStrokeWidth;
    brush.radiusX = radius;
    brush.radiusY = radius;
    impl_->card = std::make_shared<msimeui::Card>(brush, 0.0f);
    // 与候选窗同一对柔和阴影（candidate_presenter.cpp）：light α .18/.10，dark α .34/.22。
    // 阴影边距可以伸出屏幕（ShadowInsetsPx），开着阴影工具栏本体也能贴住屏幕边缘；
    // 关掉阴影时透明边距只留描边外沿。
    const bool shadow = GetConfiguredFloatingToolbarShadow();
    impl_->shadow = shadow;
    impl_->card->SetShadowEnabled(shadow);
    if (shadow)
    {
        impl_->card->SetShadowPasses({
            {12.0f, impl_->light ? 0.18f : 0.34f, 8.0f, 10.0f},
            {4.0f, impl_->light ? 0.10f : 0.22f, 2.0f, 3.0f},
        });
    }
    impl_->card->AddChild(row);
    impl_->frame = std::make_shared<msimeui::Container>();
    if (shadow)
    {
        impl_->frame->SetPadding({kShadowPadLeft, kShadowPadTop, kShadowPadRight, kShadowPadBottom});
    }
    else
    {
        impl_->frame->SetPadding(kNoShadowPad);
    }
    impl_->frame->SetChild(impl_->card);
    auto root = std::make_shared<msimeui::StackPanel>(0.0f);
    root->AddChild(impl_->frame);
    impl_->root = root;
    auto scene = std::make_unique<msimeui::Scene>();
    scene->SetRoot(impl_->root);
    impl_->window->SetScene(std::move(scene));
}

void FloatingToolbarPresenter::ApplyTheme()
{
    if (!impl_)
    {
        return;
    }
    const bool light = ResolveConfiguredTheme(GetConfiguredThemeFtb()) == "light";
    // 跟随候选窗皮肤，外部皮肤用其 base，再叠上 [toolbar] 的覆盖。
    // 皮肤在当前候选布局/主题下不生效时整份不套用，与候选窗同一条判定。
    const std::string skinId = GetConfiguredCandidateSkin();
    const std::filesystem::path skinsRoot = std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"skins";
    std::optional<CandidateSkinCatalog::Package> package;
    std::string baseSkinId = skinId;
    if (!CandidateSkinCatalog::IsBuiltIn(skinId))
    {
        package = CandidateSkinCatalog::LoadSupported(skinsRoot, skinId, GetConfiguredCandidateWindowLayout(),
                                                      ResolveConfiguredTheme(GetConfiguredThemeCand()));
        baseSkinId = package ? package->base : "fluent";
    }
    FloatingToolbarSkin skin =
        ResolveFloatingToolbarSkin(CandidateSkinCatalog::ResolveBuiltInLook(skinsRoot, baseSkinId), light);
    if (package)
    {
        ApplyFloatingToolbarSkinOverrides(skin, *package, light);
    }
    impl_->imageMode = package && package->toolbarMode == "image";
    impl_->skinAnimates = impl_->imageMode && package->schemaVersion >= 2;
    impl_->imagePackage = {};
    std::wstring imageKey;
    if (impl_->imageMode)
    {
        impl_->packageRoot = (skinsRoot / std::filesystem::u8path(package->id)).wstring() + L"\\";
        impl_->imagePath = impl_->packageRoot + string_to_wstring(package->toolbarImage);
        impl_->imageWidthDip = static_cast<float>(package->toolbarImageWidthDip);
        impl_->imageHeightDip = static_cast<float>(package->toolbarImageHeightDip);
        impl_->imagePackage = *package;
        imageKey = impl_->imagePath + L"|" + std::to_wstring(GetCandidateSkinReloadRevision());
    }
    if (imageKey != impl_->imageKey)
    {
        // 换了皮肤图或强制重载过（设置页改了皮肤文件）：丢掉解码过的图片与形状，动画重新起算。
        impl_->imageKey = imageKey;
        StopAnimationFrames();
        impl_->resources.ClearImageCaches();
        impl_->resources.SetAnimationEpoch(msimeui::DeviceResources::AnimationClock::now());
        impl_->shapePath.clear();
    }
    impl_->fill = skin.fill;
    impl_->border = skin.border;
    impl_->glyph = skin.glyph;
    impl_->hover = skin.hover;
    impl_->divider = skin.divider;
    impl_->handle = skin.handle;
    impl_->radius = skin.radius;
    impl_->iconRadius = skin.iconRadius;
    impl_->light = light;
    ApplyAppearance();
}

void FloatingToolbarPresenter::ApplyAppearance()
{
    RebuildScene();
    RelayoutHost();
}

void FloatingToolbarPresenter::RelayoutHost(FLOAT scaleOverride, const RECT *suggestedRect)
{
    if (!bound_ || !hwnd_ || !impl_ || !impl_->root)
    {
        return;
    }
    impl_->root->InvalidateMeasure();
    // 条形高不过 240；图片模式按皮肤的图开窗口，上限与 image_height_dip 相同。
    const SizeF measured = impl_->root->MeasureInLayout({2000.0f, impl_->imageMode ? 2000.0f : 240.0f});
    impl_->root->InvalidateArrange();
    impl_->root->ArrangeInLayout({0.0f, 0.0f, measured.width, measured.height});
    ::FTB_CONTENT_WIDTH_DIP = measured.width;
    ::FTB_CONTENT_HEIGHT_DIP = measured.height;
    ::FTB_WND_WIDTH = static_cast<int>(std::ceil(measured.width));
    ::FTB_WND_HEIGHT = static_cast<int>(std::ceil(measured.height));
    impl_->cardHeightDip = impl_->card ? impl_->card->GetBounds().height : 0.0f;

    FLOAT scale = scaleOverride > 0.0f ? scaleOverride : GetWindowScale(hwnd_);
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }
    // Cap against the monitor the host is about to land on: after a DPI change
    // that is the suggested rect's monitor, not the one it is leaving.
    RECT anchor{};
    if (suggestedRect)
    {
        anchor = *suggestedRect;
    }
    else
    {
        GetWindowRect(hwnd_, &anchor);
    }
    const HalfScreenDipLimits limits = QueryHalfScreenDipLimitsForPoint({anchor.left, anchor.top});
    const float widthDip = static_cast<float>(ClampWidthDipToHalfScreen(static_cast<double>(measured.width), limits));
    const float heightDip =
        static_cast<float>(ClampHeightDipToHalfScreen(static_cast<double>(measured.height), limits));
    const int widthPx = (std::max)(1, static_cast<int>(std::ceil(widthDip * scale)));
    const int heightPx = (std::max)(1, static_cast<int>(std::ceil(heightDip * scale)));
    const RECT insets = ShadowInsetsPx(widthPx, heightPx, scale);
    const POINT pos = PlaceResizedHost(hwnd_, widthPx, heightPx, suggestedRect, insets,
                                       ::FTB_HOST_INSETS_VALID ? &::FTB_HOST_INSETS_PX : nullptr);
    SetWindowPos(hwnd_, nullptr, pos.x, pos.y, widthPx, heightPx, SWP_NOZORDER | SWP_NOACTIVATE);
    ::FTB_HOST_INSETS_PX = insets;
    ::FTB_HOST_INSETS_VALID = true;
    // 背景图画在 (0, 0) 起、皮肤声明的大小上（窗口可能因按钮排到图外而更大）；形状要和它在屏幕上的像素一一对上。
    const float userScale = ToolbarUserScale();
    UpdateShape(static_cast<int>(std::lround(impl_->imageWidthDip * userScale * scale)),
                static_cast<int>(std::lround(impl_->imageHeightDip * userScale * scale)), scale);
    ClipWindowToVisibleScreens(hwnd_, ScreenArea::WorkArea, ShapeRegion());
    impl_->resources.EnsureForComposition(hwnd_);
    impl_->captionRect = {};
    if (impl_->imageMode)
    {
        // 整张图都能拖，按钮除外（见 HitCaptionDrag）。
        impl_->captionRect = {0.0f, 0.0f, measured.width, measured.height};
    }
    else if (impl_->card && impl_->dragLimit)
    {
        const RectF card = impl_->card->GetBounds();
        const RectF limit = impl_->dragLimit->GetBounds();
        impl_->captionRect = {0.0f, card.y, limit.x + limit.width, card.height};
    }
    Present();
}

bool FloatingToolbarPresenter::HitCaptionDrag(POINT clientPoint) const
{
    if (!bound_ || !impl_ || !impl_->window || impl_->captionRect.width <= 0.0f)
    {
        return false;
    }
    const PointF dip = impl_->window->ClientPixelsToDips(clientPoint);
    // 按钮上的点要留给按钮自己：答成 HTCAPTION 的话按下就进了系统的拖动循环，按钮收不到点击。
    for (const auto &button : impl_->imageButtons)
    {
        if (button->HitTest(dip))
        {
            return false;
        }
    }
    return dip.x >= impl_->captionRect.x && dip.x < impl_->captionRect.x + impl_->captionRect.width &&
           dip.y >= impl_->captionRect.y && dip.y < impl_->captionRect.y + impl_->captionRect.height;
}

HRGN FloatingToolbarPresenter::ShapeRegion() const
{
    return bound_ && impl_ && impl_->imageMode ? impl_->shape : nullptr;
}

void FloatingToolbarPresenter::UpdateShape(int imageWidthPx, int imageHeightPx, float scale)
{
    if (!impl_->imageMode || imageWidthPx <= 0 || imageHeightPx <= 0)
    {
        return;
    }
    if (impl_->shapePath != impl_->imagePath || impl_->shapeWidthPx != imageWidthPx ||
        impl_->shapeHeightPx != imageHeightPx)
    {
        if (impl_->imageShape)
        {
            DeleteObject(impl_->imageShape);
        }
        // 窗口区域连画面一起裁，所以动图按所有帧合起来的轮廓裁，哪一帧画到的地方都留在窗口里。
        // 半透明的抗锯齿边缘也算在窗口里，边缘才不会被裁出锯齿。
        impl_->imageShape = msimeui::CreateAlphaMaskRegion(impl_->imagePath, static_cast<UINT>(imageWidthPx),
                                                           static_cast<UINT>(imageHeightPx));
        impl_->shapePath = impl_->imagePath;
        impl_->shapeWidthPx = imageWidthPx;
        impl_->shapeHeightPx = imageHeightPx;
        FTB_DIAG_LOGF(L"ftb-d2d image shape {}x{} region={}", imageWidthPx, imageHeightPx, impl_->imageShape ? 1 : 0);
    }
    if (impl_->shape)
    {
        DeleteObject(impl_->shape);
        impl_->shape = nullptr;
    }
    // 读不出图时就是矩形窗口。
    if (!impl_->imageShape)
    {
        return;
    }
    // 皮肤摆出来的按钮整个算在窗口里：落在背景图透明处的按钮也不会被裁掉、点不到。
    impl_->shape = CreateRectRgn(0, 0, 0, 0);
    CombineRgn(impl_->shape, impl_->imageShape, nullptr, RGN_COPY);
    for (const auto &button : impl_->imageButtons)
    {
        const RectF box = button->GetBounds();
        HRGN rect =
            CreateRectRgn(static_cast<int>(std::floor(box.x * scale)), static_cast<int>(std::floor(box.y * scale)),
                          static_cast<int>(std::ceil((box.x + box.width) * scale)),
                          static_cast<int>(std::ceil((box.y + box.height) * scale)));
        CombineRgn(impl_->shape, impl_->shape, rect, RGN_OR);
        DeleteObject(rect);
    }
}

RECT FloatingToolbarPresenter::ShadowInsetsPx(int hostWidthPx, int hostHeightPx, FLOAT scale) const
{
    // 图片模式没有阴影与描边；图里的透明边由窗口形状处理，不当成可以伸出屏幕的边距。
    if (!impl_ || scale <= 0.0f || impl_->imageMode)
    {
        return {};
    }
    // Where the border's outer edge lands in the host, as Present lays it out:
    // the frame stretches the card to the host width, while the root stack keeps
    // the measured height, so the rounded-up host height is spare room below.
    // Fixed pads alone left that rounding (1-3 px) between the bar and the edge.
    // Without the shadow this trims the kNoShadowPad slack the same way.
    const float padLeft = impl_->shadow ? kShadowPadLeft : kNoShadowPad;
    const float padTop = impl_->shadow ? kShadowPadTop : kNoShadowPad;
    const float padRight = impl_->shadow ? kShadowPadRight : kNoShadowPad;
    const float s = static_cast<float>(scale);
    const float half = kToolbarStrokeWidth * 0.5f;
    const float hostWidthDip = static_cast<float>(hostWidthPx) / s;
    const float left = padLeft - half;
    const float top = padTop - half;
    const float right = hostWidthDip - padRight + half;
    const float bottom = padTop + impl_->cardHeightDip + half;
    // Floor / ceil keep the antialiased edge pixel of the border on screen.
    const auto clamp = [](float px) { return (std::max)(0L, static_cast<LONG>(px)); };
    return {clamp(std::floor(left * s)), clamp(std::floor(top * s)),
            clamp(static_cast<float>(hostWidthPx) - std::ceil(right * s)),
            clamp(static_cast<float>(hostHeightPx) - std::ceil(bottom * s))};
}

void FloatingToolbarPresenter::SyncUi(int cnEn, int doubleSingleByte, int punctuation, int englishInputMode,
                                      int capsLock, int japaneseInputMode)
{
    if (!impl_)
    {
        return;
    }
    impl_->cnEn = cnEn;
    impl_->doubleSingleByte = doubleSingleByte;
    impl_->punctuation = punctuation;
    impl_->englishInputMode = englishInputMode;
    impl_->capsLock = capsLock;
    impl_->japaneseInputMode = japaneseInputMode;
    if (!bound_)
    {
        return;
    }
    ApplyAppearance();
}

void FloatingToolbarPresenter::Present()
{
    if (!bound_ || !hwnd_ || !impl_ || !impl_->window)
    {
        return;
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const UINT width = static_cast<UINT>((std::max)(1L, rc.right));
    const UINT height = static_cast<UINT>((std::max)(1L, rc.bottom));
    if (!impl_->resources.EnsureForComposition(hwnd_))
    {
        return;
    }
    impl_->resources.Resize(width, height);
    // 系统的「动画效果」开关随时可能改，每帧都重读；bar 与 v1 皮肤从不播放。
    impl_->resources.SetAnimationsEnabled(impl_->skinAnimates && SystemAnimationsEnabled());
    // Samples the animation clock for this frame and starts the next-frame deadline over.
    impl_->resources.BeginFrame();
    ID2D1RenderTarget *target = impl_->resources.GetRenderTarget();
    if (!target)
    {
        return;
    }
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0.0f));
    msimeui::Scene *scene = impl_->window->GetScene();
    if (scene && impl_->opacity > 0.0f)
    {
        const float dpi = impl_->window->GetDpi();
        scene->EnsureLayout({msimeui::PixelsToDips(static_cast<float>(width), dpi),
                             msimeui::PixelsToDips(static_cast<float>(height), dpi)});
        // A layer fades the card, its shadow and the icons as one image, the
        // way the layered-window alpha used to.
        const bool faded = impl_->opacity < 1.0f;
        if (faded)
        {
            target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                    D2D1::IdentityMatrix(), impl_->opacity),
                              nullptr);
        }
        scene->Render(impl_->resources);
        if (faded)
        {
            target->PopLayer();
        }
    }
    const HRESULT hr = target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        impl_->resources.DiscardTarget();
        return;
    }
    impl_->resources.Present();
    ScheduleAnimationFrame();
}

void FloatingToolbarPresenter::ScheduleAnimationFrame()
{
    if (!hwnd_ || !impl_ || !impl_->window)
    {
        return;
    }
    const msimeui::Scene *scene = impl_->window->GetScene();
    const auto deadline = scene ? scene->GetNextFrameDeadline() : std::nullopt;
    // 工具栏常驻：隐藏或淡出到看不见时不画，免得一直占着 CPU 和 GPU。再显示时的 Present 会接着排。
    if (!deadline || impl_->opacity <= 0.0f || !IsWindowVisible(hwnd_))
    {
        StopAnimationFrames();
        return;
    }
    const auto wait =
        std::chrono::ceil<std::chrono::milliseconds>(*deadline - msimeui::DeviceResources::AnimationClock::now());
    const UINT delay = static_cast<UINT>((std::clamp)(wait.count(), static_cast<long long>(USER_TIMER_MINIMUM),
                                                      static_cast<long long>(USER_TIMER_MAXIMUM)));
    // One shot per frame, replacing any pending one; the slack lets Windows batch the wake-up.
    SetCoalescableTimer(hwnd_, kToolbarAnimationTimerId, delay, nullptr, (std::min)(delay / 10U, 8U));
}

void FloatingToolbarPresenter::StopAnimationFrames()
{
    if (hwnd_)
    {
        KillTimer(hwnd_, kToolbarAnimationTimerId);
    }
}

void FloatingToolbarPresenter::SetOpacity(float opacity)
{
    if (!impl_)
    {
        return;
    }
    opacity = (std::clamp)(opacity, 0.0f, 1.0f);
    if (opacity == impl_->opacity)
    {
        return;
    }
    impl_->opacity = opacity;
    Present();
}

bool FloatingToolbarPresenter::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (!bound_ || !impl_ || !impl_->window)
    {
        return false;
    }
    switch (message)
    {
    case WM_ERASEBKGND:
        return true;
    case WM_TIMER:
        if (wParam != kToolbarAnimationTimerId)
        {
            return false;
        }
        // One-shot: the Present below schedules the next frame if anything still animates.
        StopAnimationFrames();
        if (IsWindowVisible(hwnd_))
        {
            Present();
        }
        return true;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Present();
        EndPaint(hwnd_, &ps);
        return true;
    }
    case WM_SIZE:
        impl_->resources.Resize(static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)));
        Present();
        return true;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MOUSELEAVE:
        impl_->window->DispatchImportedMessage(message, wParam, lParam);
        Present();
        return true;
    default:
        return false;
    }
}

FloatingToolbarPresenter::~FloatingToolbarPresenter() = default;
