#pragma once

// Declarations shared by the control implementation files (Button.cpp, ListView.cpp, ...). Internal to msimeui:
// only src/*.cpp include this, never the public headers or product code.

#include "msimeui/Controls.h"

#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <wrl/client.h>

namespace msimeui
{
using Microsoft::WRL::ComPtr;

namespace controls_detail
{
constexpr float kControlCornerRadius = 12.0f;
constexpr float kListItemCornerRadius = 14.0f;

IDWriteFactory *GetSharedDWriteFactory();
bool PointInRect(const RectF &rect, const PointF &point);
bool PointInRoundedRect(const RectF &rect, float radius, const PointF &point);
SizeF MeasureText(IDWriteFactory *factory, const std::wstring &text, float fontSize, bool bold, float maxWidth);
// Defined once in ControlsInternal.cpp so every control shares the one text-format cache.
ComPtr<IDWriteTextLayout> CreateCachedTextLayout(IDWriteFactory *factory, const std::wstring &fontFamily,
                                                 const std::wstring &text, float fontSize,
                                                 DWRITE_FONT_WEIGHT fontWeight, float width, float height,
                                                 DWRITE_TEXT_ALIGNMENT textAlignment,
                                                 DWRITE_PARAGRAPH_ALIGNMENT paragraphAlignment,
                                                 DWRITE_WORD_WRAPPING wordWrapping,
                                                 const std::vector<std::wstring> &fallbackFamilies = {});
// Moves a text layout's draw origin onto the device pixel grid: the left edge and the first baseline land on
// whole pixels. D2D rounds the baseline itself; doing it first keeps that rounding away from the half-pixel
// boundary, where float noise in the transform (the composition atlas offset) would flip it from frame to
// frame. Snapping the left edge keeps the same text rasterized the same wherever the row reflows it to.
D2D1_POINT_2F SnapTextOriginToPixelGrid(ID2D1RenderTarget *target, IDWriteTextLayout *layout, D2D1_POINT_2F origin);
void FillRoundedRect(DeviceResources &deviceResources, const RectF &bounds, float radius, D2D1_COLOR_F fill,
                     D2D1_COLOR_F stroke, float strokeWidth);
void DrawLabel(DeviceResources &deviceResources, const std::wstring &text, float fontSize, bool bold,
               D2D1_COLOR_F color, const RectF &rect, DWRITE_TEXT_ALIGNMENT alignment,
               DWRITE_PARAGRAPH_ALIGNMENT paragraphAlignment,
               DWRITE_WORD_WRAPPING wrapping = DWRITE_WORD_WRAPPING_NO_WRAP);
} // namespace controls_detail
} // namespace msimeui
