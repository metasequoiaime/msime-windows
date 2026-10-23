#include "settings/native/native_page_dictionary_panel.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Window.h"

#include <algorithm>

// partials/dict.html + modules/dict.ts + styles/modules/dict.css
namespace native_settings
{
namespace
{
// `.dict-tabs`: padding 14px 18px 0, gap 4px, bottom border; `.dict-tab`: padding 11px 20px,
// 15px system font (a <button>), 2px accent underline when active.
class DictTabs : public msimeui::Visual
{
  public:
    using SelectHandler = std::function<void(size_t index)>;

    explicit DictTabs(std::vector<std::wstring> labels) : labels_(std::move(labels))
    {
    }

    void SetOnSelect(SelectHandler handler)
    {
        onSelect_ = std::move(handler);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        return {availableSize.width, 14.0f + 11.0f + TabLineHeight() + 11.0f + 2.0f + CssBorder()};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        const float border = CssBorder();
        FillRect(dr, {bounds_.x, bounds_.y + bounds_.height - border, bounds_.width, border}, p.border);
        for (size_t i = 0; i < labels_.size(); ++i)
        {
            const RectF tab = TabRect(i);
            const bool active = i == selected_;
            // Chrome sits the system font lower in its line box than DirectWrite does.
            DrawTextInRect(dr, labels_[i], TabStyle(), {tab.x, tab.y + 1.33f, tab.width, tab.height - 2.0f},
                           active ? p.text : p.textMuted, true);
            if (active)
                FillRect(dr, {tab.x, tab.y + tab.height - 2.0f, tab.width, 2.0f}, p.accent);
        }
    }

    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        pressed_ = HitTab(point);
        return pressed_ != labels_.size();
    }

    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        const size_t hit = HitTab(point);
        const bool clicked = hit == pressed_ && hit < labels_.size();
        pressed_ = labels_.size();
        if (clicked && hit != selected_)
        {
            selected_ = hit;
            InvalidateVisual();
            if (onSelect_)
            {
                const auto handler = onSelect_;
                handler(hit);
            }
        }
        return true;
    }

    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, IDC_HAND);
    }

  private:
    // Chrome lays the 15px system font out in a ~21.3px line, a little taller than
    // DirectWrite's natural line (measured against the former web page).
    static float TabLineHeight()
    {
        return std::max(LineHeight(TabStyle()), 21.33f);
    }

    static TextStyle TabStyle()
    {
        TextStyle style = ControlStyle();
        style.size = kFontSectionTitle;
        return style;
    }

    RectF TabRect(size_t index) const
    {
        float x = bounds_.x + 18.0f;
        for (size_t i = 0; i < index; ++i)
            x += MeasureTextWidth(labels_[i], TabStyle()) + 40.0f + 4.0f;
        const float width = MeasureTextWidth(labels_[index], TabStyle()) + 40.0f;
        return {x, bounds_.y + 14.0f, width, bounds_.height - 14.0f - CssBorder()};
    }

    size_t HitTab(const POINT &point) const
    {
        if (!window_)
            return labels_.size();
        const PointF dip = window_->ClientPixelsToDips(point);
        for (size_t i = 0; i < labels_.size(); ++i)
        {
            if (Contains(TabRect(i), dip))
                return i;
        }
        return labels_.size();
    }

    std::vector<std::wstring> labels_;
    size_t selected_ = 0;
    size_t pressed_ = 3;
    SelectHandler onSelect_;
};

// `.dict-export-row`: label and button, min-height 62px, divider on top.
class ExportRow : public msimeui::Container
{
  public:
    ExportRow(VisualPtr content, float minHeight, bool topBorder)
        : msimeui::Container(std::move(content)), minHeight_(minHeight), topBorder_(topBorder)
    {
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        const float border = topBorder_ ? CssBorder() : 0.0f;
        const SizeF size = child_ ? child_->MeasureInLayout(availableSize) : SizeF{};
        return {availableSize.width, std::max(minHeight_, size.height + border)};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        const float border = topBorder_ ? CssBorder() : 0.0f;
        if (!child_)
            return;
        // align-items: center inside the min-height.
        const float height = child_->MeasureInLayout({finalRect.width, finalRect.height - border}).height;
        const float top = finalRect.y + border + std::max(0.0f, (finalRect.height - border - height) * 0.5f);
        child_->ArrangeInLayout({finalRect.x, top, finalRect.width, height});
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        if (topBorder_)
            FillRect(dr, {bounds_.x, bounds_.y, bounds_.width, CssBorder()}, CurrentPalette().divider);
        msimeui::Container::Render(dr);
    }

  private:
    float minHeight_ = 62.0f;
    bool topBorder_ = true;
};
} // namespace

VisualPtr MakeExportRow(const std::wstring &label, std::function<void()> onExport, float minHeight, bool topBorder)
{
    auto row = std::make_shared<Row>(18.0f);
    row->SetJustify(Row::Justify::SpaceBetween);
    TextStyle style;
    style.size = kFontDefault;
    row->Add(std::make_shared<Label>(label, style), 1.0f);
    auto button = std::make_shared<Button>(L"导出", ButtonKind::Primary);
    button->SetMinWidth(68.0f);
    button->SetOnClick(std::move(onExport));
    row->Add(button);
    return std::make_shared<ExportRow>(row, minHeight, topBorder);
}

VisualPtr BuildDictionaryPage(PageContext &context)
{
    PageBuilder page(L"词库");
    auto panel = std::make_shared<DictionaryPanel>(context, DictionaryPanel::Kind::Dictionary);

    // .dict-card
    auto cardBody = std::make_shared<Column>(0.0f);
    auto tabs = std::make_shared<DictTabs>(std::vector<std::wstring>{L"全拼词库", L"五笔词库", L"英文词库"});
    cardBody->Add(tabs);
    panel->AddTo(*cardBody);
    auto card = std::make_shared<PageCard>(cardBody, PageCard::Style::DictCard, Thickness{});
    page.AddRaw(card, 18.0f);

    // .dict-hans-import-section
    TextStyle hansStyle = SectionTitleStyle();
    hansStyle.lineHeight = 1.45f;
    hansStyle.wrap = true;
    auto hansTitle = std::make_shared<Label>(L"批量导入纯汉字词组(e.g.人名)到拼音词库", hansStyle);
    auto hansButton = std::make_shared<Button>(L"导入", ButtonKind::Primary);
    hansButton->SetMinWidth(68.0f);
    hansButton->SetOnClick([panel]() { panel->Import("importHans"); });
    auto hansRow = std::make_shared<Row>(10.0f);
    hansRow->Add(hansTitle, 1.0f);
    hansRow->Add(hansButton);
    auto hansCard =
        std::make_shared<PageCard>(hansRow, PageCard::Style::Section, Thickness{24.0f, 20.0f, 24.0f, 20.0f});
    hansCard->SetMargin({0.0f, 0.0f, 0.0f, 18.0f});
    auto hans = std::make_shared<Hideable>(hansCard);
    page.AddRaw(hans, 0.0f);

    tabs->SetOnSelect([panel](size_t index) {
        static const char *const kDictionaries[] = {"quanpin", "wubi", "english"};
        panel->SetDictionary(kDictionaries[std::min<size_t>(index, 2)]);
    });
    std::weak_ptr<DictionaryPanel> weakPanel = panel;
    panel->SetOnModeChanged([weakPanel, hans, hansTitle]() {
        auto locked = weakPanel.lock();
        if (!locked)
            return;
        // 纯汉字导入按当前标签页出码；英文词库收不了纯汉字，整块隐藏。
        hans->SetVisible(locked->Dictionary() != "english");
        hansTitle->SetText(locked->Dictionary() == "quanpin" ? L"批量导入纯汉字词组(e.g.人名)到拼音词库"
                                                             : L"批量导入纯汉字词组(e.g.人名)到五笔词库");
    });

    // .dict-export-section: padding-top 18px, padding-bottom 0.
    auto exportBody = std::make_shared<Column>(0.0f);
    auto exportTitle = MakeTitle(L"导出词库");
    exportTitle->SetMargin({0.0f, 0.0f, 0.0f, 14.0f});
    exportBody->Add(exportTitle);
    exportBody->Add(MakeExportRow(L"导出拼音用户词库", [panel]() { panel->Export("quanpin"); }, 62.0f, true));
    exportBody->Add(MakeExportRow(L"导出五笔用户词库", [panel]() { panel->Export("wubi"); }, 62.0f, true));
    exportBody->Add(MakeExportRow(L"导出英文用户词库", [panel]() { panel->Export("english"); }, 62.0f, true));
    page.AddRaw(std::make_shared<PageCard>(exportBody, PageCard::Style::Section, Thickness{24.0f, 18.0f, 24.0f, 0.0f}),
                18.0f);

    // The panel lives as long as the page; the page visual keeps it alive through the
    // callbacks above, which hold it by value.
    auto content = page.Build();
    struct Holder : msimeui::Container
    {
        Holder(VisualPtr child, std::shared_ptr<DictionaryPanel> panel)
            : msimeui::Container(std::move(child)), panel_(std::move(panel))
        {
        }
        std::shared_ptr<DictionaryPanel> panel_;
    };
    return std::make_shared<Holder>(content, panel);
}
} // namespace native_settings
