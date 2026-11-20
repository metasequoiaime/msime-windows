#include "settings/native/native_page_dictionary_panel.h"
#include "settings/foreign_dictionary.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Window.h"

#include <algorithm>
#include <iterator>

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
class CellDictionaryManager : public std::enable_shared_from_this<CellDictionaryManager>
{
  public:
    CellDictionaryManager(PageContext &context, std::shared_ptr<DictionaryPanel> entries)
        : context_(context), entries_(std::move(entries))
    {
        Dropdown::Options options;
        options.minWidth = 280.0f;
        options.menuAlignLeft = true;
        sources_ = std::make_shared<Dropdown>(std::vector<DropdownOption>{}, options);
        sources_->SetOnChanged([this](size_t, const DropdownOption &) { SelectSource(); });
        toggle_ = std::make_shared<Button>(L"停用", ButtonKind::Neutral);
        toggle_->SetOnClick([this]() {
            const Source *source = Selected();
            if (source != nullptr)
                Post("setSourceEnabled", {{"sourceId", source->id}, {"enabled", !source->enabled}});
        });
        remove_ = std::make_shared<Button>(L"删除词库", ButtonKind::Neutral);
        remove_->SetOnClick([this]() {
            if (Selected() == nullptr)
                return;
            const Source source = *Selected();
            context_.host.Confirm(L"确定删除细胞词库“" + Widen(source.name) + L"”吗？其中的词条会一并删除。",
                                  [weak = weak_from_this(), source](bool confirmed) {
                                      if (confirmed)
                                          if (const auto manager = weak.lock())
                                              manager->Post("deleteSource", {{"sourceId", source.id}});
                                  });
        });
        SyncButtons();
    }

    VisualPtr Build()
    {
        context_.OnMessage("dictionaryResponse", [weak = weak_from_this()](const Json &message) {
            if (const auto manager = weak.lock())
                manager->OnResponse(message);
        });
        auto body = std::make_shared<Column>(0.0f);
        body->Add(MakeTitle(L"细胞词库", L"每次导入都形成独立词库，按来源整理、启停或删除"));
        auto description = MakeDescription(L"支持 Rime、LibIME（fcitx5）词库。已有词条会跳过；名称留空时使用文件名。"
                                           L"细胞词库不会混入个人词库，停用后词条仍然保留");
        description->SetMargin({0.0f, 4.0f, 0.0f, 14.0f});
        body->Add(description);

        std::vector<DropdownOption> options;
        for (const auto &format : ForeignDictionary::Formats())
            options.push_back({format.label, format.id});
        Dropdown::Options settings;
        settings.minWidth = 200.0f;
        settings.menuAlignLeft = true;
        settings.compact = true;
        formats_ = std::make_shared<Dropdown>(std::move(options), settings);
        formats_->SetSelectedIndex(0);
        name_ = std::make_shared<DictInput>(kFontDefault, L"词库名称，留空使用文件名");
        auto import = std::make_shared<Button>(L"导入", ButtonKind::Primary);
        import->SetMinWidth(68.0f);
        import->SetOnClick([this]() {
            if (entries_)
                entries_->ImportForeign(formats_->GetValue(), Narrow(name_->GetText()));
        });
        auto importRow = std::make_shared<Row>(10.0f);
        importRow->Add(formats_);
        importRow->Add(name_, 1.0f);
        importRow->Add(import);
        importRow->SetMargin({0.0f, 0.0f, 0.0f, 14.0f});
        body->Add(importRow);

        auto sourceRow = std::make_shared<Row>(10.0f);
        sourceRow->Add(sources_, 1.0f);
        sourceRow->Add(toggle_);
        sourceRow->Add(remove_);
        sourceRow->SetPadding({22.0f, 0.0f, 22.0f, 14.0f});
        body->Add(sourceRow);
        entries_->AddTo(*body);
        return body;
    }

    void Load()
    {
        if (loaded_)
            return;
        loaded_ = true;
        RequestSources();
    }

  private:
    struct Source
    {
        std::string id;
        std::string name;
        bool enabled = false;
        size_t entryCount = 0;
    };

    const Source *Selected() const
    {
        const auto found = std::find_if(items_.begin(), items_.end(),
                                        [this](const Source &source) { return source.id == selectedId_; });
        return found == items_.end() ? nullptr : &*found;
    }

    void SelectSource()
    {
        selectedId_ = sources_->GetValue();
        entries_->SetSourceId(selectedId_);
        SyncButtons();
    }

    void SyncButtons()
    {
        const Source *source = Selected();
        toggle_->SetDisabled(source == nullptr);
        remove_->SetDisabled(source == nullptr);
        if (source != nullptr)
            toggle_->SetText(source->enabled ? L"停用" : L"启用");
    }

    void Post(const std::string &action, Json data)
    {
        const std::string requestId = "cells-" + std::to_string(++counter_);
        pending_[requestId] = action;
        data["requestId"] = requestId;
        data["dictionary"] = "cells";
        data["action"] = action;
        context_.model.Post("dictionaryRequest", std::move(data));
    }

    void RequestSources()
    {
        Post("listSources", {});
    }

    void OnResponse(const Json &message)
    {
        const std::string requestId = message.value("requestId", std::string());
        const auto found = pending_.find(requestId);
        if (found == pending_.end())
        {
            if (requestId.rfind("cell-", 0) == 0 && message.value("action", std::string()) == "importForeign" &&
                message.value("ok", false))
            {
                name_->SetText(L"");
                RequestSources();
            }
            return;
        }
        const std::string action = found->second;
        pending_.erase(found);
        if (!message.value("ok", false))
        {
            if (action != "listSources")
                context_.host.ShowToast(Widen(message.value("message", std::string("操作失败"))), false);
            return;
        }
        if (action != "listSources")
        {
            context_.host.ShowToast(Widen(message.value("message", std::string("操作成功"))), true);
            if (action == "deleteSource")
                selectedId_.clear();
            RequestSources();
            return;
        }
        items_.clear();
        std::vector<DropdownOption> options;
        if (message.contains("sources") && message["sources"].is_array())
        {
            for (const auto &object : message["sources"])
            {
                Source source;
                source.id = object.value("id", std::string());
                source.name = object.value("name", std::string());
                source.enabled = object.value("enabled", false);
                source.entryCount = object.value("entryCount", uint64_t{0});
                options.push_back({Widen(source.name) + L"（" + std::to_wstring(source.entryCount) + L" 条，" +
                                       (source.enabled ? L"已启用" : L"已停用") + L"）",
                                   source.id});
                items_.push_back(std::move(source));
            }
        }
        if (std::none_of(items_.begin(), items_.end(),
                         [this](const Source &source) { return source.id == selectedId_; }))
            selectedId_ = items_.empty() ? std::string{} : items_.front().id;
        sources_->SetOptions(std::move(options));
        sources_->SetValue(selectedId_);
        entries_->SetSourceId(selectedId_);
        SyncButtons();
    }

    PageContext &context_;
    std::shared_ptr<DictionaryPanel> entries_;
    std::shared_ptr<Dropdown> formats_;
    std::shared_ptr<DictInput> name_;
    std::shared_ptr<Dropdown> sources_;
    std::shared_ptr<Button> toggle_;
    std::shared_ptr<Button> remove_;
    std::vector<Source> items_;
    std::string selectedId_;
    std::map<std::string, std::string> pending_;
    uint64_t counter_ = 0;
    bool loaded_ = false;
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
    auto tabs = std::make_shared<DictTabs>(std::vector<std::wstring>{L"全拼词库", L"五笔词库", L"英文词库", L"字母词"});
    cardBody->Add(tabs);
    panel->AddTo(*cardBody);
    auto card = std::make_shared<PageCard>(cardBody, PageCard::Style::DictCard, Thickness{});
    page.AddRaw(card, 18.0f);

    // .dict-hans-import-section
    TextStyle hansStyle = SectionTitleStyle();
    hansStyle.lineHeight = 1.45f;
    hansStyle.wrap = true;
    auto hansTitle = std::make_shared<Label>(L"批量导入纯汉字词组(e.g.人名)到拼音词库", hansStyle);
    IndexSettingTitle(hansTitle, L"批量导入纯汉字词组(e.g.人名)到拼音词库");
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

    // 五笔标签页管理「输入」页选的那一版码表（86 / 98 / 新世纪）：三版共用一个标签，词库 id 随配置走。
    SettingsModel *model = &context.model;
    const auto wubiDictionary = [model]() { return WubiDictionaryId(model->GetString("input.wubi_schema")); };
    tabs->SetOnSelect([panel, wubiDictionary](size_t index) {
        static const char *const kDictionaries[] = {"quanpin", "wubi", "english", "latin"};
        const std::string dictionary = kDictionaries[std::min<size_t>(index, std::size(kDictionaries) - 1)];
        panel->SetDictionary(dictionary == "wubi" ? wubiDictionary() : dictionary);
    });
    std::weak_ptr<DictionaryPanel> weakPanel = panel;
    context.OnConfig([weakPanel, wubiDictionary]() {
        // 在五笔标签页上时换了码表版本，跟着换到那一版的表。
        auto locked = weakPanel.lock();
        if (locked && IsWubiDictionaryId(locked->Dictionary()) && locked->Dictionary() != wubiDictionary())
            locked->SetDictionary(wubiDictionary());
    });
    panel->SetOnModeChanged([weakPanel, hans, hansTitle]() {
        auto locked = weakPanel.lock();
        if (!locked)
            return;
        // 纯汉字导入按当前标签页出码；英文词库收不了纯汉字，整块隐藏。字母词页换成按词导入、自动注音。
        const std::string &dictionary = locked->Dictionary();
        hans->SetVisible(dictionary != "english");
        hansTitle->SetText(dictionary == "quanpin" ? L"批量导入纯汉字词组(e.g.人名)到拼音词库"
                           : dictionary == "latin"
                               ? L"批量导入字母词(e.g.T恤)，每行一个词，自动注音"
                               : L"批量导入纯汉字词组(e.g.人名)到" + WubiDictionaryLabel(dictionary));
    });

    auto cellEntries = std::make_shared<DictionaryPanel>(context, DictionaryPanel::Kind::Cell);
    auto cells = std::make_shared<CellDictionaryManager>(context, cellEntries);
    auto cellCard =
        std::make_shared<PageCard>(cells->Build(), PageCard::Style::DictCard, Thickness{24.0f, 18.0f, 0.0f, 0.0f});
    struct CellHolder : msimeui::Container
    {
        CellHolder(VisualPtr child, std::shared_ptr<CellDictionaryManager> manager)
            : msimeui::Container(std::move(child)), manager_(std::move(manager))
        {
        }
        void Attach(msimeui::Window *window) override
        {
            msimeui::Container::Attach(window);
            manager_->Load();
        }
        std::shared_ptr<CellDictionaryManager> manager_;
    };
    page.AddRaw(std::make_shared<CellHolder>(cellCard, cells), 18.0f);

    // .dict-export-section: padding-top 18px, padding-bottom 0.
    auto exportBody = std::make_shared<Column>(0.0f);
    auto exportTitle = MakeTitle(L"导出词库");
    exportTitle->SetMargin({0.0f, 0.0f, 0.0f, 14.0f});
    exportBody->Add(exportTitle);
    exportBody->Add(MakeExportRow(L"导出拼音用户词库", [panel]() { panel->Export("quanpin"); }, 62.0f, true));
    // 三版五笔的用户词各记各的，各导各的；86 沿用原来的词库 id 和文件名。
    exportBody->Add(MakeExportRow(L"导出 86 五笔用户词库", [panel]() { panel->Export("wubi"); }, 62.0f, true));
    exportBody->Add(MakeExportRow(L"导出 98 五笔用户词库", [panel]() { panel->Export("wubi98"); }, 62.0f, true));
    exportBody->Add(MakeExportRow(L"导出新世纪五笔用户词库", [panel]() { panel->Export("wubixsj"); }, 62.0f, true));
    exportBody->Add(MakeExportRow(L"导出英文用户词库", [panel]() { panel->Export("english"); }, 62.0f, true));
    exportBody->Add(MakeExportRow(L"导出字母词用户词库", [panel]() { panel->Export("latin"); }, 62.0f, true));
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
