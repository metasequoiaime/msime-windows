// Helpers shared by the control implementations: the shared DirectWrite factory, hit testing, text
// measurement, the cached text-format layout factory and rounded-rect/label drawing.
#include "msimeui/Controls.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Fonts.h"
#include "msimeui/Theme.h"

#include "ControlsInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace msimeui
{
namespace controls_detail
{
namespace
{
RectF MakeInsetRect(const RectF &rect, float insetX, float insetY)
{
    return {rect.x + insetX, rect.y + insetY, std::max(rect.width - insetX * 2.0f, 0.0f),
            std::max(rect.height - insetY * 2.0f, 0.0f)};
}
} // namespace

IDWriteFactory *GetSharedDWriteFactory()
{
    static ComPtr<IDWriteFactory> factory;
    if (!factory)
    {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown **>(factory.GetAddressOf()));
    }

    return factory.Get();
}

bool PointInRect(const RectF &rect, const PointF &point)
{
    return point.x >= rect.x && point.x <= (rect.x + rect.width) && point.y >= rect.y &&
           point.y <= (rect.y + rect.height);
}

bool PointInRoundedRect(const RectF &rect, float radius, const PointF &point)
{
    if (!PointInRect(rect, point))
    {
        return false;
    }

    const float clampedRadius = std::min(radius, std::min(rect.width, rect.height) * 0.5f);
    if (clampedRadius <= 0.0f)
    {
        return true;
    }

    const float left = rect.x;
    const float top = rect.y;
    const float right = rect.x + rect.width;
    const float bottom = rect.y + rect.height;

    if ((point.x >= left + clampedRadius && point.x <= right - clampedRadius) ||
        (point.y >= top + clampedRadius && point.y <= bottom - clampedRadius))
    {
        return true;
    }

    const float centerX = point.x < left + clampedRadius ? left + clampedRadius : right - clampedRadius;
    const float centerY = point.y < top + clampedRadius ? top + clampedRadius : bottom - clampedRadius;
    const float dx = point.x - centerX;
    const float dy = point.y - centerY;
    return (dx * dx + dy * dy) <= (clampedRadius * clampedRadius);
}

SizeF MeasureText(IDWriteFactory *factory, const std::wstring &text, float fontSize, bool bold, float maxWidth)
{
    if (!factory)
    {
        return {maxWidth, std::ceil(fontSize * 1.5f)};
    }

    ComPtr<IDWriteTextFormat> format;
    if (FAILED(factory->CreateTextFormat(
            L"Segoe UI", nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"", format.GetAddressOf())))
    {
        return {maxWidth, std::ceil(fontSize * 1.5f)};
    }

    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), maxWidth,
                                         std::numeric_limits<float>::max(), layout.GetAddressOf())))
    {
        return {maxWidth, std::ceil(fontSize * 1.5f)};
    }

    DWRITE_TEXT_METRICS metrics = {};
    if (FAILED(layout->GetMetrics(&metrics)))
    {
        return {maxWidth, std::ceil(fontSize * 1.5f)};
    }

    return {std::ceil(metrics.widthIncludingTrailingWhitespace), std::ceil(metrics.height)};
}

D2D1_POINT_2F SnapTextOriginToPixelBaseline(ID2D1RenderTarget *target, IDWriteTextLayout *layout, D2D1_POINT_2F origin)
{
    if (!target || !layout)
        return origin;
    float dpiX = 96.0f;
    float dpiY = 96.0f;
    target->GetDpi(&dpiX, &dpiY);
    D2D1_MATRIX_3X2_F transform = {};
    target->GetTransform(&transform);
    if (dpiY <= 0.0f || transform._12 != 0.0f || transform._21 != 0.0f || transform._22 <= 0.0f)
        return origin;
    DWRITE_TEXT_METRICS metrics = {};
    if (FAILED(layout->GetMetrics(&metrics)) || metrics.lineCount == 0)
        return origin;
    std::vector<DWRITE_LINE_METRICS> lines(metrics.lineCount);
    UINT32 lineCount = 0;
    if (FAILED(layout->GetLineMetrics(lines.data(), static_cast<UINT32>(lines.size()), &lineCount)) || lineCount == 0)
        return origin;
    const DWRITE_LINE_METRICS &firstLine = lines.front();

    const float scaleY = transform._22 * dpiY / 96.0f;
    // The atlas offset is whole device pixels but reaches here as DIPs (x 96/dpi) and comes back with
    // float noise, e.g. 30.999998 one frame and 31.000002 the next. Treat a near-whole translation as
    // whole, so the rounding below depends only on the layout and is the same every frame.
    const float translation = transform._32 * dpiY / 96.0f;
    const float wholeTranslation = std::round(translation);
    const float pixelTranslation = std::fabs(translation - wholeTranslation) < 0.01f ? wholeTranslation : translation;
    const float baseline = (origin.y + metrics.top + firstLine.baseline) * scaleY + pixelTranslation;
    origin.y += (std::round(baseline) - baseline) / scaleY;
    return origin;
}

ComPtr<IDWriteTextLayout> CreateCachedTextLayout(IDWriteFactory *factory, const std::wstring &fontFamily,
                                                 const std::wstring &text, float fontSize,
                                                 DWRITE_FONT_WEIGHT fontWeight, float width, float height,
                                                 DWRITE_TEXT_ALIGNMENT textAlignment,
                                                 DWRITE_PARAGRAPH_ALIGNMENT paragraphAlignment,
                                                 DWRITE_WORD_WRAPPING wordWrapping,
                                                 const std::vector<std::wstring> &fallbackFamilies)
{
    ComPtr<IDWriteTextLayout> layout;
    if (!factory)
    {
        return layout;
    }

    ComPtr<IDWriteTextFormat> format;
    struct TextFormatKey
    {
        std::wstring family;
        std::vector<std::wstring> fallbackFamilies;
        float size = 0.0f;
        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
        DWRITE_TEXT_ALIGNMENT textAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        DWRITE_PARAGRAPH_ALIGNMENT paragraphAlignment = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
        DWRITE_WORD_WRAPPING wordWrapping = DWRITE_WORD_WRAPPING_NO_WRAP;
        ComPtr<IDWriteTextFormat> format;
    };
    static std::vector<TextFormatKey> formatCache;
    for (auto &entry : formatCache)
    {
        if (entry.family == fontFamily && entry.fallbackFamilies == fallbackFamilies && entry.size == fontSize &&
            entry.weight == fontWeight && entry.textAlignment == textAlignment &&
            entry.paragraphAlignment == paragraphAlignment && entry.wordWrapping == wordWrapping && entry.format)
        {
            format = entry.format;
            break;
        }
    }
    if (!format)
    {
        if (FAILED(factory->CreateTextFormat(fontFamily.c_str(), nullptr, fontWeight, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, fontSize, L"", format.GetAddressOf())))
        {
            if (FAILED(factory->CreateTextFormat(UiFontFallbackFamily(), nullptr, fontWeight, DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL, fontSize, L"", format.GetAddressOf())))
            {
                return layout;
            }
        }
        format->SetTextAlignment(textAlignment);
        format->SetParagraphAlignment(paragraphAlignment);
        format->SetWordWrapping(wordWrapping);
        ApplyFontFallback(factory, format.Get(), fallbackFamilies);
        TextFormatKey entry;
        entry.fallbackFamilies = fallbackFamilies;
        entry.family = fontFamily;
        entry.size = fontSize;
        entry.weight = fontWeight;
        entry.textAlignment = textAlignment;
        entry.paragraphAlignment = paragraphAlignment;
        entry.wordWrapping = wordWrapping;
        entry.format = format;
        formatCache.push_back(std::move(entry));
    }

    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), width, height,
                                         layout.GetAddressOf())))
    {
        layout.Reset();
    }

    return layout;
}

void FillRoundedRect(DeviceResources &deviceResources, const RectF &bounds, float radius, D2D1_COLOR_F fill,
                     D2D1_COLOR_F stroke, float strokeWidth)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    ID2D1SolidColorBrush *fillBrush = deviceResources.GetSolidColorBrush(fill);
    ID2D1SolidColorBrush *strokeBrush = deviceResources.GetSolidColorBrush(stroke);
    if (!fillBrush || !strokeBrush)
    {
        return;
    }

    const auto rounded = D2D1::RoundedRect(
        D2D1::RectF(bounds.x, bounds.y, bounds.x + bounds.width, bounds.y + bounds.height), radius, radius);
    target->FillRoundedRectangle(rounded, fillBrush);
    if (strokeWidth > 0.0f)
    {
        target->DrawRoundedRectangle(rounded, strokeBrush, strokeWidth);
    }
}

void DrawLabel(DeviceResources &deviceResources, const std::wstring &text, float fontSize, bool bold,
               D2D1_COLOR_F color, const RectF &rect, DWRITE_TEXT_ALIGNMENT alignment,
               DWRITE_PARAGRAPH_ALIGNMENT paragraphAlignment, DWRITE_WORD_WRAPPING wrapping)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    const Theme &theme = ThemeManager::GetCurrent();
    IDWriteTextFormat *format = deviceResources.GetTextFormat(
        theme.uiFontFamily, fontSize, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, alignment,
        paragraphAlignment, wrapping);
    ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(color);
    if (!format || !brush)
    {
        return;
    }

    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
                      D2D1::RectF(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height), brush);
}
} // namespace controls_detail
} // namespace msimeui
