#include "settings/native/native_page_help_widgets.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Scene.h"
#include "msimeui/Window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

// partials/stats.html + modules/stats.ts + styles/modules/stats.css
namespace native_settings
{
namespace
{
constexpr float kCalendarCell = 13.0f;      // CALENDAR_CELL_PX
constexpr float kCalendarGap = 2.0f;        // CALENDAR_GAP_PX
constexpr float kCalendarMinCell = 9.0f;    // CALENDAR_MIN_CELL_PX
constexpr int kCalendarMinWeeks = 53;       // CALENDAR_MIN_WEEKS
constexpr int kCalendarMaxWeeks = 200;      // CALENDAR_MAX_WEEKS
constexpr int kMinMonthLabelGapWeeks = 3;   // CALENDAR_MIN_MONTH_LABEL_GAP_WEEKS
constexpr size_t kDetailDays = 30;          // DETAIL_DAYS
constexpr double kStatusTimeoutMs = 4000.0; // STATUS_TIMEOUT_MS
constexpr double kPollMs = 5000.0;          // OVERVIEW_POLL_MS
constexpr double kMinIntervalMs = 1000.0;   // OVERVIEW_MIN_INTERVAL_MS
constexpr double kStaleMs = 15000.0;        // OVERVIEW_STALE_MS
// `@media (max-width: 820px)`: the viewport is the whole client area.
constexpr float kNarrowViewport = 820.0f;

// ------------------------------------------------------------------ pure helpers

int64_t JsonInt(const Json &object, const char *key)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number())
        return 0;
    return it->is_number_float() ? static_cast<int64_t>(it->get<double>()) : it->get<int64_t>();
}

double JsonNumber(const Json &object, const char *key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_number() ? it->get<double>() : 0.0;
}

// Days since 1970-01-01 for a civil date (proleptic Gregorian).
int64_t DaysFromCivil(int64_t y, int64_t m, int64_t d)
{
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

struct Civil
{
    int year = 1970;
    int month = 1;
    int day = 1;
};

Civil CivilFromDays(int64_t z)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp + (mp < 10 ? 3 : -9);
    return {static_cast<int>(y + (m <= 2 ? 1 : 0)), static_cast<int>(m), static_cast<int>(d)};
}

int64_t DayNumber(int64_t dayKey)
{
    return DaysFromCivil(dayKey / 10000, (dayKey / 100) % 100, dayKey % 100);
}

int64_t DayKeyOf(int64_t dayNumber)
{
    const Civil c = CivilFromDays(dayNumber);
    return c.year * 10000LL + c.month * 100LL + c.day;
}

// 0 = Sunday, like Date.getDay().
int WeekdayOf(int64_t dayNumber)
{
    // 1970-01-01 was a Thursday.
    return static_cast<int>(((dayNumber % 7) + 7 + 4) % 7);
}

std::wstring Pad2(int value)
{
    return value < 10 ? L"0" + std::to_wstring(value) : std::to_wstring(value);
}

std::wstring FormatDayKey(int64_t dayKey)
{
    if (!dayKey)
        return L"-";
    return std::to_wstring(dayKey / 10000) + L"-" + Pad2(static_cast<int>((dayKey / 100) % 100)) + L"-" +
           Pad2(static_cast<int>(dayKey % 100));
}

// `Math.round(value).toLocaleString('zh-CN')`
std::wstring FormatNumber(double value)
{
    const int64_t rounded = static_cast<int64_t>(std::floor(value + 0.5));
    std::wstring digits = std::to_wstring(rounded < 0 ? -rounded : rounded);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
        digits.insert(static_cast<size_t>(i), L",");
    return rounded < 0 ? L"-" + digits : digits;
}

std::wstring FormatDuration(double ms)
{
    if (ms <= 0.0)
        return L"0 分钟";
    const double minutes = ms / 60000.0;
    if (minutes < 1.0)
        return L"不足 1 分钟";
    if (minutes < 60.0)
    {
        if (minutes < 10.0)
        {
            wchar_t buffer[32] = {};
            std::swprintf(buffer, 32, L"%.1f 分钟", minutes);
            return buffer;
        }
        return std::to_wstring(static_cast<int64_t>(std::floor(minutes + 0.5))) + L" 分钟";
    }
    const int64_t hours = static_cast<int64_t>(minutes / 60.0);
    const int64_t rest = static_cast<int64_t>(std::floor(std::fmod(minutes, 60.0) + 0.5));
    return std::to_wstring(hours) + L" 小时 " + std::to_wstring(rest) + L" 分";
}

std::wstring FormatSpeed(double charsPerMinute)
{
    return std::to_wstring(static_cast<int64_t>(std::floor(charsPerMinute + 0.5))) + L" 字/分";
}

std::wstring FormatRatio(const Json *ratio)
{
    if (!ratio || !ratio->is_number())
        return L"–";
    return std::to_wstring(static_cast<int64_t>(std::floor(ratio->get<double>() * 100.0 + 0.5))) + L"%";
}

int64_t TotalOf(const Json &row)
{
    return JsonInt(row, "cjk") + JsonInt(row, "latin") + JsonInt(row, "digit") + JsonInt(row, "punct") +
           JsonInt(row, "other");
}

double SpeedOf(const Json &row)
{
    const double readable = static_cast<double>(JsonInt(row, "cjk") + JsonInt(row, "latin"));
    const double active = static_cast<double>(JsonInt(row, "activeMs"));
    if (readable <= 0.0 || active <= 0.0)
        return 0.0;
    return readable / (std::max(active, 60000.0) / 60000.0);
}

int CalendarLevel(int64_t chars, int64_t maxChars)
{
    if (chars <= 0 || maxChars <= 0)
        return 0;
    const double ratio = static_cast<double>(chars) / static_cast<double>(maxChars);
    if (ratio <= 0.25)
        return 1;
    if (ratio <= 0.5)
        return 2;
    if (ratio <= 0.75)
        return 3;
    return 4;
}

int CalendarWeeks(float width)
{
    const int weeks = static_cast<int>(std::floor((width + kCalendarGap) / (kCalendarCell + kCalendarGap)));
    return std::max(kCalendarMinWeeks, std::min(kCalendarMaxWeeks, weeks));
}

float CalendarCell(float width, int weeks)
{
    const float available = width - kCalendarGap * static_cast<float>(weeks - 1);
    const float cell = std::floor(available / static_cast<float>(weeks));
    return std::max(kCalendarMinCell, std::min(kCalendarCell, cell));
}

D2D1_COLOR_F LevelColor(int level)
{
    const Palette &p = CurrentPalette();
    static const float kAlpha[] = {0.0f, 0.28f, 0.48f, 0.7f, 0.95f};
    return level <= 0 ? p.surfaceSunken : WithAlpha(p.accent, kAlpha[level]);
}

float ViewportWidth(msimeui::Window *window)
{
    if (!window || !window->GetHandle())
        return 1000.0f;
    RECT rc{};
    GetClientRect(window->GetHandle(), &rc);
    return msimeui::PixelsToDips(static_cast<float>(rc.right - rc.left), window->GetDpi());
}

float Baseline(TextLayout &layout, float width)
{
    IDWriteTextLayout *dw = layout.Get(width);
    if (!dw)
        return 0.0f;
    DWRITE_LINE_METRICS line{};
    UINT32 count = 0;
    dw->GetLineMetrics(&line, 1, &count);
    return count > 0 ? line.baseline : 0.0f;
}

void FillCardBox(msimeui::DeviceResources &dr, msimeui::ShadowCache &shadow, const RectF &box)
{
    const Palette &p = CurrentPalette();
    msimeui::ShadowPass passes[2] = {};
    for (int i = 0; i < p.cardShadowCount; ++i)
        passes[i] = {p.cardShadow[i].sigma, p.cardShadow[i].alpha, 0.0f, p.cardShadow[i].offsetY};
    shadow.Render(dr.GetRenderTarget(), box, 8.0f, passes, static_cast<size_t>(p.cardShadowCount));
    FillRounded(dr, box, 8.0f, Over(p.cardBg, p.chromeBg));
    StrokeRounded(dr, box, 8.0f, p.border, 1.0f);
}

// ------------------------------------------------------------------ visuals

// `.stats-status`: toast-like box above the content (`position: sticky` is not
// reproduced; it sits in the flow where the page puts it).
class StatusBox : public msimeui::Visual
{
  public:
    StatusBox()
    {
        text_.Set(L"", DocStyle(kFontCaption, DWRITE_FONT_WEIGHT_NORMAL, 0.0f, true));
        SetMargin({0.0f, 0.0f, 0.0f, 18.0f});
    }
    void Set(const std::wstring &text, bool error)
    {
        text_.SetText(text);
        error_ = error;
        InvalidateMeasure();
        InvalidateVisual();
    }
    SizeF Measure(const SizeF &availableSize) override
    {
        const float border = CssBorder();
        const SizeF text = text_.Measure(std::max(availableSize.width - 28.0f - border * 2.0f, 1.0f));
        return {availableSize.width, text.height + 18.0f + border * 2.0f};
    }
    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }
    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        const float border = CssBorder();
        if (error_)
        {
            FillRounded(dr, bounds_, 8.0f, Over(Rgba(255, 119, 119, 0.12f), Over(p.chromeBg, p.chromeBg)));
            StrokeRounded(dr, bounds_, 8.0f, Rgba(255, 119, 119, 0.35f), 1.0f);
        }
        else
        {
            msimeui::ShadowPass passes[2] = {};
            for (int i = 0; i < p.cardShadowCount; ++i)
                passes[i] = {p.cardShadow[i].sigma, p.cardShadow[i].alpha, 0.0f, p.cardShadow[i].offsetY};
            shadow_.Render(dr.GetRenderTarget(), bounds_, 8.0f, passes, static_cast<size_t>(p.cardShadowCount));
            FillRounded(dr, bounds_, 8.0f, p.toastBg);
            StrokeRounded(dr, bounds_, 8.0f, p.accentSoftBorder, 1.0f);
        }
        const float width = bounds_.width - 28.0f - border * 2.0f;
        text_.Draw(dr, {bounds_.x + 14.0f + border, bounds_.y + 9.0f + border}, error_ ? p.dangerText : p.toastText,
                   width);
    }

  private:
    TextLayout text_;
    bool error_ = false;
    msimeui::ShadowCache shadow_;
};

// `.stats-cards`: grid of five `.stats-card`, auto-fit with minmax(min(160px, 100%), 1fr).
class CardsGrid : public msimeui::Visual
{
  public:
    struct Card
    {
        std::wstring title, value, unit, note;
    };

    CardsGrid()
    {
        SetMargin({0.0f, 0.0f, 0.0f, 18.0f});
    }

    void SetCards(std::vector<Card> cards)
    {
        cards_ = std::move(cards);
        shadows_.resize(cards_.size());
        InvalidateMeasure();
        InvalidateVisual();
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        const int columns = Columns(availableSize.width);
        const int rows = cards_.empty() ? 0 : static_cast<int>((cards_.size() + columns - 1) / columns);
        return {availableSize.width, rows * CardHeight() + std::max(rows - 1, 0) * 12.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        const int columns = Columns(bounds_.width);
        const float cardWidth = (bounds_.width - 12.0f * static_cast<float>(columns - 1)) / static_cast<float>(columns);
        const float height = CardHeight();
        const float border = CssBorder();
        for (size_t i = 0; i < cards_.size(); ++i)
        {
            const int row = static_cast<int>(i) / columns;
            const int column = static_cast<int>(i) % columns;
            const RectF box = {bounds_.x + column * (cardWidth + 12.0f), bounds_.y + row * (height + 12.0f), cardWidth,
                               height};
            FillCardBox(dr, shadows_[i], box);
            const float innerX = box.x + 16.0f + border;
            const float innerWidth = std::max(box.width - 32.0f - border * 2.0f, 1.0f);
            float y = box.y + 14.0f + border;
            ID2D1RenderTarget *target = dr.GetRenderTarget();
            target->PushAxisAlignedClip(ToD2D({innerX, box.y, innerWidth, box.height}), D2D1_ANTIALIAS_MODE_ALIASED);
            TextLayout title;
            title.Set(cards_[i].title, DocStyle(kFontCaption));
            title.Draw(dr, {innerX, y}, p.textMuted, innerWidth);
            y += LineHeight(DocStyle(kFontCaption)) + 6.0f;
            // .stats-card-value: strong 24/600 and the unit on the same baseline, gap 4px.
            TextLayout value;
            value.Set(cards_[i].value, DocStyle(24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
            TextLayout unit;
            unit.Set(cards_[i].unit, DocStyle(12.0f));
            const float valueBaseline = Baseline(value, innerWidth);
            const float unitBaseline = Baseline(unit, innerWidth);
            const float valueWidth = value.Measure(innerWidth).width;
            value.Draw(dr, {innerX, y}, p.text, innerWidth);
            if (!cards_[i].unit.empty())
                unit.Draw(dr, {innerX + valueWidth + 4.0f, y + valueBaseline - unitBaseline}, p.textMuted, innerWidth);
            y += ValueRowHeight() + 6.0f;
            TextLayout note;
            note.Set(cards_[i].note, DocStyle(12.0f));
            note.Draw(dr, {innerX, y}, p.textSubtle, innerWidth);
            target->PopAxisAlignedClip();
        }
    }

  private:
    static int Columns(float width)
    {
        const int fit = static_cast<int>(std::floor((width + 12.0f) / (std::min(160.0f, width) + 12.0f)));
        return std::max(1, std::min(5, fit));
    }

    static float ValueRowHeight()
    {
        // Baseline-aligned row: the 24px number dominates both above and below the baseline.
        return LineHeight(DocStyle(24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
    }

    static float CardHeight()
    {
        return CssBorder() * 2.0f + 28.0f + LineHeight(DocStyle(kFontCaption)) + 6.0f + ValueRowHeight() + 6.0f +
               std::max(18.0f, LineHeight(DocStyle(12.0f)));
    }

    std::vector<Card> cards_;
    std::vector<msimeui::ShadowCache> shadows_;
};

// `.stats-speed-grid`: three `.stats-speed-item` (two under 820px).
class SpeedGrid : public msimeui::Visual
{
  public:
    struct Item
    {
        std::wstring label, value, note;
    };

    SpeedGrid()
    {
        SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
    }

    void SetItems(std::vector<Item> items)
    {
        items_ = std::move(items);
        InvalidateMeasure();
        InvalidateVisual();
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        const int columns = Columns();
        const int rows = items_.empty() ? 0 : static_cast<int>((items_.size() + columns - 1) / columns);
        return {availableSize.width, rows * ItemHeight() + std::max(rows - 1, 0) * 12.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        const int columns = Columns();
        const float width = (bounds_.width - 12.0f * static_cast<float>(columns - 1)) / static_cast<float>(columns);
        const float height = ItemHeight();
        const float border = CssBorder();
        for (size_t i = 0; i < items_.size(); ++i)
        {
            const int row = static_cast<int>(i) / columns;
            const int column = static_cast<int>(i) % columns;
            const RectF box = {bounds_.x + column * (width + 12.0f), bounds_.y + row * (height + 12.0f), width, height};
            FillRounded(dr, box, 8.0f, p.surfaceSubtle);
            StrokeRounded(dr, box, 8.0f, p.border, 1.0f);
            const float x = box.x + 14.0f + border;
            const float innerWidth = std::max(box.width - 28.0f - border * 2.0f, 1.0f);
            float y = box.y + 10.0f + border;
            TextLayout text;
            text.Set(items_[i].label, DocStyle(kFontCaption));
            text.Draw(dr, {x, y}, p.textMuted, innerWidth);
            y += LineHeight(DocStyle(kFontCaption)) + 2.0f;
            text.Set(items_[i].value, DocStyle(19.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
            text.Draw(dr, {x, y}, p.text, innerWidth);
            y += LineHeight(DocStyle(19.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 2.0f;
            text.Set(items_[i].note, DocStyle(12.0f));
            text.Draw(dr, {x, y}, p.textSubtle, innerWidth);
        }
    }

  private:
    int Columns() const
    {
        return ViewportWidth(window_) <= kNarrowViewport ? 2 : 3;
    }

    static float ItemHeight()
    {
        return CssBorder() * 2.0f + 20.0f + LineHeight(DocStyle(kFontCaption)) * 2.0f +
               LineHeight(DocStyle(19.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 4.0f;
    }

    std::vector<Item> items_;
};

// The cell `title` of the heatmap, shown as an overlay above the hovered cell so that
// neither the content scroller nor the sections below can cover it.
class CalendarTip : public msimeui::Visual
{
  public:
    // Cleared by the scene when it drops its popups (page switch, a click elsewhere).
    bool shown = false;

    void Set(const std::wstring &text, const RectF &anchor)
    {
        if (text != text_)
        {
            text_ = text;
            layout_.Set(text_, DocStyle(kFontCaption));
        }
        anchor_ = anchor;
    }

    void SetAnchor(const RectF &anchor)
    {
        anchor_ = anchor;
    }

    SizeF Measure(const SizeF &) override
    {
        return {};
    }
    void Arrange(const RectF &) override
    {
    }
    void LayoutOverlay(const SizeF &viewportSize) override
    {
        viewport_ = viewportSize;
    }
    // Never takes the pointer: the cell under it keeps its hover.
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

    void Render(msimeui::DeviceResources &dr) override
    {
        if (text_.empty())
            return;
        const Palette &p = CurrentPalette();
        const float border = CssBorder();
        const SizeF text = layout_.Measure(kUnboundedWidth);
        const float width = text.width + 16.0f + border * 2.0f;
        const float height = text.height + 8.0f + border * 2.0f;
        float x = anchor_.x + (anchor_.width - width) * 0.5f;
        float y = anchor_.y - height - 6.0f;
        if (viewport_.width > 0.0f)
            x = std::clamp(x, 4.0f, std::max(viewport_.width - width - 4.0f, 4.0f));
        // Flip below the cell when there is no room above it (under the title bar).
        if (y < 36.0f)
            y = anchor_.y + anchor_.height + 6.0f;
        const RectF box = {x, y, width, height};
        FillRounded(dr, box, 4.0f, p.tooltipBg);
        StrokeRounded(dr, box, 4.0f, p.tooltipBorder, 1.0f);
        layout_.Draw(dr, {x + 8.0f + border, y + 4.0f + border}, p.tooltipText, kUnboundedWidth);
    }

  private:
    static constexpr float kUnboundedWidth = 10000.0f;
    std::wstring text_;
    TextLayout layout_;
    RectF anchor_ = {};
    SizeF viewport_ = {};
};

// `.stats-calendar-scroll` with the month labels and the week columns. It scrolls
// sideways under the page's 8px `::-webkit-scrollbar` and starts pinned to today like
// pinCalendarToToday; it stays pinned until the user scrolls away from the right end.
class CalendarView : public msimeui::Visual
{
  public:
    CalendarView() : tip_(std::make_shared<CalendarTip>())
    {
        SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
    }

    void SetData(std::map<int64_t, int64_t> chars, int64_t maxChars, int64_t todayDayKey)
    {
        chars_ = std::move(chars);
        maxChars_ = maxChars;
        today_ = todayDayKey;
        // The columns may have moved under the pointer; the next move shows it again.
        HideTip();
        InvalidateMeasure();
        InvalidateVisual();
    }

    float CellSize() const
    {
        return cell_;
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        const int weeks = CalendarWeeks(availableSize.width);
        cell_ = CalendarCell(availableSize.width, weeks + 1);
        overflow_ = ContentWidth(availableSize.width) > availableSize.width + 0.5f;
        return {availableSize.width, GridHeight() + 15.0f + 4.0f + (overflow_ ? kScrollbarHeight : 0.0f)};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        if (!today_)
            return;
        const Palette &p = CurrentPalette();
        const int64_t today = DayNumber(today_);
        const int64_t start = FirstDay(bounds_.width);
        const size_t columns = static_cast<size_t>((today - start) / 7 + 1);
        const float column = cell_ + kCalendarGap;
        const float x0 = ContentLeft();
        dr.PushClip({bounds_.x, bounds_.y, bounds_.width, bounds_.height});

        // Month labels: a column containing the 1st of a month; the first column gets
        // the starting month when the next boundary is far enough away.
        std::vector<std::wstring> labels(columns);
        for (size_t i = 0; i < columns; ++i)
        {
            for (int d = 0; d < 7; ++d)
            {
                const Civil c = CivilFromDays(start + static_cast<int64_t>(i) * 7 + d);
                if (c.day == 1)
                {
                    labels[i] = std::to_wstring(c.month) + L"月";
                    break;
                }
            }
        }
        if (!labels.empty() && labels[0].empty())
        {
            size_t boundary = labels.size();
            for (size_t i = 0; i < labels.size(); ++i)
            {
                if (!labels[i].empty())
                {
                    boundary = i;
                    break;
                }
            }
            if (static_cast<int>(boundary) >= kMinMonthLabelGapWeeks)
                labels[0] = std::to_wstring(CivilFromDays(start).month) + L"月";
        }
        TextStyle month = DocStyle(10.0f, DWRITE_FONT_WEIGHT_NORMAL, 1.5f);
        for (size_t i = 0; i < labels.size(); ++i)
        {
            const float x = x0 + static_cast<float>(i) * column;
            if (labels[i].empty() || x > bounds_.x + bounds_.width || x + 40.0f < bounds_.x)
                continue;
            TextLayout text;
            text.Set(labels[i], month);
            text.Draw(dr, {x, bounds_.y}, p.textSubtle, 200.0f);
        }

        for (size_t i = 0; i < columns; ++i)
        {
            const float x = x0 + static_cast<float>(i) * column;
            if (x > bounds_.x + bounds_.width || x + cell_ < bounds_.x)
                continue;
            for (int d = 0; d < 7; ++d)
            {
                const int64_t day = start + static_cast<int64_t>(i) * 7 + d;
                const RectF cell = CellRect(i, d);
                if (day > today)
                {
                    DrawDashedOutline(dr, cell, p.border);
                    continue;
                }
                FillRounded(dr, cell, 3.0f, LevelColor(CalendarLevel(CharsOn(day), maxChars_)));
            }
        }
        dr.PopClip();

        if (overflow_)
        {
            // ::-webkit-scrollbar-thumb: no track, 2px radius, hover color on the thumb.
            const bool hot = dragging_ || thumbHot_;
            FillRounded(dr, ThumbRect(), 2.0f, hot ? p.scrollbarThumbHover : p.scrollbarThumb);
        }
        // Keep the tooltip on its cell when the page scrolls under a still pointer.
        if (tip_->shown && hoverDay_ >= start && hoverDay_ <= today)
            tip_->SetAnchor(
                CellRect(static_cast<size_t>((hoverDay_ - start) / 7), static_cast<int>((hoverDay_ - start) % 7)));
    }

    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        if (!window_ || !overflow_)
            return false;
        const PointF dip = window_->ClientPixelsToDips(point);
        if (!Contains(TrackRect(), dip))
            return false;
        HideTip();
        const RectF thumb = ThumbRect();
        if (Contains(thumb, dip))
        {
            dragging_ = true;
            dragGrab_ = dip.x - thumb.x;
        }
        else
        {
            // A click on the track pages towards it, as Chrome does.
            const float page = std::max(bounds_.width * 0.875f, 40.0f);
            SetScroll(Scroll() + (dip.x < thumb.x ? -page : page));
        }
        InvalidateVisual();
        return true;
    }

    bool OnMouseUp(const POINT &, WPARAM) override
    {
        if (!dragging_)
            return false;
        dragging_ = false;
        InvalidateVisual();
        return true;
    }

    bool OnMouseMove(const POINT &point, WPARAM) override
    {
        if (!window_)
            return false;
        const PointF dip = window_->ClientPixelsToDips(point);
        if (dragging_)
        {
            const RectF track = TrackRect();
            const float travel = track.width - ThumbRect().width;
            const float maxScroll = MaxScroll();
            if (travel > 0.0f)
                SetScroll((dip.x - dragGrab_ - track.x) / travel * maxScroll);
            return true;
        }
        const bool thumbHot = overflow_ && Contains(ThumbRect(), dip);
        if (thumbHot != thumbHot_)
        {
            thumbHot_ = thumbHot;
            InvalidateVisual();
        }
        UpdateTip(dip);
        return true;
    }

    void OnMouseLeave() override
    {
        HideTip();
        if (thumbHot_)
        {
            thumbHot_ = false;
            InvalidateVisual();
        }
    }

    bool OnMouseWheel(const POINT &, short delta, WPARAM keyState) override
    {
        HideTip();
        // Shift+wheel scrolls sideways; a plain wheel keeps scrolling the page.
        if (!overflow_ || !(keyState & MK_SHIFT))
            return false;
        SetScroll(Scroll() - static_cast<float>(delta) / WHEEL_DELTA * 100.0f);
        return true;
    }

  private:
    static constexpr float kScrollbarHeight = 8.0f; // ::-webkit-scrollbar { height: 8px }
    static constexpr float kThumbMinLength = 24.0f;

    // The Monday on or before the first day, so each column is one week.
    int64_t FirstDay(float width) const
    {
        const int weeks = CalendarWeeks(width);
        int64_t start = DayNumber(today_) - (weeks * 7 - 1);
        start -= (WeekdayOf(start) + 6) % 7;
        return start;
    }

    float ContentWidth(float width) const
    {
        int64_t columns = CalendarWeeks(width);
        if (today_)
            columns = (DayNumber(today_) - FirstDay(width)) / 7 + 1;
        return static_cast<float>(columns) * (cell_ + kCalendarGap) - kCalendarGap;
    }

    float GridHeight() const
    {
        return 7.0f * cell_ + 6.0f * kCalendarGap;
    }

    float MaxScroll() const
    {
        return std::max(ContentWidth(bounds_.width) - bounds_.width, 0.0f);
    }

    float Scroll() const
    {
        return pinned_ ? MaxScroll() : std::clamp(scroll_, 0.0f, MaxScroll());
    }

    void SetScroll(float value)
    {
        const float maxScroll = MaxScroll();
        const float previous = Scroll();
        scroll_ = std::clamp(value, 0.0f, maxScroll);
        pinned_ = scroll_ >= maxScroll - 0.5f;
        if (Scroll() != previous)
            InvalidateVisual();
    }

    float ContentLeft() const
    {
        return bounds_.x - Scroll();
    }

    RectF CellRect(size_t column, int day) const
    {
        return {ContentLeft() + static_cast<float>(column) * (cell_ + kCalendarGap),
                bounds_.y + 15.0f + static_cast<float>(day) * (cell_ + kCalendarGap), cell_, cell_};
    }

    RectF TrackRect() const
    {
        return {bounds_.x, bounds_.y + 15.0f + GridHeight() + 4.0f, bounds_.width, kScrollbarHeight};
    }

    RectF ThumbRect() const
    {
        const RectF track = TrackRect();
        const float content = std::max(ContentWidth(bounds_.width), 1.0f);
        const float length = std::min(std::max(kThumbMinLength, track.width * track.width / content), track.width);
        const float maxScroll = MaxScroll();
        const float x = track.x + (maxScroll > 0.0f ? (track.width - length) * Scroll() / maxScroll : 0.0f);
        return {x, track.y, length, track.height};
    }

    int64_t CharsOn(int64_t day) const
    {
        const auto it = chars_.find(DayKeyOf(day));
        return it == chars_.end() ? 0 : it->second;
    }

    // Day number of the cell under `point`, 0 between cells and outside the grid.
    int64_t DayAt(const PointF &point) const
    {
        if (!today_ || !Contains({bounds_.x, bounds_.y + 15.0f, bounds_.width, GridHeight()}, point))
            return 0;
        const float column = cell_ + kCalendarGap;
        const float dx = point.x - ContentLeft();
        const float dy = point.y - (bounds_.y + 15.0f);
        const int64_t c = static_cast<int64_t>(std::floor(dx / column));
        const int64_t d = static_cast<int64_t>(std::floor(dy / column));
        if (c < 0 || d < 0 || d > 6 || dx - c * column >= cell_ || dy - d * column >= cell_)
            return 0;
        const int64_t start = FirstDay(bounds_.width);
        const int64_t day = start + c * 7 + d;
        return day <= start + static_cast<int64_t>((DayNumber(today_) - start) / 7) * 7 + 6 ? day : 0;
    }

    void UpdateTip(const PointF &point)
    {
        const int64_t day = DayAt(point);
        if (!day)
        {
            HideTip();
            return;
        }
        if (day == hoverDay_)
            return;
        hoverDay_ = day;
        if (!tip_->shown)
        {
            // The tooltip waits 500ms on a cell before it shows; once it is up it follows
            // the pointer from cell to cell at once.
            if (window_ && window_->GetHandle())
                tipDelay_ = std::make_unique<UiTimer>(window_->GetHandle(), 500, [this]() {
                    tipDelay_.reset();
                    ShowTip();
                });
            return;
        }
        ShowTip();
    }

    void ShowTip()
    {
        const int64_t day = hoverDay_;
        if (!day)
            return;
        // stats.ts: `${formatDayKey(dayKey)}：${chars > 0 ? `${formatNumber(chars)} 字` : '无记录'}`
        const int64_t chars = CharsOn(day);
        const std::wstring text = FormatDayKey(DayKeyOf(day)) + L"：" +
                                  (chars > 0 ? FormatNumber(static_cast<double>(chars)) + L" 字" : L"无记录");
        const int64_t start = FirstDay(bounds_.width);
        tip_->Set(text, CellRect(static_cast<size_t>((day - start) / 7), static_cast<int>((day - start) % 7)));
        msimeui::Scene *scene = window_ ? window_->GetScene() : nullptr;
        if (!scene)
            return;
        if (!tip_->shown)
        {
            std::weak_ptr<CalendarTip> weak = tip_;
            scene->AddPopup(tip_, [weak]() {
                if (auto tip = weak.lock())
                    tip->shown = false;
            });
            tip_->shown = true;
        }
        window_->Invalidate();
    }

    void HideTip()
    {
        hoverDay_ = 0;
        tipDelay_.reset();
        if (!tip_->shown)
            return;
        tip_->shown = false;
        if (msimeui::Scene *scene = window_ ? window_->GetScene() : nullptr)
            scene->RemovePopup(tip_.get(), false);
    }

    static void DrawDashedOutline(msimeui::DeviceResources &dr, const RectF &cell, const D2D1_COLOR_F &color)
    {
        ID2D1RenderTarget *target = dr.GetRenderTarget();
        ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(color);
        if (!brush)
            return;
        static Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dashed;
        static Microsoft::WRL::ComPtr<ID2D1Factory> factoryOwner;
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        target->GetFactory(factory.GetAddressOf());
        if (!dashed || factoryOwner.Get() != factory.Get())
        {
            dashed.Reset();
            factoryOwner = factory;
            factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                   D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.0f,
                                                                   D2D1_DASH_STYLE_DASH),
                                       nullptr, 0, dashed.GetAddressOf());
        }
        // `outline: 1px dashed` sits just outside the box.
        const float w = CssBorder();
        target->DrawRectangle(D2D1::RectF(cell.x - w * 0.5f, cell.y - w * 0.5f, cell.x + cell.width + w * 0.5f,
                                          cell.y + cell.height + w * 0.5f),
                              brush, w, dashed.Get());
    }

    std::map<int64_t, int64_t> chars_;
    int64_t maxChars_ = 1;
    int64_t today_ = 0;
    float cell_ = kCalendarCell;
    bool overflow_ = false;
    float scroll_ = 0.0f;
    bool pinned_ = true;
    bool dragging_ = false;
    float dragGrab_ = 0.0f;
    bool thumbHot_ = false;
    int64_t hoverDay_ = 0;
    std::shared_ptr<CalendarTip> tip_;
    // Pending show of the tooltip; destroyed (and the timer killed) with the view.
    std::unique_ptr<UiTimer> tipDelay_;
};

// `.stats-legend`: 少 ▢▢▢▢▢ 多, cells as large as the calendar's.
class Legend : public msimeui::Visual
{
  public:
    explicit Legend(std::shared_ptr<CalendarView> calendar) : calendar_(std::move(calendar))
    {
        SetMargin({0.0f, 10.0f, 0.0f, 0.0f});
    }
    SizeF Measure(const SizeF &availableSize) override
    {
        return {availableSize.width, std::max(LineHeight(DocStyle(11.0f)), calendar_->CellSize())};
    }
    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }
    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        const float cell = calendar_->CellSize();
        const TextStyle style = DocStyle(11.0f);
        float x = bounds_.x;
        const auto text = [&](const wchar_t *value) {
            const float width = MeasureTextWidth(value, style);
            DrawTextInRect(dr, value, style, {x, bounds_.y, width + 1.0f, bounds_.height}, p.textSubtle);
            x += width + 4.0f;
        };
        text(L"少");
        for (int level = 0; level <= 4; ++level)
        {
            FillRounded(dr, {x, bounds_.y + (bounds_.height - cell) * 0.5f, cell, cell}, 3.0f, LevelColor(level));
            x += cell + 4.0f;
        }
        text(L"多");
    }

  private:
    std::shared_ptr<CalendarView> calendar_;
};

// `.stats-hourly`: 24 bars, 130px tall including the hour labels.
class HourlyChart : public msimeui::Visual
{
  public:
    HourlyChart()
    {
        SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
    }
    void SetValues(std::vector<int64_t> values)
    {
        values_ = std::move(values);
        InvalidateVisual();
    }
    SizeF Measure(const SizeF &availableSize) override
    {
        return {availableSize.width, 130.0f};
    }
    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }
    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        int64_t max = 1;
        for (int64_t v : values_)
            max = std::max(max, v);
        const float columnWidth = (bounds_.width - 3.0f * 23.0f) / 24.0f;
        const float trackHeight = bounds_.height - 15.0f;
        const TextStyle label = DocStyle(10.0f, DWRITE_FONT_WEIGHT_NORMAL, 1.5f);
        ID2D1RenderTarget *target = dr.GetRenderTarget();
        for (int hour = 0; hour < 24; ++hour)
        {
            const int64_t value = hour < static_cast<int>(values_.size()) ? values_[static_cast<size_t>(hour)] : 0;
            const float x = bounds_.x + hour * (columnWidth + 3.0f);
            const float percent =
                std::max(2.0f, std::round(static_cast<float>(value) / static_cast<float>(max) * 100.0f));
            const float height = trackHeight * percent / 100.0f;
            const RectF bar = {x, bounds_.y + trackHeight - height, columnWidth, height};
            const D2D1_COLOR_F color = value > 0 ? WithAlpha(p.accent, 0.85f) : p.surfaceSunken;
            // border-radius: 3px 3px 1px 1px
            if (ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(color))
                FillBar(target, bar, brush);
            if (hour % 3 == 0)
            {
                TextLayout text;
                TextStyle centered = label;
                centered.align = DWRITE_TEXT_ALIGNMENT_CENTER;
                text.Set(std::to_wstring(hour), centered);
                text.Draw(dr, {x - 10.0f, bounds_.y + trackHeight}, p.textSubtle, columnWidth + 20.0f);
            }
        }
    }

  private:
    // One path, so a translucent bar is not darkened where pieces would overlap.
    static void FillBar(ID2D1RenderTarget *target, const RectF &bar, ID2D1SolidColorBrush *brush)
    {
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        target->GetFactory(factory.GetAddressOf());
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (!factory || FAILED(factory->CreatePathGeometry(path.GetAddressOf())) ||
            FAILED(path->Open(sink.GetAddressOf())))
            return;
        const float top = std::min(3.0f, std::min(bar.width, bar.height) * 0.5f);
        const float bottom = std::min(1.0f, top);
        const float l = bar.x, t = bar.y, r = bar.x + bar.width, b = bar.y + bar.height;
        const auto arc = [&](float x, float y, float radius) {
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y), D2D1::SizeF(radius, radius), 0.0f,
                                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        };
        sink->BeginFigure(D2D1::Point2F(l, t + top), D2D1_FIGURE_BEGIN_FILLED);
        arc(l + top, t, top);
        sink->AddLine(D2D1::Point2F(r - top, t));
        arc(r, t + top, top);
        sink->AddLine(D2D1::Point2F(r, b - bottom));
        arc(r - bottom, b, bottom);
        sink->AddLine(D2D1::Point2F(l + bottom, b));
        arc(l, b - bottom, bottom);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        target->FillGeometry(path.Get(), brush);
    }

    std::vector<int64_t> values_;
};

// `.stats-categories`: grid 34px | bar | stat per category.
class CategoryBars : public msimeui::Visual
{
  public:
    struct Row
    {
        std::wstring name;
        float fraction = 0.0f;
        std::wstring stat;
    };

    CategoryBars()
    {
        SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
    }
    void SetRows(std::vector<Row> rows)
    {
        rows_ = std::move(rows);
        InvalidateMeasure();
        InvalidateVisual();
    }
    SizeF Measure(const SizeF &availableSize) override
    {
        const float row = RowHeight();
        const float count = static_cast<float>(rows_.size());
        return {availableSize.width, rows_.empty() ? 0.0f : count * row + (count - 1.0f) * 9.0f};
    }
    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }
    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        static const D2D1_COLOR_F kColors[] = {
            {0, 0, 0, 0}, Hex(0x4A90D9), Hex(0xD99B3C), Hex(0x9A7AD1), Hex(0x98A2B8)};
        const TextStyle name = DocStyle(kFontCaption);
        const TextStyle stat = DocStyle(12.0f);
        const float rowHeight = RowHeight();
        const float trackX = bounds_.x + 34.0f + 10.0f;
        for (size_t i = 0; i < rows_.size(); ++i)
        {
            // Every `.stats-cat-row` is a grid of its own: the auto column is this row's stat.
            const float statWidth = MeasureTextWidth(rows_[i].stat, stat);
            const float trackWidth = std::max(bounds_.width - 34.0f - 20.0f - statWidth, 0.0f);
            const float y = bounds_.y + static_cast<float>(i) * (rowHeight + 9.0f);
            DrawTextInRect(dr, rows_[i].name, name, {bounds_.x, y, 34.0f, rowHeight}, p.textSecondary);
            const RectF track = {trackX, y + (rowHeight - 10.0f) * 0.5f, trackWidth, 10.0f};
            FillRounded(dr, track, 5.0f, p.surfaceSunken);
            const float width = std::min(track.width, std::max(2.0f, track.width * rows_[i].fraction));
            ID2D1RenderTarget *target = dr.GetRenderTarget();
            target->PushAxisAlignedClip(ToD2D(track), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            FillRounded(dr, {track.x, track.y, width, track.height}, 5.0f, i == 0 ? p.accent : kColors[i % 5]);
            target->PopAxisAlignedClip();
            const float statX = bounds_.x + bounds_.width - statWidth;
            TextStyle right = stat;
            right.align = DWRITE_TEXT_ALIGNMENT_TRAILING;
            TextLayout text;
            text.Set(rows_[i].stat, right);
            text.Draw(dr, {statX, y + (rowHeight - LineHeight(stat)) * 0.5f}, p.textSubtle, statWidth);
        }
    }

  private:
    static float RowHeight()
    {
        return std::max(LineHeight(DocStyle(kFontCaption)), 10.0f);
    }
    std::vector<Row> rows_;
};

// `.stats-detail-table`: auto-layout table at 100% width.
class DetailTable : public msimeui::Visual
{
  public:
    enum class Kind
    {
        Text,
        MonoStrong,
        MonoDim,
        Mono,
    };
    struct Cell
    {
        std::wstring text;
        Kind kind = Kind::Text;
    };

    DetailTable()
    {
        SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
    }
    void SetRows(std::vector<std::vector<Cell>> rows)
    {
        rows_ = std::move(rows);
        InvalidateMeasure();
        InvalidateVisual();
    }
    SizeF Measure(const SizeF &availableSize) override
    {
        return {availableSize.width, HeaderHeight() + static_cast<float>(rows_.size()) * RowHeight()};
    }
    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }
    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        static const wchar_t *kHeaders[] = {L"日期", L"字数", L"中文", L"英文", L"数字",
                                            L"标点", L"其他", L"活跃", L"速度", L"首词"};
        constexpr size_t kColumns = 10;
        // Column widths: max-content plus a share of the spare width.
        float widths[kColumns] = {};
        for (size_t c = 0; c < kColumns; ++c)
            widths[c] = MeasureTextWidth(kHeaders[c], HeaderStyle()) + 16.0f;
        for (const auto &row : rows_)
        {
            for (size_t c = 0; c < kColumns && c < row.size(); ++c)
                widths[c] = std::max(widths[c], MeasureTextWidth(row[c].text, StyleOf(row[c].kind)) + 16.0f);
        }
        float total = 0.0f;
        for (float w : widths)
            total += w;
        if (total < bounds_.width && total > 0.0f)
        {
            const float scale = bounds_.width / total;
            for (float &w : widths)
                w *= scale;
        }
        ID2D1RenderTarget *target = dr.GetRenderTarget();
        target->PushAxisAlignedClip(ToD2D(bounds_), D2D1_ANTIALIAS_MODE_ALIASED);
        const float border = CssBorder();
        float y = bounds_.y;
        float x = bounds_.x;
        const float header = HeaderHeight();
        for (size_t c = 0; c < kColumns; ++c)
        {
            DrawTextInRect(dr, kHeaders[c], HeaderStyle(), {x + 8.0f, y, widths[c], header - border}, p.textSubtle);
            x += widths[c];
        }
        FillRect(dr, {bounds_.x, y + header - border, bounds_.width, border}, p.tableBorder);
        y += header;
        const float rowHeight = RowHeight();
        for (size_t r = 0; r < rows_.size(); ++r)
        {
            x = bounds_.x;
            for (size_t c = 0; c < kColumns && c < rows_[r].size(); ++c)
            {
                const Cell &cell = rows_[r][c];
                const D2D1_COLOR_F color = cell.kind == Kind::MonoStrong ? p.text
                                           : cell.kind == Kind::MonoDim  ? p.textSubtle
                                                                         : p.textSecondary;
                const bool last = r + 1 == rows_.size();
                DrawTextInRect(dr, cell.text, StyleOf(cell.kind),
                               {x + 8.0f, y, widths[c], rowHeight - (last ? 0.0f : border)}, color);
                x += widths[c];
            }
            if (r + 1 < rows_.size())
                FillRect(dr, {bounds_.x, y + rowHeight - border, bounds_.width, border}, p.tableBorder);
            y += rowHeight;
        }
        target->PopAxisAlignedClip();
    }

  private:
    static TextStyle HeaderStyle()
    {
        return DocStyle(kFontCaption, DWRITE_FONT_WEIGHT_MEDIUM);
    }
    static TextStyle StyleOf(Kind kind)
    {
        switch (kind)
        {
        case Kind::MonoStrong:
            return MonoStyle(kFontCaption, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        case Kind::MonoDim:
        case Kind::Mono:
            return MonoStyle(kFontCaption);
        case Kind::Text:
        default:
            return DocStyle(kFontCaption);
        }
    }
    static float HeaderHeight()
    {
        return LineHeight(HeaderStyle()) + 12.0f + CssBorder();
    }
    static float RowHeight()
    {
        // The row is as tall as its tallest cell: the page font beats Consolas.
        return std::max(LineHeight(DocStyle(kFontCaption)), LineHeight(MonoStyle(kFontCaption))) + 12.0f + CssBorder();
    }
    std::vector<std::vector<Cell>> rows_;
};

// `.stats-split`: grid 3fr | 2fr with an 18px gap, one column under 820px. The cells
// stretch to the taller one, like grid items do.
class SplitRow : public msimeui::Panel
{
  public:
    SplitRow(VisualPtr left, VisualPtr right) : left_(std::move(left)), right_(std::move(right))
    {
        AddChild(left_);
        AddChild(right_);
    }
    SizeF Measure(const SizeF &availableSize) override
    {
        if (Narrow())
        {
            leftSize_ = left_->MeasureInLayout(availableSize);
            rightSize_ = right_->MeasureInLayout(availableSize);
            return {availableSize.width, leftSize_.height + 18.0f + rightSize_.height};
        }
        const float usable = std::max(availableSize.width - 18.0f, 0.0f);
        leftSize_ = left_->MeasureInLayout({usable * 0.6f, availableSize.height});
        rightSize_ = right_->MeasureInLayout({usable * 0.4f, availableSize.height});
        return {availableSize.width, std::max(leftSize_.height, rightSize_.height)};
    }
    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        if (Narrow())
        {
            left_->ArrangeInLayout({finalRect.x, finalRect.y, finalRect.width, leftSize_.height});
            right_->ArrangeInLayout(
                {finalRect.x, finalRect.y + leftSize_.height + 18.0f, finalRect.width, rightSize_.height});
            return;
        }
        const float usable = std::max(finalRect.width - 18.0f, 0.0f);
        const float height = std::max(leftSize_.height, rightSize_.height);
        left_->ArrangeInLayout({finalRect.x, finalRect.y, usable * 0.6f, height});
        right_->ArrangeInLayout({finalRect.x + usable * 0.6f + 18.0f, finalRect.y, usable * 0.4f, height});
    }
    void Render(msimeui::DeviceResources &dr) override
    {
        left_->Render(dr);
        right_->Render(dr);
    }

  private:
    bool Narrow() const
    {
        return ViewportWidth(window_) <= kNarrowViewport;
    }
    VisualPtr left_;
    VisualPtr right_;
    SizeF leftSize_ = {};
    SizeF rightSize_ = {};
};

// A `.section` whose content stretches to the height its grid cell gives it.
std::shared_ptr<Section> MakeCard(VisualPtr content)
{
    auto section = std::make_shared<Section>(std::move(content));
    section->SetMargin({0.0f, 0.0f, 0.0f, 18.0f});
    section->SetVerticalAlignment(msimeui::VerticalAlignment::Stretch);
    return section;
}

// ------------------------------------------------------------------ page state

struct StatsState
{
    SettingsModel *model = nullptr;
    HostServices *host = nullptr;
    // Raw: the root owns this state.
    msimeui::Visual *root = nullptr;
    bool enabled = false;
    bool hasOverview = false;
    Json overview;
    std::string signature;
    std::map<std::string, std::string> pending;
    double lastRequestAt = -1e12;
    bool wasVisible = false;
    double statusShownAt = 0.0;
    bool statusAutoHide = false;
    std::unique_ptr<UiTimer> timer;

    std::shared_ptr<ToggleSwitch> toggle;
    std::shared_ptr<StatusBox> status;
    std::shared_ptr<Hideable> statusHolder;
    std::shared_ptr<Hideable> disabledEmpty;
    std::shared_ptr<Label> disabledText;
    std::shared_ptr<Hideable> noDataEmpty;
    std::shared_ptr<Hideable> content;
    std::shared_ptr<CardsGrid> cards;
    std::shared_ptr<SpeedGrid> speed;
    std::shared_ptr<Label> bestDay;
    std::shared_ptr<CalendarView> calendar;
    std::shared_ptr<HourlyChart> hourly;
    std::shared_ptr<CategoryBars> categories;
    std::shared_ptr<DetailTable> details;
    std::shared_ptr<Dropdown> retention;
    std::shared_ptr<Label> clearStatus;

    void RenderEmptyState()
    {
        const bool hasData = hasOverview && overview.value("hasData", false);
        noDataEmpty->SetVisible(enabled && !hasData);
        disabledEmpty->SetVisible(!enabled);
        disabledText->SetText(
            hasData ? L"统计当前已关闭，历史数据仍保留在下方；重新开启后继续记录。"
                    : L"开启后这里会显示输入字数、速度与时段分布。统计只保存在本机，不记录输入内容，也不联网。");
    }

    void RenderOverview(const Json &next)
    {
        const std::string nextSignature = next.dump();
        const bool unchanged = hasOverview && nextSignature == signature;
        signature = nextSignature;
        overview = next;
        hasOverview = true;
        if (unchanged)
            return;
        RenderEmptyState();
        const bool hasData = overview.value("hasData", false);
        content->SetVisible(hasData);
        if (!hasData)
            return;

        // Cards
        const Json *todayRatio = overview.contains("todayFirstWordRatio") ? &overview["todayFirstWordRatio"] : nullptr;
        const Json *totalRatio = overview.contains("totalFirstWordRatio") ? &overview["totalFirstWordRatio"] : nullptr;
        const int64_t todayTotal = JsonInt(overview, "todayFirstWordTotal");
        const std::wstring todayNote =
            todayTotal > 0 ? L"今日 " + FormatNumber(static_cast<double>(todayTotal)) + L" 次选词" : L"今日无候选上屏";
        const std::wstring ratioNote =
            totalRatio && totalRatio->is_number() ? todayNote + L" · 累计 " + FormatRatio(totalRatio) : todayNote;
        cards->SetCards({
            {L"今日输入", FormatNumber(JsonNumber(overview, "todayChars")), L"字",
             L"活跃 " + FormatDuration(JsonNumber(overview, "todayActiveMs"))},
            {L"累计输入", FormatNumber(JsonNumber(overview, "totalChars")), L"字",
             L"共记录 " + std::to_wstring(JsonInt(overview, "days")) + L" 天"},
            {L"日均输入", FormatNumber(JsonNumber(overview, "averagePerDay")), L"字",
             L"自 " + FormatDayKey(JsonInt(overview, "firstDayKey"))},
            {L"连续输入", FormatNumber(JsonNumber(overview, "currentStreak")), L"天",
             L"历史最长 " + std::to_wstring(JsonInt(overview, "longestStreak")) + L" 天"},
            {L"首词命中率", FormatRatio(todayRatio), L"", ratioNote},
        });

        // Speed
        const int64_t fastestDay = JsonInt(overview, "fastestDayKey");
        speed->SetItems({
            {L"今日速度", FormatSpeed(JsonNumber(overview, "todaySpeed")),
             FormatDuration(JsonNumber(overview, "todayActiveMs"))},
            {L"平均速度", FormatSpeed(JsonNumber(overview, "averageSpeed")),
             FormatDuration(JsonNumber(overview, "totalActiveMs"))},
            {L"最快速度", FormatSpeed(JsonNumber(overview, "fastestSpeed")),
             fastestDay ? FormatDayKey(fastestDay) : L"暂无"},
        });
        const int64_t bestKey = JsonInt(overview, "bestDayKey");
        bestDay->SetText(bestKey ? L"最高日：" + FormatDayKey(bestKey) + L" · " +
                                       FormatNumber(JsonNumber(overview, "bestDayChars")) + L" 字"
                                 : L"最高日：暂无记录");

        // Calendar
        std::map<int64_t, int64_t> daily;
        int64_t maxChars = 1;
        const Json empty = Json::array();
        const Json &rows = overview.contains("daily") && overview["daily"].is_array() ? overview["daily"] : empty;
        for (const Json &row : rows)
        {
            const int64_t total = TotalOf(row);
            daily[JsonInt(row, "dayKey")] = total;
            maxChars = std::max(maxChars, total);
        }
        calendar->SetData(std::move(daily), maxChars, JsonInt(overview, "todayDayKey"));

        // Hourly
        std::vector<int64_t> hours;
        if (overview.contains("todayHourly") && overview["todayHourly"].is_array())
        {
            for (const Json &value : overview["todayHourly"])
                hours.push_back(value.is_number() ? value.get<int64_t>() : 0);
        }
        hourly->SetValues(std::move(hours));

        // Categories
        const Json categoriesJson = overview.contains("categories") && overview["categories"].is_object()
                                        ? overview["categories"]
                                        : Json::object();
        const int64_t values[] = {JsonInt(categoriesJson, "cjk"), JsonInt(categoriesJson, "latin"),
                                  JsonInt(categoriesJson, "digit"), JsonInt(categoriesJson, "punct"),
                                  JsonInt(categoriesJson, "other")};
        static const wchar_t *kNames[] = {L"中文", L"英文", L"数字", L"标点", L"其他"};
        int64_t total = 0;
        for (int64_t v : values)
            total += v;
        std::vector<CategoryBars::Row> categoryRows;
        for (int i = 0; i < 5; ++i)
        {
            const float fraction = total > 0 ? static_cast<float>(values[i]) / static_cast<float>(total) : 0.0f;
            const int64_t percent = total > 0 ? static_cast<int64_t>(std::floor(fraction * 100.0f + 0.5f)) : 0;
            categoryRows.push_back(
                {kNames[i], total > 0 ? fraction : 0.0f,
                 FormatNumber(static_cast<double>(values[i])) + L" 字 · " + std::to_wstring(percent) + L"%"});
        }
        categories->SetRows(std::move(categoryRows));

        // Details: the last 30 days, newest first.
        std::vector<std::vector<DetailTable::Cell>> detailRows;
        const size_t count = rows.size();
        for (size_t i = 0; i < std::min(count, kDetailDays); ++i)
        {
            const Json &row = rows[count - 1 - i];
            std::vector<DetailTable::Cell> cells;
            cells.push_back({FormatDayKey(JsonInt(row, "dayKey")), DetailTable::Kind::Mono});
            cells.push_back({FormatNumber(static_cast<double>(TotalOf(row))), DetailTable::Kind::MonoStrong});
            for (const char *key : {"cjk", "latin", "digit", "punct", "other"})
                cells.push_back({FormatNumber(static_cast<double>(JsonInt(row, key))), DetailTable::Kind::MonoDim});
            cells.push_back({FormatDuration(static_cast<double>(JsonInt(row, "activeMs"))), DetailTable::Kind::Text});
            cells.push_back({FormatSpeed(SpeedOf(row)), DetailTable::Kind::Text});
            const int64_t firstTotal = JsonInt(row, "firstWordTotal");
            std::wstring first = L"–";
            if (firstTotal > 0)
                first =
                    std::to_wstring(static_cast<int64_t>(std::floor(static_cast<double>(JsonInt(row, "firstWordHits")) /
                                                                        static_cast<double>(firstTotal) * 100.0 +
                                                                    0.5))) +
                    L"%";
            cells.push_back({first, DetailTable::Kind::Text});
            detailRows.push_back(std::move(cells));
        }
        details->SetRows(std::move(detailRows));
    }

    void ShowStatus(const std::wstring &message, bool ok)
    {
        status->Set(message, !ok);
        statusHolder->SetVisible(true);
        statusAutoHide = ok;
        statusShownAt = msimeui::NowMs();
    }

    void SetClearStatus(const std::wstring &message, bool error)
    {
        clearStatus->SetText(message);
        clearStatus->SetRole(error ? TextRole::Danger : TextRole::Muted);
    }

    void Post(const std::string &action)
    {
        const std::string requestId = model->NewRequestId("stats");
        if (action == "overview")
        {
            lastRequestAt = msimeui::NowMs();
            for (auto it = pending.begin(); it != pending.end();)
                it = it->second == "overview" ? pending.erase(it) : std::next(it);
        }
        pending[requestId] = action;
        model->Post("statsRequest", Json{{"requestId", requestId}, {"action", action}});
    }

    bool HasPendingOverview() const
    {
        if (msimeui::NowMs() - lastRequestAt > kStaleMs)
            return false;
        for (const auto &entry : pending)
        {
            if (entry.second == "overview")
                return true;
        }
        return false;
    }

    bool IsVisible() const
    {
        const HWND hwnd = host->Hwnd();
        return root && root->GetParentVisual() && hwnd && IsWindowVisible(hwnd) && !IsIconic(hwnd);
    }

    // Visibility flips, focus and the poll all end up here, throttled and de-duplicated.
    void Refresh()
    {
        if (!IsVisible() || HasPendingOverview())
            return;
        if (msimeui::NowMs() - lastRequestAt < kMinIntervalMs)
            return;
        Post("overview");
    }

    void Tick()
    {
        const bool visible = IsVisible();
        const double now = msimeui::NowMs();
        if (visible && (!wasVisible || now - lastRequestAt >= kPollMs))
            Refresh();
        wasVisible = visible;
        if (statusAutoHide && now - statusShownAt >= kStatusTimeoutMs)
        {
            statusAutoHide = false;
            statusHolder->SetVisible(false);
        }
    }

    void HandleResponse(const Json &response, const std::string &action)
    {
        const bool ok = response.value("ok", false);
        const std::wstring message = Widen(response.value("message", std::string()));
        if (!ok)
        {
            if (action == "clearAll")
                SetClearStatus(message.empty() ? L"清空统计数据失败" : message, true);
            else
                ShowStatus(message.empty() ? L"统计操作失败" : message, false);
            return;
        }
        if (response.contains("overview") && response["overview"].is_object())
            RenderOverview(response["overview"]);
        if (action == "openDirectory")
        {
            ShowStatus(message.empty() ? L"已打开数据目录" : message, true);
        }
        else if (action == "clearAll")
        {
            const int64_t removed = JsonInt(response, "removedDays");
            SetClearStatus(!message.empty() ? message
                           : removed > 0    ? L"已删除 " + std::to_wstring(removed) + L" 天的记录"
                                            : L"没有需要清理的数据",
                           false);
        }
    }
};

TextStyle EmptyTitleStyle()
{
    TextStyle style = SectionTitleStyle();
    style.align = DWRITE_TEXT_ALIGNMENT_CENTER;
    return style;
}

TextStyle EmptyTextStyle()
{
    TextStyle style = DocStyle(kFontCaption, DWRITE_FONT_WEIGHT_NORMAL, 1.6f, true);
    style.align = DWRITE_TEXT_ALIGNMENT_CENTER;
    return style;
}

CssButton::Style StatsButtonStyle(CssButton::Look look)
{
    // .stats-btn: 13px, padding 7px 16px, 6px radius, --control-border, transitions .2s
    CssButton::Style style;
    style.look = look;
    style.fontSize = 13.0f;
    style.paddingX = 16.0f;
    style.paddingY = 7.0f;
    style.radius = 6.0f;
    style.controlBorder = true;
    style.transitionMs = 200.0;
    return style;
}

// `.stats-clear-row`: flex row, gap 10px, margin-top 12px.
std::shared_ptr<Row> ClearRow()
{
    auto row = std::make_shared<Row>(10.0f);
    row->SetWrap(true, 10.0f);
    row->SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
    return row;
}

std::shared_ptr<Label> Note(std::wstring text)
{
    return MakeLabel(std::move(text), DocStyle(12.0f), TextRole::Subtle);
}
} // namespace

VisualPtr BuildStatsPage(PageContext &context)
{
    auto state = std::make_shared<StatsState>();
    state->model = &context.model;
    state->host = &context.host;
    StatsState *s = state.get();

    PageBuilder page(L"统计");

    // 开关
    state->toggle = std::make_shared<ToggleSwitch>(false);
    state->toggle->SetOnChanged([s](bool on) {
        s->enabled = on;
        s->model->Update("statistics.enabled", on);
        s->RenderEmptyState();
    });
    page.AddSection(
        MakeRow(L"启用输入统计", L"只在本机记录输入字数与活跃时长，不记录输入内容，不联网；默认关闭", state->toggle));

    // 状态提示
    state->status = std::make_shared<StatusBox>();
    state->statusHolder = std::make_shared<Hideable>(state->status, false);
    page.AddRaw(state->statusHolder, 0.0f);

    // 空状态：未启用
    {
        auto column = std::make_shared<Column>(0.0f);
        auto title = MakeLabel(L"统计未启用", EmptyTitleStyle(), TextRole::Primary);
        title->SetMargin({0.0f, 0.0f, 0.0f, 8.0f});
        column->Add(title);
        state->disabledText = MakeLabel(L"", EmptyTextStyle(), TextRole::Muted);
        state->disabledText->SetMaxWidth(520.0f);
        state->disabledText->SetHorizontalAlignment(msimeui::HorizontalAlignment::Center);
        state->disabledText->SetMargin({0.0f, 0.0f, 0.0f, 16.0f});
        column->Add(state->disabledText);
        auto enable = std::make_shared<CssButton>(L"启用输入统计", StatsButtonStyle(CssButton::Look::StatsPrimary));
        enable->SetHorizontalAlignment(msimeui::HorizontalAlignment::Center);
        enable->SetOnClick([s]() {
            s->enabled = true;
            s->toggle->SetChecked(true);
            s->RenderEmptyState();
            s->model->Update("statistics.enabled", true);
        });
        column->Add(enable);
        auto section = std::make_shared<PaddedSection>(column, Thickness{24.0f, 36.0f, 24.0f, 36.0f});
        section->SetMargin({0.0f, 0.0f, 0.0f, 18.0f});
        state->disabledEmpty = std::make_shared<Hideable>(section, true);
        page.AddRaw(state->disabledEmpty, 0.0f);
    }

    // 空状态：已启用但还没有数据
    {
        auto column = std::make_shared<Column>(0.0f);
        auto title = MakeLabel(L"还没有统计数据", EmptyTitleStyle(), TextRole::Primary);
        title->SetMargin({0.0f, 0.0f, 0.0f, 8.0f});
        column->Add(title);
        auto text =
            MakeLabel(L"开启后正常打字即可积累统计，中文和英文模式都会记录。", EmptyTextStyle(), TextRole::Muted);
        text->SetMaxWidth(520.0f);
        text->SetHorizontalAlignment(msimeui::HorizontalAlignment::Center);
        text->SetMargin({0.0f, 0.0f, 0.0f, 16.0f});
        column->Add(text);
        auto section = std::make_shared<PaddedSection>(column, Thickness{24.0f, 36.0f, 24.0f, 36.0f});
        section->SetMargin({0.0f, 0.0f, 0.0f, 18.0f});
        state->noDataEmpty = std::make_shared<Hideable>(section, false);
        page.AddRaw(state->noDataEmpty, 0.0f);
    }

    // 统计内容
    {
        auto column = std::make_shared<Column>(0.0f);
        state->cards = std::make_shared<CardsGrid>();
        column->Add(state->cards);

        auto speed = std::make_shared<Column>(0.0f);
        speed->Add(std::make_shared<TitleNoteRow>(
            L"输入速度", L"可读字数（中文+英文）÷ 活跃分钟；数字、标点不计入，相邻上屏间隔不超过 10 秒计为活跃"));
        state->speed = std::make_shared<SpeedGrid>();
        speed->Add(state->speed);
        state->bestDay = MakeLabel(L"", DocStyle(kFontCaption), TextRole::Secondary);
        state->bestDay->SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
        speed->Add(state->bestDay);
        column->Add(MakeCard(speed));

        auto calendar = std::make_shared<Column>(0.0f);
        calendar->Add(std::make_shared<TitleNoteRow>(L"日历热力图", L"近 12 个月，颜色越深输入越多"));
        state->calendar = std::make_shared<CalendarView>();
        calendar->Add(state->calendar);
        calendar->Add(std::make_shared<Legend>(state->calendar));
        column->Add(MakeCard(calendar));

        auto hourly = std::make_shared<Column>(0.0f);
        hourly->Add(MakeTitle(L"今日时段分布"));
        state->hourly = std::make_shared<HourlyChart>();
        hourly->Add(state->hourly);
        auto categories = std::make_shared<Column>(0.0f);
        categories->Add(MakeTitle(L"字符分类"));
        state->categories = std::make_shared<CategoryBars>();
        categories->Add(state->categories);
        column->Add(std::make_shared<SplitRow>(MakeCard(hourly), MakeCard(categories)));

        auto details = std::make_shared<Column>(0.0f);
        details->Add(std::make_shared<TitleNoteRow>(L"按日明细", L"最近 30 天"));
        state->details = std::make_shared<DetailTable>();
        details->Add(state->details);
        column->Add(MakeCard(details));

        state->content = std::make_shared<Hideable>(column, false);
        page.AddRaw(state->content, 0.0f);
    }

    // 数据清理
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(MakeTitleBlock(L"数据清理",
                                   L"自动清理按保留策略删除超期记录，在 Server "
                                   L"跨天后首次写入统计时执行；「清空统计数据」会删除全部每日与每小时记录（包括今天），"
                                   L"荣誉室的解锁记录不受影响，删除后无法恢复"));
        auto retentionRow = ClearRow();
        retentionRow->Add(MakeLabel(L"自动清理", DocStyle(kFontDefault), TextRole::Primary));
        // A native <select> (forms.css): as wide as its widest option plus padding 10px, the
        // border and the drop-down arrow, whichever option is selected.
        Dropdown::Options select;
        select.fixedWidth = MeasureTextWidth(L"保留最近 180 天", ControlStyle()) + 20.0f + CssBorder() * 2.0f + 20.0f;
        select.matchToggleWidth = true;
        state->retention = std::make_shared<Dropdown>(Options({{"forever", L"永久保留"},
                                                               {"30d", L"保留最近 30 天"},
                                                               {"90d", L"保留最近 90 天"},
                                                               {"180d", L"保留最近 180 天"},
                                                               {"365d", L"保留最近 365 天"}}),
                                                      select);
        state->retention->SetOnChanged([s](size_t, const DropdownOption &option) {
            s->model->Update("statistics.retention", option.value);
            if (option.value == "forever")
                s->SetClearStatus(L"已保存：永久保留，不会自动删除任何记录。", false);
            else
                s->SetClearStatus(L"已保存自动清理策略：" + option.label + L"；Server 跨天后首次写入统计时执行清理。",
                                  false);
        });
        retentionRow->Add(state->retention);
        retentionRow->Add(Note(L"选中即保存"));
        column->Add(retentionRow);

        auto clearRow = ClearRow();
        auto clear = std::make_shared<CssButton>(L"清空统计数据", StatsButtonStyle(CssButton::Look::StatsDanger));
        clear->SetOnClick([s]() {
            s->host->Confirm(
                L"确定要清空全部统计数据吗？今天与历史的统计记录都会被删除，荣誉室的解锁记录保留；删除后无法恢复。",
                [s](bool confirmed) {
                    if (!confirmed)
                        return;
                    s->SetClearStatus(L"正在清空统计数据…", false);
                    s->Post("clearAll");
                },
                L"清空统计数据", L"清空");
        });
        clearRow->Add(clear);
        clearRow->Add(Note(L"删除全部统计记录且无法恢复"));
        column->Add(clearRow);

        // .stats-clear-status: margin-top 10px, min-height 18px.
        state->clearStatus = MakeLabel(L"", DocStyle(kFontCaption), TextRole::Muted);
        state->clearStatus->SetMargin({0.0f, 10.0f, 0.0f, 0.0f});
        state->clearStatus->SetMinHeight(18.0f);
        column->Add(state->clearStatus);
        page.AddSection(column);
    }

    // 数据位置与隐私
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(MakeTitle(L"数据位置与隐私"));
        column->Add(
            MakeDescription(L"统计数据保存在本机数据目录的 stats.db（SQLite）中，只包含每天每类字符的数量、活跃时长，"
                            L"以及荣誉室的成就编号与解锁日期，没有任何输入文本；不联网、不上传。关闭开关后停止记录，"
                            L"历史数据继续保留。"));
        auto row = ClearRow();
        auto open = std::make_shared<CssButton>(L"打开数据目录", StatsButtonStyle(CssButton::Look::Secondary));
        open->SetOnClick([s]() { s->Post("openDirectory"); });
        row->Add(open);
        column->Add(row);
        page.AddSection(column);
    }

    // Config snapshots: statistics.enabled / statistics.retention.
    context.OnConfig([s]() {
        s->enabled = s->model->GetBool("statistics.enabled");
        s->toggle->SetChecked(s->enabled);
        s->RenderEmptyState();
        const std::string retention = s->model->GetString("statistics.retention");
        if (retention == "forever" || retention == "30d" || retention == "90d" || retention == "180d" ||
            retention == "365d")
            s->retention->SetValue(retention);
    });
    std::weak_ptr<StatsState> weak = state;
    context.OnMessage("statsResponse", [weak](const Json &message) {
        auto self = weak.lock();
        if (!self)
            return;
        const std::string requestId = message.value("requestId", std::string());
        const auto it = self->pending.find(requestId);
        if (it == self->pending.end())
            return;
        const std::string action = it->second;
        self->pending.erase(it);
        self->HandleResponse(message, action);
    });

    auto root = std::make_shared<OwnerBox>(page.Build(), state);
    state->root = root.get();
    state->timer = std::make_unique<UiTimer>(context.host.Hwnd(), 1000, [s]() { s->Tick(); });
    state->Post("overview");
    return root;
}
} // namespace native_settings
