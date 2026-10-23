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
            previewBox->SetDecorated(external && external->contains("decorationTopDip") &&
                                     (*external)["decorationTopDip"].is_number() &&
                                     (*external)["decorationTopDip"].get<double>() > 0.0);
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

        addRow(MakeRow(L"每页候选项数量", L"",
                       BindDropdown(context, "appearance.page_size", NumberOptions(3, 9), Dropdown::Options{true},
                                    ValueKind::Integer)));
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
        auto expander = std::make_shared<Expander>(L"主题模式");
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
        expander->SetOnToggled([hideable](bool expanded) { hideable->SetVisible(expanded); });
        column->Add(hideable);
        page.AddSection(column);
    }

    // 候选项排列方式
    page.AddSection(MakeRows({
        MakeRow(L"候选项排列方式", L"",
                BindDropdown(context, "appearance.candidate_window_layout",
                             Options({{"horizontal", L"横向"}, {"vertical", L"纵向"}}))),
        MakeRow(L"横向候选强制单行", L"仅横向排列时生效：候选窗口最宽可到当前屏幕宽度，超出屏幕才换行",
                BindToggle(context, "appearance.candidate_window_single_line")),
    }));

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
