// CandidateList: a vertical or horizontal list of candidates with labels, annotations and translations,
// plus its text-metric cache.
#include "msimeui/Controls.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Theme.h"
#include "msimeui/Window.h"

#include "ControlsInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <utility>

namespace msimeui
{
using namespace controls_detail;

namespace
{
constexpr D2D1_DRAW_TEXT_OPTIONS kColorClipTextOptions =
    static_cast<D2D1_DRAW_TEXT_OPTIONS>(D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);

constexpr float kCandidateItemGap = 1.0f;

// Per-corner radii in top-left, top-right, bottom-right, bottom-left order.
void FillRoundedRectCorners(DeviceResources &deviceResources, const RectF &bounds, const float (&radii)[4],
                            D2D1_COLOR_F fill)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    ID2D1SolidColorBrush *fillBrush = deviceResources.GetSolidColorBrush(fill);
    if (!target || !fillBrush)
    {
        return;
    }

    const float limit = std::max(std::min(bounds.width, bounds.height) * 0.5f, 0.0f);
    const float tl = std::clamp(radii[0], 0.0f, limit);
    const float tr = std::clamp(radii[1], 0.0f, limit);
    const float br = std::clamp(radii[2], 0.0f, limit);
    const float bl = std::clamp(radii[3], 0.0f, limit);
    const float left = bounds.x;
    const float top = bounds.y;
    const float right = bounds.x + bounds.width;
    const float bottom = bounds.y + bounds.height;

    ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.GetAddressOf());
    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (!factory || FAILED(factory->CreatePathGeometry(geometry.GetAddressOf())) ||
        FAILED(geometry->Open(sink.GetAddressOf())))
    {
        return;
    }

    const auto arcTo = [&sink](float x, float y, float radius) {
        if (radius > 0.0f)
        {
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y), D2D1::SizeF(radius, radius), 0.0f,
                                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        }
    };
    sink->BeginFigure(D2D1::Point2F(left + tl, top), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(right - tr, top));
    arcTo(right, top + tr, tr);
    sink->AddLine(D2D1::Point2F(right, bottom - br));
    arcTo(right - br, bottom, br);
    sink->AddLine(D2D1::Point2F(left + bl, bottom));
    arcTo(left, bottom - bl, bl);
    sink->AddLine(D2D1::Point2F(left, top + tl));
    arcTo(left + tl, top, tl);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (SUCCEEDED(sink->Close()))
    {
        target->FillGeometry(geometry.Get(), fillBrush);
    }
}
} // namespace

CandidateList::CandidateList(float itemHeight)
{
    appearance_.itemHeight = itemHeight;
}

void CandidateList::AddItem(Item item)
{
    items_.push_back(std::move(item));
    InvalidateMeasure();
}

void CandidateList::SetItems(std::vector<Item> items)
{
    items_ = std::move(items);
    if (selectedIndex_ >= items_.size())
    {
        selectedIndex_ = items_.empty() ? 0 : items_.size() - 1;
    }
    pressedIndex_ = static_cast<size_t>(-1);
    hoveredIndex_ = static_cast<size_t>(-1);
    InvalidateMeasure();
}

void CandidateList::ClearItems()
{
    items_.clear();
    selectedIndex_ = 0;
    pressedIndex_ = static_cast<size_t>(-1);
    InvalidateMeasure();
}

void CandidateList::SetSelectedIndex(size_t index)
{
    if (items_.empty())
    {
        selectedIndex_ = 0;
        return;
    }

    const size_t clamped = std::min(index, items_.size() - 1);
    if (selectedIndex_ == clamped)
    {
        return;
    }

    selectedIndex_ = clamped;
    if (onSelectionChanged_)
    {
        const auto handler = onSelectionChanged_;
        handler(selectedIndex_);
    }
    InvalidateVisual();
}

size_t CandidateList::GetSelectedIndex() const
{
    return selectedIndex_;
}

void CandidateList::SetOnSelectionChanged(SelectionChangedHandler handler)
{
    onSelectionChanged_ = std::move(handler);
}

void CandidateList::SetOnItemActivated(ItemActivatedHandler handler)
{
    onItemActivated_ = std::move(handler);
}

void CandidateList::SetOnContextMenu(ContextMenuHandler handler)
{
    onContextMenu_ = std::move(handler);
}

const CandidateList::Item *CandidateList::GetItem(size_t index) const
{
    return index < items_.size() ? &items_[index] : nullptr;
}

void CandidateList::SetAppearance(Appearance appearance)
{
    appearance_ = appearance;
    // The fallback font list is part of every layout and metric but not of their keys.
    textLayoutCache_.clear();
    textMetricCache_.clear();
    InvalidateMeasure();
}

const std::wstring &CandidateList::ResolvedFontFamily() const
{
    const Theme &theme = ThemeManager::GetCurrent();
    return appearance_.fontFamily.empty() ? theme.textInputFontFamily : appearance_.fontFamily;
}

namespace
{
// The exact bits of every number go into the key, so only an identical
// measurement shares an entry.
std::wstring TextCacheKey(wchar_t kind, const std::wstring &text, std::initializer_list<float> numbers)
{
    static_assert(sizeof(float) == sizeof(uint32_t));
    std::wstring key;
    key.reserve(text.size() + 1 + numbers.size() * 2);
    key.push_back(kind);
    for (const float number : numbers)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &number, sizeof(bits));
        key.push_back(static_cast<wchar_t>(bits & 0xFFFF));
        key.push_back(static_cast<wchar_t>(bits >> 16));
    }
    key.append(text);
    return key;
}

// Enough for several pages of candidates with labels, annotations and
// translations; past it the cache simply starts over.
constexpr size_t kMaxTextMetricEntries = 2048;
// Layouts hold shaped glyph runs, so far fewer are kept: a few pages' worth.
constexpr size_t kMaxTextLayoutEntries = 256;
} // namespace

bool CandidateList::LookupTextMetric(wchar_t kind, const std::wstring &text, float fontSize, float width,
                                     float &value) const
{
    const std::wstring &family = ResolvedFontFamily();
    if (textMetricFamily_ != family)
    {
        textMetricCache_.clear();
        textMetricFamily_ = family;
        return false;
    }
    const auto found = textMetricCache_.find(TextCacheKey(kind, text, {fontSize, width}));
    if (found == textMetricCache_.end())
    {
        return false;
    }
    value = found->second;
    return true;
}

void CandidateList::StoreTextMetric(wchar_t kind, const std::wstring &text, float fontSize, float width,
                                    float value) const
{
    if (textMetricCache_.size() >= kMaxTextMetricEntries)
    {
        textMetricCache_.clear();
    }
    textMetricCache_[TextCacheKey(kind, text, {fontSize, width})] = value;
}

ComPtr<IDWriteTextLayout> CandidateList::TextLayoutFor(IDWriteFactory *factory, const std::wstring &fontFamily,
                                                       const std::wstring &text, float fontSize, const RectF &box)
{
    if (text.empty())
    {
        return {};
    }
    if (textLayoutFamily_ != fontFamily)
    {
        textLayoutCache_.clear();
        textLayoutFamily_ = fontFamily;
    }
    const float width = std::max(box.width, 1.0f);
    const float height = std::max(box.height, 1.0f);
    std::wstring key = TextCacheKey(L'l', text, {fontSize, width, height});
    if (const auto found = textLayoutCache_.find(key); found != textLayoutCache_.end())
    {
        return found->second;
    }
    ComPtr<IDWriteTextLayout> layout = CreateCachedTextLayout(
        factory, fontFamily, text, fontSize, DWRITE_FONT_WEIGHT_NORMAL, width, height, DWRITE_TEXT_ALIGNMENT_LEADING,
        DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_WRAP, appearance_.fallbackFontFamilies);
    if (!layout)
    {
        return {};
    }
    if (textLayoutCache_.size() >= kMaxTextLayoutEntries)
    {
        textLayoutCache_.clear();
    }
    textLayoutCache_.emplace(std::move(key), layout);
    return layout;
}

void CandidateList::SetOrientation(Orientation orientation)
{
    orientation_ = orientation;
    InvalidateMeasure();
}

void CandidateList::SetHoverEnabled(bool enabled)
{
    hoverEnabled_ = enabled;
    if (!enabled)
    {
        hoveredIndex_ = static_cast<size_t>(-1);
    }
    InvalidateVisual();
}

float CandidateList::EstimateTextWidth(const std::wstring &text, float fontSize) const
{
    if (text.empty())
    {
        return 0.0f;
    }

    float cached = 0.0f;
    if (LookupTextMetric(L'w', text, fontSize, 0.0f, cached))
    {
        return cached;
    }

    IDWriteFactory *factory = GetSharedDWriteFactory();
    const std::wstring &fontFamily = ResolvedFontFamily();
    ComPtr<IDWriteTextLayout> layout = CreateCachedTextLayout(
        factory, fontFamily, text, fontSize, DWRITE_FONT_WEIGHT_NORMAL, 4096.0f, std::max(fontSize * 2.0f, 1.0f),
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP,
        appearance_.fallbackFontFamilies);
    if (!layout)
    {
        float width = 0.0f;
        for (const wchar_t ch : text)
        {
            width += (ch < 128) ? fontSize * 0.72f : fontSize;
        }
        return width;
    }

    DWRITE_TEXT_METRICS metrics = {};
    layout->GetMetrics(&metrics);
    DWRITE_OVERHANG_METRICS overhang = {};
    layout->GetOverhangMetrics(&overhang);
    const float width = std::ceil(metrics.widthIncludingTrailingWhitespace + std::max(overhang.right, 0.0f) + 1.0f);
    StoreTextMetric(L'w', text, fontSize, 0.0f, width);
    return width;
}

float CandidateList::MeasureTextHeight(const std::wstring &text, float fontSize, float width) const
{
    if (text.empty())
        return 0.0f;
    float cached = 0.0f;
    if (LookupTextMetric(L'h', text, fontSize, width, cached))
        return cached;
    const auto layout = CreateCachedTextLayout(GetSharedDWriteFactory(), ResolvedFontFamily(), text, fontSize,
                                               DWRITE_FONT_WEIGHT_NORMAL, width, 65536.0f,
                                               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
                                               DWRITE_WORD_WRAPPING_WRAP, appearance_.fallbackFontFamilies);
    DWRITE_TEXT_METRICS metrics{};
    if (layout && SUCCEEDED(layout->GetMetrics(&metrics)))
    {
        const float height = std::ceil(metrics.height);
        StoreTextMetric(L'h', text, fontSize, width, height);
        return height;
    }
    return std::ceil(EstimateTextWidth(text, fontSize) / width) * fontSize * 1.25f;
}

CandidateList::ItemGeometry CandidateList::MeasureItem(size_t index, float width) const
{
    const auto &item = items_[index];
    ItemGeometry geometry;
    const float labelX = appearance_.contentPadLeft + appearance_.textPadLeft;
    const float labelW = std::max(EstimateTextWidth(item.label, appearance_.labelFontSize), 9.0f);
    const float tagW = item.tag.empty() ? 0.0f : EstimateTextWidth(item.tag, appearance_.tagFontSize);
    const float tagX = labelX + labelW + appearance_.labelGap;
    const float textX = tagX + (item.tag.empty() ? 0.0f : tagW + appearance_.tagGap);
    const float contentWidth = std::max(width - textX - appearance_.contentPadRight, 1.0f);
    const float textW = std::min(std::max(EstimateTextWidth(item.text, appearance_.fontSize), 8.0f), contentWidth);
    const float textH = std::max(appearance_.itemHeight, MeasureTextHeight(item.text, appearance_.fontSize, textW));
    geometry.label = {labelX, 0.0f, labelW, appearance_.itemHeight};
    geometry.tag = {tagX, 0.0f, tagW, appearance_.itemHeight};
    geometry.text = {textX, 0.0f, textW, textH};
    float height = textH;
    float lineEnd = item.text.empty() ? 0.0f : textW;
    // height 末尾属于行底内边距的部分：只有单行高度（itemHeight）里才含这段内边距，
    // 换到下方的辅助码和翻译从它上面开始排，排完再把它补回最后一行下面。
    const float padBottom = std::clamp(appearance_.contentPadBottom, 0.0f, appearance_.itemHeight * 0.5f);
    float padBelow = textH > appearance_.itemHeight ? 0.0f : padBottom;

    // 短字段保持原有并排方式；空间不足时让辅助码和翻译完整换行。
    if (!item.annotation.empty())
    {
        const float annotationW =
            std::min(EstimateTextWidth(item.annotation, appearance_.annotationFontSize), contentWidth);
        const bool inlineAnnotation = lineEnd + 4.0f + annotationW <= contentWidth;
        const float annotationH = std::max(
            appearance_.itemHeight, MeasureTextHeight(item.annotation, appearance_.annotationFontSize, annotationW));
        geometry.annotation = {textX + (inlineAnnotation ? lineEnd + 4.0f : 0.0f),
                               inlineAnnotation ? 0.0f : height - padBelow, annotationW, annotationH};
        if (geometry.annotation.y + annotationH > height)
        {
            height = geometry.annotation.y + annotationH;
            padBelow = annotationH > appearance_.itemHeight ? 0.0f : padBottom;
        }
        lineEnd = geometry.annotation.x - textX + annotationW;
    }
    if (!item.translation.empty())
    {
        const float fontSize = appearance_.fontSize * appearance_.translationFontSizeScale;
        const float gap = item.text.empty() ? 0.0f : appearance_.fontSize * 0.65f;
        const float naturalTranslationWidth = EstimateTextWidth(item.translation, fontSize);
        const float translationW = std::min(naturalTranslationWidth, contentWidth);
        const bool inlineTranslation = orientation_ == Orientation::Vertical && geometry.annotation.y == 0.0f &&
                                       lineEnd + gap + translationW <= contentWidth;
        const float translationH =
            inlineTranslation
                ? textH
                : (naturalTranslationWidth <= contentWidth
                       ? fontSize * 1.25f
                       : std::max(fontSize * 1.25f, MeasureTextHeight(item.translation, fontSize, translationW)));
        geometry.translation = {textX + (inlineTranslation ? lineEnd + gap : 0.0f),
                                inlineTranslation ? 0.0f : height - padBelow, translationW, translationH};
        height = std::max(height, geometry.translation.y + translationH + (inlineTranslation ? 0.0f : padBelow));
    }
    geometry.bounds = {0.0f, 0.0f, width, height};
    return geometry;
}

RectF CandidateList::ItemRect(size_t index) const
{
    if (index >= itemGeometry_.size())
        return {};
    RectF rect = itemGeometry_[index].bounds;
    rect.x += bounds_.x;
    rect.y += bounds_.y;
    // 竖排：每行都应铺满列表实际宽度（列表已被拉伸到卡片内宽），使选中高亮与命中区域
    // 覆盖整行。这里直接用列表的最终布局宽度 bounds_，而非候选项自然宽度，从而不受测量/
    // 布局缓存影响（绘制前 Present 可能以不同可用宽度重新测量，把每项宽度还原为自然宽度）。
    // 右侧留白由外层卡片内边距提供，与左侧保持一致。
    // 多列竖排（columnMajor_）例外：每列宽度由该列最宽词条决定，不能铺满整表宽度。
    if (orientation_ == Orientation::Vertical && !columnMajor_)
    {
        rect.x = bounds_.x;
        rect.width = bounds_.width;
    }
    else if (appearance_.justifyHorizontalRows)
    {
        // 横排铺满：列表比这一行的自然宽度宽时（卡片保持了同一输入下更宽的尺寸），把多出的宽度
        // 均分给这一行的每一项，末项右边贴着列表右边，高亮不会在行尾留下一截空白。
        const float lineY = itemGeometry_[index].bounds.y;
        size_t first = index;
        while (first > 0 && itemGeometry_[first - 1].bounds.y == lineY)
            --first;
        size_t last = index;
        while (last + 1 < itemGeometry_.size() && itemGeometry_[last + 1].bounds.y == lineY)
            ++last;
        const RectF &tail = itemGeometry_[last].bounds;
        const float extra = bounds_.width - (tail.x + tail.width);
        if (extra > 0.0f)
        {
            const float share = extra / static_cast<float>(last - first + 1);
            rect.x += share * static_cast<float>(index - first);
            rect.width += share;
        }
    }
    return rect;
}

RectF CandidateList::GetItemBounds(size_t index) const
{
    return ItemRect(index);
}

SizeF CandidateList::Measure(const SizeF &availableSize)
{
    itemGeometry_.clear();
    columnMajor_ = false;
    const float availableWidth = std::max(availableSize.width, 1.0f);
    const float gap = appearance_.itemGap;
    const bool horizontal = orientation_ == Orientation::Horizontal;
    // 竖排多列：开启 verticalColumns 时按列优先排布——先沿一列向下排，下一项会越过列高预算
    // 就另起一列向右排；每列宽度取该列最宽词条，列数受 verticalColumns 与列宽预算双重限制，
    // 放不下时末列在底部溢出裁切。列高/列宽预算优先取 appearance（弹窗会以宿主视口重新测量
    // 子控件，宿主比面板真正可用的屏幕空间大，必须用固定预算才能让布局稳定）。
    const float budgetHeight =
        appearance_.columnBudgetHeight > 0.0f ? appearance_.columnBudgetHeight : availableSize.height;
    const float budgetWidth = appearance_.columnBudgetWidth > 0.0f ? appearance_.columnBudgetWidth : availableWidth;
    const bool columnMajor = !horizontal && appearance_.verticalColumns > 0 && budgetHeight < 100000.0f;
    if (columnMajor)
    {
        const float availableHeight = std::max(budgetHeight, 1.0f);
        const float colGap = appearance_.columnGap >= 0.0f ? appearance_.columnGap : gap;
        std::vector<ItemGeometry> geos(items_.size());
        std::vector<float> naturalWidth(items_.size());
        for (size_t i = 0; i < items_.size(); ++i)
        {
            const auto &item = items_[i];
            const float textWidth = EstimateTextWidth(item.text, appearance_.fontSize) +
                                    EstimateTextWidth(item.annotation, appearance_.annotationFontSize);
            const float translationWidth =
                EstimateTextWidth(item.translation, appearance_.fontSize * appearance_.translationFontSizeScale);
            const float contentWidth =
                textWidth + (item.translation.empty() ? 0.0f : appearance_.fontSize * 0.65f + translationWidth);
            const float width =
                appearance_.contentPadLeft + appearance_.textPadLeft +
                std::max(EstimateTextWidth(item.label, appearance_.labelFontSize), 9.0f) + appearance_.labelGap +
                (item.tag.empty() ? 0.0f : EstimateTextWidth(item.tag, appearance_.tagFontSize) + appearance_.tagGap) +
                contentWidth + appearance_.contentPadRight;
            naturalWidth[i] = std::min(width, budgetWidth);
            geos[i] = MeasureItem(i, naturalWidth[i]);
        }
        std::vector<std::vector<size_t>> columns;
        std::vector<float> columnHeights;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            const float h = geos[i].bounds.height;
            // 列内纵向间距用 itemGap；columnGap 只用于列与列之间的横向间距。
            if (!columns.empty() && columnHeights.back() + gap + h > availableHeight)
            {
                columns.emplace_back();
                columnHeights.push_back(0.0f);
            }
            if (columns.empty())
            {
                columns.emplace_back();
                columnHeights.push_back(0.0f);
            }
            if (columns.back().empty())
            {
                columns.back().push_back(i);
                columnHeights.back() = h;
            }
            else
            {
                columns.back().push_back(i);
                columnHeights.back() += gap + h;
            }
        }
        auto columnWidth = [&](const std::vector<size_t> &column) {
            float w = 0.0f;
            for (size_t idx : column)
                w = std::max(w, naturalWidth[idx]);
            return w;
        };
        auto totalWidth = [&]() {
            float w = 0.0f;
            for (size_t c = 0; c < columns.size(); ++c)
            {
                w += columnWidth(columns[c]);
                if (c + 1 < columns.size())
                    w += colGap;
            }
            return w;
        };
        auto mergeLastTwo = [&]() {
            const size_t a = columns.size() - 2;
            columns[a].insert(columns[a].end(), columns[a + 1].begin(), columns[a + 1].end());
            columnHeights[a] += gap + columnHeights[a + 1];
            columns.pop_back();
            columnHeights.pop_back();
        };
        while (columns.size() > 1 &&
               (static_cast<int>(columns.size()) > appearance_.verticalColumns || totalWidth() > budgetWidth))
            mergeLastTwo();

        itemGeometry_.assign(items_.size(), ItemGeometry{});
        float x = 0.0f;
        float measuredWidth = 0.0f;
        float contentHeight = 0.0f;
        for (size_t c = 0; c < columns.size(); ++c)
        {
            // 列宽 = 该列最宽词条；列内每一项的高亮与命中区都铺满整列宽，与单列竖排的整行高亮一致。
            const float colW = columnWidth(columns[c]);
            float y = 0.0f;
            for (size_t idx : columns[c])
            {
                geos[idx].bounds.x = x;
                geos[idx].bounds.y = y;
                geos[idx].bounds.width = colW;
                itemGeometry_[idx] = geos[idx];
                y += geos[idx].bounds.height + gap;
            }
            x += colW + colGap;
            measuredWidth = std::max(measuredWidth, x - colGap);
            contentHeight = std::max(contentHeight, columnHeights[c]);
        }
        columnMajor_ = true;
        layoutWidth_ = measuredWidth;
        return {measuredWidth, std::min(contentHeight, availableHeight)};
    }
    std::vector<float> widths;
    float maxWidth = appearance_.fitContent ? 0.0f : 80.0f;
    // 等宽网格：可用宽度按最小列宽能放下几列（不超过 gridColumns）均分，项宽取整列。
    const bool grid = horizontal && appearance_.gridColumns > 0 && availableWidth < 100000.0f;
    int columns = 1;
    float cellWidth = availableWidth;
    if (grid)
    {
        const float minCell = std::max(appearance_.minItemWidth, 1.0f);
        columns = std::clamp(static_cast<int>(std::floor((availableWidth + gap) / (minCell + gap))), 1,
                             appearance_.gridColumns);
        cellWidth =
            std::max((availableWidth - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns), 1.0f);
    }
    for (const auto &item : items_)
    {
        // fitContent keeps only the gap MeasureItem puts before an inline annotation.
        const float textSlack = !appearance_.fitContent ? 6.0f : (item.annotation.empty() ? 0.0f : 4.0f);
        const float textWidth = EstimateTextWidth(item.text, appearance_.fontSize) + textSlack +
                                EstimateTextWidth(item.annotation, appearance_.annotationFontSize);
        const float translationWidth =
            EstimateTextWidth(item.translation, appearance_.fontSize * appearance_.translationFontSizeScale);
        const float contentWidth =
            horizontal
                ? std::max(textWidth, translationWidth)
                : textWidth + (item.translation.empty() ? 0.0f : appearance_.fontSize * 0.65f + translationWidth);
        float width =
            appearance_.contentPadLeft + appearance_.textPadLeft +
            std::max(EstimateTextWidth(item.label, appearance_.labelFontSize), 9.0f) + appearance_.labelGap +
            (item.tag.empty() ? 0.0f : EstimateTextWidth(item.tag, appearance_.tagFontSize) + appearance_.tagGap) +
            contentWidth + appearance_.contentPadRight;
        if (grid)
        {
            const int span =
                std::clamp(static_cast<int>(std::ceil((width + gap) / (cellWidth + gap) - 0.001f)), 1, columns);
            width = cellWidth * static_cast<float>(span) + gap * static_cast<float>(span - 1);
        }
        else if (horizontal)
            width = std::max(width, appearance_.minItemWidth);
        widths.push_back(std::min(width, availableWidth));
        maxWidth = std::max(maxWidth, width);
    }
    const float verticalWidth = std::min(maxWidth, availableWidth);
    float x = 0.0f;
    float y = 0.0f;
    float rowHeight = 0.0f;
    float measuredWidth = horizontal ? 0.0f : verticalWidth;
    for (size_t i = 0; i < items_.size(); ++i)
    {
        auto geometry = MeasureItem(i, horizontal ? widths[i] : verticalWidth);
        // 网格的整列宽度是除出来的，末列可能因为浮点误差多出一点点，不能因此换行。
        if (horizontal && x > 0.0f && x + geometry.bounds.width > availableWidth + (grid ? 0.5f : 0.0f))
        {
            y += rowHeight + gap;
            x = 0.0f;
            rowHeight = 0.0f;
        }
        geometry.bounds.x = x;
        geometry.bounds.y = y;
        itemGeometry_.push_back(geometry);
        measuredWidth = std::max(measuredWidth, x + geometry.bounds.width);
        rowHeight = std::max(rowHeight, geometry.bounds.height);
        if (horizontal)
            x += geometry.bounds.width + gap;
        else if (i + 1 < items_.size())
        {
            y += geometry.bounds.height + gap;
            rowHeight = 0.0f;
        }
    }
    layoutWidth_ = measuredWidth;
    const float height = items_.empty() ? appearance_.itemHeight : y + rowHeight;
    return {measuredWidth, std::min(height, availableSize.height)};
}

void CandidateList::Arrange(const RectF &finalRect)
{
    if (std::abs(finalRect.width - layoutWidth_) > 0.5f)
        Measure({finalRect.width, std::numeric_limits<float>::max()});
    bounds_ = finalRect;
}

void CandidateList::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    IDWriteFactory *factory = deviceResources.GetDWriteFactory();
    if (!target || !factory || itemGeometry_.size() != items_.size())
    {
        return;
    }

    const std::wstring &fontFamily = ResolvedFontFamily();

    // The list's corners in list coordinates: its arranged width (the list is
    // stretched to its container) and the bottom of the last row of items.
    // Only item corners that really sit on them take outerCornerRadius, so a
    // full-bleed highlight never rounds a corner in the middle of the frame.
    const float contentRight = bounds_.width;
    float contentBottom = 0.0f;
    for (size_t index = 0; index < items_.size(); ++index)
    {
        const RectF itemRect = ItemRect(index);
        contentBottom = std::max(contentBottom, itemRect.y - bounds_.y + itemRect.height);
    }

    for (size_t index = 0; index < items_.size(); ++index)
    {
        const RectF itemRect = ItemRect(index);
        const bool selected = appearance_.selectable && index == selectedIndex_;
        const bool hovered = hoverEnabled_ && index == hoveredIndex_;
        const bool pressed = pressed_ && index == pressedIndex_;

        if (selected || pressed || (hovered && !selected))
        {
            const D2D1_COLOR_F fill = pressed ? appearance_.rowFillPressed
                                              : (selected ? appearance_.rowFillSelected : appearance_.rowFillHover);
            if (fill.a > 0.001f)
            {
                if (appearance_.outerCornerRadius > 0.0f)
                {
                    constexpr float kEdgeEpsilon = 0.5f;
                    const float x = itemRect.x - bounds_.x;
                    const float y = itemRect.y - bounds_.y;
                    const bool top = appearance_.outerTopCornersEnabled && y <= kEdgeEpsilon;
                    const bool bottom = y + itemRect.height >= contentBottom - kEdgeEpsilon;
                    const bool left = x <= kEdgeEpsilon;
                    const bool right = x + itemRect.width >= contentRight - kEdgeEpsilon;
                    const float inner = appearance_.cornerRadius;
                    const float outer = appearance_.outerCornerRadius;
                    const float radii[4] = {top && left ? outer : inner, top && right ? outer : inner,
                                            bottom && right ? outer : inner, bottom && left ? outer : inner};
                    FillRoundedRectCorners(deviceResources, itemRect, radii, fill);
                }
                else
                {
                    FillRoundedRect(deviceResources, itemRect, appearance_.cornerRadius, fill, fill, 0.0f);
                }
            }

            if (appearance_.showSelectedBar && (selected || pressed))
            {
                const float barWidth = (std::max)(appearance_.selectedBarWidth, 3.0f);
                const float barHeight = (std::max)(appearance_.selectedBarHeight, barWidth * 2.0f);
                const float barY = itemRect.y + std::max((itemRect.height - barHeight) * 0.5f, 0.0f);
                const float barX = appearance_.selectedBarInside ? itemRect.x : itemRect.x - barWidth * 0.5f;
                const RectF selectedBar = {barX, barY, barWidth, barHeight};
                FillRoundedRect(deviceResources, selectedBar, barWidth * 0.5f, appearance_.selectedBarColor,
                                appearance_.selectedBarColor, 0.0f);
            }
        }

        const float translationFontSize = appearance_.fontSize * appearance_.translationFontSizeScale;
        const auto &geometry = itemGeometry_[index];
        const auto absolute = [&](RectF rect) {
            rect.x += itemRect.x;
            rect.y += itemRect.y;
            return rect;
        };
        const RectF labelRect = absolute(geometry.label);
        const RectF tagRect = absolute(geometry.tag);
        const RectF textRect = absolute(geometry.text);
        const RectF annotationRect = absolute(geometry.annotation);
        const RectF translationRect = absolute(geometry.translation);

        const Item &item = items_[index];
        const ComPtr<IDWriteTextLayout> labelLayout =
            TextLayoutFor(factory, fontFamily, item.label, appearance_.labelFontSize, labelRect);
        const ComPtr<IDWriteTextLayout> tagLayout =
            item.tag.empty() ? nullptr : TextLayoutFor(factory, fontFamily, item.tag, appearance_.tagFontSize, tagRect);
        const ComPtr<IDWriteTextLayout> textLayout =
            TextLayoutFor(factory, fontFamily, item.text, appearance_.fontSize, textRect);
        const ComPtr<IDWriteTextLayout> annotationLayout =
            TextLayoutFor(factory, fontFamily, item.annotation, appearance_.annotationFontSize, annotationRect);
        const ComPtr<IDWriteTextLayout> translationLayout =
            TextLayoutFor(factory, fontFamily, item.translation, translationFontSize, translationRect);

        const bool highlighted = selected || pressed;
        const D2D1_COLOR_F &labelColor = highlighted && appearance_.rowLabelSelected.a > 0.001f
                                             ? appearance_.rowLabelSelected
                                             : appearance_.labelColor;
        const D2D1_COLOR_F &tagColor =
            highlighted && appearance_.rowTagSelected.a > 0.001f ? appearance_.rowTagSelected : appearance_.tagColor;
        const D2D1_COLOR_F &textColor =
            highlighted && appearance_.rowTextSelected.a > 0.001f ? appearance_.rowTextSelected : appearance_.textColor;
        ID2D1SolidColorBrush *labelBrush = deviceResources.GetSolidColorBrush(labelColor);
        ID2D1SolidColorBrush *tagBrush = item.tag.empty() ? nullptr : deviceResources.GetSolidColorBrush(tagColor);
        ID2D1SolidColorBrush *textBrush = deviceResources.GetSolidColorBrush(textColor);
        // 辅助码与翻译在 CSS 里都是 .text 的子节点（.cand-content / .cand-translation），
        // 颜色继承自 .text，选中行的 `.first .text { color: ... }` 会一并覆盖它们。
        // D2D 端分开绘制，所以这里显式跟随选中行文字色，translation 再乘 CSS 的 opacity .62。
        const D2D1_COLOR_F &annotationColor = highlighted && appearance_.rowTextSelected.a > 0.001f
                                                  ? appearance_.rowTextSelected
                                                  : appearance_.annotationColor;
        ID2D1SolidColorBrush *annotationBrush = deviceResources.GetSolidColorBrush(annotationColor);
        D2D1_COLOR_F translationColor = highlighted && appearance_.rowTranslationSelected.a > 0.001f
                                            ? appearance_.rowTranslationSelected
                                            : appearance_.translationColor;
        if (translationColor.a <= 0.001f)
        {
            translationColor = annotationColor;
            translationColor.a *= 0.62f;
        }
        ID2D1SolidColorBrush *translationBrush = deviceResources.GetSolidColorBrush(translationColor);
        if (translationLayout)
        {
            // 布局按文本缓存、各行共用，所以每次绘制都先清掉上一次留下的分段画刷。
            translationLayout->SetDrawingEffect(nullptr, {0, static_cast<UINT32>(item.translation.size())});
        }
        if (translationLayout && !item.translationRuns.empty())
        {
            // 选中行先看选中专用色，再看普通色，都没有就与本行翻译同色，对应 CSS 里
            // .first .cand-pos → .cand-pos → 继承 .cand-translation。
            const auto runColor = [&](const D2D1_COLOR_F &selectedColor, const D2D1_COLOR_F &normalColor) {
                if (highlighted && selectedColor.a > 0.001f)
                    return selectedColor;
                return normalColor.a > 0.001f ? normalColor : translationColor;
            };
            ID2D1SolidColorBrush *secondaryBrush = deviceResources.GetSolidColorBrush(
                runColor(appearance_.rowTranslationSecondarySelected, appearance_.translationSecondaryColor));
            ID2D1SolidColorBrush *delimiterBrush = deviceResources.GetSolidColorBrush(
                runColor(appearance_.rowTranslationDelimiterSelected, appearance_.translationDelimiterColor));
            for (const TextRun &run : item.translationRuns)
            {
                translationLayout->SetDrawingEffect(
                    run.role == TextRunRole::Delimiter ? delimiterBrush : secondaryBrush, {run.start, run.length});
            }
        }
        if (labelLayout && labelBrush)
        {
            target->DrawTextLayout(D2D1::Point2F(labelRect.x, labelRect.y), labelLayout.Get(), labelBrush,
                                   kColorClipTextOptions);
        }
        if (tagLayout && tagBrush)
        {
            target->DrawTextLayout(D2D1::Point2F(tagRect.x, tagRect.y), tagLayout.Get(), tagBrush,
                                   kColorClipTextOptions);
        }
        if (textLayout && textBrush)
        {
            target->DrawTextLayout(D2D1::Point2F(textRect.x, textRect.y), textLayout.Get(), textBrush,
                                   kColorClipTextOptions);
        }
        if (annotationLayout && annotationBrush)
        {
            target->DrawTextLayout(D2D1::Point2F(annotationRect.x, annotationRect.y), annotationLayout.Get(),
                                   annotationBrush, kColorClipTextOptions);
        }
        if (translationLayout && translationBrush)
        {
            target->DrawTextLayout(D2D1::Point2F(translationRect.x, translationRect.y), translationLayout.Get(),
                                   translationBrush, kColorClipTextOptions);
        }
    }
}

bool CandidateList::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

bool CandidateList::IsFocusable() const
{
    return true;
}

void CandidateList::OnFocusChanged(bool focused)
{
    focused_ = focused;
    InvalidateVisual();
}

bool CandidateList::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const size_t hit = HitTestItem(dipPoint);
    if (hit == static_cast<size_t>(-1))
    {
        return false;
    }

    pressed_ = true;
    pressedIndex_ = hit;
    InvalidateVisual();
    return true;
}

bool CandidateList::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !pressed_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const size_t hit = HitTestItem(dipPoint);
    const size_t pressedIndex = pressedIndex_;
    pressed_ = false;
    pressedIndex_ = static_cast<size_t>(-1);

    if (hit != static_cast<size_t>(-1) && hit == pressedIndex)
    {
        SetSelectedIndex(hit);
        if (onItemActivated_)
        {
            const auto handler = onItemActivated_;
            handler(hit);
        }
    }

    InvalidateVisual();
    return true;
}

bool CandidateList::OnContextMenu(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const size_t hit = HitTestItem(dipPoint);
    if (hit == static_cast<size_t>(-1))
    {
        return false;
    }

    SetSelectedIndex(hit);
    InvalidateVisual();
    if (onContextMenu_)
    {
        const auto handler = onContextMenu_;
        handler(hit, point);
    }
    return true;
}

bool CandidateList::OnMouseMove(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !hoverEnabled_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const size_t hit = HitTestItem(dipPoint);
    if (hoveredIndex_ == hit)
    {
        return hit != static_cast<size_t>(-1);
    }

    hoveredIndex_ = hit;
    InvalidateVisual();
    return hit != static_cast<size_t>(-1);
}

void CandidateList::OnMouseLeave()
{
    if (hoveredIndex_ == static_cast<size_t>(-1))
    {
        return;
    }

    hoveredIndex_ = static_cast<size_t>(-1);
    InvalidateVisual();
}

bool CandidateList::OnKeyDown(WPARAM key, LPARAM lParam)
{
    (void)lParam;
    if (items_.empty())
    {
        return false;
    }

    size_t next = selectedIndex_;
    switch (key)
    {
    case VK_UP:
    case VK_LEFT:
        next = selectedIndex_ == 0 ? items_.size() - 1 : selectedIndex_ - 1;
        break;
    case VK_DOWN:
    case VK_RIGHT:
        next = selectedIndex_ + 1 >= items_.size() ? 0 : selectedIndex_ + 1;
        break;
    case VK_HOME:
        next = 0;
        break;
    case VK_END:
        next = items_.size() - 1;
        break;
    case VK_PRIOR:
        next = selectedIndex_ > 3 ? selectedIndex_ - 3 : 0;
        break;
    case VK_NEXT:
        next = (std::min)(selectedIndex_ + 3, items_.size() - 1);
        break;
    default:
        return false;
    }
    SetSelectedIndex(next);
    return true;
}

HCURSOR CandidateList::GetCursor() const
{
    return LoadCursor(nullptr, IDC_ARROW);
}

size_t CandidateList::HitTestItem(const PointF &point) const
{
    if (!HitTest(point) || items_.empty())
    {
        return static_cast<size_t>(-1);
    }

    for (size_t i = 0; i < items_.size(); ++i)
    {
        const RectF rect = ItemRect(i);
        if (PointInRect(rect, point))
        {
            return i;
        }
    }
    return static_cast<size_t>(-1);
}
} // namespace msimeui
