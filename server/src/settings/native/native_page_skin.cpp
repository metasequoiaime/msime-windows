#include "settings/native/native_page_skin_common.h"

#include "skin/candidate_skin_catalog.h"

#include <algorithm>

// partials/skin.html + modules/skin.ts + styles/modules/skin.css.
namespace native_settings
{
namespace
{
using skinpreview::Band;
using skinpreview::FlushCard;
using skinpreview::PillButton;

// 内置皮肤的名称与说明写在它的清单（skins/default/<id>/skin.toml）里。
std::wstring BuiltinName(const SettingsModel &model, const std::string &id)
{
    return Widen(CandidateSkinCatalog::ResolveBuiltInInfo(skinpreview::SkinsRoot(model), id).name);
}

std::wstring BuiltinDescription(const SettingsModel &model, const std::string &id)
{
    return Widen(CandidateSkinCatalog::ResolveBuiltInInfo(skinpreview::SkinsRoot(model), id).description);
}

std::vector<std::string> StringList(const Json *value)
{
    std::vector<std::string> list;
    if (value && value->is_array())
    {
        for (const auto &item : *value)
        {
            if (item.is_string())
                list.push_back(item.get<std::string>());
        }
    }
    return list;
}

std::wstring JoinList(const std::vector<std::string> &items)
{
    std::wstring text;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i)
            text += L"/";
        text += Widen(items[i]);
    }
    return text;
}

std::string JsonString(const Json &object, const char *key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

TextStyle CaptionMuted()
{
    TextStyle style;
    style.size = kFontCaption;
    style.wrap = true;
    return style;
}

// `.skin-theme-toggle`: a switch that only ever turns on; selecting another skin turns
// the others off through the snapshot.
class SkinSwitch : public ToggleSwitch
{
  public:
    SkinSwitch(SettingsModel *model, std::string skinId) : model_(model), skinId_(std::move(skinId))
    {
        SetOnChanged([this](bool) {
            SetChecked(true);
            model_->Update("appearance.candidate_skin", skinId_);
        });
    }

    void Sync()
    {
        SetChecked(model_->GetString("appearance.candidate_skin", "fluent") == skinId_);
    }

  private:
    SettingsModel *model_;
    std::string skinId_;
};

// `code.external-skin-directory`: inline monospace chip.
class CodeChip : public msimeui::Visual
{
  public:
    void SetText(std::wstring text)
    {
        if (text == layout_.Text())
            return;
        layout_.Set(std::move(text), Style());
        InvalidateMeasure();
        InvalidateVisual();
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        const float maxWidth = std::min(availableSize.width, 620.0f);
        const SizeF text = layout_.Measure(std::max(maxWidth - 14.0f, 1.0f));
        return {std::min(std::ceil(text.width) + 14.0f, maxWidth), std::ceil(text.height) + 8.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        FillRounded(dr, bounds_, 5.0f, p.buttonSecondaryBg);
        layout_.Draw(dr, {bounds_.x + 7.0f, bounds_.y + 4.0f}, p.textMuted, std::max(bounds_.width - 14.0f, 1.0f));
    }

  private:
    static TextStyle Style()
    {
        TextStyle style;
        style.family = L"Consolas";
        style.size = 13.0f;
        style.wrap = true;
        return style;
    }

    TextLayout layout_;
};

struct SkinCard
{
    std::string id;
    bool external = false;
    // 官方贴图皮肤：照外部皮肤渲染，但和内置皮肤列在一起，标题同样带深浅后缀。
    bool official = false;
    std::vector<std::string> themes;
    int previewOverride = -1; // -1 follows the candidate theme, 0 dark, 1 light
    std::shared_ptr<Label> title;
    std::shared_ptr<Label> description;
    std::shared_ptr<SkinSwitch> toggle;
    std::shared_ptr<PillButton> previewSwitch;
    std::shared_ptr<skinpreview::CandidatePreview> horizontal;
    std::shared_ptr<skinpreview::CandidatePreview> vertical;
    std::shared_ptr<skinpreview::ToolbarPreview> toolbar;
    std::shared_ptr<FlushCard> card;
};

class SkinPage
{
  public:
    explicit SkinPage(PageContext &context) : model_(&context.model)
    {
    }

    VisualPtr Build(PageContext &context);
    void Sync();

  private:
    std::shared_ptr<SkinCard> MakeCard(const std::string &id, const Json *external, bool official = false);
    bool PreviewLight(const SkinCard &card) const;
    void ApplyCard(SkinCard &card);
    void RebuildExternal();

    SettingsModel *model_;
    std::vector<std::shared_ptr<SkinCard>> builtin_;
    std::vector<std::shared_ptr<SkinCard>> external_;
    std::vector<std::shared_ptr<SkinCard>> official_;
    std::shared_ptr<msimeui::Container> officialHost_;
    std::shared_ptr<msimeui::Container> externalHost_;
    std::shared_ptr<CodeChip> directory_;
    std::string catalogKey_ = "\x01";
    std::string themeKey_;
    bool candLight_ = false;
    bool diagnosticsOpen_ = false;
};

std::shared_ptr<SkinCard> SkinPage::MakeCard(const std::string &id, const Json *external, bool official)
{
    auto card = std::make_shared<SkinCard>();
    card->id = id;
    card->external = external != nullptr;
    card->official = official;

    auto titles = std::make_shared<Column>(0.0f);
    card->title = std::make_shared<Label>(external ? Widen(JsonString(*external, "name")) : BuiltinName(*model_, id),
                                          SectionTitleStyle());
    titles->Add(card->title);
    bool compatible = true;
    if (external)
    {
        card->themes = StringList(external->contains("themes") ? &(*external)["themes"] : nullptr);
        compatible = !(external->contains("compatible") && (*external)["compatible"].is_boolean() &&
                       !(*external)["compatible"].get<bool>());
        if (!official)
        {
            std::wstring meta = Widen(id);
            const std::string version = JsonString(*external, "version");
            const std::string author = JsonString(*external, "author");
            if (!version.empty())
                meta += L" · v" + Widen(version);
            if (!author.empty())
                meta += L" · " + Widen(author);
            auto metaLabel = std::make_shared<Label>(meta, CaptionMuted(), TextRole::Muted);
            metaLabel->SetMargin({0.0f, 4.0f, 0.0f, 0.0f});
            titles->Add(metaLabel);
        }

        std::wstring description;
        if (compatible)
        {
            description = Widen(JsonString(*external, "description"));
            if (description.empty())
                description = L"基于 " + Widen(skinpreview::SkinBase(*model_, id));
        }
        else
        {
            description = L"当前布局或明暗模式不受支持（" +
                          JoinList(StringList(external->contains("layouts") ? &(*external)["layouts"] : nullptr)) +
                          L"，" + JoinList(card->themes) + L"）";
        }
        card->description = MakeDescription(description);
    }
    else
    {
        card->description = MakeDescription(BuiltinDescription(*model_, id));
    }
    titles->Add(card->description);

    auto actions = std::make_shared<Column>(8.0f);
    actions->SetHorizontalContentAlignment(msimeui::HorizontalAlignment::Trailing);
    card->toggle = std::make_shared<SkinSwitch>(model_, id);
    card->toggle->SetDisabled(!compatible);
    actions->Add(card->toggle);
    card->previewSwitch = std::make_shared<PillButton>(L"预览浅色");
    std::weak_ptr<SkinCard> weakCard = card;
    card->previewSwitch->SetOnClick([this, weakCard]() {
        auto target = weakCard.lock();
        if (!target)
            return;
        target->previewOverride = PreviewLight(*target) ? 0 : 1;
        ApplyCard(*target);
    });
    actions->Add(card->previewSwitch);

    auto header = std::make_shared<Row>(16.0f);
    header->Add(titles, 1.0f);
    header->Add(actions);

    const bool decorated = external && external->contains("decorationTopDip") &&
                           (*external)["decorationTopDip"].is_number() &&
                           (*external)["decorationTopDip"].get<double>() > 0.0;
    auto stage = [decorated](VisualPtr child, Thickness padding) {
        auto band = std::make_shared<Band>(std::move(child), padding);
        band->SetClip(!decorated);
        return band;
    };
    card->horizontal = std::make_shared<skinpreview::CandidatePreview>();
    card->vertical = std::make_shared<skinpreview::CandidatePreview>();
    card->toolbar = std::make_shared<skinpreview::ToolbarPreview>();
    auto previews = std::make_shared<Column>(0.0f);
    previews->Add(stage(card->horizontal, {24.0f, 14.0f, 36.0f, 38.0f}));
    previews->Add(stage(card->vertical, {24.0f, 14.0f, 36.0f, 38.0f}));
    previews->Add(stage(card->toolbar, {24.0f, 9.0f, 24.0f, 9.0f}));
    auto list = std::make_shared<Band>(previews, Thickness{0.0f, 9.0f, 0.0f, 9.0f});
    list->SetBackground([]() { return CurrentPalette().skinPreviewStageBg; });

    card->card = std::make_shared<FlushCard>();
    card->card->SetClip(!decorated);
    card->card->Add(std::make_shared<Band>(header, Thickness{24.0f, 20.0f, 24.0f, 20.0f}));
    card->card->Add(list);
    return card;
}

// skin.ts resolvedPreviewTheme().
bool SkinPage::PreviewLight(const SkinCard &card) const
{
    if (card.previewOverride >= 0)
        return card.previewOverride == 1;
    const std::string active = candLight_ ? "light" : "dark";
    if (card.themes.empty() || std::find(card.themes.begin(), card.themes.end(), active) != card.themes.end())
        return candLight_;
    return card.themes.front() == "light";
}

void SkinPage::ApplyCard(SkinCard &card)
{
    const bool light = PreviewLight(card);
    if (!card.external)
        card.title->SetText(BuiltinName(*model_, card.id) + (light ? L"(Light)" : L"(Dark)"));
    else if (card.official)
    {
        const Json *manifest = skinpreview::FindExternalSkin(*model_, card.id);
        card.title->SetText(Widen(manifest ? JsonString(*manifest, "name") : card.id) +
                            (light ? L"(Light)" : L"(Dark)"));
    }
    card.previewSwitch->SetText(light ? L"预览深色" : L"预览浅色");

    // 预览直接用候选窗的场景按皮肤清单画：翻页箭头、卷轴展开按钮都跟皮肤自己的开关走。
    const skinpreview::CandidateFont font = skinpreview::ReadCandidateFont(*model_);
    skinpreview::CandidatePreview::Options options;
    options.horizontal = true;
    card.horizontal->Set(*model_, card.id, light, font, options);
    options.horizontal = false;
    card.vertical->Set(*model_, card.id, light, font, options);
    card.toolbar->Set(skinpreview::ResolveToolbarLook(*model_, card.id, light), {});
}

void SkinPage::RebuildExternal()
{
    external_.clear();
    official_.clear();
    auto column = std::make_shared<Column>(0.0f);
    const Json *skins = model_->Find("appearance.external_candidate_skins");
    if (skins && skins->is_array())
    {
        for (const auto &skin : *skins)
        {
            if (!skin.is_object() || !skin.contains("id") || !skin["id"].is_string() || !skin.contains("name") ||
                !skin["name"].is_string())
                continue;
            const std::string id = skin["id"].get<std::string>();
            if (CandidateSkinCatalog::IsOfficialPackage(id))
                continue;
            external_.push_back(MakeCard(id, &skin));
        }
        // 官方贴图皮肤按 OfficialPackageIds 的顺序排在内置皮肤后面；用户删掉了目录就不列。
        for (const auto &id : CandidateSkinCatalog::OfficialPackageIds())
        {
            if (const Json *skin = skinpreview::FindExternalSkin(*model_, id);
                skin && skin->contains("name") && (*skin)["name"].is_string())
                official_.push_back(MakeCard(id, skin, true));
        }
    }
    auto officialColumn = std::make_shared<Column>(0.0f);
    for (const auto &card : official_)
    {
        card->card->SetMargin({0.0f, 0.0f, 0.0f, 18.0f});
        officialColumn->Add(card->card);
    }
    officialHost_->SetChild(officialColumn);
    officialHost_->InvalidateMeasure();
    // `.external-skin-list { display: grid; gap: 18px }`: the cards' own 18px margins do
    // not collapse into the gap.
    for (size_t i = 0; i < external_.size(); ++i)
    {
        external_[i]->card->SetMargin({0.0f, 0.0f, 0.0f, i + 1 < external_.size() ? 54.0f : 18.0f});
        column->Add(external_[i]->card);
    }

    if (external_.empty())
    {
        auto text = std::make_shared<Label>(model_->GetBool("appearance.candidate_skin_catalog_scanned", false)
                                                ? L"没有发现外部皮肤。"
                                                : L"尚未扫描。点击“刷新皮肤”读取皮肤目录。",
                                            BodyStyle(), TextRole::Muted);
        auto centered = std::make_shared<Row>(0.0f);
        centered->SetJustify(Row::Justify::Center);
        centered->Add(text);
        auto empty = std::make_shared<FlushCard>();
        empty->Add(std::make_shared<Band>(centered, Thickness{22.0f, 22.0f, 22.0f, 22.0f}));
        empty->SetMargin({0.0f, 0.0f, 0.0f, 18.0f});
        column->Add(empty);
    }

    const Json *issues = model_->Find("appearance.candidate_skin_scan_issues");
    std::vector<std::wstring> entries;
    // 加载失败的目录和能用但带提示的皮肤都进 issues，汇总里要分开数。两者由 Scan 标好，设置页不按卡片是否已列出推断。
    size_t invalid = 0;
    size_t noticed = 0;
    if (issues && issues->is_array())
    {
        for (const auto &issue : *issues)
        {
            if (issue.is_object() && issue.contains("folder") && issue["folder"].is_string() &&
                issue.contains("reason") && issue["reason"].is_string())
            {
                const std::string folder = issue["folder"].get<std::string>();
                ++(issue.value("noticed", false) ? noticed : invalid);
                entries.push_back(Widen(folder) + L"：" + Widen(issue["reason"].get<std::string>()));
            }
        }
    }
    if (!entries.empty())
    {
        // `<details>` with its summary; collapsed by default.
        auto diagnostics = std::make_shared<Column>(0.0f);
        // margin: 10px 4px 0; the top collapses into the empty card's 18px margin.
        diagnostics->SetMargin({4.0f, external_.empty() ? 0.0f : 10.0f, 4.0f, 0.0f});
        std::wstring summary;
        if (invalid > 0)
            summary = L"已忽略 " + std::to_wstring(invalid) + L" 个无效皮肤目录";
        if (noticed > 0)
            summary += (summary.empty() ? L"" : L"，") + std::to_wstring(noticed) + L" 个皮肤有未生效的字段";
        auto toggle =
            std::make_shared<Label>((diagnosticsOpen_ ? L"▾ " : L"▸ ") + summary, CaptionMuted(), TextRole::Muted);
        auto list = std::make_shared<Column>(0.0f);
        list->SetMargin({20.0f, 7.0f, 0.0f, 0.0f});
        for (const auto &entry : entries)
            list->Add(std::make_shared<Label>(L"•  " + entry, CaptionMuted(), TextRole::Muted));
        auto details = std::make_shared<Hideable>(list, diagnosticsOpen_);
        std::weak_ptr<Label> weakToggle = toggle;
        std::weak_ptr<Hideable> weakDetails = details;
        toggle->SetOnClick([this, weakToggle, weakDetails, summary]() {
            diagnosticsOpen_ = !diagnosticsOpen_;
            if (auto label = weakToggle.lock())
                label->SetText((diagnosticsOpen_ ? L"▾ " : L"▸ ") + summary);
            if (auto hideable = weakDetails.lock())
                hideable->SetVisible(diagnosticsOpen_);
        });
        diagnostics->Add(toggle);
        diagnostics->Add(details);
        column->Add(diagnostics);
    }
    else if (!external_.empty())
    {
        // The empty #externalSkinDiagnostics still keeps its 10px top margin.
        column->Add(std::make_shared<Spacer>(0.0f, 10.0f));
    }

    externalHost_->SetChild(column);
    externalHost_->InvalidateMeasure();
}

void SkinPage::Sync()
{
    // skin.ts applyCandidateSkinCatalog() only rebuilds when the catalog changed.
    Json catalog = Json::array();
    for (const char *path : {"appearance.external_candidate_skins", "appearance.candidate_skin_scan_issues",
                             "appearance.candidate_skin_directory", "appearance.candidate_skin_catalog_scanned"})
    {
        const Json *value = model_->Find(path);
        catalog.push_back(value ? *value : Json());
    }
    const std::string key = catalog.dump();
    if (key != catalogKey_)
    {
        catalogKey_ = key;
        RebuildExternal();
    }
    const std::string directory = model_->GetString("appearance.candidate_skin_directory");
    directory_->SetText(directory.empty() ? L"%LOCALAPPDATA%\\metasequoiaime\\skins" : Widen(directory));

    // theme.ts updateSkinThemeCard(): a new candidate theme drops the preview overrides.
    candLight_ = skinpreview::ResolveSurfaceLight(*model_, "appearance.theme_cand");
    const std::string mode = model_->GetString("appearance.theme_mode", "system");
    const bool settingsLight = skinpreview::ResolveSurfaceLight(*model_, "appearance.theme_settings");
    const std::string themeKey = mode + (settingsLight ? "L" : "D") + (candLight_ ? "L" : "D");
    const bool themeChanged = themeKey != themeKey_;
    themeKey_ = themeKey;
    if (!builtin_.empty())
    {
        const wchar_t *modeLabel = mode == "light" ? L"浅色" : mode == "dark" ? L"深色" : L"跟随系统";
        builtin_.front()->description->SetText(std::wstring(L"全局 ") + modeLabel + L" · 设置 " +
                                               (settingsLight ? L"浅色" : L"深色") + L" · 候选 " +
                                               (candLight_ ? L"浅色" : L"深色"));
    }
    for (auto *cards : {&builtin_, &official_, &external_})
    {
        for (auto &card : *cards)
        {
            if (themeChanged)
                card->previewOverride = -1;
            card->toggle->Sync();
            ApplyCard(*card);
        }
    }
}

VisualPtr SkinPage::Build(PageContext &)
{
    PageBuilder page(L"皮肤");
    const auto &skins = skinpreview::BuiltinSkins();
    for (size_t i = 0; i < skins.size(); ++i)
    {
        auto card = MakeCard(skins[i], nullptr);
        builtin_.push_back(card);
        page.AddRaw(card->card, 18.0f);
    }
    // 官方贴图皮肤的卡片各带 18px 下边距；最后一张卡片的 18px 并进标题前的 28px。
    officialHost_ = std::make_shared<msimeui::Container>();
    page.AddRaw(officialHost_, 10.0f);

    // `.external-skin-heading`.
    auto titles = std::make_shared<Column>(0.0f);
    titles->Add(MakeTitle(L"外部皮肤"));
    titles->Add(MakeDescription(L"把包含 skin.toml 的皮肤文件夹复制到下面的目录，然后刷新。"));
    directory_ = std::make_shared<CodeChip>();
    directory_->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
    auto chipRow = std::make_shared<Row>(0.0f);
    chipRow->Add(directory_);
    titles->Add(chipRow);
    SettingsModel *model = model_;
    auto open = std::make_shared<PillButton>(L"打开目录", 30.0f);
    open->SetOnClick([model]() { model->Post("openSkinDirectory"); });
    auto refresh = std::make_shared<PillButton>(L"刷新皮肤", 30.0f);
    refresh->SetOnClick([model]() { model->Post("skinCatalogRequest"); });
    auto buttons = std::make_shared<Row>(8.0f);
    buttons->Add(open);
    buttons->Add(refresh);
    buttons->SetMargin({0.0f, 2.0f, 0.0f, 0.0f});
    auto heading = std::make_shared<Row>(18.0f);
    heading->SetAlign(Row::Align::Start);
    heading->Add(titles, 1.0f);
    heading->Add(buttons);
    page.AddRaw(std::make_shared<Band>(heading, Thickness{4.0f, 0.0f, 4.0f, 0.0f}), 12.0f);

    externalHost_ = std::make_shared<msimeui::Container>();
    page.AddRaw(externalHost_, 0.0f);

    return page.Build();
}
} // namespace

VisualPtr BuildSkinPage(PageContext &context)
{
    // Kept alive by the config listener, like the page's module state.
    auto page = std::make_shared<SkinPage>(context);
    VisualPtr content = page->Build(context);
    context.OnConfig([page]() { page->Sync(); });
    return content;
}
} // namespace native_settings
