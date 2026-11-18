#include "settings/native/native_page_skin_common.h"
#include "settings/native/native_settings_pages.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Fonts.h"
#include "msimeui/Window.h"

#include <commdlg.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>

#pragma comment(lib, "comdlg32.lib")

// partials/appearance.html + modules/appearance.ts (+ the appearance part of
// config-sync.ts and theme.ts), candidate preview from partials/candidate/*.html with
// styles/modules/candidate/style-v.css, style-h.css and page-arrows.css.
namespace native_settings
{
namespace
{
using Microsoft::WRL::ComPtr;

// FALLBACK_CJK_FONTS + FALLBACK_ENGLISH_FONTS, used until the snapshot brings the
// system font list.
const std::vector<std::wstring> &DefaultFontList()
{
    static const std::vector<std::wstring> fonts = {
        L"Noto Sans SC",  L"Microsoft YaHei", L"Microsoft YaHei UI", L"SimHei", L"SimSun",  L"DengXian",
        L"KaiTi",         L"FangSong",        L"Segoe UI",           L"Arial",  L"Calibri", L"Consolas",
        L"Cascadia Mono", L"Times New Roman", L"Courier New",        L"Tahoma"};
    return fonts;
}

std::vector<std::wstring> StringArray(const Json *value)
{
    std::vector<std::wstring> result;
    if (value && value->is_array())
    {
        for (const auto &item : *value)
        {
            if (item.is_string())
                result.push_back(Widen(item.get<std::string>()));
        }
    }
    return result;
}

bool SystemPrefersLight()
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
           value != 0;
}

// theme.ts resolveTheme(): a surface theme of dark/light wins, otherwise the global mode.
bool ResolveLight(const SettingsModel &model, const char *surfacePath)
{
    const std::string surface = model.GetString(surfacePath, "follow");
    if (surface == "light")
        return true;
    if (surface == "dark")
        return false;
    std::string mode = model.GetString("appearance.theme_mode", "dark");
    if (mode == "auto")
        mode = "system";
    if (mode == "system")
        return SystemPrefersLight();
    return mode == "light";
}

// `.cand-preview`: the candidate window centered in a box at least 16rem tall (22rem
// with 16px padding when the skin hangs a decoration on the window).
class PreviewBox : public msimeui::Container
{
  public:
    explicit PreviewBox(VisualPtr child) : msimeui::Container(std::move(child))
    {
    }

    void SetDecorated(bool decorated)
    {
        if (decorated_ == decorated)
            return;
        decorated_ = decorated;
        InvalidateMeasure();
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        const float pad = decorated_ ? 16.0f : 0.0f;
        childSize_ =
            child_ ? child_->MeasureInLayout({std::max(availableSize.width - pad * 2.0f, 0.0f), availableSize.height})
                   : SizeF{};
        return {availableSize.width, std::max(decorated_ ? 352.0f : 256.0f, childSize_.height + pad * 2.0f)};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        if (child_)
            child_->ArrangeInLayout({std::round(finalRect.x + (finalRect.width - childSize_.width) * 0.5f),
                                     std::round(finalRect.y + (finalRect.height - childSize_.height) * 0.5f),
                                     childSize_.width, childSize_.height});
    }

  private:
    SizeF childSize_ = {};
    bool decorated_ = false;
};

// ---------------------------------------------------------------------------------
// Fallback fonts (.fallback-font-list): wrapping row of font comboboxes + "+".
// ---------------------------------------------------------------------------------
class FlowPanel : public msimeui::Panel
{
  public:
    explicit FlowPanel(float gap) : gap_(gap)
    {
    }

    void Clear()
    {
        for (const auto &child : children_)
            ReleaseChild(child);
        children_.clear();
        InvalidateMeasure();
    }

    void Add(VisualPtr child)
    {
        AddChild(child);
        if (window_)
            child->Attach(window_);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        sizes_.clear();
        float x = 0.0f, y = 0.0f, lineHeight = 0.0f, widest = 0.0f;
        for (const auto &child : children_)
        {
            const SizeF size = child->MeasureInLayout(availableSize);
            if (x > 0.0f && x + gap_ + size.width > availableSize.width)
            {
                widest = std::max(widest, x);
                x = 0.0f;
                y += lineHeight + gap_;
                lineHeight = 0.0f;
            }
            sizes_.push_back({x + (x > 0.0f ? gap_ : 0.0f), y, size.width, size.height});
            x += (x > 0.0f ? gap_ : 0.0f) + size.width;
            lineHeight = std::max(lineHeight, size.height);
        }
        widest = std::max(widest, x);
        lines_ = y + lineHeight;
        return {std::min(widest, availableSize.width), children_.empty() ? 0.0f : lines_};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        // align-items: center within each line: all items share one height here.
        for (size_t i = 0; i < children_.size() && i < sizes_.size(); ++i)
            children_[i]->ArrangeInLayout(
                {finalRect.x + sizes_[i].x, finalRect.y + sizes_[i].y, sizes_[i].width, sizes_[i].height});
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        for (const auto &child : children_)
            child->Render(dr);
    }

  private:
    float gap_ = 8.0f;
    float lines_ = 0.0f;
    std::vector<RectF> sizes_;
};

// <button class="dropdown-toggle fallback-font-add"><span>+</span></button>
class AddFontButton : public msimeui::Visual
{
  public:
    void SetOnClick(std::function<void()> handler)
    {
        onClick_ = std::move(handler);
    }
    void SetDisabled(bool disabled)
    {
        if (disabled_ != disabled)
        {
            disabled_ = disabled;
            InvalidateVisual();
        }
    }
    SizeF Measure(const SizeF &) override
    {
        // width: 50px, but the .dropdown-toggle min-width of 100px wins.
        return {100.0f, 18.0f + 8.0f + CssBorder() * 2.0f};
    }
    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }
    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        const bool hot = hovered_ && !disabled_;
        if (disabled_)
            PushOpacity(dr, 0.4f);
        FillRounded(dr, bounds_, 8.0f, hot ? p.surfaceRaised : p.dropdownBg);
        StrokeRounded(dr, bounds_, 8.0f, hot ? p.borderStrong : p.border, 1.0f);
        DrawTextInRect(dr, L"+", ControlStyle(),
                       {bounds_.x + (bounds_.width - 18.0f) * 0.5f, bounds_.y, 18.0f, bounds_.height}, p.text, true);
        if (disabled_)
            PopOpacity(dr);
    }
    bool OnMouseDown(const POINT &, WPARAM) override
    {
        pressed_ = !disabled_;
        return pressed_;
    }
    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        if (!pressed_)
            return false;
        pressed_ = false;
        if (window_ && HitTest(window_->ClientPixelsToDips(point)) && onClick_)
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
        InvalidateVisual();
    }

  private:
    std::function<void()> onClick_;
    bool disabled_ = false;
    bool hovered_ = false;
    bool pressed_ = false;
};

class FallbackFontEditor
{
  public:
    FallbackFontEditor(SettingsModel &model, HostServices &host)
        : model_(model), host_(host), panel_(std::make_shared<FlowPanel>(8.0f))
    {
        // .fallback-font-list { margin: 8px 0 }
        panel_->SetMargin({0.0f, 8.0f, 0.0f, 8.0f});
    }

    const std::shared_ptr<FlowPanel> &Panel() const
    {
        return panel_;
    }

    void Sync(std::vector<std::wstring> fonts, std::vector<std::wstring> fontList)
    {
        if (fonts == fonts_ && fontList == fontList_ && rendered_)
            return;
        fonts_ = std::move(fonts);
        fontList_ = std::move(fontList);
        adding_ = false;
        RenderSoon();
    }

  private:
    // The list is rebuilt from a dropdown's own change handler, so the rebuild waits for
    // the message loop instead of destroying that dropdown while it runs.
    void RenderSoon()
    {
        Instance() = this;
        SetTimer(host_.Hwnd(), kRebuildTimer, 0, &FallbackFontEditor::OnRebuild);
    }

    static FallbackFontEditor *&Instance()
    {
        static FallbackFontEditor *instance = nullptr;
        return instance;
    }

    static void CALLBACK OnRebuild(HWND hwnd, UINT, UINT_PTR id, DWORD)
    {
        KillTimer(hwnd, id);
        if (FallbackFontEditor *editor = Instance())
            editor->Render();
    }

    static constexpr UINT_PTR kRebuildTimer = 0x4E61;

    void Render()
    {
        rendered_ = true;
        panel_->Clear();
        std::vector<std::wstring> displayed = fonts_;
        if (adding_ || displayed.empty())
            displayed.emplace_back();
        std::shared_ptr<Dropdown> focusTarget;
        for (size_t index = 0; index < displayed.size(); ++index)
        {
            const std::wstring font = displayed[index];
            std::vector<DropdownOption> options;
            std::set<std::wstring> seen;
            if (!font.empty())
                options.push_back({L"移除此字体", "", false, {}});
            auto add = [&](const std::wstring &name) {
                if (!seen.insert(name).second)
                    return;
                const bool taken = name != font && std::find(fonts_.begin(), fonts_.end(), name) != fonts_.end();
                options.push_back({name, Narrow(name), taken, name});
            };
            if (!font.empty())
                add(font);
            for (const auto &name : fontList_)
                add(name);

            Dropdown::Options settings;
            settings.compact = true;
            settings.searchable = true;
            settings.fixedWidth = 150.0f;
            settings.menuAlignLeft = true;
            settings.menuMinWidth = 168.0f;
            settings.menuMaxWidth = 320.0f;
            settings.placeholder = L"搜索字体";
            auto dropdown = std::make_shared<Dropdown>(std::move(options), settings);
            if (!font.empty())
                dropdown->SetValue(Narrow(font));
            dropdown->SetOnChanged([this, index](size_t, const DropdownOption &option) {
                const std::wstring value = Widen(option.value);
                for (size_t i = 0; i < fonts_.size(); ++i)
                {
                    if (i != index && fonts_[i] == value && !value.empty())
                        return;
                }
                if (value.empty())
                {
                    if (index < fonts_.size())
                        fonts_.erase(fonts_.begin() + static_cast<std::ptrdiff_t>(index));
                }
                else if (index < fonts_.size())
                {
                    fonts_[index] = value;
                }
                else
                {
                    fonts_.push_back(value);
                }
                adding_ = false;
                Json array = Json::array();
                for (const auto &name : fonts_)
                    array.push_back(Narrow(name));
                model_.Update("appearance.fallback_fonts", array.dump());
            });
            if (font.empty())
                focusTarget = dropdown;
            panel_->Add(dropdown);
        }
        auto add = std::make_shared<AddFontButton>();
        add->SetDisabled(adding_ || fonts_.empty() || fonts_.size() >= 32);
        add->SetOnClick([this]() {
            adding_ = true;
            RenderSoon();
        });
        panel_->Add(add);
        (void)focusTarget;
    }

    SettingsModel &model_;
    HostServices &host_;
    std::shared_ptr<FlowPanel> panel_;
    std::vector<std::wstring> fonts_;
    std::vector<std::wstring> fontList_;
    bool adding_ = false;
    bool rendered_ = false;
};

// ---------------------------------------------------------------------------------
// 候选文字颜色: <input type="color"> + 跟随主题.
// ---------------------------------------------------------------------------------
class ColorSwatch : public msimeui::Visual
{
  public:
    void SetColor(D2D1_COLOR_F color)
    {
        color_ = color;
        InvalidateVisual();
    }
    void SetOnClick(std::function<void()> handler)
    {
        onClick_ = std::move(handler);
    }
    SizeF Measure(const SizeF &) override
    {
        return {36.0f, 28.0f};
    }
    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }
    void Render(msimeui::DeviceResources &dr) override
    {
        // border 1px var(--control-border), radius 6; the swatch wrapper pads 4px 2px and
        // the swatch has Chrome's 1px #777 border.
        StrokeRounded(dr, bounds_, 6.0f, CurrentPalette().controlBorder, 1.0f);
        const float b = CssBorder();
        const RectF swatch = {bounds_.x + b + 2.0f, bounds_.y + b + 4.0f, bounds_.width - 2.0f * (b + 2.0f),
                              bounds_.height - 2.0f * (b + 4.0f)};
        FillRect(dr, swatch, color_);
        StrokeRounded(dr, swatch, 0.0f, Hex(0x777777), 1.0f);
    }
    bool OnMouseDown(const POINT &, WPARAM) override
    {
        pressed_ = true;
        return true;
    }
    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        if (!pressed_)
            return false;
        pressed_ = false;
        if (window_ && HitTest(window_->ClientPixelsToDips(point)) && onClick_)
        {
            // A handler may tear down the visual that owns onClick_ while it runs.
            const auto handler = onClick_;
            handler();
        }
        return true;
    }
    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, IDC_HAND);
    }

  private:
    D2D1_COLOR_F color_ = Hex(0xE9E8E8);
    std::function<void()> onClick_;
    bool pressed_ = false;
};

// ---------------------------------------------------------------------------------
// 横排固定宽度：屏幕宽度比例滑块，类似 <input type="range"> 带 <datalist> 刻度。
// 1/8–3/4 之间连续可取，靠近 1/8、1/4、3/8、1/2、5/8、3/4 这几个刻度时吸附过去；右侧读数
// 显示比例和按设置窗口所在屏幕折算的像素宽度。拖动只更新读数，松手才写配置。
// ---------------------------------------------------------------------------------
class ScreenFractionSlider : public msimeui::Visual
{
  public:
    using ChangeHandler = std::function<void(double ratio)>;

    explicit ScreenFractionSlider(std::function<int()> screenWidthPx) : screenWidthPx_(std::move(screenWidthPx))
    {
    }

    void SetValue(double ratio)
    {
        if (dragging_)
            return;
        value_ = Clamp(ratio);
        InvalidateVisual();
    }
    void SetOnCommit(ChangeHandler handler)
    {
        onCommit_ = std::move(handler);
    }

    SizeF Measure(const SizeF &) override
    {
        return {kTrackWidth + kReadoutGap + kReadoutWidth, kHeight};
    }
    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }
    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &palette = CurrentPalette();
        const RectF track = TrackRect();
        const float centerY = track.y + kTrackHeight * 0.5f;
        FillRounded(dr, track, kTrackHeight * 0.5f, palette.toggleOff);
        const float thumbX = XForValue(value_);
        FillRounded(dr, {track.x, track.y, thumbX - track.x, kTrackHeight}, kTrackHeight * 0.5f, palette.accent);

        // 刻度：轨道上的小圆点，下面一行小字；当前值正落在刻度上时那个刻度用强调色。
        TextStyle tickStyle;
        tickStyle.size = 11.0f;
        for (const Stop &stop : Stops())
        {
            const float x = XForValue(stop.ratio);
            const bool reached = stop.ratio <= value_ + 1e-6;
            const bool current = std::abs(stop.ratio - value_) < 1e-4;
            const float dot = 1.5f;
            FillRounded(dr, {x - dot, centerY - dot, dot * 2.0f, dot * 2.0f}, dot,
                        reached ? palette.accentStrong : palette.textSubtle);
            const RectF label = {x - 14.0f, track.y + kTrackHeight + 7.0f, 28.0f, 14.0f};
            DrawTextInRect(dr, stop.label, tickStyle, label, current ? palette.accent : palette.textMuted, true);
        }

        // 滑块：白底圆，hover/拖动时描边加粗并带一圈淡淡的强调色光晕。
        const bool hot = hovered_ || dragging_;
        if (hot)
        {
            const float halo = kThumbRadius + 4.0f;
            FillRounded(dr, {thumbX - halo, centerY - halo, halo * 2.0f, halo * 2.0f}, halo, palette.accentSoft);
        }
        const RectF thumb = {thumbX - kThumbRadius, centerY - kThumbRadius, kThumbRadius * 2.0f, kThumbRadius * 2.0f};
        FillRounded(dr, thumb, kThumbRadius, Hex(0xFFFFFF));
        StrokeRounded(dr, thumb, kThumbRadius, palette.accent, hot ? 2.0f : 1.5f);

        // 读数：比例 + 像素，放在一个小胶囊里。
        const RectF pill = {bounds_.x + kTrackWidth + kReadoutGap, centerY - 11.0f, kReadoutWidth, 22.0f};
        FillRounded(dr, pill, 11.0f, palette.surfaceSunken);
        StrokeRounded(dr, pill, 11.0f, palette.border, 1.0f);
        TextStyle readoutStyle;
        readoutStyle.size = 12.0f;
        DrawTextInRect(dr, Readout(), readoutStyle, pill, palette.textSecondary, true);
    }
    bool HitTest(const PointF &point) const override
    {
        const RectF track = TrackRect();
        const RectF hit = {track.x - kThumbRadius, bounds_.y, track.width + kThumbRadius * 2.0f, bounds_.height};
        return Contains(hit, point);
    }
    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        if (!window_)
            return false;
        dragging_ = true;
        UpdateFromPoint(point);
        return true;
    }
    bool OnMouseMove(const POINT &point, WPARAM keyState) override
    {
        if (!dragging_ || !(keyState & MK_LBUTTON))
        {
            const bool hovered = window_ && HitTest(window_->ClientPixelsToDips(point));
            if (hovered != hovered_)
            {
                hovered_ = hovered;
                InvalidateVisual();
            }
            return false;
        }
        UpdateFromPoint(point);
        return true;
    }
    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        if (!dragging_)
            return false;
        UpdateFromPoint(point);
        dragging_ = false;
        InvalidateVisual();
        if (onCommit_ && std::abs(value_ - committed_) > 1e-6)
        {
            committed_ = value_;
            const auto handler = onCommit_;
            handler(value_);
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
    // 配置快照里的值，用来判断松手时是否真的变了。
    void SetCommitted(double ratio)
    {
        committed_ = Clamp(ratio);
    }

  private:
    struct Stop
    {
        double ratio;
        const wchar_t *label;
    };
    static const std::vector<Stop> &Stops()
    {
        static const std::vector<Stop> stops = {{0.125, L"1/8"}, {0.25, L"1/4"},  {0.375, L"3/8"},
                                                {0.5, L"1/2"},   {0.625, L"5/8"}, {0.75, L"3/4"}};
        return stops;
    }
    static double Clamp(double ratio)
    {
        return std::clamp(ratio, kCandidateFixedWidthMin, kCandidateFixedWidthMax);
    }
    RectF TrackRect() const
    {
        // 轨道两端各留出半个滑块，滑块走到头也不出界。
        return {bounds_.x + kThumbRadius, bounds_.y + 8.0f, kTrackWidth - kThumbRadius * 2.0f, kTrackHeight};
    }
    float XForValue(double ratio) const
    {
        const RectF track = TrackRect();
        const double t = (ratio - kCandidateFixedWidthMin) / (kCandidateFixedWidthMax - kCandidateFixedWidthMin);
        return track.x + track.width * static_cast<float>(std::clamp(t, 0.0, 1.0));
    }
    void UpdateFromPoint(const POINT &point)
    {
        const RectF track = TrackRect();
        const PointF dip = window_->ClientPixelsToDips(point);
        const double t = std::clamp(static_cast<double>((dip.x - track.x) / track.width), 0.0, 1.0);
        double ratio = kCandidateFixedWidthMin + t * (kCandidateFixedWidthMax - kCandidateFixedWidthMin);
        // 千分之五一档；离刻度不到 4 DIP 时吸附到刻度上。
        ratio = std::round(ratio / 0.005) * 0.005;
        const double snap = 4.0 * (kCandidateFixedWidthMax - kCandidateFixedWidthMin) / track.width;
        for (const Stop &stop : Stops())
        {
            if (std::abs(stop.ratio - ratio) <= snap)
                ratio = stop.ratio;
        }
        ratio = Clamp(ratio);
        if (std::abs(ratio - value_) > 1e-9)
        {
            value_ = ratio;
            InvalidateVisual();
        }
    }
    std::wstring Readout() const
    {
        std::wstring fraction;
        for (const Stop &stop : Stops())
        {
            if (std::abs(stop.ratio - value_) < 1e-4)
                fraction = stop.label;
        }
        if (fraction.empty())
        {
            const double percent = value_ * 100.0;
            wchar_t buffer[16] = {};
            swprintf_s(buffer, std::abs(percent - std::round(percent)) < 0.05 ? L"%.0f%%" : L"%.1f%%", percent);
            fraction = buffer;
        }
        const int screen = screenWidthPx_ ? screenWidthPx_() : 0;
        if (screen <= 0)
            return fraction;
        return fraction + L" · " + std::to_wstring(static_cast<int>(std::lround(value_ * screen))) + L" px";
    }

    static constexpr double kCandidateFixedWidthMin = 0.125;
    static constexpr double kCandidateFixedWidthMax = 0.75;
    static constexpr float kTrackWidth = 220.0f;
    static constexpr float kTrackHeight = 4.0f;
    static constexpr float kThumbRadius = 7.0f;
    static constexpr float kReadoutGap = 12.0f;
    static constexpr float kReadoutWidth = 104.0f;
    static constexpr float kHeight = 38.0f;

    std::function<int()> screenWidthPx_;
    double value_ = 0.25;
    double committed_ = 0.25;
    bool dragging_ = false;
    bool hovered_ = false;
    ChangeHandler onCommit_;
};

// 设置窗口当前所在屏幕的宽度（物理像素）；候选窗用的是光标所在屏幕，单屏时两者一样。
int SettingsMonitorWidthPx(HWND hwnd)
{
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info))
        return GetSystemMetrics(SM_CXSCREEN);
    return info.rcMonitor.right - info.rcMonitor.left;
}

std::shared_ptr<Dropdown> SurfaceThemeDropdown(PageContext &context, const char *path)
{
    return BindDropdown(context, path, Options({{"follow", L"跟随全局"}, {"dark", L"深色"}, {"light", L"浅色"}}));
}

// `.theme-surface-details .section-header`: min-height 38px, sub-item titles, no dividers.
std::shared_ptr<SettingRow> SurfaceRow(PageContext &context, const wchar_t *title, const char *path)
{
    auto row = MakeRow(title, L"", SurfaceThemeDropdown(context, path), true);
    row->SetMinHeight(38.0f);
    return row;
}
} // namespace

VisualPtr BuildAppearancePage(PageContext &context)
{
    PageBuilder page(L"外观");
    SettingsModel *model = &context.model;
    HostServices *host = &context.host;

    // 候选窗口预览: the same preview the skin page uses, so the current skin, theme,
    // fonts, colors and page size show exactly as there.
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(MakeTitle(L"候选窗口预览"));
        auto preview = std::make_shared<skinpreview::CandidatePreview>();
        auto box = std::make_shared<PreviewBox>(preview);
        column->Add(box);
        page.AddSection(column);
        std::weak_ptr<skinpreview::CandidatePreview> weak = preview;
        std::weak_ptr<PreviewBox> weakBox = box;
        context.OnConfig([weak, weakBox, model]() {
            auto control = weak.lock();
            auto previewBox = weakBox.lock();
            if (!control || !previewBox)
                return;
            const bool horizontal = model->GetString("appearance.candidate_window_layout", "vertical") == "horizontal";
            const bool light = skinpreview::ResolveSurfaceLight(*model, "appearance.theme_cand");
            // The preview host takes the configured candidate font size (--cand-font-size)
            // and preedit size (--preedit-font-size).
            skinpreview::CandidateFont font = skinpreview::ReadCandidateFont(*model);
            font.size = static_cast<float>(model->GetInt("appearance.font_size", 16));
            const std::string skin = model->GetString("appearance.candidate_skin", "fluent");
            skinpreview::CandidatePreview::Options options;
            options.preeditFontSize = static_cast<float>(
                model->GetInt("appearance.candidate_window_preedit_font_size", static_cast<int64_t>(font.size)));
            options.textColor = model->GetString("appearance.cand_text_color", "auto");
            options.fitContent = model->GetBool("appearance.candidate_window_fit_content", false);
            options.horizontal = horizontal;
            options.pageSize = static_cast<size_t>(model->GetInt("appearance.page_size", 6));
            options.preedit = model->GetString("appearance.candidate_window_preedit_style", "pinyin") != "empty";
            const std::string schema = model->GetString("input.schema", "shuangpin");
            if (schema == "quanpin")
                options.helpcode = model->GetBool("helpcode.quanpin_helpcode", true) &&
                                   model->GetBool("helpcode.show_qp_helpcode_in_candidate_window", true);
            else if (schema == "shuangpin")
                options.helpcode = model->GetBool("helpcode.shuangpin_helpcode", true) &&
                                   model->GetBool("helpcode.show_sp_helpcode_in_candidate_window", true);
            else
                options.helpcode = false;
            // 翻页箭头与卷轴展开按钮由预览按皮肤清单自己决定，与候选窗同一套判定。
            const Json *external = skinpreview::FindExternalSkin(*model, skin);
            previewBox->SetDecorated(skinpreview::HasDecorationInLayout(external, horizontal));

            control->Set(*model, skin, light, font, options);
        });
    }

    // 候选窗样式
    {
        auto rows = std::make_shared<Column>(0.0f);
        auto addRow = [&rows](VisualPtr row, bool divider = true) {
            if (divider)
                rows->Add(std::make_shared<Divider>());
            rows->Add(std::move(row));
        };
        addRow(MakeRow(L"候选窗口按文字收紧宽度",
                       L"窗口宽度严格按候选文字所需渲染，不保留最小宽度；皮肤自身开启时始终收紧",
                       BindToggle(context, "appearance.candidate_window_fit_content")),
               false);
        addRow(MakeRow(L"候选窗口跟随光标", L"关闭后保持首次位置，直到候选窗口消失",
                       BindToggle(context, "appearance.candidate_window_follow_cursor")));
        addRow(MakeRow(L"固定候选徽标", L"为手动固定的候选词显示角标，便于辨认",
                       BindToggle(context, "appearance.candidate_fixed_badge")));
        addRow(MakeRow(L"固定候选徽标样式", L"",
                       BindDropdown(context, "appearance.candidate_fixed_badge_style",
                                    Options({{"paperclip", L"回形针"}, {"pushpin", L"图钉"}, {"dot", L"圆点"}}))));
        addRow(MakeRow(L"AI 联想候选徽标样式", L"AI 联想候选词末尾的来源角标：机器人 🤖 或小鲸鱼 🐳",
                       BindDropdown(context, "appearance.candidate_ai_badge_style",
                                    Options({{"robot", L"机器人"}, {"whale", L"小鲸鱼"}}))));

        // 候选窗主字体: searchable system font list, each row in its own font.
        Dropdown::Options fontOptions;
        fontOptions.compact = true;
        fontOptions.searchable = true;
        fontOptions.matchToggleWidth = true;
        fontOptions.minWidth = 168.0f;
        auto fontDropdown = BindDropdown(context, "appearance.english_font", {}, fontOptions);
        addRow(MakeRow(L"候选窗主字体", L"", fontDropdown));

        addRow(MakeRow(L"候选窗补充字体", L"主字体缺字时按以下顺序回落，最后使用系统字体。", nullptr));
        auto fallback = std::make_shared<FallbackFontEditor>(context.model, context.host);
        rows->Add(fallback->Panel());

        addRow(MakeRow(L"候选窗字号", L"",
                       BindDropdown(context, "appearance.font_size", NumberOptions(12, 32), Dropdown::Options{true},
                                    ValueKind::Integer)));
        addRow(MakeRow(L"候选窗预编辑字号", L"",
                       BindDropdown(context, "appearance.candidate_window_preedit_font_size", NumberOptions(12, 32),
                                    Dropdown::Options{true}, ValueKind::Integer)));

        // 候选文字颜色
        auto swatch = std::make_shared<ColorSwatch>();
        auto reset = std::make_shared<Button>(L"跟随主题", ButtonKind::Soft);
        auto colorControl = std::make_shared<Row>(10.0f);
        colorControl->Add(swatch);
        colorControl->Add(reset);
        addRow(MakeRow(L"候选文字颜色", L"", colorControl));
        swatch->SetOnClick([model, host, swatchWeak = std::weak_ptr<ColorSwatch>(swatch)]() {
            const std::string current = model->GetString("appearance.cand_text_color", "auto");
            D2D1_COLOR_F color{};
            if (current == "auto" || !ParseCssColor(current, color))
                color = ResolveLight(*model, "appearance.theme_cand") ? Hex(0x1A1A1A) : Hex(0xE9E8E8);
            static COLORREF custom[16] = {};
            CHOOSECOLORW dialog{sizeof(dialog)};
            dialog.hwndOwner = host->Hwnd();
            dialog.rgbResult =
                RGB(static_cast<int>(std::lround(color.r * 255.0f)), static_cast<int>(std::lround(color.g * 255.0f)),
                    static_cast<int>(std::lround(color.b * 255.0f)));
            dialog.lpCustColors = custom;
            dialog.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (!ChooseColorW(&dialog))
                return;
            const D2D1_COLOR_F chosen = {GetRValue(dialog.rgbResult) / 255.0f, GetGValue(dialog.rgbResult) / 255.0f,
                                         GetBValue(dialog.rgbResult) / 255.0f, 1.0f};
            model->Update("appearance.cand_text_color", ToCssHex(chosen));
        });
        reset->SetOnClick([model]() { model->Update("appearance.cand_text_color", "auto"); });

        // 横排固定宽度时一页放几个由宽度决定，这里的数只是上限，标题跟着改。
        auto pageSizeTitle = MakeTitle(L"每页候选项数量");
        auto pageSizeTitleBlock = std::make_shared<Column>(0.0f);
        pageSizeTitleBlock->Add(pageSizeTitle);
        addRow(std::make_shared<SettingRow>(pageSizeTitleBlock,
                                            BindDropdown(context, "appearance.page_size", NumberOptions(3, 9),
                                                         Dropdown::Options{true}, ValueKind::Integer)));
        context.OnConfig([model, weakTitle = std::weak_ptr<Label>(pageSizeTitle)]() {
            auto title = weakTitle.lock();
            if (!title)
                return;
            const bool fixedWidth =
                model->GetString("appearance.candidate_window_layout", "vertical") == "horizontal" &&
                model->GetBool("appearance.candidate_window_fixed_width", false);
            const std::wstring text = fixedWidth ? L"每页候选项数量上限" : L"每页候选项数量";
            if (title->GetText() != text)
                title->SetText(text);
        });
        page.AddSection(rows);

        std::weak_ptr<Dropdown> weakFont = fontDropdown;
        std::weak_ptr<ColorSwatch> weakSwatch = swatch;
        std::weak_ptr<Button> weakReset = reset;
        context.OnConfig([model, weakFont, weakSwatch, weakReset, fallback]() {
            std::vector<std::wstring> fontList = StringArray(model->Find("appearance.system_fonts"));
            if (fontList.empty())
                fontList = DefaultFontList();
            if (auto font = weakFont.lock())
            {
                const std::wstring selected = Widen(model->GetString("appearance.english_font", "Segoe UI"));
                const std::wstring selectedCss = Widen(model->GetString("appearance.english_font_css_family"));
                std::vector<DropdownOption> options;
                // populateDropdownMenu: an uninstalled selection still gets a row, first.
                if (!selected.empty() && std::find(fontList.begin(), fontList.end(), selected) == fontList.end())
                    options.push_back(
                        {selected, Narrow(selected), false, selectedCss.empty() ? selected : selectedCss});
                for (const auto &name : fontList)
                    options.push_back(
                        {name, Narrow(name), false, name == selected && !selectedCss.empty() ? selectedCss : name});
                if (options.size() != font->GetOptions().size())
                    font->SetOptions(std::move(options));
                font->SetValue(Narrow(selected));
            }
            std::vector<std::wstring> fallbacks;
            if (model->Find("appearance.fallback_fonts"))
                fallbacks = StringArray(model->Find("appearance.fallback_fonts"));
            else
                fallbacks = {Widen(model->GetString("appearance.font", "Noto Sans SC")),
                             Widen(model->GetString("appearance.default_font", "Microsoft YaHei"))};
            fallback->Sync(fallbacks, fontList);

            const std::string color = model->GetString("appearance.cand_text_color", "auto");
            D2D1_COLOR_F parsed{};
            const bool automatic = color == "auto" || !ParseCssColor(color, parsed);
            if (automatic)
                parsed = ResolveLight(*model, "appearance.theme_cand") ? Hex(0x1A1A1A) : Hex(0xE9E8E8);
            if (auto swatch = weakSwatch.lock())
                swatch->SetColor(parsed);
            if (auto reset = weakReset.lock())
                reset->SetActive(automatic);
        });
    }

    // 主题模式
    {
        auto column = std::make_shared<Column>(0.0f);
        auto expander = MakeExpander(L"主题模式");
        column->Add(std::make_shared<SettingRow>(
            expander, BindDropdown(context, "appearance.theme_mode",
                                   Options({{"dark", L"深色"}, {"light", L"浅色"}, {"system", L"跟随系统"}}))));
        auto details = std::make_shared<Column>(0.0f);
        details->Add(SurfaceRow(context, L"设置界面主题", "appearance.theme_settings"));
        details->Add(SurfaceRow(context, L"候选窗口主题", "appearance.theme_cand"));
        details->Add(SurfaceRow(context, L"悬浮工具栏主题", "appearance.theme_ftb"));
        details->Add(SurfaceRow(context, L"菜单主题", "appearance.theme_menu"));
        details->Add(SurfaceRow(context, L"表情面板主题", "appearance.theme_emoji"));
        details->Add(SurfaceRow(context, L"屏幕键盘主题", "appearance.theme_screen_keyboard"));
        details->Add(SurfaceRow(context, L"手写识别板主题", "appearance.theme_handwriting"));
        details->Add(SurfaceRow(context, L"语音输入弹出条主题", "appearance.theme_voice"));
        // .theme-surface-details { margin: 4px 0 0 18px }
        details->SetMargin({18.0f, 4.0f, 0.0f, 0.0f});
        auto hideable = std::make_shared<Hideable>(details, false);
        BindExpander(expander, hideable);
        column->Add(hideable);
        page.AddSection(column);
    }

    // 候选项排列方式：横排的强制单行、固定宽度与竖排的固定高度只在对应排列下出现。
    {
        auto rows = std::make_shared<Column>(0.0f);
        rows->Add(MakeRow(L"候选项排列方式", L"",
                          BindDropdown(context, "appearance.candidate_window_layout",
                                       Options({{"horizontal", L"横向"}, {"vertical", L"纵向"}}))));
        // 分隔线随行一起隐藏。
        const auto withDivider = [](VisualPtr row) {
            auto column = std::make_shared<Column>(0.0f);
            column->Add(std::make_shared<Divider>());
            column->Add(std::move(row));
            return column;
        };

        auto singleLine =
            std::make_shared<Dimmable>(MakeRow(L"强制单行", L"候选窗口最宽可到当前屏幕宽度，超出屏幕才换行",
                                               BindToggle(context, "appearance.candidate_window_single_line")));
        auto singleLineRow = std::make_shared<Hideable>(withDivider(singleLine), false);
        rows->Add(singleLineRow);

        auto fixedWidthRow = std::make_shared<Hideable>(
            withDivider(MakeRow(L"固定宽度", L"候选窗口宽度固定为屏幕宽度的一定比例，一行放不下的候选移到下一页",
                                BindToggle(context, "appearance.candidate_window_fixed_width"))),
            false);
        rows->Add(fixedWidthRow);

        auto slider = std::make_shared<ScreenFractionSlider>([host]() { return SettingsMonitorWidthPx(host->Hwnd()); });
        slider->SetOnCommit(
            [model](double ratio) { model->Update("appearance.candidate_window_fixed_width_ratio", ratio); });
        auto widthRow = MakeRow(L"窗口宽度", L"占当前屏幕宽度的比例，靠近刻度时自动吸附", slider, true);
        widthRow->SetMinHeight(48.0f);
        widthRow->SetMargin({18.0f, 0.0f, 0.0f, 4.0f});
        auto widthRowHideable = std::make_shared<Hideable>(widthRow, false);
        rows->Add(widthRowHideable);

        auto fixedHeightRow = std::make_shared<Hideable>(
            withDivider(MakeRow(L"固定高度", L"候选窗口始终保持放得下每页候选项数量的高度，候选少时下方留空",
                                BindToggle(context, "appearance.candidate_window_fixed_height"))),
            false);
        rows->Add(fixedHeightRow);
        page.AddSection(rows);

        context.OnConfig([model, weakSingle = std::weak_ptr<Hideable>(singleLineRow),
                          weakSingleDim = std::weak_ptr<Dimmable>(singleLine),
                          weakFixedWidth = std::weak_ptr<Hideable>(fixedWidthRow),
                          weakWidth = std::weak_ptr<Hideable>(widthRowHideable),
                          weakSlider = std::weak_ptr<ScreenFractionSlider>(slider),
                          weakFixedHeight = std::weak_ptr<Hideable>(fixedHeightRow)]() {
            const bool horizontal = model->GetString("appearance.candidate_window_layout", "vertical") == "horizontal";
            const bool fixedWidth = model->GetBool("appearance.candidate_window_fixed_width", false);
            const double ratio = model->GetNumber("appearance.candidate_window_fixed_width_ratio", 0.25);
            if (auto row = weakSingle.lock())
                row->SetVisible(horizontal);
            // 固定宽度下宽度已经定死，强制单行不起作用。
            if (auto dim = weakSingleDim.lock())
                dim->SetDisabled(fixedWidth);
            if (auto row = weakFixedWidth.lock())
                row->SetVisible(horizontal);
            if (auto row = weakWidth.lock())
                row->SetVisible(horizontal && fixedWidth);
            if (auto control = weakSlider.lock())
            {
                control->SetCommitted(ratio);
                control->SetValue(ratio);
            }
            if (auto row = weakFixedHeight.lock())
                row->SetVisible(!horizontal);
        });
    }

    // 行内预编辑
    page.AddSection(MakeRows({
        MakeRow(L"行内预编辑", L"",
                BindDropdown(context, "appearance.tsf_preedit_style",
                             Options({{"raw", L"原始按键"}, {"pinyin", L"拼音分词"}, {"empty", L"不显示"}}))),
        MakeRow(L"双拼显示转换后的全拼", L"双拼方案下，原始按键显示不带分词符号的全拼，拼音分词显示带分词符号的全拼",
                BindToggle(context, "appearance.tsf_preedit_shuangpin_quanpin")),
    }));

    // 候选窗预编辑
    page.AddSection(MakeRows({
        MakeRow(L"候选窗预编辑", L"",
                BindDropdown(context, "appearance.candidate_window_preedit_style",
                             Options({{"pinyin", L"拼音分词"}, {"empty", L"不显示"}}))),
        MakeRow(L"双拼显示转换后的全拼", L"双拼方案下，候选窗预编辑显示带分词符号的全拼",
                BindToggle(context, "appearance.candidate_window_preedit_shuangpin_quanpin")),
    }));

    return page.Build();
}
} // namespace native_settings
