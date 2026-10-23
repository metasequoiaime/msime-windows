#include "settings/native/native_settings_pill.h"

#include "settings/native/native_settings_theme.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Layout.h"

#include <algorithm>

namespace native_settings
{
namespace
{
constexpr float kDot = 6.0f;
constexpr float kGap = 6.0f;
constexpr float kPaddingX = 8.0f;
constexpr float kHeight = 22.0f;

D2D1_COLOR_F DotColor(const std::string &kind, const Palette &p)
{
    // 与 StatusText 的三档语义一致；颜色取调色板里的危险色与强调色，深色主题下也不会掉对比度。
    if (kind == "success")
        return Hex(0x4CAF72);
    if (kind == "error")
        return p.dangerText;
    return p.textMuted;
}
} // namespace

StatusPill::StatusPill(std::wstring text, std::string kind) : text_(std::move(text)), kind_(std::move(kind))
{
    TextStyle style;
    style.size = kFontCaption;
    layout_.Set(text_, style);
}

void StatusPill::Set(std::wstring text, std::string kind)
{
    text_ = std::move(text);
    kind_ = std::move(kind);
    layout_.SetText(text_);
    InvalidateMeasure();
    InvalidateVisual();
}

void StatusPill::Clear()
{
    Set({}, "pending");
}

SizeF StatusPill::Measure(const SizeF &availableSize)
{
    if (text_.empty())
    {
        measuredWidth_ = 0.0f;
        return {0.0f, 0.0f};
    }
    const SizeF text = layout_.Measure(availableSize.width);
    measuredWidth_ = std::min(kPaddingX * 2.0f + kDot + kGap + text.width, availableSize.width);
    return {measuredWidth_, kHeight};
}

void StatusPill::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void StatusPill::Render(msimeui::DeviceResources &dr)
{
    if (text_.empty() || bounds_.width <= 0.0f)
        return;
    const Palette &p = CurrentPalette();
    FillRounded(dr, bounds_, kHeight * 0.5f, p.surfaceSubtle);
    StrokeRounded(dr, bounds_, kHeight * 0.5f, p.border, 1.0f);
    // 指示点，垂直居中。
    const D2D1_COLOR_F dot = DotColor(kind_, p);
    if (ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(dot))
    {
        const D2D1_POINT_2F center =
            D2D1::Point2F(bounds_.x + kPaddingX + kDot * 0.5f, bounds_.y + bounds_.height * 0.5f);
        dr.GetRenderTarget()->FillEllipse(D2D1::Ellipse(center, kDot * 0.5f, kDot * 0.5f), brush);
    }
    const float textLeft = bounds_.x + kPaddingX + kDot + kGap;
    const float width = std::max(bounds_.x + bounds_.width - kPaddingX - textLeft, 0.0f);
    layout_.Draw(dr, {textLeft, bounds_.y + (bounds_.height - LineHeight(layout_.Style())) * 0.5f}, p.textSecondary,
                 width);
}
} // namespace native_settings
