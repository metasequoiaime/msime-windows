#include "settings/native/native_settings_controls.h"

#include "utils/common_utils.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Fonts.h"
#include "msimeui/Scene.h"
#include "msimeui/ScrollAxis.h"
#include "msimeui/Window.h"

#include <d2d1_1.h>
#include <dwrite_1.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace native_settings
{
namespace
{
constexpr float kUnbounded = 100000.0f;
constexpr size_t kNone = static_cast<size_t>(-1);

struct FormatEntry
{
    std::wstring family;
    float size = 0.0f;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    ComPtr<IDWriteTextFormat> format;
};

IDWriteTextFormat *GetFormat(const std::wstring &family, float size, DWRITE_FONT_WEIGHT weight)
{
    static std::vector<FormatEntry> cache;
    for (const auto &entry : cache)
    {
        if (entry.size == size && entry.weight == weight && entry.family == family)
            return entry.format.Get();
    }
    IDWriteFactory *factory = DWriteFactory();
    if (!factory)
        return nullptr;
    FormatEntry entry{family, size, weight, nullptr};
    if (FAILED(factory->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", entry.format.GetAddressOf())))
        return nullptr;
    msimeui::ApplyUiFontFallback(factory, entry.format.Get());
    cache.push_back(entry);
    return cache.back().format.Get();
}

const std::wstring &FamilyOf(const TextStyle &style)
{
    return style.family.empty() ? UiFontFamily() : style.family;
}

float DpiOf(msimeui::DeviceResources &dr)
{
    float dpiX = 96.0f;
    float dpiY = 96.0f;
    if (ID2D1RenderTarget *target = dr.GetRenderTarget())
        target->GetDpi(&dpiX, &dpiY);
    return dpiX > 0.0f ? dpiX : 96.0f;
}

// Rounds a rect to device pixels, as the browser snaps boxes before painting them.
RectF Snap(msimeui::DeviceResources &dr, const RectF &rect)
{
    const float scale = DpiOf(dr) / 96.0f;
    const float left = std::round(rect.x * scale) / scale;
    const float top = std::round(rect.y * scale) / scale;
    const float right = std::round((rect.x + rect.width) * scale) / scale;
    const float bottom = std::round((rect.y + rect.height) * scale) / scale;
    return {left, top, std::max(right - left, 0.0f), std::max(bottom - top, 0.0f)};
}

float OnePixel(msimeui::DeviceResources &dr)
{
    return 96.0f / DpiOf(dr);
}

struct LayerStack
{
    std::vector<ComPtr<ID2D1Layer>> free;
    std::vector<ComPtr<ID2D1Layer>> used;
    std::vector<D2D1_TEXT_ANTIALIAS_MODE> textModes;
    ComPtr<ID2D1RenderTarget> target;
};

LayerStack &Layers()
{
    static LayerStack stack;
    return stack;
}

std::wstring Lower(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return text;
}

bool HitSelf(msimeui::Window *window, const msimeui::Visual &visual, const POINT &point)
{
    return window && visual.HitTest(window->ClientPixelsToDips(point));
}
} // namespace

// ---------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------
IDWriteFactory *DWriteFactory()
{
    static ComPtr<IDWriteFactory> factory = [] {
        ComPtr<IDWriteFactory> created;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown **>(created.GetAddressOf()));
        return created;
    }();
    return factory.Get();
}

D2D1_RECT_F ToD2D(const RectF &rect)
{
    return D2D1::RectF(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height);
}

bool Contains(const RectF &rect, const PointF &point)
{
    return point.x >= rect.x && point.x < rect.x + rect.width && point.y >= rect.y && point.y < rect.y + rect.height;
}

RectF Inflate(const RectF &rect, float amount)
{
    return {rect.x - amount, rect.y - amount, rect.width + amount * 2.0f, rect.height + amount * 2.0f};
}

RectF Deflate(const RectF &rect, const Thickness &t)
{
    return {rect.x + t.left, rect.y + t.top, std::max(rect.width - t.left - t.right, 0.0f),
            std::max(rect.height - t.top - t.bottom, 0.0f)};
}

void FillRect(msimeui::DeviceResources &dr, const RectF &rect, const D2D1_COLOR_F &color)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    if (!target || color.a <= 0.0f)
        return;
    if (ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(color))
        target->FillRectangle(ToD2D(Snap(dr, rect)), brush);
}

void FillRounded(msimeui::DeviceResources &dr, const RectF &rect, float radius, const D2D1_COLOR_F &color)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    if (!target || color.a <= 0.0f)
        return;
    ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(color);
    if (!brush)
        return;
    const RectF snapped = Snap(dr, rect);
    if (radius <= 0.0f)
    {
        target->FillRectangle(ToD2D(snapped), brush);
        return;
    }
    const float r = std::min(radius, std::min(snapped.width, snapped.height) * 0.5f);
    target->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(snapped), r, r), brush);
}

void StrokeRounded(msimeui::DeviceResources &dr, const RectF &rect, float radius, const D2D1_COLOR_F &color,
                   float width)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    if (!target || color.a <= 0.0f || width <= 0.0f)
        return;
    ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(color);
    if (!brush)
        return;
    // Borders paint in whole device pixels, as the browser rounds them.
    width = CssBorder(width);
    const RectF snapped = Snap(dr, rect);
    const float inset = width * 0.5f;
    const RectF inner = {snapped.x + inset, snapped.y + inset, std::max(snapped.width - width, 0.0f),
                         std::max(snapped.height - width, 0.0f)};
    const float r = std::max(std::min(radius - inset, std::min(inner.width, inner.height) * 0.5f), 0.0f);
    if (r <= 0.0f)
        target->DrawRectangle(ToD2D(inner), brush, width);
    else
        target->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(inner), r, r), brush, width);
}

void DrawLine(msimeui::DeviceResources &dr, PointF from, PointF to, const D2D1_COLOR_F &color, float width)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    if (!target || color.a <= 0.0f)
        return;
    if (ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(color))
        target->DrawLine(D2D1::Point2F(from.x, from.y), D2D1::Point2F(to.x, to.y), brush, width);
}

void DrawChevron(msimeui::DeviceResources &dr, PointF center, float size, float degrees, const D2D1_COLOR_F &color,
                 float strokeWidth)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    ID2D1SolidColorBrush *brush = target ? dr.GetSolidColorBrush(color) : nullptr;
    if (!brush)
        return;
    // A size x size box whose right and bottom borders are `strokeWidth` wide, rotated
    // about its center: the two border strips are filled as rectangles.
    D2D1_MATRIX_3X2_F previous;
    target->GetTransform(&previous);
    target->SetTransform(D2D1::Matrix3x2F::Rotation(degrees, D2D1::Point2F(0.0f, 0.0f)) *
                         D2D1::Matrix3x2F::Translation(center.x, center.y) * previous);
    const float half = size * 0.5f;
    target->FillRectangle(D2D1::RectF(half - strokeWidth, -half, half, half), brush);
    target->FillRectangle(D2D1::RectF(-half, half - strokeWidth, half - strokeWidth, half), brush);
    target->SetTransform(previous);
}

void PushOpacity(msimeui::DeviceResources &dr, float opacity)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    if (!target)
        return;
    LayerStack &stack = Layers();
    if (stack.target.Get() != target)
    {
        stack.free.clear();
        stack.used.clear();
        stack.textModes.clear();
        stack.target = target;
    }
    ComPtr<ID2D1Layer> layer;
    if (!stack.free.empty())
    {
        layer = stack.free.back();
        stack.free.pop_back();
    }
    else
    {
        target->CreateLayer(nullptr, layer.GetAddressOf());
    }
    stack.used.push_back(layer);
    // ClearType text drawn into a transparent layer comes out wrong (dark text vanishes);
    // the browser also falls back to grayscale inside an opacity layer.
    stack.textModes.push_back(target->GetTextAntialiasMode());
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                            D2D1::IdentityMatrix(), opacity),
                      layer.Get());
}

ScopedTextAntialias::ScopedTextAntialias(msimeui::DeviceResources &dr, D2D1_TEXT_ANTIALIAS_MODE mode)
    : target_(dr.GetRenderTarget())
{
    if (target_)
    {
        previous_ = target_->GetTextAntialiasMode();
        target_->SetTextAntialiasMode(mode);
    }
}

ScopedTextAntialias::~ScopedTextAntialias()
{
    if (target_)
        target_->SetTextAntialiasMode(previous_);
}

void PopOpacity(msimeui::DeviceResources &dr)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    LayerStack &stack = Layers();
    if (!target || stack.used.empty())
        return;
    target->PopLayer();
    stack.free.push_back(stack.used.back());
    stack.used.pop_back();
    if (!stack.textModes.empty())
    {
        target->SetTextAntialiasMode(stack.textModes.back());
        stack.textModes.pop_back();
    }
}

void DrawSvg(msimeui::DeviceResources &dr, const std::string &markup, const RectF &box, float opacity)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    if (!target || markup.empty() || box.width <= 0.0f || box.height <= 0.0f)
        return;
    const float scale = DpiOf(dr) / 96.0f;
    D2D1_MATRIX_3X2_F transform;
    target->GetTransform(&transform);
    RectF dest = box;
    // Only a translation keeps device pixels on a grid the box can be rounded to.
    if (transform._11 == 1.0f && transform._22 == 1.0f && transform._12 == 0.0f && transform._21 == 0.0f)
    {
        const float width = std::max(std::round(box.width * scale), 1.0f) / scale;
        const float height = std::max(std::round(box.height * scale), 1.0f) / scale;
        // Keep the box centered where it was, then round its origin in device space.
        const float x = box.x + (box.width - width) * 0.5f + transform._31;
        const float y = box.y + (box.height - height) * 0.5f + transform._32;
        dest = {std::round(x * scale) / scale - transform._31, std::round(y * scale) / scale - transform._32, width,
                height};
    }
    if (ID2D1Bitmap *bitmap = dr.GetSvgBitmap(markup, {dest.width, dest.height}))
        target->DrawBitmap(bitmap, ToD2D(dest), opacity);
}

// ---------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------
void TextLayout::Set(std::wstring text, TextStyle style)
{
    text_ = std::move(text);
    style_ = std::move(style);
    layout_.Reset();
}

void TextLayout::SetText(std::wstring text)
{
    if (text == text_)
        return;
    text_ = std::move(text);
    layout_.Reset();
}

namespace
{
struct BlinkFontMetrics
{
    bool valid = false;
    float ascent = 0.0f;
    float descent = 0.0f;
    float lineGap = 0.0f;
};

// Metrics of the primary font as Blink uses them for line boxes: ascent, descent and line
// gap each rounded to whole pixels. DirectWrite's natural line height keeps the fractions,
// so text lines came out up to a pixel taller or shorter than in the page.
BlinkFontMetrics RoundedFontMetrics(const std::wstring &family, float size, DWRITE_FONT_WEIGHT weight)
{
    struct Entry
    {
        std::wstring family;
        DWRITE_FONT_WEIGHT weight;
        bool found;
        DWRITE_FONT_METRICS metrics;
    };
    static std::vector<Entry> cache;
    const Entry *entry = nullptr;
    for (const auto &item : cache)
    {
        if (item.weight == weight && item.family == family)
        {
            entry = &item;
            break;
        }
    }
    if (!entry)
    {
        Entry created{family, weight, false, {}};
        ComPtr<IDWriteFontCollection> fonts;
        UINT32 index = 0;
        BOOL exists = FALSE;
        ComPtr<IDWriteFontFamily> fontFamily;
        ComPtr<IDWriteFont> font;
        if (DWriteFactory() && SUCCEEDED(DWriteFactory()->GetSystemFontCollection(fonts.GetAddressOf())) &&
            SUCCEEDED(fonts->FindFamilyName(family.c_str(), &index, &exists)) && exists &&
            SUCCEEDED(fonts->GetFontFamily(index, fontFamily.GetAddressOf())) &&
            SUCCEEDED(fontFamily->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                       font.GetAddressOf())))
        {
            font->GetMetrics(&created.metrics);
            created.found = created.metrics.designUnitsPerEm > 0;
        }
        cache.push_back(created);
        entry = &cache.back();
    }
    BlinkFontMetrics result;
    if (!entry->found)
        return result;
    const float scale = size / static_cast<float>(entry->metrics.designUnitsPerEm);
    result.valid = true;
    result.ascent = std::round(static_cast<float>(entry->metrics.ascent) * scale);
    result.descent = std::round(static_cast<float>(entry->metrics.descent) * scale);
    result.lineGap = std::round(static_cast<float>(entry->metrics.lineGap) * scale);
    return result;
}
} // namespace

IDWriteTextLayout *TextLayout::Get(float maxWidth)
{
    const bool fixedWidth = style_.wrap || style_.align != DWRITE_TEXT_ALIGNMENT_LEADING;
    const float width = fixedWidth ? std::max(maxWidth, 1.0f) : kUnbounded;
    if (layout_ && width_ == width)
        return layout_.Get();
    layout_.Reset();
    width_ = width;
    IDWriteTextFormat *format = GetFormat(FamilyOf(style_), style_.size, style_.weight);
    IDWriteFactory *factory = DWriteFactory();
    if (!format || !factory ||
        FAILED(factory->CreateTextLayout(text_.c_str(), static_cast<UINT32>(text_.size()), format, width, kUnbounded,
                                         layout_.GetAddressOf())))
        return nullptr;
    layout_->SetWordWrapping(style_.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    layout_->SetTextAlignment(style_.align);
    if (style_.letterSpacing != 0.0f)
    {
        ComPtr<IDWriteTextLayout1> layout1;
        if (SUCCEEDED(layout_.As(&layout1)))
            layout1->SetCharacterSpacing(0.0f, style_.letterSpacing, 0.0f,
                                         DWRITE_TEXT_RANGE{0, static_cast<UINT32>(text_.size())});
    }
    // CSS `text-spacing-trim: normal` (trim-adjacent), which Chrome applies: a full-width
    // closing mark followed by another punctuation mark loses the blank half after it, and
    // an opening mark after punctuation loses the blank half before it, so "）。" and
    // "」，" set tight.
    {
        static const std::wstring kClosing = L"、。，．：；！？）」』】〕〉》\x201D\x2019";
        static const std::wstring kOpening = L"（「『【〔〈《\x201C\x2018";
        const auto isClosing = [](wchar_t c) { return kClosing.find(c) != std::wstring::npos; };
        const auto isOpening = [](wchar_t c) { return kOpening.find(c) != std::wstring::npos; };
        ComPtr<IDWriteTextLayout1> layout1;
        const float half = style_.size * 0.5f;
        const bool spacing = SUCCEEDED(layout_.As(&layout1));
        for (size_t i = 0; spacing && i < text_.size(); ++i)
        {
            const wchar_t c = text_[i];
            float leading = 0.0f;
            float trailing = style_.letterSpacing;
            // Only closing brackets carry their blank on the end; 、。， already sit left.
            if (isClosing(c) && i + 1 < text_.size() && (isClosing(text_[i + 1]) || isOpening(text_[i + 1])) &&
                kClosing.find(c) >= 8)
                trailing -= half;
            if (isOpening(c) && i > 0 && (isClosing(text_[i - 1]) || isOpening(text_[i - 1])))
                leading -= half;
            if (leading != 0.0f || trailing != style_.letterSpacing)
                layout1->SetCharacterSpacing(leading, trailing, 0.0f, DWRITE_TEXT_RANGE{static_cast<UINT32>(i), 1});
        }
    }
    // Lines are as tall as the browser makes them: line-height normal is the font's rounded
    // ascent, descent and gap; an explicit line-height splits its extra space above and below.
    const BlinkFontMetrics font = RoundedFontMetrics(FamilyOf(style_), style_.size, style_.weight);
    if (font.valid)
    {
        const float content = font.ascent + font.descent;
        const float spacing = style_.lineHeight > 0.0f ? style_.size * style_.lineHeight : content + font.lineGap;
        layout_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, spacing, font.ascent + (spacing - content) * 0.5f);
    }
    return layout_.Get();
}

SizeF TextLayout::Measure(float maxWidth)
{
    IDWriteTextLayout *layout = Get(maxWidth);
    if (!layout)
        return {};
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    float width = metrics.widthIncludingTrailingWhitespace + style_.stroke;
    if (style_.wrap || style_.align != DWRITE_TEXT_ALIGNMENT_LEADING)
        width = std::min(width, std::max(maxWidth, 0.0f));
    float height = metrics.height;
    if (text_.empty())
        height = LineHeight(style_);
    return {std::ceil(width), height};
}

void TextLayout::Draw(msimeui::DeviceResources &dr, PointF origin, const D2D1_COLOR_F &color, float maxWidth)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    IDWriteTextLayout *layout = Get(maxWidth);
    ID2D1SolidColorBrush *brush = target ? dr.GetSolidColorBrush(color) : nullptr;
    if (!layout || !brush || color.a <= 0.0f)
        return;
    const D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT;
    target->DrawTextLayout(D2D1::Point2F(origin.x, origin.y), layout, brush, options);
    if (style_.stroke > 0.0f)
        target->DrawTextLayout(D2D1::Point2F(origin.x + style_.stroke, origin.y), layout, brush, options);
}

float MeasureTextWidth(const std::wstring &text, const TextStyle &style)
{
    TextLayout layout;
    TextStyle single = style;
    single.wrap = false;
    single.align = DWRITE_TEXT_ALIGNMENT_LEADING;
    layout.Set(text, single);
    return layout.Measure(kUnbounded).width;
}

float LineHeight(const TextStyle &style)
{
    if (style.lineHeight > 0.0f)
        return style.size * style.lineHeight;
    if (const BlinkFontMetrics font = RoundedFontMetrics(FamilyOf(style), style.size, style.weight); font.valid)
        return font.ascent + font.descent + font.lineGap;
    struct Entry
    {
        std::wstring family;
        float size;
        DWRITE_FONT_WEIGHT weight;
        float height;
    };
    static std::vector<Entry> cache;
    const std::wstring &family = FamilyOf(style);
    for (const auto &entry : cache)
    {
        if (entry.size == style.size && entry.weight == style.weight && entry.family == family)
            return entry.height;
    }
    TextLayout probe;
    TextStyle single = style;
    single.wrap = false;
    probe.Set(L"水Ag", single);
    float height = style.size * 1.32f;
    if (IDWriteTextLayout *layout = probe.Get(kUnbounded))
    {
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        height = metrics.height;
    }
    cache.push_back({family, style.size, style.weight, height});
    return height;
}

void DrawTextInRect(msimeui::DeviceResources &dr, const std::wstring &text, const TextStyle &style, const RectF &rect,
                    const D2D1_COLOR_F &color, bool center)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    if (!target || text.empty())
        return;
    TextLayout layout;
    TextStyle single = style;
    single.wrap = false;
    single.align = DWRITE_TEXT_ALIGNMENT_LEADING;
    layout.Set(text, single);
    const SizeF size = layout.Measure(kUnbounded);
    const float x = center ? rect.x + (rect.width - size.width) * 0.5f : rect.x;
    const float y = rect.y + (rect.height - size.height) * 0.5f;
    target->PushAxisAlignedClip(ToD2D(rect), D2D1_ANTIALIAS_MODE_ALIASED);
    layout.Draw(dr, {x, y}, color, kUnbounded);
    target->PopAxisAlignedClip();
}

ColorTransition::ColorTransition(double durationMs, msimeui::Easing easing) : durationMs_(durationMs), easing_(easing)
{
}

void ColorTransition::SetTarget(const D2D1_COLOR_F &target, msimeui::Window *window)
{
    if (!initialized_)
    {
        from_ = to_ = target;
        initialized_ = true;
        return;
    }
    if (target.r == to_.r && target.g == to_.g && target.b == to_.b && target.a == to_.a)
        return;
    const double now = window ? window->GetFrameTime() : msimeui::NowMs();
    from_ = msimeui::LerpColor(from_, to_, progress_.Value(now));
    to_ = target;
    progress_.Start(0.0f, 1.0f, durationMs_, easing_, now);
    if (window)
        window->RequestAnimationFrame();
}

D2D1_COLOR_F ColorTransition::Value(msimeui::Window *window)
{
    const double now = window ? window->GetFrameTime() : msimeui::NowMs();
    if (progress_.IsRunning(now) && window)
        window->RequestAnimationFrame();
    return msimeui::LerpColor(from_, to_, progress_.Value(now));
}

D2D1_COLOR_F RoleColor(TextRole role)
{
    const Palette &p = CurrentPalette();
    switch (role)
    {
    case TextRole::Secondary:
        return p.textSecondary;
    case TextRole::Muted:
        return p.textMuted;
    case TextRole::Subtle:
        return p.textSubtle;
    case TextRole::Sidebar:
        return p.sidebarItem;
    case TextRole::Accent:
        return p.accent;
    case TextRole::Danger:
        return p.dangerText;
    case TextRole::Primary:
    case TextRole::Custom:
    default:
        return p.text;
    }
}

Label::Label(std::wstring text, TextStyle style, TextRole role) : role_(role)
{
    layout_.Set(std::move(text), std::move(style));
}

void Label::SetText(std::wstring text)
{
    if (text == layout_.Text())
        return;
    layout_.SetText(std::move(text));
    InvalidateMeasure();
    InvalidateVisual();
}

const std::wstring &Label::GetText() const
{
    return layout_.Text();
}

void Label::SetRole(TextRole role)
{
    role_ = role;
    InvalidateVisual();
}

void Label::SetColor(D2D1_COLOR_F color)
{
    role_ = TextRole::Custom;
    color_ = color;
    InvalidateVisual();
}

void Label::SetStyle(TextStyle style)
{
    layout_.Set(layout_.Text(), std::move(style));
    InvalidateMeasure();
}

void Label::SetOpacity(float opacity)
{
    opacity_ = opacity;
    InvalidateVisual();
}

void Label::SetOnClick(std::function<void()> handler)
{
    onClick_ = std::move(handler);
}

void Label::SetHoverColor(D2D1_COLOR_F color)
{
    hoverColor_ = color;
}

SizeF Label::Measure(const SizeF &availableSize)
{
    const SizeF size = layout_.Measure(availableSize.width);
    return {std::min(size.width, availableSize.width), size.height};
}

void Label::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void Label::Render(msimeui::DeviceResources &dr)
{
    D2D1_COLOR_F color = role_ == TextRole::Custom ? color_ : RoleColor(role_);
    if (hovered_ && hoverColor_.a > 0.0f)
        color = hoverColor_;
    color.a *= opacity_;
    layout_.Draw(dr, {bounds_.x, bounds_.y}, color, bounds_.width);
}

bool Label::OnMouseDown(const POINT &point, WPARAM)
{
    pressed_ = onClick_ && HitSelf(window_, *this, point);
    return pressed_;
}

bool Label::OnMouseUp(const POINT &point, WPARAM)
{
    if (!pressed_)
        return false;
    pressed_ = false;
    if (HitSelf(window_, *this, point) && onClick_)
    {
        const auto handler = onClick_;
        handler();
    }
    return true;
}

void Label::OnMouseEnter()
{
    hovered_ = true;
    if (hoverColor_.a > 0.0f)
        InvalidateVisual();
}

void Label::OnMouseLeave()
{
    hovered_ = false;
    pressed_ = false;
    if (hoverColor_.a > 0.0f)
        InvalidateVisual();
}

HCURSOR Label::GetCursor() const
{
    return LoadCursor(nullptr, onClick_ ? IDC_HAND : IDC_ARROW);
}

TextStyle PageTitleStyle()
{
    TextStyle style;
    style.size = kFontPageTitle;
    style.weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
    return style;
}

TextStyle SectionTitleStyle()
{
    TextStyle style;
    style.size = kFontSectionTitle;
    style.weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
    return style;
}

TextStyle SubItemTitleStyle()
{
    TextStyle style;
    style.size = kFontBody;
    style.stroke = 0.25f;
    return style;
}

TextStyle BodyStyle()
{
    TextStyle style;
    style.size = kFontBody;
    return style;
}

TextStyle ControlStyle()
{
    TextStyle style;
    style.size = kFontBody;
    style.family = SystemFontFamily();
    return style;
}

TextStyle CaptionStyle()
{
    TextStyle style;
    style.size = kFontCaption;
    style.lineHeight = kLineHeightCaption;
    style.wrap = true;
    return style;
}

std::shared_ptr<Label> MakeTitle(std::wstring text)
{
    return std::make_shared<Label>(std::move(text), SectionTitleStyle());
}

std::shared_ptr<Label> MakeSubTitle(std::wstring text)
{
    return std::make_shared<Label>(std::move(text), SubItemTitleStyle());
}

std::shared_ptr<Label> MakeDescription(std::wstring text)
{
    auto label = std::make_shared<Label>(std::move(text), CaptionStyle(), TextRole::Muted);
    label->SetMargin({0.0f, 4.0f, 0.0f, 0.0f});
    return label;
}

// ---------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------
Column::Column(float gap) : msimeui::StackPanel(gap)
{
}

Column &Column::Add(VisualPtr child)
{
    AddChild(std::move(child));
    return *this;
}

Row::Row(float gap) : gap_(gap)
{
}

Row &Row::Add(VisualPtr child, float grow)
{
    grow_.push_back(grow);
    AddChild(std::move(child));
    return *this;
}

void Row::SetAlign(Align align)
{
    align_ = align;
    InvalidateArrange();
}

void Row::SetJustify(Justify justify)
{
    justify_ = justify;
    InvalidateArrange();
}

void Row::SetWrap(bool wrap, float rowGap)
{
    wrap_ = wrap;
    rowGap_ = rowGap;
    InvalidateMeasure();
}

SizeF Row::Measure(const SizeF &availableSize)
{
    const SizeF inner = {std::max(availableSize.width - padding_.left - padding_.right, 0.0f),
                         std::max(availableSize.height - padding_.top - padding_.bottom, 0.0f)};
    sizes_.assign(children_.size(), SizeF{});
    lines_.clear();

    // Fixed children first, at their natural width; growing ones then share the rest.
    float fixedWidth = 0.0f;
    float totalGrow = 0.0f;
    size_t visibleCount = 0;
    for (size_t i = 0; i < children_.size(); ++i)
    {
        const float grow = i < grow_.size() ? grow_[i] : 0.0f;
        if (grow > 0.0f && !wrap_)
        {
            totalGrow += grow;
            continue;
        }
        sizes_[i] = children_[i]->MeasureInLayout(
            {wrap_ ? inner.width : std::max(inner.width - fixedWidth, 0.0f), inner.height});
        fixedWidth += sizes_[i].width;
        if (sizes_[i].width > 0.0f || sizes_[i].height > 0.0f)
            ++visibleCount;
    }
    if (!wrap_)
    {
        size_t growCount = 0;
        for (float g : grow_)
            growCount += g > 0.0f ? 1 : 0;
        const size_t gaps = visibleCount + growCount > 0 ? visibleCount + growCount - 1 : 0;
        const float remaining = std::max(inner.width - fixedWidth - gap_ * static_cast<float>(gaps), 0.0f);
        for (size_t i = 0; i < children_.size(); ++i)
        {
            const float grow = i < grow_.size() ? grow_[i] : 0.0f;
            if (grow <= 0.0f)
                continue;
            const SizeF measured = children_[i]->MeasureInLayout({remaining * grow / totalGrow, inner.height});
            sizes_[i] = {remaining * grow / totalGrow, measured.height};
        }
    }

    // Break into lines (a single line unless wrapping).
    Line line;
    for (size_t i = 0; i < children_.size(); ++i)
    {
        const SizeF &size = sizes_[i];
        const bool empty = size.width <= 0.0f && size.height <= 0.0f;
        const float add = empty ? 0.0f : size.width + (line.end > line.begin && line.width > 0.0f ? gap_ : 0.0f);
        if (wrap_ && line.end > line.begin && line.width + add > inner.width + 0.5f)
        {
            lines_.push_back(line);
            line = Line{i, i, 0.0f, 0.0f};
        }
        line.width += empty ? 0.0f : size.width + (line.width > 0.0f ? gap_ : 0.0f);
        line.height = std::max(line.height, size.height);
        line.end = i + 1;
    }
    lines_.push_back(line);

    float width = 0.0f;
    float height = 0.0f;
    for (size_t i = 0; i < lines_.size(); ++i)
    {
        width = std::max(width, lines_[i].width);
        height += lines_[i].height + (i > 0 ? rowGap_ : 0.0f);
    }
    if (totalGrow > 0.0f && !wrap_)
        width = inner.width;
    return {width + padding_.left + padding_.right, height + padding_.top + padding_.bottom};
}

void Row::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    const RectF inner = Deflate(finalRect, padding_);
    float y = inner.y;
    if (lines_.size() == 1 && align_ != Align::Start)
    {
        // A single line uses the whole cross size, so `align-items` applies to it.
        lines_[0].height = std::max(lines_[0].height, inner.height);
    }
    for (const Line &line : lines_)
    {
        size_t visible = 0;
        for (size_t i = line.begin; i < line.end; ++i)
            visible += (sizes_[i].width > 0.0f || sizes_[i].height > 0.0f) ? 1 : 0;
        float x = inner.x;
        float gap = gap_;
        const float free = std::max(inner.width - line.width, 0.0f);
        if (justify_ == Justify::Center)
            x += free * 0.5f;
        else if (justify_ == Justify::End)
            x += free;
        else if (justify_ == Justify::SpaceBetween && visible > 1)
            gap += free / static_cast<float>(visible - 1);
        bool first = true;
        for (size_t i = line.begin; i < line.end; ++i)
        {
            const SizeF size = sizes_[i];
            const bool empty = size.width <= 0.0f && size.height <= 0.0f;
            if (!empty && !first)
                x += gap;
            float childY = y;
            float childHeight = size.height;
            switch (align_)
            {
            case Align::Center:
                childY = y + (line.height - size.height) * 0.5f;
                break;
            case Align::End:
                childY = y + line.height - size.height;
                break;
            case Align::Stretch:
                childHeight = line.height;
                break;
            case Align::Start:
            default:
                break;
            }
            children_[i]->ArrangeInLayout({x, childY, size.width, childHeight});
            if (!empty)
            {
                x += size.width;
                first = false;
            }
        }
        y += line.height + rowGap_;
    }
}

void Row::Render(msimeui::DeviceResources &dr)
{
    for (const auto &child : children_)
    {
        const RectF &b = child->GetBounds();
        if (dr.IsVisible(Inflate(b, 24.0f)))
            child->Render(dr);
    }
}

CenteredColumn::CenteredColumn(VisualPtr child, float maxWidth)
    : msimeui::Container(std::move(child)), maxWidth_(maxWidth)
{
}

SizeF CenteredColumn::Measure(const SizeF &availableSize)
{
    const float innerWidth = std::max(0.0f, std::min(availableSize.width - padding_.left - padding_.right, maxWidth_));
    const float innerHeight = std::max(0.0f, availableSize.height - padding_.top - padding_.bottom);
    const SizeF childSize = child_ ? child_->MeasureInLayout({innerWidth, innerHeight}) : SizeF{};
    return {availableSize.width, childSize.height + padding_.top + padding_.bottom};
}

void CenteredColumn::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    if (!child_)
        return;
    const float available = std::max(0.0f, finalRect.width - padding_.left - padding_.right);
    const float width = std::min(available, maxWidth_);
    const float x = finalRect.x + padding_.left + (available - width) * 0.5f;
    child_->ArrangeInLayout(
        {x, finalRect.y + padding_.top, width, std::max(0.0f, finalRect.height - padding_.top - padding_.bottom)});
}

Hideable::Hideable(VisualPtr child, bool visible) : msimeui::Container(std::move(child)), visible_(visible)
{
}

void Hideable::SetVisible(bool visible)
{
    if (visible_ == visible)
        return;
    visible_ = visible;
    InvalidateMeasure();
    InvalidateVisual();
}

SizeF Hideable::Measure(const SizeF &availableSize)
{
    // MeasureInLayout already took a margin set since the last pass off `availableSize`
    // and adds margin_ to the result after this returns: move it into spacing_ now.
    SizeF available = availableSize;
    if (margin_.left != 0.0f || margin_.top != 0.0f || margin_.right != 0.0f || margin_.bottom != 0.0f)
    {
        available.width += margin_.left + margin_.right;
        available.height += margin_.top + margin_.bottom;
        spacing_ = {spacing_.left + margin_.left, spacing_.top + margin_.top, spacing_.right + margin_.right,
                    spacing_.bottom + margin_.bottom};
        margin_ = {};
    }
    if (!visible_ || !child_)
        return {};
    const Thickness inset = {padding_.left + spacing_.left, padding_.top + spacing_.top,
                             padding_.right + spacing_.right, padding_.bottom + spacing_.bottom};
    const SizeF childSize = child_->MeasureInLayout({std::max(available.width - inset.left - inset.right, 0.0f),
                                                     std::max(available.height - inset.top - inset.bottom, 0.0f)});
    return {childSize.width + inset.left + inset.right, childSize.height + inset.top + inset.bottom};
}

void Hideable::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    // A hidden child was never measured; arranging it would read measurements it does not have.
    if (child_ && visible_)
        child_->ArrangeInLayout(
            Deflate(finalRect, {padding_.left + spacing_.left, padding_.top + spacing_.top,
                                padding_.right + spacing_.right, padding_.bottom + spacing_.bottom}));
}

void Hideable::Render(msimeui::DeviceResources &dr)
{
    if (visible_)
        msimeui::Container::Render(dr);
}

msimeui::Visual *Hideable::FindVisualAt(const PointF &point)
{
    return visible_ ? msimeui::Container::FindVisualAt(point) : nullptr;
}

msimeui::Visual *Hideable::FindFocusableAt(const PointF &point)
{
    return visible_ ? msimeui::Container::FindFocusableAt(point) : nullptr;
}

msimeui::Visual *Hideable::FindFirstFocusableDescendant()
{
    return visible_ ? msimeui::Container::FindFirstFocusableDescendant() : nullptr;
}

Dimmable::Dimmable(VisualPtr child, float disabledOpacity)
    : msimeui::Container(std::move(child)), opacity_(disabledOpacity)
{
}

void Dimmable::SetDisabled(bool disabled)
{
    if (disabled_ == disabled)
        return;
    disabled_ = disabled;
    InvalidateVisual();
}

void Dimmable::Render(msimeui::DeviceResources &dr)
{
    if (!disabled_)
    {
        msimeui::Container::Render(dr);
        return;
    }
    PushOpacity(dr, opacity_);
    msimeui::Container::Render(dr);
    PopOpacity(dr);
}

msimeui::Visual *Dimmable::FindVisualAt(const PointF &point)
{
    if (!disabled_)
        return msimeui::Container::FindVisualAt(point);
    return HitTest(point) ? this : nullptr;
}

msimeui::Visual *Dimmable::FindFocusableAt(const PointF &point)
{
    return disabled_ ? nullptr : msimeui::Container::FindFocusableAt(point);
}

Spacer::Spacer(float width, float height) : width_(width), height_(height)
{
}

SizeF Spacer::Measure(const SizeF &availableSize)
{
    return {std::min(width_, availableSize.width), height_};
}

void Spacer::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void Spacer::Render(msimeui::DeviceResources &)
{
}

// ---------------------------------------------------------------------------------
// Page structure
// ---------------------------------------------------------------------------------
Section::Section(VisualPtr child) : msimeui::Container(std::move(child))
{
}

SizeF Section::Measure(const SizeF &availableSize)
{
    const float border = CssBorder();
    padding_ = {24.0f + border, 20.0f + border, 24.0f + border, 20.0f + border};
    return msimeui::Container::Measure(availableSize);
}

void Section::Arrange(const RectF &finalRect)
{
    msimeui::Container::Arrange(finalRect);
}

void Section::Render(msimeui::DeviceResources &dr)
{
    const Palette &p = CurrentPalette();
    constexpr float kRadius = 8.0f;
    const RectF box = Snap(dr, bounds_);
    msimeui::ShadowPass passes[2] = {};
    for (int i = 0; i < p.cardShadowCount; ++i)
        passes[i] = {p.cardShadow[i].sigma, p.cardShadow[i].alpha, 0.0f, p.cardShadow[i].offsetY};
    shadow_.Render(dr.GetRenderTarget(), box, kRadius, passes, static_cast<size_t>(p.cardShadowCount));
    // The card is translucent in the dark theme; it always sits on the window
    // background, so paint the composited color and the shadow cannot show through.
    FillRounded(dr, box, kRadius, Over(p.cardBg, p.chromeBg));
    StrokeRounded(dr, box, kRadius, p.border, 1.0f);
    msimeui::Container::Render(dr);
}

Divider::Divider(Kind kind) : kind_(kind)
{
    if (kind_ == Kind::Section)
        SetMargin({0.0f, 12.0f, 0.0f, 12.0f});
}

SizeF Divider::Measure(const SizeF &availableSize)
{
    return {availableSize.width, kind_ == Kind::Section ? 0.5f : 0.8f};
}

void Divider::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void Divider::Render(msimeui::DeviceResources &dr)
{
    // Sub-pixel heights paint as one device pixel after the browser snaps them.
    const float pixel = OnePixel(dr);
    const float scale = DpiOf(dr) / 96.0f;
    const float top = std::round(bounds_.y * scale) / scale;
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    if (ID2D1SolidColorBrush *brush = target ? dr.GetSolidColorBrush(CurrentPalette().divider) : nullptr)
        target->FillRectangle(D2D1::RectF(bounds_.x, top, bounds_.x + bounds_.width, top + pixel), brush);
}

SettingRow::SettingRow(VisualPtr left, VisualPtr control) : left_(std::move(left)), control_(std::move(control))
{
    if (left_)
        AddChild(left_);
    if (control_)
        AddChild(control_);
}

void SettingRow::SetSearchText(std::wstring title, std::wstring description)
{
    searchTitle_ = std::move(title);
    searchDescription_ = std::move(description);
}

SizeF SettingRow::Measure(const SizeF &availableSize)
{
    constexpr float kGap = 10.0f;
    controlSize_ = control_ ? control_->MeasureInLayout(availableSize) : SizeF{};
    const bool hasControl = control_ && (controlSize_.width > 0.0f || controlSize_.height > 0.0f);
    const float leftWidth = std::max(0.0f, availableSize.width - (hasControl ? controlSize_.width + kGap : 0.0f));
    leftSize_ = left_ ? left_->MeasureInLayout({leftWidth, availableSize.height}) : SizeF{};
    return {availableSize.width, std::max(leftSize_.height, controlSize_.height)};
}

void SettingRow::Arrange(const RectF &finalRect)
{
    constexpr float kGap = 10.0f;
    bounds_ = finalRect;
    const bool hasControl = control_ && (controlSize_.width > 0.0f || controlSize_.height > 0.0f);
    if (control_)
    {
        control_->ArrangeInLayout({finalRect.x + finalRect.width - controlSize_.width,
                                   finalRect.y + (finalRect.height - controlSize_.height) * 0.5f, controlSize_.width,
                                   controlSize_.height});
    }
    if (left_)
    {
        const float leftWidth = std::max(0.0f, finalRect.width - (hasControl ? controlSize_.width + kGap : 0.0f));
        left_->ArrangeInLayout(
            {finalRect.x, finalRect.y + (finalRect.height - leftSize_.height) * 0.5f, leftWidth, leftSize_.height});
    }
}

void SettingRow::Render(msimeui::DeviceResources &dr)
{
    for (const auto &child : children_)
        child->Render(dr);
}

VisualPtr MakeTitleBlock(std::wstring title, std::wstring description, bool subItem)
{
    auto column = std::make_shared<Column>(0.0f);
    column->Add(subItem ? MakeSubTitle(std::move(title)) : MakeTitle(std::move(title)));
    if (!description.empty())
    {
        auto desc = MakeDescription(std::move(description));
        if (subItem)
            desc->SetMargin({0.0f, 0.0f, 0.0f, 0.0f});
        column->Add(desc);
    }
    return column;
}

namespace
{
// Innermost SettingsIndexScope, or empty when no page is being indexed.
struct RowCollectorState
{
    std::string pageId;
    std::function<void(const SettingsIndexScope::Row &)> collector;
};

thread_local std::vector<RowCollectorState> g_rowCollectorStack;
} // namespace

std::shared_ptr<SettingRow> MakeRow(std::wstring title, std::wstring description, VisualPtr control, bool subItem)
{
    auto row = std::make_shared<SettingRow>(MakeTitleBlock(title, description, subItem), std::move(control));
    // Search text is kept on the row because the index is built from it later; the labels above
    // only own their own copies.
    row->SetSearchText(std::move(title), std::move(description));
    if (!g_rowCollectorStack.empty() && g_rowCollectorStack.back().collector)
        g_rowCollectorStack.back().collector({row->SearchTitle(), row->SearchDescription(), row});
    return row;
}

SettingsIndexScope::SettingsIndexScope(std::string pageId, std::function<void(const Row &)> collector)
{
    g_rowCollectorStack.push_back({std::move(pageId), std::move(collector)});
}

SettingsIndexScope::~SettingsIndexScope()
{
    if (!g_rowCollectorStack.empty())
        g_rowCollectorStack.pop_back();
}

PageBuilder::PageBuilder(std::wstring title) : column_(std::make_shared<Column>(0.0f))
{
    // `.content` of the partial: padding 12px, margin-top 2px.
    column_->SetPadding({12.0f, 14.0f, 12.0f, 12.0f});
    // `.content-header`: padding 0 24px 12px 0.5em (8px at the inherited 16px), margin-bottom 8px.
    header_ = std::make_shared<Label>(std::move(title), PageTitleStyle());
    header_->SetMargin({8.0f, 0.0f, 24.0f, 20.0f});
    column_->Add(header_);
}

void PageBuilder::SetTitleRole(TextRole role)
{
    header_->SetRole(role);
}

std::shared_ptr<Section> PageBuilder::AddSection(VisualPtr content)
{
    auto section = std::make_shared<Section>(std::move(content));
    section->SetMargin({0.0f, 0.0f, 0.0f, 18.0f});
    column_->Add(section);
    return section;
}

void PageBuilder::AddRaw(VisualPtr visual, float marginBottom)
{
    visual->SetMargin({0.0f, 0.0f, 0.0f, marginBottom});
    column_->Add(std::move(visual));
}

VisualPtr PageBuilder::Build()
{
    return column_;
}

std::shared_ptr<Column> MakeRows(const std::vector<VisualPtr> &rows)
{
    auto column = std::make_shared<Column>(0.0f);
    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (i > 0)
            column->Add(std::make_shared<Divider>());
        column->Add(rows[i]);
    }
    return column;
}

// ---------------------------------------------------------------------------------
// ToggleSwitch
// ---------------------------------------------------------------------------------
ToggleSwitch::ToggleSwitch(bool on) : on_(on), knob_(on ? 1.0f : 0.0f)
{
}

void ToggleSwitch::SetOnChanged(ChangeHandler handler)
{
    onChanged_ = std::move(handler);
}

void ToggleSwitch::SetChecked(bool on)
{
    if (on_ == on)
        return;
    on_ = on;
    knob_.AnimateTo(on ? 1.0f : 0.0f, 200.0, msimeui::Easing::Ease(), msimeui::NowMs());
    if (window_)
        window_->RequestAnimationFrame();
    InvalidateVisual();
}

bool ToggleSwitch::IsChecked() const
{
    return on_;
}

void ToggleSwitch::SetDisabled(bool disabled)
{
    if (disabled_ == disabled)
        return;
    disabled_ = disabled;
    InvalidateVisual();
}

SizeF ToggleSwitch::Measure(const SizeF &)
{
    return {38.0f, 19.0f};
}

void ToggleSwitch::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void ToggleSwitch::Render(msimeui::DeviceResources &dr)
{
    const Palette &p = CurrentPalette();
    const double now = window_ ? window_->GetFrameTime() : msimeui::NowMs();
    const float t = knob_.Value(now);
    if (knob_.IsRunning(now) && window_)
        window_->RequestAnimationFrame();

    if (disabled_)
        PushOpacity(dr, 0.45f);
    const RectF track = {bounds_.x, bounds_.y + (bounds_.height - 19.0f) * 0.5f, 38.0f, 19.0f};
    FillRounded(dr, track, 9.0f, msimeui::LerpColor(p.toggleOff, p.toggleOn, t));

    ID2D1RenderTarget *target = dr.GetRenderTarget();
    const float cx = track.x + 1.0f + 8.0f + 20.0f * t;
    const float cy = track.y + track.height * 0.5f;
    // box-shadow: 0 1px 2px rgba(0, 0, 0, 0.3)
    if (ID2D1SolidColorBrush *shadow = dr.GetSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.12f)))
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + 1.0f), 8.8f, 8.8f), shadow);
    if (ID2D1SolidColorBrush *shadow = dr.GetSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.18f)))
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + 1.0f), 8.2f, 8.2f), shadow);
    if (ID2D1SolidColorBrush *knob = dr.GetSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)))
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.0f, 8.0f), knob);
    if (disabled_)
        PopOpacity(dr);
}

bool ToggleSwitch::IsFocusable() const
{
    return !disabled_;
}

bool ToggleSwitch::OnMouseDown(const POINT &point, WPARAM)
{
    pressed_ = !disabled_ && HitSelf(window_, *this, point);
    return pressed_;
}

bool ToggleSwitch::OnMouseUp(const POINT &point, WPARAM)
{
    if (!pressed_)
        return false;
    pressed_ = false;
    if (HitSelf(window_, *this, point))
        Toggle();
    return true;
}

bool ToggleSwitch::OnKeyDown(WPARAM key, LPARAM)
{
    if (!disabled_ && (key == VK_SPACE || key == VK_RETURN))
    {
        Toggle();
        return true;
    }
    return false;
}

HCURSOR ToggleSwitch::GetCursor() const
{
    return LoadCursor(nullptr, IDC_ARROW);
}

void ToggleSwitch::Toggle()
{
    SetChecked(!on_);
    if (onChanged_)
    {
        const auto handler = onChanged_;
        handler(on_);
    }
}

// ---------------------------------------------------------------------------------
// CompactMenuScrollbar
// ---------------------------------------------------------------------------------
namespace
{
constexpr float kMenuThumbWidth = 4.0f;
constexpr float kMenuThumbRightGap = 2.0f;
constexpr float kMenuThumbEndGap = 4.0f;
constexpr float kMenuThumbMinLength = 24.0f;
constexpr float kMenuScrollbarHitWidth = 8.0f;
// A wheel notch at the system's default of 3 lines scrolls two compact rows (28 + 2).
constexpr float kMenuWheelStep = 2.0f * (28.0f + 2.0f);
} // namespace

void CompactMenuScrollbar::Layout(float contentHeight, float viewport, const RectF &bounds)
{
    contentHeight_ = contentHeight;
    viewport_ = viewport;
    bounds_ = bounds;
    offset_ = msimeui::ClampScrollOffset(offset_, Extent());
}

void CompactMenuScrollbar::Reset()
{
    offset_ = 0.0f;
    hovered_ = false;
    dragging_ = false;
}

bool CompactMenuScrollbar::CanScroll() const
{
    return msimeui::CanScroll(Extent());
}

float CompactMenuScrollbar::GetOffset() const
{
    return offset_;
}

float CompactMenuScrollbar::GetPlacedOffset(float dpi) const
{
    const float scale = dpi / 96.0f;
    return scale > 0.0f ? std::round(offset_ * scale) / scale : offset_;
}

bool CompactMenuScrollbar::SetOffset(float offset)
{
    const float clamped = msimeui::ClampScrollOffset(offset, Extent());
    if (clamped == offset_)
        return false;
    offset_ = clamped;
    return true;
}

bool CompactMenuScrollbar::Reveal(float start, float length)
{
    return SetOffset(msimeui::OffsetToReveal(Extent(), offset_, start, length));
}

bool CompactMenuScrollbar::Wheel(short delta)
{
    const float notch = msimeui::WheelNotchDistance(msimeui::SystemWheelScrollLines(), kMenuWheelStep, viewport_);
    return SetOffset(offset_ + msimeui::WheelScrollDelta(delta, notch));
}

bool CompactMenuScrollbar::IsOnScrollbar(const PointF &point) const
{
    return CanScroll() && Contains(bounds_, point) && point.x >= bounds_.x + bounds_.width - kMenuScrollbarHitWidth;
}

bool CompactMenuScrollbar::IsDragging() const
{
    return dragging_;
}

bool CompactMenuScrollbar::IsHot() const
{
    return hovered_ || dragging_;
}

bool CompactMenuScrollbar::Press(const PointF &point)
{
    const msimeui::ThumbGrab grab = msimeui::PressScrollbar(Extent(), Track(), offset_, point.y);
    dragging_ = true;
    grip_ = grab.grip;
    SetOffset(grab.offset);
    return true;
}

bool CompactMenuScrollbar::Move(const PointF &point, WPARAM keyState)
{
    if (dragging_)
        return (keyState & MK_LBUTTON) && SetOffset(msimeui::DragThumb(Extent(), Track(), grip_, point.y));
    const bool hovered = IsOnScrollbar(point);
    if (hovered == hovered_)
        return false;
    hovered_ = hovered;
    return true;
}

bool CompactMenuScrollbar::Release(const PointF &point)
{
    dragging_ = false;
    hovered_ = IsOnScrollbar(point);
    return true;
}

bool CompactMenuScrollbar::Leave()
{
    if (!hovered_)
        return false;
    hovered_ = false;
    return !dragging_;
}

void CompactMenuScrollbar::Render(msimeui::DeviceResources &dr, const D2D1_COLOR_F &color,
                                  const D2D1_COLOR_F &hotColor) const
{
    if (!CanScroll())
        return;
    const msimeui::ThumbSpan thumb = msimeui::ThumbAt(Extent(), Track(), offset_);
    FillRounded(
        dr,
        {bounds_.x + bounds_.width - kMenuThumbRightGap - kMenuThumbWidth, thumb.start, kMenuThumbWidth, thumb.length},
        kMenuThumbWidth * 0.5f, IsHot() ? hotColor : color);
}

msimeui::ScrollExtent CompactMenuScrollbar::Extent() const
{
    return {contentHeight_, viewport_};
}

msimeui::ScrollTrack CompactMenuScrollbar::Track() const
{
    return {bounds_.y, bounds_.height, kMenuThumbEndGap, kMenuThumbMinLength};
}

// ---------------------------------------------------------------------------------
// Dropdown
// ---------------------------------------------------------------------------------
namespace
{
// `.dropdown-item`: margin 2px, padding 4px 8px, 14px text at line-height normal.
float MenuItemHeight(bool compact)
{
    return compact ? 28.0f : LineHeight(BodyStyle()) + 8.0f;
}
constexpr float kMenuItemGap = 2.0f;
constexpr float kMenuInset = 2.0f;
constexpr size_t kCompactRows = 6;
// .compact-scroll-menu max-height for 6 rows, border box.
constexpr float kCompactMenuHeight = 6.0f * 28.0f + 5.0f * 2.0f + 2.0f * 2.0f;

const std::string &ArrowSvg()
{
    static const std::string markup = LoadAssetSvg(L"arrow.svg");
    return markup;
}
} // namespace

class DropdownMenu : public msimeui::Visual
{
  public:
    explicit DropdownMenu(Dropdown *owner) : owner_(owner)
    {
    }

    void Prepare()
    {
        filtered_.clear();
        for (size_t i = 0; i < owner_->options_.size(); ++i)
        {
            if (query_.empty() || Lower(owner_->options_[i].label).find(query_) != std::wstring::npos)
                filtered_.push_back(i);
        }
        hovered_ = kNone;
        pressed_ = kNone;
        focused_ = kNone;
        arranged_ = false;
        pendingReveal_ = kNone;
        scrollbar_.Reset();
        LayoutScrollbar();
        layouts_.clear();
        for (size_t row = 0; row < filtered_.size(); ++row)
        {
            if (filtered_[row] == owner_->selected_)
                ScrollIntoView(row);
        }
        InvalidateMeasure();
    }

    void SetQuery(const std::wstring &query)
    {
        query_ = Lower(query);
        Prepare();
    }

    bool IsEmpty() const
    {
        return filtered_.empty();
    }

    float PreferredWidth()
    {
        float widest = 0.0f;
        for (size_t index : filtered_)
            widest = std::max(widest, MeasureTextWidth(owner_->options_[index].label, ItemStyle(index)));
        // item padding 8 + 8, item margins 2 + 2, menu border 1 + 1
        return widest + 16.0f + kMenuInset * 2.0f + 2.0f;
    }

    float PreferredHeight() const
    {
        const size_t rows = filtered_.size();
        const float content = ContentHeight();
        if (owner_->settings_.compact)
            return std::min(content, kCompactMenuHeight);
        (void)rows;
        return content;
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        return {availableSize.width, PreferredHeight() - 2.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        arranged_ = true;
        LayoutScrollbar();
        ScrollIntoView(std::exchange(pendingReveal_, kNone));
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        ID2D1RenderTarget *target = dr.GetRenderTarget();
        if (!target)
            return;
        const Palette &p = CurrentPalette();
        target->PushAxisAlignedClip(ToD2D(bounds_), D2D1_ANTIALIAS_MODE_ALIASED);
        const size_t marked = hovered_ != kNone ? hovered_ : focused_;
        for (size_t row = 0; row < filtered_.size(); ++row)
        {
            const RectF item = ItemRect(row);
            if (item.y + item.height < bounds_.y || item.y > bounds_.y + bounds_.height)
                continue;
            const DropdownOption &option = owner_->options_[filtered_[row]];
            const bool hot = row == marked && !option.disabled;
            if (hot)
                FillRounded(dr, item, 8.0f, p.dropdownItemHover);
            D2D1_COLOR_F color = hot ? p.text : p.dropdownItem;
            if (option.disabled)
                color = WithAlpha(p.textSecondary, 0.45f);
            DrawTextInRect(dr, option.label, ItemStyle(filtered_[row]),
                           {item.x + 8.0f, item.y, item.width - 16.0f, item.height}, color);
        }
        target->PopAxisAlignedClip();

        scrollbar_.Render(dr, p.scrollbarThumb, p.scrollbarThumbHover);
    }

    // The search box of a searchable dropdown keeps the keyboard while the menu is used
    // with the mouse; otherwise the menu takes it to move between rows.
    bool IsFocusable() const override
    {
        return !owner_->search_;
    }

    // Window sends a press to whatever this returns, focusable or not, so the menu claims
    // its whole box: a press must not reach a control the menu covers.
    msimeui::Visual *FindFocusableAt(const PointF &point) override
    {
        return HitTest(point) ? this : nullptr;
    }

    bool OnMouseMove(const POINT &point, WPARAM keyState) override
    {
        if (!window_)
            return false;
        const PointF dip = window_->ClientPixelsToDips(point);
        bool changed = scrollbar_.Move(dip, keyState);
        if (!scrollbar_.IsDragging())
        {
            const size_t row = scrollbar_.IsOnScrollbar(dip) ? kNone : HitTestRow(dip);
            changed = changed || row != hovered_;
            hovered_ = row;
        }
        if (changed)
            InvalidateVisual();
        return true;
    }

    void OnMouseLeave() override
    {
        const bool changed = scrollbar_.Leave() || hovered_ != kNone;
        hovered_ = kNone;
        if (changed)
            InvalidateVisual();
    }

    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        pressed_ = kNone;
        if (!window_)
            return true;
        const PointF dip = window_->ClientPixelsToDips(point);
        if (scrollbar_.IsOnScrollbar(dip))
        {
            scrollbar_.Press(dip);
            hovered_ = kNone;
            InvalidateVisual();
            return true;
        }
        pressed_ = HitTestRow(dip);
        return true;
    }

    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        if (scrollbar_.IsDragging())
        {
            // The press belonged to the scrollbar, so releasing over a row picks nothing.
            scrollbar_.Release(window_ ? window_->ClientPixelsToDips(point) : PointF{});
            pressed_ = kNone;
            InvalidateVisual();
            return true;
        }
        const size_t row = window_ ? HitTestRow(window_->ClientPixelsToDips(point)) : kNone;
        const bool clicked = row != kNone && row == pressed_;
        pressed_ = kNone;
        if (clicked && !owner_->options_[filtered_[row]].disabled)
            owner_->Commit(filtered_[row]);
        return true;
    }

    bool OnMouseWheel(const POINT &, short delta, WPARAM) override
    {
        if (scrollbar_.Wheel(delta))
            InvalidateVisual();
        return true;
    }

    bool OnKeyDown(WPARAM key, LPARAM) override
    {
        const size_t count = filtered_.size();
        if (count == 0)
        {
            if (key == VK_ESCAPE)
            {
                owner_->Close();
                return true;
            }
            return false;
        }
        size_t next = focused_ < count ? focused_ : kNone;
        switch (key)
        {
        case VK_UP:
            next = next == kNone || next == 0 ? count - 1 : next - 1;
            break;
        case VK_DOWN:
            next = next == kNone ? 0 : (next + 1) % count;
            break;
        case VK_HOME:
            next = 0;
            break;
        case VK_END:
            next = count - 1;
            break;
        case VK_RETURN:
        case VK_SPACE:
            if (focused_ < count && !owner_->options_[filtered_[focused_]].disabled)
                owner_->Commit(filtered_[focused_]);
            return true;
        case VK_ESCAPE:
            owner_->Close();
            return true;
        default:
            return false;
        }
        focused_ = next;
        hovered_ = kNone;
        ScrollIntoView(next);
        InvalidateVisual();
        return true;
    }

    void FocusEdge(bool first)
    {
        if (filtered_.empty())
            return;
        focused_ = first ? 0 : filtered_.size() - 1;
        ScrollIntoView(focused_);
        InvalidateVisual();
    }

  private:
    TextStyle ItemStyle(size_t index) const
    {
        TextStyle style = BodyStyle();
        style.weight = owner_->settings_.weight;
        if (index < owner_->options_.size() && !owner_->options_[index].previewFamily.empty())
            style.family = owner_->options_[index].previewFamily;
        return style;
    }

    float ContentHeight() const
    {
        const size_t rows = filtered_.size();
        const float item = MenuItemHeight(owner_->settings_.compact);
        if (rows == 0)
            return kMenuInset * 2.0f + 2.0f;
        return 2.0f + kMenuInset * 2.0f + static_cast<float>(rows) * item + static_cast<float>(rows - 1) * kMenuItemGap;
    }

    // The rows inside the 1 DIP border (ContentHeight counts the border) against the
    // height the menu shows. Until the rows just prepared are arranged, that height is
    // not known yet and the one the menu asks for stands in.
    void LayoutScrollbar()
    {
        const float visible = arranged_ ? bounds_.height : PreferredHeight() - 2.0f;
        scrollbar_.Layout(ContentHeight() - 2.0f, visible, bounds_);
    }

    float PlacedScroll() const
    {
        return scrollbar_.GetPlacedOffset(window_ ? window_->GetDpi() : 96.0f);
    }

    RectF ItemRect(size_t row) const
    {
        const float item = MenuItemHeight(owner_->settings_.compact);
        return {bounds_.x + kMenuInset,
                bounds_.y + kMenuInset + static_cast<float>(row) * (item + kMenuItemGap) - PlacedScroll(),
                bounds_.width - kMenuInset * 2.0f, item};
    }

    size_t HitTestRow(const PointF &point) const
    {
        if (!Contains(bounds_, point))
            return kNone;
        for (size_t row = 0; row < filtered_.size(); ++row)
        {
            if (Contains(ItemRect(row), point))
                return row;
        }
        return kNone;
    }

    // Brings the row and the inset around it into view. Before the rows are arranged
    // the window may still cut the menu shorter than it asks, so the reveal waits for the
    // height the menu gets.
    void ScrollIntoView(size_t row)
    {
        if (row >= filtered_.size())
            return;
        if (!arranged_)
        {
            pendingReveal_ = row;
            return;
        }
        const float item = MenuItemHeight(owner_->settings_.compact);
        scrollbar_.Reveal(static_cast<float>(row) * (item + kMenuItemGap), item + kMenuInset * 2.0f);
    }

    Dropdown *owner_ = nullptr;
    std::vector<size_t> filtered_;
    std::wstring query_;
    std::vector<ComPtr<IDWriteTextLayout>> layouts_;
    size_t hovered_ = kNone;
    size_t pressed_ = kNone;
    size_t focused_ = kNone;
    // Whether bounds_ holds the box of the rows Prepare last filtered.
    bool arranged_ = false;
    // The row to reveal at the next arrange.
    size_t pendingReveal_ = kNone;
    CompactMenuScrollbar scrollbar_;
};

class DropdownPopup : public msimeui::Popup
{
  public:
    explicit DropdownPopup(const std::shared_ptr<DropdownMenu> &menu) : msimeui::Popup(menu), menu_(menu.get())
    {
        SetPadding(1.0f);
        SetPlacement(msimeui::PopupPlacement::BelowLeading);
        SetCornerRadius(10.0f);
        SetShadowEnabled(true);
        SetConstrainToViewport(true);
        SetMatchAnchorWidth(false);
    }

    void SetWidth(float width)
    {
        width_ = width;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        // An open menu is a composited layer of its own in the browser: grayscale text.
        ScopedTextAntialias grayscale(dr, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        msimeui::Popup::Render(dr);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        const SizeF size = msimeui::Popup::Measure(availableSize);
        return {width_ > 0.0f ? width_ : size.width, size.height};
    }

    bool OnMouseWheel(const POINT &point, short delta, WPARAM keyState) override
    {
        if (!window_ || !HitTest(window_->ClientPixelsToDips(point)))
            return false;
        menu_->OnMouseWheel(point, delta, keyState);
        return true;
    }

  private:
    DropdownMenu *menu_ = nullptr;
    float width_ = 0.0f;
};

Dropdown::Dropdown(std::vector<DropdownOption> options, Options settings)
    : options_(std::move(options)), settings_(settings)
{
    menu_ = std::make_shared<DropdownMenu>(this);
    popup_ = std::make_shared<DropdownPopup>(menu_);
    if (settings_.searchable)
    {
        search_ = std::make_shared<msimeui::TextBox>(20.0f, settings_.placeholder);
        search_->SetChromeVisible(false);
        search_->SetFontSize(kFontBody);
        search_->SetPlaceholderFontSize(kFontBody);
        msimeui::TextBox::Appearance appearance;
        appearance.borderWidth = 0.0f;
        appearance.paddingX = 0.0f;
        appearance.paddingY = 0.0f;
        appearance.fontFamily = UiFontFamily();
        search_->SetAppearance(appearance);
        search_->SetOnTextChanged([this](const std::wstring &text) { ApplySearch(text); });
        search_->SetOnFocusChanged([this](bool focused) {
            if (focused && !open_)
                Open();
            if (!focused)
                search_->SetText(CurrentLabel());
        });
        search_->SetOnSubmit([this]() {
            if (open_ && !menu_->IsEmpty())
                menu_->OnKeyDown(VK_RETURN, 0);
        });
        AdoptChild(search_);
    }
}

Dropdown::Dropdown(std::vector<DropdownOption> options) : Dropdown(std::move(options), Options{})
{
}

void Dropdown::SetOnChanged(ChangeHandler handler)
{
    onChanged_ = std::move(handler);
}

void Dropdown::SetOptions(std::vector<DropdownOption> options)
{
    const std::string value = GetValue();
    options_ = std::move(options);
    maxTextWidth_ = -1.0f;
    selected_ = kNone;
    SetValue(value);
    InvalidateMeasure();
    InvalidateVisual();
}

const std::vector<DropdownOption> &Dropdown::GetOptions() const
{
    return options_;
}

void Dropdown::SetValue(const std::string &value)
{
    for (size_t i = 0; i < options_.size(); ++i)
    {
        if (options_[i].value == value)
        {
            SetSelectedIndex(i);
            return;
        }
    }
    if (value.empty())
        return;
    selected_ = kNone;
    unknownLabel_ = string_to_wstring(value);
    if (search_ && !search_->IsFocused())
        search_->SetText(CurrentLabel());
    InvalidateMeasure();
    InvalidateVisual();
}

std::string Dropdown::GetValue() const
{
    return selected_ < options_.size() ? options_[selected_].value : std::string();
}

void Dropdown::SetSelectedIndex(size_t index)
{
    if (index >= options_.size())
        return;
    const bool changed = index != selected_;
    selected_ = index;
    unknownLabel_.clear();
    if (search_ && !search_->IsFocused())
        search_->SetText(CurrentLabel());
    if (changed)
    {
        InvalidateMeasure();
        InvalidateVisual();
    }
}

size_t Dropdown::GetSelectedIndex() const
{
    return selected_;
}

void Dropdown::SetLabelOverride(std::wstring label)
{
    labelOverride_ = std::move(label);
    InvalidateMeasure();
    InvalidateVisual();
}

void Dropdown::SetDisabled(bool disabled)
{
    if (disabled_ == disabled)
        return;
    disabled_ = disabled;
    if (disabled_)
        Close();
    InvalidateVisual();
}

std::wstring Dropdown::CurrentLabel() const
{
    if (!labelOverride_.empty())
        return labelOverride_;
    if (selected_ < options_.size())
        return options_[selected_].label;
    return unknownLabel_;
}

TextStyle Dropdown::LabelStyle() const
{
    TextStyle style = ControlStyle();
    style.weight = settings_.weight;
    return style;
}

SizeF Dropdown::Measure(const SizeF &availableSize)
{
    // padding: 4px 6px 4px 12px, 18px arrow, 1px border; the toggle grows with the
    // label it currently shows.
    const float border = CssBorder();
    const float labelWidth = settings_.searchable ? 132.0f : MeasureTextWidth(CurrentLabel(), LabelStyle());
    float width = labelWidth + settings_.paddingLeft + settings_.paddingRight + 18.0f + border * 2.0f;
    width = std::max(width, settings_.minWidth);
    if (settings_.fixedWidth > 0.0f)
        width = settings_.fixedWidth;
    // The searchable combobox is a <div>, so its text is in the page font.
    const float lineHeight = LineHeight(settings_.searchable ? BodyStyle() : LabelStyle());
    const float height = std::max(lineHeight, 18.0f) + 8.0f + border * 2.0f;
    if (search_)
        search_->MeasureInLayout({132.0f, LineHeight(BodyStyle())});
    return {std::min(width, availableSize.width), height};
}

void Dropdown::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    popup_->SetAnchorRect(bounds_);
    if (search_)
    {
        const float lineHeight = LineHeight(BodyStyle());
        search_->ArrangeInLayout({bounds_.x + 13.0f, bounds_.y + (bounds_.height - lineHeight) * 0.5f,
                                  std::max(0.0f, bounds_.width - 13.0f - 6.0f - 18.0f - 1.0f), lineHeight});
    }
}

void Dropdown::Render(msimeui::DeviceResources &dr)
{
    const Palette &p = CurrentPalette();
    const bool raised = hovered_ || open_;
    background_.SetTarget(raised ? p.surfaceRaised : p.dropdownBg, window_);
    borderColor_.SetTarget(open_ ? p.accentSoftBorder : (hovered_ ? p.borderStrong : p.border), window_);

    if (disabled_)
        PushOpacity(dr, 0.45f);
    if (open_)
    {
        // box-shadow: 0 0 0 2px var(--accent-soft), 0 5px 14px rgba(0, 0, 0, 0.16)
        FillRounded(dr, Inflate(bounds_, 2.0f), 10.0f, p.accentSoft);
    }
    FillRounded(dr, bounds_, 8.0f, background_.Value(window_));
    StrokeRounded(dr, bounds_, 8.0f, borderColor_.Value(window_), 1.0f);

    if (search_)
    {
        // The box defaults to msimeui's own theme; give it the page colors.
        if (searchThemedLight_ != (p.light ? 1 : 0))
        {
            searchThemedLight_ = p.light ? 1 : 0;
            msimeui::TextBox::Appearance appearance;
            appearance.borderWidth = 0.0f;
            appearance.paddingX = 0.0f;
            appearance.paddingY = 0.0f;
            appearance.fontFamily = UiFontFamily();
            appearance.text = p.text;
            appearance.placeholder = p.textSubtle;
            appearance.selection = p.accent;
            search_->SetAppearance(appearance);
        }
        search_->Render(dr);
    }
    else
    {
        const float border = CssBorder();
        DrawTextInRect(
            dr, CurrentLabel(), LabelStyle(),
            {bounds_.x + settings_.paddingLeft + border, bounds_.y,
             std::max(0.0f, bounds_.width - settings_.paddingLeft - settings_.paddingRight - 18.0f - border * 2.0f),
             bounds_.height},
            p.text);
    }

    const float arrowX = bounds_.x + bounds_.width - CssBorder() - settings_.paddingRight - 18.0f;
    const float arrowY = bounds_.y + (bounds_.height - 18.0f) * 0.5f;
    const std::string &arrow = ArrowSvg();
    if (!arrow.empty())
    {
        static std::string lightArrow = InvertSvgColors(arrow, 0.25f);
        DrawSvg(dr, p.light ? lightArrow : arrow, {arrowX, arrowY, 18.0f, 18.0f});
    }
    if (disabled_)
        PopOpacity(dr);
}

void Dropdown::Attach(msimeui::Window *window)
{
    msimeui::Visual::Attach(window);
    popup_->Attach(window);
    if (search_)
        search_->Attach(window);
}

bool Dropdown::IsFocusable() const
{
    return !disabled_;
}

msimeui::Visual *Dropdown::FindVisualAt(const PointF &point)
{
    if (!HitTest(point))
        return nullptr;
    if (search_ && !disabled_ && search_->HitTest(point))
        return search_.get();
    return this;
}

msimeui::Visual *Dropdown::FindFocusableAt(const PointF &point)
{
    if (!HitTest(point) || disabled_)
        return nullptr;
    if (search_ && search_->HitTest(point))
        return search_.get();
    return this;
}

bool Dropdown::OnMouseDown(const POINT &point, WPARAM)
{
    pressed_ = !disabled_ && HitSelf(window_, *this, point);
    InvalidateVisual();
    return pressed_;
}

bool Dropdown::OnMouseUp(const POINT &point, WPARAM)
{
    if (!pressed_)
        return false;
    pressed_ = false;
    if (HitSelf(window_, *this, point))
    {
        if (open_)
            Close();
        else
            Open();
    }
    InvalidateVisual();
    return true;
}

void Dropdown::OnMouseEnter()
{
    hovered_ = true;
    InvalidateVisual();
}

void Dropdown::OnMouseLeave()
{
    hovered_ = false;
    InvalidateVisual();
}

bool Dropdown::OnKeyDown(WPARAM key, LPARAM)
{
    if (disabled_)
        return false;
    if (key == VK_DOWN || key == VK_UP)
    {
        Open();
        menu_->FocusEdge(key == VK_DOWN);
        return true;
    }
    if (key == VK_SPACE || key == VK_RETURN)
    {
        open_ ? Close() : Open();
        return true;
    }
    return false;
}

bool Dropdown::KeepsPopupsOpenOnClick() const
{
    return true;
}

HCURSOR Dropdown::GetCursor() const
{
    return LoadCursor(nullptr, IDC_ARROW);
}

void Dropdown::Open()
{
    msimeui::Scene *scene = window_ ? window_->GetScene() : nullptr;
    if (open_ || !scene || disabled_)
        return;
    menu_->SetQuery(L"");
    // An empty menu (e.g. a model list not fetched yet) does not pop up an empty box.
    if (menu_->IsEmpty() && !settings_.searchable)
        return;
    // Only one menu is open at a time.
    scene->ClearPopups(true);
    const Palette &p = CurrentPalette();
    popup_->SetBackgroundFill(p.dropdownMenuBg);
    popup_->SetBorderColor(p.dropdownMenuBorder);
    popup_->SetTopHighlight(p.dropdownMenuHighlight);
    popup_->SetShadowPasses({{p.menuShadow[0].sigma, p.menuShadow[0].alpha, 0.0f, p.menuShadow[0].offsetY},
                             {p.menuShadow[1].sigma, p.menuShadow[1].alpha, 0.0f, p.menuShadow[1].offsetY}});
    menu_->Prepare();

    // `.dropdown-menu { right: 0; top: calc(100% + 2px) }`: right-aligned to the toggle,
    // shrink-to-fit with min-width 100px, or exactly as wide as the toggle.
    float width = settings_.matchToggleWidth ? bounds_.width : std::max(menu_->PreferredWidth(), 100.0f);
    if (settings_.menuMinWidth > 0.0f)
        width = std::max(width, settings_.menuMinWidth);
    if (settings_.menuMaxWidth > 0.0f)
        width = std::min(width, settings_.menuMaxWidth);
    popup_->SetWidth(width);
    popup_->SetAnchorRect(bounds_);
    popup_->SetOffset(settings_.menuAlignLeft ? 0.0f : bounds_.width - width, 2.0f);
    scene->AddPopup(popup_, [this]() {
        open_ = false;
        InvalidateVisual();
    });
    popup_->BeginAppearAnimation(140.0, -3.0f, 0.985f);
    open_ = true;
    if (!search_)
        window_->FocusVisual(menu_.get());
    window_->Invalidate();
}

void Dropdown::Close()
{
    if (!open_)
        return;
    if (msimeui::Scene *scene = window_ ? window_->GetScene() : nullptr)
        scene->RemovePopup(popup_.get(), false);
    open_ = false;
    if (window_)
    {
        if (!search_)
            window_->FocusVisual(this);
        window_->Invalidate();
    }
}

void Dropdown::ApplySearch(const std::wstring &query)
{
    if (!open_)
        Open();
    menu_->SetQuery(query);
    if (window_)
    {
        if (msimeui::Scene *scene = window_->GetScene())
            scene->RelayoutPopups();
    }
    InvalidateVisual();
}

void Dropdown::Commit(size_t index)
{
    Close();
    if (index >= options_.size())
        return;
    const bool changed = index != selected_;
    selected_ = index;
    unknownLabel_.clear();
    if (search_)
    {
        search_->SetText(CurrentLabel());
        if (window_)
            window_->FocusVisual(this);
    }
    InvalidateMeasure();
    InvalidateVisual();
    if (changed && onChanged_)
    {
        const auto handler = onChanged_;
        handler(index, options_[index]);
    }
}

// ---------------------------------------------------------------------------------
// CheckOption
// ---------------------------------------------------------------------------------
namespace
{
// Chrome's default margins of an `appearance: none` checkbox / radio, which the page
// never resets: checkbox 3px 3px 3px 4px, radio 3px 3px 0 5px.
constexpr Thickness kCheckboxMargin = {4.0f, 3.0f, 3.0f, 3.0f};
constexpr Thickness kRadioMargin = {5.0f, 3.0f, 3.0f, 0.0f};
} // namespace

CheckOption::CheckOption(std::wstring text, bool checked) : checked_(checked)
{
    text_.Set(std::move(text), BodyStyle());
}

void CheckOption::SetOnChanged(ChangeHandler handler)
{
    onChanged_ = std::move(handler);
}

void CheckOption::SetChecked(bool checked)
{
    if (checked_ == checked)
        return;
    checked_ = checked;
    InvalidateVisual();
}

bool CheckOption::IsChecked() const
{
    return checked_;
}

void CheckOption::SetDisabled(bool disabled)
{
    if (disabled_ == disabled)
        return;
    disabled_ = disabled;
    InvalidateVisual();
}

void CheckOption::SetText(std::wstring text)
{
    text_.SetText(std::move(text));
    InvalidateMeasure();
}

SizeF CheckOption::Measure(const SizeF &availableSize)
{
    // padding 8px 4px; the box with its margins, gap 8px, then the label.
    const float box = kCheckboxMargin.left + 18.0f + kCheckboxMargin.right;
    const SizeF label = text_.Measure(availableSize.width);
    const float height = std::max(kCheckboxMargin.top + 18.0f + kCheckboxMargin.bottom, label.height) + 16.0f;
    return {std::min(4.0f + box + 8.0f + label.width + 4.0f, availableSize.width), height};
}

void CheckOption::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void CheckOption::Render(msimeui::DeviceResources &dr)
{
    const Palette &p = CurrentPalette();
    if (disabled_)
        PushOpacity(dr, 0.45f);
    const float boxOuterHeight = kCheckboxMargin.top + 18.0f + kCheckboxMargin.bottom;
    const float boxTop = bounds_.y + (bounds_.height - boxOuterHeight) * 0.5f + kCheckboxMargin.top;
    const RectF box = {bounds_.x + 4.0f + kCheckboxMargin.left, boxTop, 18.0f, 18.0f};
    if (checked_)
    {
        // The translucent checked color is both the background and the 2px border, and the
        // background runs under the border: the edge composites it twice and reads darker.
        FillRounded(dr, box, 2.0f, p.checkboxChecked);
        StrokeRounded(dr, box, 2.0f, p.checkboxChecked, 2.0f);
        // ::after: 4x9 box at (4, 1) inside the border, right/bottom 2px white borders,
        // rotated 45deg about its center.
        ID2D1RenderTarget *target = dr.GetRenderTarget();
        if (ID2D1SolidColorBrush *white = dr.GetSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)))
        {
            const PointF center = {box.x + 2.0f + 4.0f + 3.0f, box.y + 2.0f + 1.0f + 5.5f};
            D2D1_MATRIX_3X2_F previous;
            target->GetTransform(&previous);
            target->SetTransform(D2D1::Matrix3x2F::Rotation(45.0f) * D2D1::Matrix3x2F::Translation(center.x, center.y) *
                                 previous);
            target->FillRectangle(D2D1::RectF(1.0f, -5.5f, 3.0f, 5.5f), white);
            target->FillRectangle(D2D1::RectF(-3.0f, 3.5f, 3.0f, 5.5f), white);
            target->SetTransform(previous);
        }
    }
    else
    {
        StrokeRounded(dr, box, 2.0f, hovered_ && !disabled_ ? p.checkboxBorderHover : p.checkboxBorder, 2.0f);
    }
    const float textX = box.x + 18.0f + kCheckboxMargin.right + 8.0f;
    const SizeF label = text_.Measure(bounds_.x + bounds_.width - textX);
    text_.Draw(dr, {textX, bounds_.y + (bounds_.height - label.height) * 0.5f}, p.text,
               bounds_.x + bounds_.width - textX);
    if (disabled_)
        PopOpacity(dr);
}

bool CheckOption::IsFocusable() const
{
    return !disabled_;
}

bool CheckOption::OnMouseDown(const POINT &point, WPARAM)
{
    pressed_ = !disabled_ && HitSelf(window_, *this, point);
    return pressed_;
}

bool CheckOption::OnMouseUp(const POINT &point, WPARAM)
{
    if (!pressed_)
        return false;
    pressed_ = false;
    if (HitSelf(window_, *this, point))
    {
        checked_ = !checked_;
        InvalidateVisual();
        if (onChanged_)
        {
            const auto handler = onChanged_;
            handler(checked_);
        }
    }
    return true;
}

void CheckOption::OnMouseEnter()
{
    hovered_ = true;
    InvalidateVisual();
}

void CheckOption::OnMouseLeave()
{
    hovered_ = false;
    InvalidateVisual();
}

bool CheckOption::OnKeyDown(WPARAM key, LPARAM)
{
    if (disabled_ || key != VK_SPACE)
        return false;
    checked_ = !checked_;
    InvalidateVisual();
    if (onChanged_)
    {
        const auto handler = onChanged_;
        handler(checked_);
    }
    return true;
}

// ---------------------------------------------------------------------------------
// RadioOption
// ---------------------------------------------------------------------------------
RadioOption::RadioOption(std::wstring text, bool checked) : checked_(checked)
{
    text_.Set(std::move(text), BodyStyle());
}

void RadioOption::SetOnSelected(std::function<void()> handler)
{
    onSelected_ = std::move(handler);
}

void RadioOption::SetChecked(bool checked)
{
    if (checked_ == checked)
        return;
    checked_ = checked;
    InvalidateVisual();
}

bool RadioOption::IsChecked() const
{
    return checked_;
}

void RadioOption::SetDisabled(bool disabled)
{
    if (disabled_ == disabled)
        return;
    disabled_ = disabled;
    InvalidateVisual();
}

void RadioOption::SetInnerPadding(Thickness padding)
{
    inner_ = padding;
    InvalidateMeasure();
}

SizeF RadioOption::Measure(const SizeF &availableSize)
{
    const float ring = kRadioMargin.left + 20.0f + kRadioMargin.right;
    const SizeF label = text_.Measure(availableSize.width);
    const float height =
        std::max(kRadioMargin.top + 20.0f + kRadioMargin.bottom, label.height) + inner_.top + inner_.bottom;
    return {std::min(inner_.left + ring + 10.0f + label.width + inner_.right, availableSize.width), height};
}

void RadioOption::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void RadioOption::Render(msimeui::DeviceResources &dr)
{
    const Palette &p = CurrentPalette();
    if (disabled_)
        PushOpacity(dr, 0.45f);
    const RectF inner = Deflate(bounds_, inner_);
    const float outer = kRadioMargin.top + 20.0f + kRadioMargin.bottom;
    const float top = inner.y + (inner.height - outer) * 0.5f + kRadioMargin.top;
    const RectF ring = {inner.x + kRadioMargin.left, top, 20.0f, 20.0f};
    StrokeRounded(dr, ring, 10.0f, checked_ ? p.toggleOn : p.radioBorder, 2.0f);
    if (checked_)
        FillRounded(dr, {ring.x + 5.0f, ring.y + 5.0f, 10.0f, 10.0f}, 5.0f, p.toggleOn);
    const float textX = ring.x + 20.0f + kRadioMargin.right + 10.0f;
    const float width = inner.x + inner.width - textX;
    const SizeF label = text_.Measure(width);
    text_.Draw(dr, {textX, inner.y + (inner.height - label.height) * 0.5f}, p.text, width);
    if (disabled_)
        PopOpacity(dr);
}

bool RadioOption::IsFocusable() const
{
    return !disabled_;
}

bool RadioOption::OnMouseDown(const POINT &point, WPARAM)
{
    pressed_ = !disabled_ && HitSelf(window_, *this, point);
    return pressed_;
}

bool RadioOption::OnMouseUp(const POINT &point, WPARAM)
{
    if (!pressed_)
        return false;
    pressed_ = false;
    if (HitSelf(window_, *this, point) && !checked_)
    {
        checked_ = true;
        InvalidateVisual();
        if (onSelected_)
        {
            const auto handler = onSelected_;
            handler();
        }
    }
    return true;
}

std::shared_ptr<RadioOption> RadioGroup::Add(std::wstring text, std::string value)
{
    auto option = std::make_shared<RadioOption>(std::move(text), value == value_);
    option->SetOnSelected([this, value]() {
        SetValue(value);
        if (onChanged_)
        {
            const auto handler = onChanged_;
            handler(value);
        }
    });
    options_.emplace_back(std::move(value), option);
    return option;
}

void RadioGroup::SetValue(const std::string &value)
{
    value_ = value;
    for (auto &[optionValue, option] : options_)
        option->SetChecked(optionValue == value);
}

const std::string &RadioGroup::GetValue() const
{
    return value_;
}

void RadioGroup::SetOnChanged(ChangeHandler handler)
{
    onChanged_ = std::move(handler);
}

void RadioGroup::SetDisabled(bool disabled)
{
    for (auto &entry : options_)
        entry.second->SetDisabled(disabled);
}

// ---------------------------------------------------------------------------------
// Button
// ---------------------------------------------------------------------------------
Button::Button(std::wstring text, ButtonKind kind) : text_(std::move(text)), kind_(kind)
{
}

void Button::SetOnClick(std::function<void()> handler)
{
    onClick_ = std::move(handler);
}

void Button::SetText(std::wstring text)
{
    if (text == text_)
        return;
    text_ = std::move(text);
    InvalidateMeasure();
    InvalidateVisual();
}

void Button::SetDisabled(bool disabled)
{
    if (disabled_ == disabled)
        return;
    disabled_ = disabled;
    InvalidateVisual();
}

void Button::SetActive(bool active)
{
    if (active_ == active)
        return;
    active_ = active;
    InvalidateVisual();
}

void Button::SetMinWidth(float width)
{
    minWidth_ = width;
    InvalidateMeasure();
}

void Button::SetHeight(float height)
{
    height_ = height;
    InvalidateMeasure();
}

void Button::SetFontSize(float size)
{
    fontSize_ = size;
    InvalidateMeasure();
}

void Button::SetIcon(std::string svgMarkup, float size, float gap)
{
    icon_ = std::move(svgMarkup);
    iconSize_ = size;
    iconGap_ = gap;
    InvalidateMeasure();
}

void Button::SetTooltip(std::wstring)
{
}

SizeF Button::Measure(const SizeF &availableSize)
{
    TextStyle style = BodyStyle();
    float paddingX = 12.0f;
    float height = 0.0f;
    float minWidth = 0.0f;
    switch (kind_)
    {
    case ButtonKind::Secondary:
        paddingX = 12.0f;
        height = LineHeight(style) + 10.0f + CssBorder() * 2.0f;
        minWidth = 100.0f;
        break;
    case ButtonKind::Primary:
    case ButtonKind::Neutral:
    case ButtonKind::Danger:
        paddingX = 13.0f;
        height = 34.0f;
        break;
    case ButtonKind::Soft:
        // .cand-color-reset sets no font-family: system font.
        style = ControlStyle();
        style.size = kFontCaption;
        paddingX = 10.0f;
        height = LineHeight(style) + 8.0f + CssBorder() * 2.0f;
        break;
    case ButtonKind::Link:
        paddingX = 0.0f;
        height = LineHeight(style);
        break;
    }
    if (fontSize_ > 0.0f)
        style.size = fontSize_;
    if (height_ > 0.0f)
        height = height_;
    if (minWidth_ >= 0.0f)
        minWidth = minWidth_;
    const bool bordered = kind_ != ButtonKind::Primary && kind_ != ButtonKind::Link;
    float width = MeasureTextWidth(text_, style) + paddingX * 2.0f + (bordered ? CssBorder() * 2.0f : 0.0f);
    if (!icon_.empty())
        width += iconSize_ + (text_.empty() ? 0.0f : iconGap_);
    return {std::min(std::max(width, minWidth), availableSize.width), height};
}

void Button::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

D2D1_COLOR_F Button::FillColor() const
{
    const Palette &p = CurrentPalette();
    const bool hot = (hovered_ || pressed_) && !disabled_;
    switch (kind_)
    {
    case ButtonKind::Secondary:
        return hot ? p.surfaceRaised : p.dropdownBg;
    case ButtonKind::Primary:
        return hot ? p.accentStrong : p.accent;
    case ButtonKind::Neutral:
        return hot ? p.buttonSecondaryHover : p.buttonSecondaryBg;
    case ButtonKind::Danger:
        return hot ? WithAlpha(p.dangerText, 0.12f) : D2D1::ColorF(0, 0.0f);
    case ButtonKind::Soft:
        if (active_)
            return p.accentSoft;
        return hot ? p.buttonSecondaryHover : p.buttonSecondaryBg;
    case ButtonKind::Link:
    default:
        return D2D1::ColorF(0, 0.0f);
    }
}

D2D1_COLOR_F Button::BorderColor() const
{
    const Palette &p = CurrentPalette();
    const bool hot = (hovered_ || pressed_) && !disabled_;
    switch (kind_)
    {
    case ButtonKind::Secondary:
        return hot ? p.borderStrong : p.border;
    case ButtonKind::Neutral:
        return p.border;
    case ButtonKind::Danger:
        return WithAlpha(p.dangerText, 0.55f);
    case ButtonKind::Soft:
        return active_ ? p.accentSoftBorder : p.controlBorder;
    default:
        return D2D1::ColorF(0, 0.0f);
    }
}

D2D1_COLOR_F Button::TextColor() const
{
    const Palette &p = CurrentPalette();
    switch (kind_)
    {
    case ButtonKind::Primary:
        return D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
    case ButtonKind::Neutral:
        return p.buttonSecondaryText;
    case ButtonKind::Danger:
        return hovered_ && !disabled_ ? p.dangerHover : p.dangerText;
    case ButtonKind::Soft:
        return active_ ? p.text : p.buttonSecondaryText;
    case ButtonKind::Link:
        return hovered_ && !disabled_ ? p.accentStrong : p.accent;
    case ButtonKind::Secondary:
    default:
        return p.text;
    }
}

void Button::Render(msimeui::DeviceResources &dr)
{
    const float radius = kind_ == ButtonKind::Secondary ? 8.0f : 6.0f;
    const bool animated = kind_ == ButtonKind::Secondary;
    if (animated)
    {
        fill_.SetTarget(FillColor(), window_);
        border_.SetTarget(BorderColor(), window_);
    }
    if (disabled_)
        PushOpacity(dr, kind_ == ButtonKind::Secondary ? 0.65f : 0.5f);
    FillRounded(dr, bounds_, radius, animated ? fill_.Value(window_) : FillColor());
    StrokeRounded(dr, bounds_, radius, animated ? border_.Value(window_) : BorderColor(), 1.0f);

    TextStyle style = BodyStyle();
    if (kind_ == ButtonKind::Soft)
    {
        style = ControlStyle();
        style.size = kFontCaption;
    }
    if (fontSize_ > 0.0f)
        style.size = fontSize_;
    const D2D1_COLOR_F color = TextColor();
    const float textWidth = text_.empty() ? 0.0f : MeasureTextWidth(text_, style);
    const float contentWidth = textWidth + (icon_.empty() ? 0.0f : iconSize_ + (text_.empty() ? 0.0f : iconGap_));
    float x = bounds_.x + (bounds_.width - contentWidth) * 0.5f;
    if (!icon_.empty())
    {
        const float y = bounds_.y + (bounds_.height - iconSize_) * 0.5f;
        DrawSvg(dr, TintSvg(icon_, color), {x, y, iconSize_, iconSize_});
        x += iconSize_ + iconGap_;
    }
    if (!text_.empty())
        DrawTextInRect(dr, text_, style, {x, bounds_.y, textWidth + 1.0f, bounds_.height}, color);
    if (disabled_)
        PopOpacity(dr);
}

bool Button::IsFocusable() const
{
    return !disabled_;
}

bool Button::OnMouseDown(const POINT &point, WPARAM)
{
    pressed_ = !disabled_ && HitSelf(window_, *this, point);
    InvalidateVisual();
    return pressed_;
}

bool Button::OnMouseUp(const POINT &point, WPARAM)
{
    if (!pressed_)
        return false;
    pressed_ = false;
    InvalidateVisual();
    if (HitSelf(window_, *this, point) && onClick_)
    {
        const auto handler = onClick_;
        handler();
    }
    return true;
}

void Button::OnMouseEnter()
{
    hovered_ = true;
    InvalidateVisual();
}

void Button::OnMouseLeave()
{
    hovered_ = false;
    pressed_ = false;
    InvalidateVisual();
}

bool Button::OnKeyDown(WPARAM key, LPARAM)
{
    if (!disabled_ && (key == VK_RETURN || key == VK_SPACE) && onClick_)
    {
        const auto handler = onClick_;
        handler();
        return true;
    }
    return false;
}

HCURSOR Button::GetCursor() const
{
    return LoadCursor(nullptr, disabled_ ? IDC_ARROW : IDC_HAND);
}

// ---------------------------------------------------------------------------------
// TextField
// ---------------------------------------------------------------------------------
class TextField::RevealButton : public msimeui::Visual
{
  public:
    explicit RevealButton(TextField *owner) : owner_(owner)
    {
    }
    SizeF Measure(const SizeF &) override
    {
        return {28.0f, 28.0f};
    }
    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }
    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        const D2D1_COLOR_F color = hovered_ ? p.text : p.textMuted;
        ID2D1RenderTarget *target = dr.GetRenderTarget();
        ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(color);
        if (!target || !brush)
            return;
        // Eye outline, crossed out while the secret is hidden.
        const float cx = bounds_.x + bounds_.width * 0.5f;
        const float cy = bounds_.y + bounds_.height * 0.5f;
        ComPtr<ID2D1Factory> factory;
        target->GetFactory(factory.GetAddressOf());
        ComPtr<ID2D1PathGeometry> eye;
        if (factory && SUCCEEDED(factory->CreatePathGeometry(eye.GetAddressOf())))
        {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(eye->Open(sink.GetAddressOf())))
            {
                sink->BeginFigure(D2D1::Point2F(cx - 7.0f, cy), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddQuadraticBezier(
                    D2D1::QuadraticBezierSegment(D2D1::Point2F(cx, cy - 7.0f), D2D1::Point2F(cx + 7.0f, cy)));
                sink->AddQuadraticBezier(
                    D2D1::QuadraticBezierSegment(D2D1::Point2F(cx, cy + 7.0f), D2D1::Point2F(cx - 7.0f, cy)));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                target->DrawGeometry(eye.Get(), brush, 1.3f);
            }
        }
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 2.2f, 2.2f), brush, 1.3f);
        if (!owner_->revealed_)
            target->DrawLine(D2D1::Point2F(cx - 6.0f, cy + 6.0f), D2D1::Point2F(cx + 6.0f, cy - 6.0f), brush, 1.3f);
    }
    bool OnMouseDown(const POINT &, WPARAM) override
    {
        return true;
    }
    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        if (HitSelf(window_, *this, point))
        {
            owner_->revealed_ = !owner_->revealed_;
            owner_->box_->SetPasswordMode(!owner_->revealed_);
            InvalidateVisual();
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
        InvalidateVisual();
    }
    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, IDC_HAND);
    }

  private:
    TextField *owner_ = nullptr;
    bool hovered_ = false;
};

TextField::TextField(Options options) : options_(std::move(options))
{
    box_ = std::make_shared<msimeui::TextBox>(options_.height, options_.placeholder);
    box_->SetMultiline(options_.multiline);
    box_->SetPasswordMode(options_.password);
    box_->SetFontSize(options_.fontSize);
    box_->SetPlaceholderFontSize(options_.fontSize);
    box_->SetOnTextChanged([this](const std::wstring &text) {
        if (onChanged_)
        {
            const auto handler = onChanged_;
            handler(text);
        }
    });
    box_->SetOnFocusChanged([this](bool focused) {
        if (!focused)
        {
            const std::wstring text = box_->GetText();
            if (text != committed_)
            {
                committed_ = text;
                if (onCommit_)
                {
                    const auto handler = onCommit_;
                    handler(text);
                }
            }
        }
    });
    box_->SetOnSubmit([this]() {
        const std::wstring text = box_->GetText();
        if (text != committed_)
        {
            committed_ = text;
            if (onCommit_)
            {
                const auto handler = onCommit_;
                handler(text);
            }
        }
    });
    SetChild(box_);
    if (options_.password && options_.revealButton)
    {
        reveal_ = std::make_shared<RevealButton>(this);
        AdoptChild(reveal_);
    }
    ApplyAppearance();
}

void TextField::ApplyAppearance()
{
    const Palette &p = CurrentPalette();
    appliedLight_ = p.light;
    msimeui::TextBox::Appearance appearance;
    appearance.cornerRadius = options_.radius;
    appearance.borderWidth = CssBorder();
    appearance.paddingX = options_.paddingX;
    appearance.paddingY = options_.paddingY;
    appearance.fill = p.controlBg;
    appearance.border = p.border;
    appearance.focusBorder = p.accent;
    appearance.text = p.text;
    appearance.selection = WithAlpha(p.accent, 0.45f);
    appearance.placeholder = p.textSubtle;
    appearance.fontFamily = options_.monospace ? std::wstring(L"Consolas") : UiFontFamily();
    box_->SetAppearance(appearance);
}

void TextField::SetText(const std::wstring &text)
{
    if (box_->IsFocused())
        return;
    committed_ = text;
    box_->SetText(text);
}

std::wstring TextField::GetText() const
{
    return box_->GetText();
}

void TextField::SetOnChanged(std::function<void(const std::wstring &)> handler)
{
    onChanged_ = std::move(handler);
}

void TextField::SetOnCommit(std::function<void(const std::wstring &)> handler)
{
    onCommit_ = std::move(handler);
}

void TextField::SetDisabled(bool disabled)
{
    box_->SetEnabled(!disabled);
}

void TextField::SetPlaceholder(std::wstring placeholder)
{
    box_->SetPlaceholderText(std::move(placeholder));
}

SizeF TextField::Measure(const SizeF &availableSize)
{
    box_->MeasureInLayout(availableSize);
    if (reveal_)
        reveal_->MeasureInLayout(availableSize);
    return {availableSize.width, options_.height};
}

void TextField::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    box_->ArrangeInLayout(finalRect);
    if (reveal_)
        reveal_->ArrangeInLayout(RevealRect());
}

RectF TextField::RevealRect() const
{
    return {bounds_.x + bounds_.width - 30.0f, bounds_.y + (bounds_.height - 28.0f) * 0.5f, 28.0f, 28.0f};
}

void TextField::Render(msimeui::DeviceResources &dr)
{
    // Colors follow the theme without rebuilding the field.
    if (appliedLight_ != CurrentPalette().light)
        ApplyAppearance();
    box_->Render(dr);
    if (reveal_)
        reveal_->Render(dr);
}

msimeui::Visual *TextField::FindVisualAt(const PointF &point)
{
    if (reveal_ && reveal_->HitTest(point))
        return reveal_.get();
    return msimeui::Container::FindVisualAt(point);
}

msimeui::Visual *TextField::FindFocusableAt(const PointF &point)
{
    if (reveal_ && reveal_->HitTest(point))
        return nullptr;
    return msimeui::Container::FindFocusableAt(point);
}

// ---------------------------------------------------------------------------------
// NumberStepper
// ---------------------------------------------------------------------------------
NumberStepper::NumberStepper(int minimum, int maximum, int step)
    : minimum_(minimum), maximum_(maximum), step_(std::max(step, 1)), value_(minimum)
{
}

void NumberStepper::SetValue(int value)
{
    value = std::clamp(value, minimum_, maximum_);
    if (value_ == value)
        return;
    value_ = value;
    InvalidateVisual();
}

int NumberStepper::GetValue() const
{
    return value_;
}

void NumberStepper::SetOnChanged(std::function<void(int)> handler)
{
    onChanged_ = std::move(handler);
}

void NumberStepper::SetSuffix(std::wstring suffix)
{
    suffix_ = std::move(suffix);
    InvalidateVisual();
}

void NumberStepper::SetDisabled(bool disabled)
{
    disabled_ = disabled;
    InvalidateVisual();
}

SizeF NumberStepper::Measure(const SizeF &)
{
    return {1.0f + 24.0f + 88.0f + 24.0f + 1.0f, 30.0f};
}

void NumberStepper::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

int NumberStepper::HitPart(const PointF &point) const
{
    if (!Contains(bounds_, point))
        return 2;
    if (point.x < bounds_.x + 25.0f)
        return -1;
    if (point.x >= bounds_.x + bounds_.width - 25.0f)
        return 1;
    return 0;
}

void NumberStepper::Render(msimeui::DeviceResources &dr)
{
    const Palette &p = CurrentPalette();
    if (disabled_)
        PushOpacity(dr, 0.45f);
    const bool focused = false;
    FillRounded(dr, bounds_, 8.0f, p.controlBg);
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    target->PushAxisAlignedClip(ToD2D(Inflate(bounds_, -1.0f)), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const RectF minus = {bounds_.x + 1.0f, bounds_.y + 1.0f, 24.0f, bounds_.height - 2.0f};
    const RectF plus = {bounds_.x + bounds_.width - 25.0f, bounds_.y + 1.0f, 24.0f, bounds_.height - 2.0f};
    const bool canMinus = value_ > minimum_ && !disabled_;
    const bool canPlus = value_ < maximum_ && !disabled_;
    if (hovered_ == -1 && canMinus)
        FillRect(dr, minus, p.surfaceRaised);
    if (hovered_ == 1 && canPlus)
        FillRect(dr, plus, p.surfaceRaised);
    target->PopAxisAlignedClip();
    TextStyle sign = BodyStyle();
    sign.size = 16.0f;
    DrawTextInRect(dr, L"−", sign, minus, canMinus ? p.accent : p.textMuted, true);
    DrawTextInRect(dr, L"+", sign, plus, canPlus ? p.accent : p.textMuted, true);
    FillRect(dr, {minus.x + minus.width, bounds_.y + 1.0f, 1.0f, bounds_.height - 2.0f}, p.border);
    FillRect(dr, {plus.x - 1.0f, bounds_.y + 1.0f, 1.0f, bounds_.height - 2.0f}, p.border);
    DrawTextInRect(dr, std::to_wstring(value_) + suffix_, BodyStyle(),
                   {minus.x + minus.width + 1.0f, bounds_.y, 86.0f, bounds_.height}, p.text, true);
    StrokeRounded(dr, bounds_, 8.0f, focused ? p.accent : p.border, 1.0f);
    if (disabled_)
        PopOpacity(dr);
}

void NumberStepper::Change(int value)
{
    value = std::clamp(value, minimum_, maximum_);
    if (value == value_)
        return;
    value_ = value;
    InvalidateVisual();
    if (onChanged_)
    {
        const auto handler = onChanged_;
        handler(value_);
    }
}

bool NumberStepper::OnMouseDown(const POINT &point, WPARAM)
{
    if (disabled_ || !window_)
        return false;
    pressed_ = HitPart(window_->ClientPixelsToDips(point));
    return pressed_ != 2;
}

bool NumberStepper::OnMouseUp(const POINT &point, WPARAM)
{
    if (pressed_ == 2 || !window_)
        return false;
    const int part = HitPart(window_->ClientPixelsToDips(point));
    if (part == pressed_ && part != 0)
        Change(value_ + part * step_);
    pressed_ = 2;
    return true;
}

bool NumberStepper::OnMouseMove(const POINT &point, WPARAM)
{
    if (!window_)
        return false;
    const int part = HitPart(window_->ClientPixelsToDips(point));
    if (part != hovered_)
    {
        hovered_ = part;
        InvalidateVisual();
    }
    return true;
}

void NumberStepper::OnMouseLeave()
{
    hovered_ = 2;
    InvalidateVisual();
}

bool NumberStepper::OnMouseWheel(const POINT &, short, WPARAM)
{
    // A page that scrolls under the pointer should keep scrolling; the browser only
    // spins a focused number input.
    return false;
}

bool NumberStepper::IsFocusable() const
{
    return !disabled_;
}

bool NumberStepper::OnKeyDown(WPARAM key, LPARAM)
{
    if (disabled_)
        return false;
    if (key == VK_UP || key == VK_RIGHT)
    {
        Change(value_ + step_);
        return true;
    }
    if (key == VK_DOWN || key == VK_LEFT)
    {
        Change(value_ - step_);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------
// Expander
// ---------------------------------------------------------------------------------
Expander::Expander(std::wstring title, bool subItem)
{
    title_.Set(std::move(title), subItem ? SubItemTitleStyle() : SectionTitleStyle());
}

void Expander::SetOnToggled(std::function<void(bool expanded)> handler)
{
    onToggled_ = std::move(handler);
}

void Expander::SetExpanded(bool expanded)
{
    if (expanded_ == expanded)
        return;
    expanded_ = expanded;
    rotation_.AnimateTo(expanded ? 1.0f : 0.0f, 160.0, msimeui::Easing::Ease(), msimeui::NowMs());
    if (window_)
        window_->RequestAnimationFrame();
    InvalidateVisual();
}

bool Expander::IsExpanded() const
{
    return expanded_;
}

SizeF Expander::Measure(const SizeF &availableSize)
{
    const SizeF title = title_.Measure(availableSize.width);
    return {std::min(title.width + 9.0f + 7.0f, availableSize.width), std::max(title.height, 10.0f)};
}

void Expander::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void Expander::Render(msimeui::DeviceResources &dr)
{
    const Palette &p = CurrentPalette();
    const SizeF title = title_.Measure(bounds_.width);
    title_.Draw(dr, {bounds_.x, bounds_.y + (bounds_.height - title.height) * 0.5f}, p.text, bounds_.width);
    const double now = window_ ? window_->GetFrameTime() : msimeui::NowMs();
    const float t = rotation_.Value(now);
    if (rotation_.IsRunning(now) && window_)
        window_->RequestAnimationFrame();
    // margin-top -3px -> 3px moves the centered box by half of it; rotate 45 -> 225deg.
    const PointF center = {bounds_.x + title.width + 9.0f + 3.5f, bounds_.y + bounds_.height * 0.5f - 1.5f + 3.0f * t};
    DrawChevron(dr, center, 7.0f, 45.0f + 180.0f * t, p.textMuted, 2.0f);
}

bool Expander::OnMouseDown(const POINT &point, WPARAM)
{
    pressed_ = HitSelf(window_, *this, point);
    return pressed_;
}

bool Expander::OnMouseUp(const POINT &point, WPARAM)
{
    if (!pressed_)
        return false;
    pressed_ = false;
    if (HitSelf(window_, *this, point))
    {
        SetExpanded(!expanded_);
        if (onToggled_)
        {
            const auto handler = onToggled_;
            handler(expanded_);
        }
    }
    return true;
}

HCURSOR Expander::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

// ---------------------------------------------------------------------------------
// KeyCaps
// ---------------------------------------------------------------------------------
namespace
{
TextStyle KbdStyle()
{
    TextStyle style;
    style.size = 12.0f;
    style.family = L"Segoe UI";
    return style;
}
} // namespace

KeyCaps::KeyCaps(std::vector<std::wstring> keys) : keys_(std::move(keys))
{
}

void KeyCaps::SetKeys(std::vector<std::wstring> keys)
{
    keys_ = std::move(keys);
    InvalidateMeasure();
    InvalidateVisual();
}

SizeF KeyCaps::Measure(const SizeF &availableSize)
{
    widths_.clear();
    TextStyle plus = BodyStyle();
    plus.size = 11.0f;
    plusWidth_ = MeasureTextWidth(L"+", plus);
    float width = 0.0f;
    for (size_t i = 0; i < keys_.size(); ++i)
    {
        // padding 2px 7px, 1px border, min-width 24px
        const float cap = std::max(24.0f, MeasureTextWidth(keys_[i], KbdStyle()) + 14.0f + 2.0f);
        widths_.push_back(cap);
        width += cap + (i + 1 < keys_.size() ? plusWidth_ + 8.0f : 0.0f);
    }
    return {std::min(width, availableSize.width), 24.0f};
}

void KeyCaps::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void KeyCaps::Render(msimeui::DeviceResources &dr)
{
    const Palette &p = CurrentPalette();
    float x = bounds_.x;
    const float height = 22.0f;
    const float y = bounds_.y + (bounds_.height - height) * 0.5f;
    TextStyle plus = BodyStyle();
    plus.size = 11.0f;
    for (size_t i = 0; i < keys_.size() && i < widths_.size(); ++i)
    {
        const RectF cap = {x, y, widths_[i], height};
        FillRounded(dr, {cap.x, cap.y + 1.0f, cap.width, cap.height}, 4.0f, p.kbdShadow);
        FillRounded(dr, cap, 4.0f, p.kbdBorderBottom);
        FillRounded(dr, {cap.x, cap.y, cap.width, cap.height - 1.0f}, 4.0f, p.kbdBorder);
        FillRounded(dr, {cap.x + 1.0f, cap.y + 1.0f, cap.width - 2.0f, cap.height - 3.0f}, 3.0f, p.kbdBg);
        DrawTextInRect(dr, keys_[i], KbdStyle(), {cap.x, cap.y, cap.width, cap.height - 1.0f}, p.kbdText, true);
        x += cap.width;
        if (i + 1 < keys_.size())
        {
            DrawTextInRect(dr, L"+", plus, {x + 4.0f, y, plusWidth_, height}, p.textMuted, true);
            x += plusWidth_ + 8.0f;
        }
    }
}

// ---------------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------------
SvgImage::SvgImage(std::string markup, float width, float height)
    : markup_(std::move(markup)), width_(width), height_(height)
{
}

void SvgImage::SetMarkup(std::string markup)
{
    markup_ = std::move(markup);
    tinted_.clear();
    InvalidateVisual();
}

void SvgImage::SetTint(std::function<D2D1_COLOR_F()> tint)
{
    tint_ = std::move(tint);
    tinted_.clear();
    InvalidateVisual();
}

void SvgImage::SetOpacity(float opacity)
{
    opacity_ = opacity;
    InvalidateVisual();
}

SizeF SvgImage::Measure(const SizeF &)
{
    return {width_, height_};
}

void SvgImage::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void SvgImage::Render(msimeui::DeviceResources &dr)
{
    if (markup_.empty())
        return;
    const std::string *markup = &markup_;
    if (tint_)
    {
        const D2D1_COLOR_F color = tint_();
        if (color.a > 0.0f)
        {
            if (tinted_.empty() || color.r != tintedFor_.r || color.g != tintedFor_.g || color.b != tintedFor_.b ||
                color.a != tintedFor_.a)
            {
                tinted_ = TintSvg(markup_, color);
                tintedFor_ = color;
            }
            markup = &tinted_;
        }
    }
    const float x = bounds_.x + (bounds_.width - width_) * 0.5f;
    const float y = bounds_.y + (bounds_.height - height_) * 0.5f;
    DrawSvg(dr, *markup, {x, y, width_, height_}, opacity_);
}

BitmapImage::BitmapImage(std::wstring path, float width, float height, float radius)
    : path_(std::move(path)), width_(width), height_(height), radius_(radius)
{
}

void BitmapImage::SetPath(std::wstring path)
{
    path_ = std::move(path);
    InvalidateVisual();
}

SizeF BitmapImage::Measure(const SizeF &)
{
    return {width_, height_};
}

void BitmapImage::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void BitmapImage::Render(msimeui::DeviceResources &dr)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    ID2D1Bitmap *bitmap = target ? dr.GetBitmapFromFile(path_) : nullptr;
    if (!bitmap)
        return;
    const float x = bounds_.x + (bounds_.width - width_) * 0.5f;
    const float y = bounds_.y + (bounds_.height - height_) * 0.5f;
    const D2D1_RECT_F destination = D2D1::RectF(x, y, x + width_, y + height_);
    ComPtr<ID2D1Layer> layer;
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    if (radius_ > 0.0f)
    {
        ComPtr<ID2D1Factory> factory;
        target->GetFactory(factory.GetAddressOf());
        if (factory &&
            SUCCEEDED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(destination, radius_, radius_),
                                                              clip.GetAddressOf())) &&
            SUCCEEDED(target->CreateLayer(nullptr, layer.GetAddressOf())))
            target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                    D2D1::IdentityMatrix(), 1.0f, nullptr,
                                                    D2D1_LAYER_OPTIONS_INITIALIZE_FOR_CLEARTYPE),
                              layer.Get());
    }
    ComPtr<ID2D1DeviceContext> context;
    if (SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(context.GetAddressOf()))))
        context->DrawBitmap(bitmap, destination, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    else
        target->DrawBitmap(bitmap, destination);
    if (layer)
        target->PopLayer();
}

std::wstring AssetPath(const std::wstring &relativePath)
{
    static const std::filesystem::path root =
        std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"assets/settings";
    return (root / relativePath).wstring();
}

std::string LoadAssetSvg(const std::wstring &relativePath)
{
    std::ifstream file(std::filesystem::path(AssetPath(relativePath)), std::ios::binary);
    if (!file)
        return {};
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

namespace
{
std::string ReplaceColors(const std::string &markup, const std::function<std::string(const D2D1_COLOR_F &)> &map)
{
    // Hex literals and the two named colors the page's icons use.
    static const std::regex pattern(R"((#[0-9a-fA-F]{6}\b|#[0-9a-fA-F]{3}\b|"white"|"black"|'white'|'black'))");
    std::string result;
    auto begin = std::sregex_iterator(markup.begin(), markup.end(), pattern);
    size_t last = 0;
    for (auto it = begin; it != std::sregex_iterator(); ++it)
    {
        const std::smatch &match = *it;
        result.append(markup, last, static_cast<size_t>(match.position()) - last);
        std::string token = match.str();
        D2D1_COLOR_F color{};
        bool quoted = false;
        if (token == "\"white\"" || token == "'white'")
        {
            color = {1.0f, 1.0f, 1.0f, 1.0f};
            quoted = true;
        }
        else if (token == "\"black\"" || token == "'black'")
        {
            color = {0.0f, 0.0f, 0.0f, 1.0f};
            quoted = true;
        }
        else
        {
            ParseCssColor(token, color);
        }
        const std::string mapped = map(color);
        result += quoted ? "\"" + mapped + "\"" : mapped;
        last = static_cast<size_t>(match.position() + match.length());
    }
    result.append(markup, last, std::string::npos);
    return result;
}
} // namespace

std::string InvertSvgColors(const std::string &markup, float brightness)
{
    return ReplaceColors(markup, [brightness](const D2D1_COLOR_F &c) {
        return ToCssHex({(1.0f - c.r) * brightness, (1.0f - c.g) * brightness, (1.0f - c.b) * brightness, 1.0f});
    });
}

std::string TintSvg(const std::string &markup, const D2D1_COLOR_F &color)
{
    const std::string hex = ToCssHex(color);
    std::string tinted = ReplaceColors(markup, [&hex](const D2D1_COLOR_F &) { return hex; });
    for (size_t pos = 0; (pos = tinted.find("currentColor", pos)) != std::string::npos;)
    {
        tinted.replace(pos, 12, hex);
        pos += hex.size();
    }
    if (color.a < 1.0f)
    {
        char opacity[32] = {};
        std::snprintf(opacity, sizeof(opacity), " opacity=\"%.3f\"", color.a);
        const size_t svg = tinted.find("<svg");
        if (svg != std::string::npos)
            tinted.insert(svg + 4, opacity);
    }
    return tinted;
}
} // namespace native_settings
