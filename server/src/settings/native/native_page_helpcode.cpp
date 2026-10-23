#include "settings/native/native_settings_pages.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Fonts.h"
#include "msimeui/Window.h"

#include <dwrite_1.h>

#include <algorithm>

// partials/helpcode.html + modules/helpcode.ts + styles/modules/helpcode.css
namespace native_settings
{
namespace
{
using Microsoft::WRL::ComPtr;

// A paragraph mixing the page font with <code> runs (monospace), for the custom
// helpcode description. `breakAll` mirrors `word-break: break-all`.
class RichText : public msimeui::Visual
{
  public:
    struct Run
    {
        std::wstring text;
        bool code = false;
    };

    RichText(std::vector<Run> runs, bool breakAll = false) : runs_(std::move(runs)), breakAll_(breakAll)
    {
    }

    void SetRuns(std::vector<Run> runs)
    {
        runs_ = std::move(runs);
        layout_.Reset();
        InvalidateMeasure();
        InvalidateVisual();
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        IDWriteTextLayout *layout = Get(availableSize.width);
        if (!layout)
            return {};
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        return {std::min(std::ceil(metrics.widthIncludingTrailingWhitespace), availableSize.width), metrics.height};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        IDWriteTextLayout *layout = Get(bounds_.width);
        ID2D1RenderTarget *target = dr.GetRenderTarget();
        ID2D1SolidColorBrush *brush = target ? dr.GetSolidColorBrush(CurrentPalette().textMuted) : nullptr;
        if (layout && brush)
            target->DrawTextLayout(D2D1::Point2F(bounds_.x, bounds_.y), layout, brush,
                                   D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }

  private:
    IDWriteTextLayout *Get(float width)
    {
        width = std::max(width, 1.0f);
        if (layout_ && width_ == width)
            return layout_.Get();
        layout_.Reset();
        width_ = width;
        IDWriteFactory *factory = DWriteFactory();
        if (!factory)
            return nullptr;
        if (!format_)
        {
            if (FAILED(factory->CreateTextFormat(UiFontFamily().c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, kFontCaption,
                                                 L"zh-cn", format_.GetAddressOf())))
                return nullptr;
            msimeui::ApplyUiFontFallback(factory, format_.Get());
        }
        std::wstring text;
        std::vector<DWRITE_TEXT_RANGE> codes;
        for (const Run &run : runs_)
        {
            if (run.code)
                codes.push_back({static_cast<UINT32>(text.size()), static_cast<UINT32>(run.text.size())});
            text += run.text;
        }
        if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format_.Get(), width,
                                             100000.0f, layout_.GetAddressOf())))
            return nullptr;
        layout_->SetWordWrapping(breakAll_ ? DWRITE_WORD_WRAPPING_CHARACTER : DWRITE_WORD_WRAPPING_WRAP);
        // line-height: 1.6, the extra space split above and below each line.
        UINT32 count = 0;
        layout_->GetLineMetrics(nullptr, 0, &count);
        std::vector<DWRITE_LINE_METRICS> lines(std::max<UINT32>(count, 1));
        layout_->GetLineMetrics(lines.data(), static_cast<UINT32>(lines.size()), &count);
        const DWRITE_LINE_METRICS &line = lines[0];
        if (count > 0 && line.height > 0.0f)
        {
            const float spacing = kFontCaption * kLineHeightCaption;
            layout_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, spacing,
                                    line.baseline + (spacing - line.height) * 0.5f);
        }
        for (const DWRITE_TEXT_RANGE &range : codes)
            layout_->SetFontFamilyName(L"Consolas", range);
        return layout_.Get();
    }

    std::vector<Run> runs_;
    bool breakAll_ = false;
    float width_ = -1.0f;
    ComPtr<IDWriteTextFormat> format_;
    ComPtr<IDWriteTextLayout> layout_;
};

// .helpcode-custom-button: 30px, transparent, muted 12px text, 6px radius.
class CustomButton : public msimeui::Visual
{
  public:
    CustomButton(std::wstring text, std::function<void()> onClick)
        : text_(std::move(text)), onClick_(std::move(onClick))
    {
        // margin-top: 2px
        SetMargin({0.0f, 2.0f, 0.0f, 0.0f});
    }

    SizeF Measure(const SizeF &) override
    {
        return {MeasureTextWidth(text_, Style()) + 20.0f + CssBorder() * 2.0f, 30.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        if (hovered_)
            FillRounded(dr, bounds_, 6.0f, p.buttonSecondaryBg);
        StrokeRounded(dr, bounds_, 6.0f, p.border, 1.0f);
        DrawTextInRect(dr, text_, Style(), bounds_, hovered_ ? p.text : p.textMuted, true);
    }

    bool OnMouseDown(const POINT &, WPARAM) override
    {
        pressed_ = true;
        return true;
    }

    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        const bool clicked = pressed_ && window_ && HitTest(window_->ClientPixelsToDips(point));
        pressed_ = false;
        if (clicked && onClick_)
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

    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, IDC_HAND);
    }

  private:
    static TextStyle Style()
    {
        TextStyle style;
        style.size = kFontCaption;
        return style;
    }

    std::wstring text_;
    std::function<void()> onClick_;
    bool hovered_ = false;
    bool pressed_ = false;
};

std::vector<DropdownOption> BuiltinSchemes()
{
    return Options({{"lantian", L"蓝天小雨点"},
                    {"ziranma", L"自然码"},
                    {"shouyou2_0", L"首右2.0"},
                    {"shouyouplus", L"首右plus"},
                    {"xiaohe", L"小鹤"},
                    {"jiajia", L"拼音加加"}});
}

// Built-in schemes followed by the custom ones the host scanned from helpcodes/custom.
std::vector<DropdownOption> SchemeOptions(const SettingsModel &model)
{
    std::vector<DropdownOption> options = BuiltinSchemes();
    if (const Json *schemas = model.Find("helpcode.custom_schemas"); schemas && schemas->is_array())
    {
        for (const Json &schema : *schemas)
        {
            if (!schema.is_object() || !schema.contains("id") || !schema["id"].is_string() ||
                !schema.contains("name") || !schema["name"].is_string())
                continue;
            options.push_back({Widen(schema["name"].get<std::string>()), schema["id"].get<std::string>(), false, {}});
        }
    }
    return options;
}

// Scheme dropdown whose options follow the snapshot (setupDropdownMenu + config-sync).
std::shared_ptr<Dropdown> SchemeDropdown(PageContext &context, std::string path)
{
    Dropdown::Options settings;
    settings.minWidth = 110.0f;
    settings.paddingLeft = 9.0f;
    auto dropdown = std::make_shared<Dropdown>(BuiltinSchemes(), settings);
    SettingsModel *model = &context.model;
    dropdown->SetOnChanged([model, path](size_t, const DropdownOption &option) { model->Update(path, option.value); });
    std::weak_ptr<Dropdown> weak = dropdown;
    context.OnConfig([weak, model, path]() {
        auto control = weak.lock();
        if (!control)
            return;
        std::vector<DropdownOption> options = SchemeOptions(*model);
        const auto &current = control->GetOptions();
        const bool same =
            current.size() == options.size() && std::equal(current.begin(), current.end(), options.begin(),
                                                           [](const DropdownOption &a, const DropdownOption &b) {
                                                               return a.value == b.value && a.label == b.label;
                                                           });
        if (!same)
            control->SetOptions(std::move(options));
        control->SetValue(model->GetString(path));
    });
    return dropdown;
}

// The trigger keys of the shuangpin end helpcode (indirect) and the mid-sentence helpcode (indirect). Both
// groups list the same keys in the same order, and a key belongs to one group at most: checking it on one
// side unchecks it on the other, which the host does too (SetHelpcodeTrigger), even when that was the
// other side's last key. A group's own last key cannot be unchecked.
struct TriggerChecks
{
    std::vector<std::shared_ptr<CheckOption>> end;
    std::vector<std::shared_ptr<CheckOption>> mid;
};

TriggerChecks ExclusiveTriggerChecks(PageContext &context,
                                     const std::vector<std::pair<std::wstring, std::string>> &endItems,
                                     const std::vector<std::pair<std::wstring, std::string>> &midItems)
{
    // The page and its controls live as long as the window, so the checks simply share the groups.
    auto groups = std::make_shared<TriggerChecks>();
    SettingsModel *model = &context.model;
    const auto build = [&](const std::vector<std::pair<std::wstring, std::string>> &items, bool end) {
        auto &own = end ? groups->end : groups->mid;
        for (size_t i = 0; i < items.size(); ++i)
        {
            auto check = std::make_shared<CheckOption>(items[i].first, model->GetBool(items[i].second));
            const std::string path = items[i].second;
            TriggerChecks *all = groups.get();
            CheckOption *self = check.get();
            check->SetOnChanged([groups, all, self, i, end, model, path](bool checked) {
                const auto &mine = end ? all->end : all->mid;
                const auto &theirs = end ? all->mid : all->end;
                if (!checked &&
                    std::none_of(mine.begin(), mine.end(), [](const auto &item) { return item->IsChecked(); }))
                {
                    self->SetChecked(true);
                    return;
                }
                if (checked && i < theirs.size())
                    theirs[i]->SetChecked(false);
                model->Update(path, checked);
            });
            std::weak_ptr<CheckOption> weak = check;
            context.OnConfig([weak, model, path]() {
                if (auto control = weak.lock())
                    control->SetChecked(model->GetBool(path));
            });
            own.push_back(check);
        }
    };
    build(endItems, true);
    build(midItems, false);
    return *groups;
}

// `.content-page-option`: margin-top 8px, check options split by `.input-option-divider`,
// then the description.
VisualPtr CheckGroup(const std::vector<std::shared_ptr<CheckOption>> &checks, std::wstring description)
{
    auto column = std::make_shared<Column>(0.0f);
    column->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
    for (size_t i = 0; i < checks.size(); ++i)
    {
        if (i > 0)
            column->Add(std::make_shared<Divider>(Divider::Kind::Option));
        column->Add(checks[i]);
    }
    if (!description.empty())
        column->Add(MakeDescription(std::move(description)));
    return column;
}

// `.section-header-*-helpcode { margin-top: 12px }`
VisualPtr Spaced(VisualPtr row)
{
    row->SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
    return row;
}

// Timeout of the 刷新方案 request.
struct RefreshState
{
    bool pending = false;
    UINT_PTR timer = 0;
    HostServices *host = nullptr;
};

RefreshState &Refresh()
{
    static RefreshState state;
    return state;
}

void CALLBACK RefreshTimeout(HWND, UINT, UINT_PTR id, DWORD)
{
    KillTimer(nullptr, id);
    RefreshState &state = Refresh();
    if (!state.pending || state.timer != id)
        return;
    state.pending = false;
    state.timer = 0;
    if (state.host)
        state.host->ShowToast(L"刷新失败，请稍后重试", false);
}
} // namespace

VisualPtr BuildHelpcodePage(PageContext &context)
{
    PageBuilder page(L"辅助码");
    SettingsModel *model = &context.model;

    // 双拼末尾辅助码与句中辅助码（间接）的触发键：一个键只能归一边，见 ExclusiveTriggerChecks。
    const TriggerChecks triggers =
        ExclusiveTriggerChecks(context,
                               {{L"` 反引号", "helpcode.shuangpin_helpcode_trigger_backtick"},
                                {L"; 分号", "helpcode.shuangpin_helpcode_trigger_semicolon"},
                                {L"A 大写字母", "helpcode.shuangpin_helpcode_trigger_uppercase"}},
                               {{L"` 反引号", "helpcode.shuangpin_mid_sentence_helpcode_backtick"},
                                {L"; 分号", "helpcode.shuangpin_mid_sentence_helpcode_semicolon"},
                                {L"A 大写字母", "helpcode.shuangpin_mid_sentence_helpcode_uppercase"}});

    // 双拼辅助码（末尾）
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(MakeRow(L"双拼辅助码", L"", BindToggle(context, "helpcode.shuangpin_helpcode")));
        column->Add(
            Spaced(MakeRow(L"双拼辅助码方案", L"", SchemeDropdown(context, "helpcode.shuangpin_helpcode_schema"))));
        column->Add(Spaced(MakeRow(L"在候选窗口中显示双拼辅助码", L"",
                                   BindToggle(context, "helpcode.show_sp_helpcode_in_candidate_window"))));

        column->Add(Spaced(MakeRow(L"直接辅助码", L"", BindToggle(context, "helpcode.shuangpin_helpcode_direct"))));
        column->Add(CheckGroup({}, L"音节后直接接辅码：完整音节后多敲一个键是单码（如 ulpbx），末尾两键带大写是双码"
                                   L"（如 ulpbxY）。关闭后改用下面的间接辅助码，不敲触发键时末尾的字母一律按拼音读"));
        column->Add(Spaced(MakeRow(L"间接辅助码触发键", L"", nullptr)));
        column->Add(CheckGroup(triggers.end,
                               L"关闭直接辅助码后生效。在输入末尾敲触发键，再接一到两个辅码，大小写均可（如 "
                               L"ulpb;xy），一码重排、两码筛选；大写字母触发时第一码敲成大写（如 ulpbXy）。"
                               L"与句中辅助码的触发键互斥：勾选一边会取消另一边的同一个键，一个都不剩时引导键"
                               L"不生效。开启句中直接辅助码时，末尾辅助码由它统一识别"));
        page.AddSection(column);
    }

    // 句中辅助码：给句中某个音节加辅码，只对双拼生效。间接（引导键）和直接（万象式）两种方式互斥。
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(MakeTitleBlock(L"句中辅助码", L"给句中某个音节加辅码，整句带着锁住的字重新打分。仅双拼生效，"
                                                  L"下面两种方式互斥：开启其中一个会自动关闭另一个"));

        // 间接 and 直接 exclude each other; the host turns the other off too.
        auto direct = std::make_shared<std::weak_ptr<ToggleSwitch>>();
        auto mid = BindToggle(context, "helpcode.shuangpin_mid_sentence_helpcode", [direct](bool on) {
            if (auto other = direct->lock(); other && on)
                other->SetChecked(false);
        });
        std::weak_ptr<ToggleSwitch> midWeak = mid;
        auto directToggle = BindToggle(context, "helpcode.shuangpin_direct_helpcode", [midWeak](bool on) {
            if (auto other = midWeak.lock(); other && on)
                other->SetChecked(false);
        });
        *direct = directToggle;

        column->Add(Spaced(MakeRow(L"间接辅助码", L"", mid)));
        column->Add(CheckGroup({}, L"在完整音节后敲触发键，后面的字母筛这个音节上的字（如 ulpb`xihfa），"
                                   L"第一码大小写均可，第二码要用大写（如 ulpb`xYihfa）"));
        column->Add(Spaced(MakeRow(L"间接辅助码触发键", L"", nullptr)));
        column->Add(CheckGroup(triggers.mid,
                               L"与双拼辅助码的间接触发键互斥，规则同上。分号在微软、搜狗、紫光双拼里也是 ing "
                               L"韵母，只有在完整音节后才当触发键；按下后预编辑里同样显示为 "
                               L"`。大写字母：完整音节后直接敲大写的第一码，相当于反引号加这个字母（如 "
                               L"ulpbXihfa），第二码仍要大写；开启后这个位置的大写字母不再当末尾双码辅助"));

        column->Add(Spaced(MakeRow(L"直接辅助码", L"", directToggle)));
        column->Add(CheckGroup({}, L"万象式，不要引导键：三码字优先，四码词优先，双辅码加斜杠聚拢强制出单字。"
                                   L"开启后双拼辅助码的直接、间接两种末尾辅助码都不再生效，由它统一识别"));
        column->Add(Spaced(MakeRow(L"直接辅助码句中四码标记", L"", nullptr)));
        column->Add(CheckGroup(
            AtLeastOneChecks(context, {{L"/ 斜杠（uiab/）", "helpcode.shuangpin_direct_helpcode_slash"},
                                       {L"第二位辅码大写（uiaB）", "helpcode.shuangpin_direct_helpcode_uppercase"}}),
            L"至少保留一个。四码在句中要标记出来，否则会让位给普通切分。斜杠：两位辅码后补一个 /；不勾时 / "
            L"照常当标点。第二位辅码大写：第二位辅码用大写敲；开启后大写字母只当四码的第二位辅码"));
        page.AddSection(column);
    }

    // 全拼辅助码
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(MakeRow(L"全拼辅助码", L"", BindToggle(context, "helpcode.quanpin_helpcode")));
        column->Add(
            Spaced(MakeRow(L"全拼辅助码方案", L"", SchemeDropdown(context, "helpcode.quanpin_helpcode_schema"))));
        column->Add(Spaced(MakeRow(L"在候选窗口中显示全拼辅助码", L"",
                                   BindToggle(context, "helpcode.show_qp_helpcode_in_candidate_window"))));
        page.AddSection(column);
    }

    // 自定义辅助码
    {
        auto left = std::make_shared<Column>(0.0f);
        left->Add(MakeTitle(L"自定义辅助码"));
        auto description = std::make_shared<RichText>(std::vector<RichText::Run>{
            {L"把 UTF-8 编码的 "},
            {L".txt", true},
            {L" 辅助码文件放进下面的文件夹，点击“刷新方案”后即可在上方方案中选择。 每行写作 "},
            {L"字=辅助码", true},
            {L"；设置中默认按文件名显示，可在文件开头用 "},
            {L"# name: 中文名", true},
            {L" 和 "},
            {L"# name_en: English name", true},
            {L" 注释指定方案名称。修改正在使用的方案文件后，需要切换一次方案或重启输入法才会生效。"},
        });
        description->SetMargin({0.0f, 4.0f, 0.0f, 0.0f});
        left->Add(description);
        auto directory = std::make_shared<RichText>(std::vector<RichText::Run>{}, true);
        // .helpcode-custom-directory { margin-top: 8px } on top of the description's 4px
        directory->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
        auto directoryBox = std::make_shared<Hideable>(directory, false);
        left->Add(directoryBox);
        std::weak_ptr<RichText> directoryWeak = directory;
        std::weak_ptr<Hideable> directoryBoxWeak = directoryBox;
        context.OnConfig([model, directoryWeak, directoryBoxWeak]() {
            const Json *value = model->Find("helpcode.custom_directory");
            auto label = directoryWeak.lock();
            auto box = directoryBoxWeak.lock();
            if (!label || !box || !value || !value->is_string())
                return;
            label->SetRuns({{L"文件夹：" + Widen(value->get<std::string>())}});
            box->SetVisible(true);
        });

        HostServices *host = &context.host;
        auto actions = std::make_shared<Row>(8.0f);
        actions->SetAlign(Row::Align::Start);
        actions->Add(std::make_shared<CustomButton>(L"打开目录", [model]() { model->Post("openHelpcodeDirectory"); }));
        actions->Add(std::make_shared<CustomButton>(L"刷新方案", [model, host]() {
            // The host rescans the folder and answers with a snapshot; say so once it is
            // there, or report a failure after 5 seconds. A second click replaces the first.
            RefreshState &state = Refresh();
            if (state.timer)
                KillTimer(nullptr, state.timer);
            state.host = host;
            state.pending = true;
            state.timer = SetTimer(nullptr, 0, 5000, &RefreshTimeout);
            model->Post("configRequest");
        }));
        context.OnConfig([model, host]() {
            RefreshState &state = Refresh();
            if (!state.pending)
                return;
            state.pending = false;
            if (state.timer)
                KillTimer(nullptr, state.timer);
            state.timer = 0;
            const Json *schemas = model->Find("helpcode.custom_schemas");
            const size_t count = schemas && schemas->is_array() ? schemas->size() : 0;
            host->ShowToast(count > 0 ? L"刷新成功，找到 " + std::to_wstring(count) + L" 个自定义方案"
                                      : std::wstring(L"刷新成功，文件夹里还没有自定义方案"),
                            true);
        });

        // .section-header:has(> .helpcode-custom-actions) { align-items: flex-start; gap: 16px }
        auto header = std::make_shared<Row>(16.0f);
        header->SetAlign(Row::Align::Start);
        header->Add(left, 1.0f);
        header->Add(actions);
        page.AddSection(header);
    }
    return page.Build();
}
} // namespace native_settings
