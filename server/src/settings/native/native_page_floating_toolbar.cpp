#include "settings/native/native_page_skin_common.h"

#include "msimeui/DeviceResources.h"

#include <algorithm>
#include <cmath>

// partials/floating-toolbar.html + modules/floating-toolbar.ts + floating-toolbar.css.
namespace native_settings
{
namespace
{
using skinpreview::Band;
using skinpreview::FlushCard;

const char *const kToolbarItems[6] = {"fullwidth", "punctuation",     "character_set",
                                      "emoji",     "screen_keyboard", "settings"};

D2D1_COLOR_F DividerColor()
{
    return CurrentPalette().divider;
}

D2D1_COLOR_F BorderColor()
{
    return CurrentPalette().border;
}

// `.floating-toolbar-setting-row`: `padding: 20px 24px`.
std::shared_ptr<Band> SettingBand(VisualPtr content)
{
    return std::make_shared<Band>(std::move(content), Thickness{24.0f, 20.0f, 24.0f, 20.0f});
}

std::shared_ptr<Label> PreviewLabel(float marginBottom)
{
    TextStyle style;
    style.size = kFontCaption;
    auto label = std::make_shared<Label>(L"预览", style, TextRole::Muted);
    label->SetMargin({0.0f, 0.0f, 0.0f, marginBottom});
    return label;
}

D2D1_COLOR_F ParseTextOverride(const std::string &value)
{
    D2D1_COLOR_F color = {};
    if (value.empty() || value == "auto" || !ParseCssColor(value, color))
        return {};
    return color;
}

// `.floating-toolbar-required-option`: a checked, disabled box (`opacity: 0.72` on the
// input only, the label keeps its color) with "始终显示" pushed to the right.
class RequiredOption : public msimeui::Visual
{
  public:
    RequiredOption()
    {
        text_.Set(L"中英文切换", BodyStyle());
        TextStyle note;
        note.size = kFontCaption;
        note_.Set(L"始终显示", note);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        // padding 8px 4px around the 18px box (Chrome margins 3px 3px 3px 4px), min-height 34px.
        const SizeF label = text_.Measure(availableSize.width);
        return {availableSize.width, std::max(34.0f, std::max(24.0f, label.height) + 16.0f)};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        const RectF box = {bounds_.x + 4.0f + 4.0f, bounds_.y + (bounds_.height - 24.0f) * 0.5f + 3.0f, 18.0f, 18.0f};
        PushOpacity(dr, 0.72f);
        FillRounded(dr, box, 2.0f, p.checkboxChecked);
        ID2D1RenderTarget *target = dr.GetRenderTarget();
        if (ID2D1SolidColorBrush *white = dr.GetSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)))
        {
            D2D1_MATRIX_3X2_F previous;
            target->GetTransform(&previous);
            target->SetTransform(D2D1::Matrix3x2F::Rotation(45.0f) *
                                 D2D1::Matrix3x2F::Translation(box.x + 9.0f, box.y + 8.5f) * previous);
            target->FillRectangle(D2D1::RectF(1.0f, -5.5f, 3.0f, 5.5f), white);
            target->FillRectangle(D2D1::RectF(-3.0f, 3.5f, 3.0f, 5.5f), white);
            target->SetTransform(previous);
        }
        PopOpacity(dr);
        const float textX = box.x + 18.0f + 3.0f + 8.0f;
        const SizeF label = text_.Measure(bounds_.width);
        text_.Draw(dr, {textX, bounds_.y + (bounds_.height - label.height) * 0.5f}, p.text, bounds_.width);
        const SizeF note = note_.Measure(bounds_.width);
        note_.Draw(dr,
                   {bounds_.x + bounds_.width - 4.0f - note.width, bounds_.y + (bounds_.height - note.height) * 0.5f},
                   p.textMuted, bounds_.width);
    }

  private:
    TextLayout text_;
    TextLayout note_;
};

// `.caret-state-option-title` row: 14px title with the hairline stroke, and the shared
// description rule (12px, line-height 1.6, margin-top 4px).
VisualPtr OptionTitle(std::wstring title, std::wstring description)
{
    auto column = std::make_shared<Column>(0.0f);
    column->Add(MakeSubTitle(std::move(title)));
    column->Add(MakeDescription(std::move(description)));
    return column;
}

// `.floating-toolbar-appearance .section-header { padding: 16px 24px; gap: 16px }`.
VisualPtr AppearanceRow(std::wstring title, std::wstring description, VisualPtr control)
{
    auto row = MakeRow(std::move(title), std::move(description), std::move(control));
    return std::make_shared<Band>(row, Thickness{24.0f, 16.0f, 24.0f, 16.0f});
}
} // namespace

VisualPtr BuildFloatingToolbarPage(PageContext &context)
{
    SettingsModel *model = &context.model;
    PageBuilder page(L"悬浮工具栏");

    // ---- Toolbar card -------------------------------------------------------------
    auto toolbarCard = std::make_shared<FlushCard>();
    toolbarCard->Add(SettingBand(
        std::make_shared<SettingRow>(MakeTitleBlock(L"在桌面显示悬浮工具栏", L"快速访问输入法状态与常用功能"),
                                     BindToggle(context, "general.floating_toolbar"))));

    auto autoHide = SettingBand(std::make_shared<SettingRow>(
        OptionTitle(L"自动隐藏工具栏",
                    L"工具栏显示一段时间无交互后淡出隐藏；鼠标悬停不隐藏，切换中英等状态变化时重新显示"),
        BindToggle(context, "general.floating_toolbar_auto_hide")));
    autoHide->SetTopRule(DividerColor);
    toolbarCard->Add(autoHide);

    // `.ftb-auto-hide-delay-row`: indented 48px, `is-disabled` while auto hide is off.
    auto delay = std::make_shared<Band>(
        std::make_shared<SettingRow>(OptionTitle(L"自动隐藏延时", L"工具栏显示多久后开始淡出（秒）"),
                                     BindStepper(context, "general.floating_toolbar_auto_hide_delay", 1, 60)),
        Thickness{48.0f, 20.0f, 24.0f, 20.0f});
    toolbarCard->Add(delay);

    auto shadowRow =
        SettingBand(std::make_shared<SettingRow>(OptionTitle(L"工具栏阴影", L"在工具栏周围显示与候选窗一致的柔和阴影"),
                                                 BindToggle(context, "general.floating_toolbar_shadow")));
    shadowRow->SetTopRule(DividerColor);
    toolbarCard->Add(shadowRow);

    auto toolbar = std::make_shared<skinpreview::ToolbarPreview>();
    auto host = std::make_shared<Band>(toolbar, Thickness{0.0f, 14.0f, 0.0f, 22.0f});
    auto previewColumn = std::make_shared<Column>(0.0f);
    previewColumn->Add(PreviewLabel(28.0f));
    previewColumn->Add(host);
    auto preview = std::make_shared<Band>(previewColumn, Thickness{24.0f, 20.0f, 24.0f, 30.0f});
    preview->SetMinHeight(190.0f);
    preview->SetTopRule(BorderColor);
    preview->SetBackground([]() {
        const Palette &p = CurrentPalette();
        return p.light ? Rgba(0, 0, 0, 0.03f) : p.surfaceSubtle;
    });
    toolbarCard->Add(preview);
    page.AddRaw(toolbarCard);

    // ---- Caret state indicator card -------------------------------------------------
    auto caretCard = std::make_shared<FlushCard>();
    auto heading = SettingBand(std::make_shared<SettingRow>(
        MakeTitleBlock(L"光标状态提示", L"在文字光标附近短暂显示输入法状态，可与悬浮工具栏同时开启"), nullptr));
    heading->SetBottomRule(DividerColor);
    caretCard->Add(heading);
    caretCard->Add(SettingBand(
        std::make_shared<SettingRow>(OptionTitle(L"切换输入法状态时提示", L"切换输入模式、全半角或中英文标点时显示"),
                                     BindToggle(context, "general.caret_state_indicator"))));
    caretCard->Add(SettingBand(std::make_shared<SettingRow>(
        OptionTitle(L"切换输入框时提示",
                    L"焦点移到另一个输入框时（包括同一窗口内和其他窗口中的），也提示当前中英文状态"),
        BindToggle(context, "general.caret_state_indicator_on_focus"))));

    Dropdown::Options compact;
    compact.compact = true;
    caretCard->Add(
        SettingBand(std::make_shared<SettingRow>(OptionTitle(L"提示位置", L"选择光标状态提示显示在光标的哪个方向"),
                                                 BindDropdown(context, "general.caret_state_indicator_position",
                                                              Options({{"top-left", L"左上方"},
                                                                       {"top", L"正上方"},
                                                                       {"top-right", L"右上方"},
                                                                       {"bottom-left", L"左下方"},
                                                                       {"bottom", L"正下方"},
                                                                       {"bottom-right", L"右下方"}}),
                                                              compact))));

    auto caret = std::make_shared<skinpreview::CaretStatePreview>();
    auto caretColumn = std::make_shared<Column>(0.0f);
    caretColumn->Add(PreviewLabel(28.0f));
    caretColumn->Add(caret);
    auto caretPreview = std::make_shared<Band>(caretColumn, Thickness{24.0f, 20.0f, 24.0f, 30.0f});
    caretPreview->SetTopRule(BorderColor);
    caretPreview->SetBackground([]() { return CurrentPalette().surfaceSubtle; });
    caretCard->Add(caretPreview);
    page.AddRaw(caretCard);

    // ---- Scale and icon size ------------------------------------------------------
    std::vector<VisualPtr> appearanceRows;
    appearanceRows.push_back(
        AppearanceRow(L"工具栏缩放", L"相对系统 DPI 的额外缩放，不改变系统显示缩放",
                      BindDropdown(context, "general.floating_toolbar_scale",
                                   Options({{"0.75", L"75%"}, {"1", L"100%"}, {"1.25", L"125%"}, {"1.5", L"150%"}}),
                                   compact, ValueKind::Number)));
    appearanceRows.push_back(AppearanceRow(L"图标尺寸", L"图标基准大小（像素），再乘以上方缩放",
                                           BindDropdown(context, "general.floating_toolbar_font_size",
                                                        Options({{"16", L"16"},
                                                                 {"18", L"18"},
                                                                 {"20", L"20"},
                                                                 {"22", L"22"},
                                                                 {"24", L"24"},
                                                                 {"26", L"26"},
                                                                 {"28", L"28"}}),
                                                        compact, ValueKind::Integer)));
    page.AddSection(MakeRows(appearanceRows));

    // ---- Components ---------------------------------------------------------------
    auto components = std::make_shared<Column>(0.0f);
    components->Add(MakeRow(L"工具栏组件", L"勾选要显示在悬浮工具栏中的功能", nullptr));
    auto list = std::make_shared<Column>(0.0f);
    list->Add(std::make_shared<RequiredOption>());
    const wchar_t *labels[6] = {L"全角 / 半角", L"中英文标点", L"简繁切换", L"表情与符号", L"屏幕键盘", L"设置"};
    for (int i = 0; i < 6; ++i)
    {
        list->Add(std::make_shared<Divider>(Divider::Kind::Option));
        list->Add(BindCheck(context, labels[i], std::string("general.floating_toolbar_") + kToolbarItems[i]));
    }
    components->Add(std::make_shared<Band>(list, Thickness{20.0f, 0.0f, 20.0f, 10.0f}));
    page.AddSection(components);

    std::weak_ptr<Band> weakDelay = delay;
    auto sync = [model, toolbar, caret, weakDelay]() {
        if (auto row = weakDelay.lock())
            row->SetDimmed(!model->GetBool("general.floating_toolbar_auto_hide", false));

        const std::string skin = model->GetString("appearance.candidate_skin", "fluent");
        skinpreview::ToolbarPreview::Options options;
        const double scale = model->GetNumber("general.floating_toolbar_scale", 1.0);
        options.scale = std::isfinite(scale) && scale > 0.0 ? static_cast<float>(scale) : 1.0f;
        const double iconSize = model->GetNumber("general.floating_toolbar_font_size", 22.0);
        options.iconSize = iconSize > 0.0 ? static_cast<float>(iconSize) : 22.0f;
        options.shadow = model->GetBool("general.floating_toolbar_shadow", true);
        options.centered = true;
        for (int i = 0; i < 6; ++i)
            options.items[i] =
                model->GetBool(std::string("general.floating_toolbar_") + kToolbarItems[i], options.items[i]);
        toolbar->Set(skinpreview::ResolveToolbarLook(*model, skin,
                                                     skinpreview::ResolveSurfaceLight(*model, "appearance.theme_ftb")),
                     options);

        const bool candLight = skinpreview::ResolveSurfaceLight(*model, "appearance.theme_cand");
        caret->Set(skinpreview::ResolveCandidateLook(*model, skin, candLight),
                   model->GetString("general.caret_state_indicator_position", "top-left"),
                   ParseTextOverride(model->GetString("appearance.cand_text_color", "auto")));
    };
    context.OnConfig(sync);
    return page.Build();
}
} // namespace native_settings
