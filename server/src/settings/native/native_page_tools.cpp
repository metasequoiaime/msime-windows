#include "settings/native/native_page_dictionary_panel.h"

// partials/tools-settings.html + modules/tools-settings.ts + styles/modules/tools-settings.css
namespace native_settings
{
namespace
{
// `.section-header` with `align-items: flex-start` and the toggle nudged down 2px
// (clipboard / U / E / M sections and the quick-phrase heading).
class TopAlignedRow : public msimeui::Panel
{
  public:
    TopAlignedRow(VisualPtr left, VisualPtr control, float controlTop)
        : left_(std::move(left)), control_(std::move(control)), controlTop_(controlTop)
    {
        AddChild(left_);
        AddChild(control_);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        constexpr float kGap = 10.0f;
        controlSize_ = control_->MeasureInLayout(availableSize);
        leftSize_ = left_->MeasureInLayout(
            {std::max(0.0f, availableSize.width - controlSize_.width - kGap), availableSize.height});
        return {availableSize.width, std::max(leftSize_.height, controlSize_.height + controlTop_)};
    }

    void Arrange(const RectF &finalRect) override
    {
        constexpr float kGap = 10.0f;
        bounds_ = finalRect;
        control_->ArrangeInLayout({finalRect.x + finalRect.width - controlSize_.width, finalRect.y + controlTop_,
                                   controlSize_.width, controlSize_.height});
        left_->ArrangeInLayout(
            {finalRect.x, finalRect.y, std::max(0.0f, finalRect.width - controlSize_.width - kGap), leftSize_.height});
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        for (const auto &child : children_)
            child->Render(dr);
    }

  private:
    VisualPtr left_;
    VisualPtr control_;
    float controlTop_ = 0.0f;
    SizeF leftSize_ = {};
    SizeF controlSize_ = {};
};

// Title + `.quick-phrase-description` (the shared description, max-width 52em).
VisualPtr QuickTitleBlock(const std::wstring &title, const std::wstring &description)
{
    auto column = std::make_shared<Column>(0.0f);
    column->Add(MakeTitle(title, description));
    auto desc = MakeDescription(description);
    desc->SetMaxWidth(52.0f * kFontCaption);
    column->Add(desc);
    return column;
}

VisualPtr DetailsRow(PageContext &context, const std::wstring &title, const std::wstring &description,
                     const std::shared_ptr<ToggleSwitch> &toggle)
{
    (void)context;
    auto row = MakeRow(title, description, toggle, true);
    // .toggle-section-details .section-header { padding: 2px 0 }
    row->SetMargin({0.0f, 2.0f, 0.0f, 2.0f});
    return row;
}

std::shared_ptr<Label> MakeDisabledHint()
{
    TextStyle style;
    style.size = kFontCaption;
    auto hint = std::make_shared<Label>(L"开启总开关后生效", style, TextRole::Muted);
    hint->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
    return hint;
}

// A toggle-section: expander + master switch, details that open below it. The sub
// switches keep showing their state but are greyed out while the master is off.
struct ToggleSection
{
    std::shared_ptr<Column> body;
    std::shared_ptr<Column> details;
};

ToggleSection MakeToggleSection(PageContext &context, const std::wstring &title, const std::string &masterPath,
                                std::function<void(bool disabled)> applyDisabled, std::shared_ptr<Hideable> &hintOut)
{
    ToggleSection section;
    section.body = std::make_shared<Column>(0.0f);
    auto expander = MakeExpander(title);
    auto master = BindToggle(context, masterPath);
    section.body->Add(std::make_shared<SettingRow>(expander, master));
    section.details = std::make_shared<Column>(0.0f);
    // margin: 4px 0 0 18px. On the inner column: a margin on the Hideable would still
    // take room while the details are display: none.
    section.details->SetMargin({18.0f, 4.0f, 0.0f, 0.0f});
    auto details = std::make_shared<Hideable>(section.details, false);
    section.body->Add(details);
    BindExpander(expander, details);
    hintOut = std::make_shared<Hideable>(MakeDisabledHint(), false);
    std::weak_ptr<Hideable> hint = hintOut;
    SettingsModel *model = &context.model;
    context.OnConfig([model, masterPath, applyDisabled, hint]() {
        const bool disabled = !model->GetBool(masterPath);
        if (applyDisabled)
            applyDisabled(disabled);
        if (auto locked = hint.lock())
            locked->SetVisible(disabled);
    });
    return section;
}

std::shared_ptr<Label> DetailsDescription(const std::wstring &text)
{
    auto label = MakeDescription(text);
    // .toggle-section-details .input-setting-description { margin-top: 0 }
    label->SetMargin({0.0f, 0.0f, 0.0f, 0.0f});
    return label;
}
} // namespace

VisualPtr BuildToolsPage(PageContext &context)
{
    PageBuilder page(L"实用功能");

    // 剪贴板管理
    page.AddSection(std::make_shared<TopAlignedRow>(
        QuickTitleBlock(L"剪贴板管理",
                        L"开启后，复制过的文本会显示在表情面板的剪贴板分页中。关闭后立即清空已保存的记录，"
                        L"且只记录文本类型。"),
        BindToggle(context, "utility.clipboard_history"), 2.0f));

    // 候选混输
    {
        auto zhEn = BindToggle(context, "general.cn_en_mixed_input");
        auto emoji = BindToggle(context, "general.emoji_mixed_input");
        auto kaomoji = BindToggle(context, "general.kaomoji_mixed_input");
        auto symbol = BindToggle(context, "general.symbol_mixed_input");
        auto quickCandidates = BindToggle(context, "utility.quick_phrase_candidates");
        auto quickFrequency = BindToggle(context, "utility.quick_phrase_frequency");
        auto dateTime = BindToggle(context, "utility.date_time_candidates");
        auto dateMenu = BindToggle(context, "utility.date_time_menu");
        Dropdown::Options frequency;
        frequency.minWidth = 110.0f;
        auto trigger = BindDropdown(context, "general.cn_en_mixed_input_min_chars", NumberOptions(1, 8), frequency,
                                    ValueKind::Integer);
        std::vector<std::shared_ptr<ToggleSwitch>> subs = {zhEn,           emoji,    kaomoji, symbol, quickCandidates,
                                                           quickFrequency, dateTime, dateMenu};
        std::shared_ptr<Hideable> hint;
        ToggleSection section = MakeToggleSection(
            context, L"候选混输", "utility.mixed_candidates",
            [subs, trigger](bool disabled) {
                for (const auto &toggle : subs)
                    toggle->SetDisabled(disabled);
                trigger->SetDisabled(disabled);
            },
            hint);
        auto &details = *section.details;
        details.Add(DetailsDescription(
            L"全拼 / 双拼输入中文时，把下面这些内容直接混进候选，不用切换到对应的快捷模式。五笔不混输。"));
        details.Add(DetailsRow(context, L"中英混输", L"在候选项中补充英文单词", zhEn));
        auto triggerRow = MakeRow(L"英文触发字符数", L"预编辑字母达到该长度后才出现英文候选项", trigger, true);
        triggerRow->SetMargin({0.0f, 2.0f, 0.0f, 2.0f});
        auto triggerBlock = std::make_shared<Dimmable>(triggerRow);
        // .frequency-option-content { margin-top: 8px }
        triggerBlock->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
        std::weak_ptr<Dimmable> weakBlock = triggerBlock;
        SettingsModel *model = &context.model;
        context.OnConfig([weakBlock, model]() {
            if (auto block = weakBlock.lock())
                block->SetDisabled(!model->GetBool("general.cn_en_mixed_input"));
        });
        details.Add(triggerBlock);
        details.Add(DetailsRow(
            context, L"emoji 混输",
            L"输入恰好等于某个 emoji 关键词时加入 emoji（排在候选项列表最后，不挤占正常汉字候选的位置）", emoji));
        details.Add(
            DetailsRow(context, L"颜文字混输", L"在候选项中加入匹配的颜文字（排在列表末尾的 emoji 之后）", kaomoji));
        details.Add(DetailsRow(context, L"符号混输",
                               L"输入恰好等于某个符号编码时加入符号，首个紧跟首选，例如 alpha → α、infinity → "
                               L"∞、szq → ①、yi → ① ⑴ ⒈、dian → · 、、shuminghao → 《》 〈〉",
                               symbol));
        details.Add(
            DetailsRow(context, L"快捷短语混入候选",
                       L"输入的编码与快捷短语的编码完全一致时，快捷短语直接出现在候选首位（固定位置的候选之后），"
                       L"不用按 Shift+K",
                       quickCandidates));
        details.Add(
            DetailsRow(context, L"快捷短语参与调频",
                       L"常越过快捷短语选别的候选时，快捷短语会往后退，最多退到首页最后一位；选中快捷短语会让它往"
                       L"前挪。关闭后快捷短语始终在首位",
                       quickFrequency));
        details.Add(DetailsRow(context, L"日期时间混入候选",
                               L"输入 rq / riqi / date、sj / shijian / time、xq / xingqi / week "
                               L"时，当前日期、时间或星期直接出现在首个候选之后，不用按 Shift+T",
                               dateTime));
        details.Add(
            DetailsRow(context, L"日期格式入口",
                       L"日期时间混入候选时，在第三位放一个「📅日期」，选中后候选框换成全部日期格式，和 Shift+T "
                       L"模式相同",
                       dateMenu));
        details.Add(hint);
        page.AddSection(section.body);
    }

    // 语言栏
    {
        auto imeMode = BindToggle(context, "language_bar.show_ime_mode");
        auto doubleSingle = BindToggle(context, "language_bar.show_double_single_byte");
        auto punctuation = BindToggle(context, "language_bar.show_punctuation");
        std::vector<std::shared_ptr<ToggleSwitch>> subs = {imeMode, doubleSingle, punctuation};
        std::shared_ptr<Hideable> hint;
        ToggleSection section = MakeToggleSection(
            context, L"语言栏", "language_bar.enabled",
            [subs](bool disabled) {
                for (const auto &toggle : subs)
                    toggle->SetDisabled(disabled);
            },
            hint);
        auto &details = *section.details;
        details.Add(
            DetailsDescription(L"在任务栏语言栏（输入法指示器旁）分别控制中/英模式、全/半角、中文标点三个按钮的显"
                               L"示。修改后需重启生效。"));
        details.Add(DetailsRow(context, L"中/英模式按钮", L"显示中/英文输入模式切换按钮", imeMode));
        details.Add(
            DetailsRow(context, L"全/半角按钮", L"显示全角/半角切换按钮（只在使用桌面语言栏时才显示）", doubleSingle));
        details.Add(DetailsRow(context, L"中文标点按钮", L"显示中文/英文标点切换按钮（只在使用桌面语言栏时才显示）",
                               punctuation));
        details.Add(hint);
        page.AddSection(section.body);
    }

    // 快捷短语(K 模式): .dict-card.quick-phrase-card
    auto panel = std::make_shared<DictionaryPanel>(context, DictionaryPanel::Kind::Quick);
    {
        auto body = std::make_shared<Column>(0.0f);
        auto heading = std::make_shared<TopAlignedRow>(
            QuickTitleBlock(L"快捷短语(K 模式)",
                            L"中文模式下按 Shift+K，再输入编码即可调用快捷短语。要让快捷短语直接混进"
                            L"候选，见上方「候选混输」"),
            BindToggle(context, "utility.quick_phrase"), 2.0f);
        // .quick-phrase-heading { padding: 20px 24px 4px }
        heading->SetMargin({24.0f, 20.0f, 24.0f, 4.0f});
        body->Add(heading);
        panel->AddTo(*body);
        page.AddRaw(std::make_shared<PageCard>(body, PageCard::Style::DictCard, Thickness{}), 18.0f);
    }
    // .quick-phrase-export-section: padding 4px 24px, row min-height 42px without a divider.
    page.AddRaw(
        std::make_shared<PageCard>(MakeExportRow(
                                       L"导出快捷短语用户词库", [panel]() { panel->Export("quick"); }, 42.0f, false),
                                   PageCard::Style::Section, Thickness{24.0f, 4.0f, 24.0f, 4.0f}),
        18.0f);

    page.AddSection(MakeRow(L"日期与时间快捷输入(T 模式)",
                            L"中文模式下按 Shift+T，再输入 rq / riqi / date 可输入当前日期；输入 sj / shijian / time "
                            L"可输入当前时间；输入 xq / xingqi / week 可输入当前星期。全拼、双拼、五笔都支持。",
                            BindToggle(context, "utility.date_time_mode")));
    page.AddSection(std::make_shared<TopAlignedRow>(
        QuickTitleBlock(L"U 模式（部件拆字）",
                        L"中文模式下按 Shift+U，输入一个字各组成部分的拼音连写即可候选出该字，候"
                        L"选上会反显它的读音。例如「晶」= 日+日+日，输入 ririri。空格上屏；数字选"
                        L"词。"),
        BindToggle(context, "utility.radical_mode"), 2.0f));
    page.AddSection(std::make_shared<TopAlignedRow>(
        QuickTitleBlock(L"Emoji 快捷输入(E 模式)",
                        L"中文模式下按 Shift+E，再输入全拼 / 简拼 / 双拼 / 英文关键词即可输入 "
                        L"emoji。例如 xiaolian、xl、xnlm（小鹤双拼）、laugh。空格上屏；数字选词"
                        L"。"),
        BindToggle(context, "utility.emoji_mode"), 2.0f));
    page.AddSection(std::make_shared<TopAlignedRow>(
        QuickTitleBlock(L"颜文字快捷输入(M 模式)",
                        L"中文模式下按 Shift+M，再输入全拼 / 简拼 / 双拼 / 英文关键词即可输入颜"
                        L"文字。例如 haixiu、hx、zai xiang、zx、kiss。空格上屏；数字选词。"),
        BindToggle(context, "utility.kaomoji_mode"), 2.0f));
    page.AddSection(std::make_shared<TopAlignedRow>(
        QuickTitleBlock(L"符号快捷输入(S 模式)",
                        L"中文模式下按 Shift+S，再输入分组码或符号名即可输入符号。例如 xl 希腊字母、sx "
                        L"数学符号、szq 带圈数字、lmd 罗马数字、jt 箭头、yi 数字 1 的各种写法、alpha、infinity。"
                        L"空格上屏；数字选词。"),
        BindToggle(context, "utility.symbol_mode"), 2.0f));

    const auto centered = [&context](const std::wstring &title, const std::wstring &description,
                                     const std::string &path) {
        return std::make_shared<SettingRow>(QuickTitleBlock(title, description), BindToggle(context, path));
    };
    page.AddSection(centered(L"超级简拼(J 模式)",
                             L"仅双拼下可用。中文模式下按 Shift+J，之后输入的每个字母都作为简拼检索词库，"
                             L"并按当前双拼方案转换声母。例如小鹤 nu → n'sh（你说），与 ns → n's 不同。"
                             L"全拼不需要该模式，直接输入声母就是简拼，包括 zh / ch / sh 声母，例如 nh → 你好、"
                             L"eq → 而且、zhly → 张靓颖。空格上屏；数字选词。",
                             "utility.jianpin_mode"));
    page.AddSection(
        centered(L"临时英文(Y 模式)",
                 L"中文模式下按 Shift+Y，之后输入的字母按英文处理，不会当作拼音。空格上屏当前输入；数字可选英"
                 L"文词库中的单词。上屏后自动回到中文输入。",
                 "utility.y_mode"));
    page.AddSection(
        centered(L"临时日语(R 模式)",
                 L"中文模式下按 Shift+R，之后输入的字母按日语罗马字处理。空格上屏首选，数字可选其他日语候选。"
                 L"上屏后自动回到中文输入。",
                 "utility.r_mode"));
    page.AddSection(
        centered(L"数字与计算(V 模式)",
                 L"中文模式下输入 V 再接数字或算式：V123 → 一百二十三 / 壹佰贰拾叁；V123.45 → "
                 L"壹佰贰拾叁元肆角伍分；V1+2*3 → 7（支持 + - * / 和括号）。双拼和五笔按 Shift+V；全拼大小写 v "
                 L"都可以。空格上屏；数字键用来输入，Shift+数字选词。",
                 "utility.v_mode"));

    struct Holder : msimeui::Container
    {
        Holder(VisualPtr child, std::shared_ptr<DictionaryPanel> panel)
            : msimeui::Container(std::move(child)), panel_(std::move(panel))
        {
        }
        std::shared_ptr<DictionaryPanel> panel_;
    };
    return std::make_shared<Holder>(page.Build(), panel);
}
} // namespace native_settings
