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
using skinpreview::PickCard;
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

std::shared_ptr<Label> MakeCaption(std::wstring text)
{
    return std::make_shared<Label>(std::move(text), CaptionMuted(), TextRole::Muted);
}

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

// Which preview every card shows. One choice for the whole page: comparing skins is the
// point of the page, and a per-card switch left each card a different height.
enum class PreviewKind
{
    Horizontal,
    Vertical,
    Toolbar,
};

struct PreviewStage
{
    std::shared_ptr<Band> band;
    // Shown instead of a skin that has no resources for the chosen layout, so a preview
    // never falls back to another skin's look without saying so.
    std::shared_ptr<Row> unsupported;
    std::shared_ptr<Label> unsupportedLabel;
    std::shared_ptr<skinpreview::CandidatePreview> horizontal;
    std::shared_ptr<skinpreview::CandidatePreview> vertical;
    std::shared_ptr<skinpreview::ToolbarPreview> toolbar;
};

struct SkinCard
{
    std::string id;
    std::wstring name;
    bool compatible = true;
    bool decoratedHorizontal = false;
    bool decoratedVertical = false;
    std::vector<std::string> layouts;
    std::vector<std::string> themes;
    std::shared_ptr<Label> title;
    std::shared_ptr<Label> description;
    std::shared_ptr<Label> badge;
    std::shared_ptr<Hideable> badgeHost;
    std::shared_ptr<PickCard> card;
    PreviewStage stage;
    // What the last FillStage ran with. Selecting another skin does not change any card's
    // preview, so the page skips the manifest parsing and scene rebuild when this matches.
    std::string appliedContentKey;
    bool appliedLight = false;
    bool applied = false;
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
    void BuildStage(PreviewStage &stage);
    void FillStage(PreviewStage &stage, const std::string &id, bool light, const std::vector<std::string> &layouts,
                   bool decoratedHorizontal, bool decoratedVertical);
    bool PreviewLight(const SkinCard &card) const;
    bool RequestedLight() const;
    bool SupportsLayout(const std::vector<std::string> &layouts) const;
    void ApplyCardState(SkinCard &card);
    void RefreshCards();
    void UpdateCurrentSkin();
    void SyncPreviewControls();
    void SetPreviewKind(PreviewKind kind);
    void SetPreviewLight(int value);
    void Activate(const std::shared_ptr<SkinCard> &card);
    const SkinCard *FindCard(const std::string &id) const;
    std::string ContentKey(const skinpreview::CandidateFont &font) const;
    void RebuildExternal();

    SettingsModel *model_;
    std::vector<std::shared_ptr<SkinCard>> builtin_;
    std::vector<std::shared_ptr<SkinCard>> external_;
    std::vector<std::shared_ptr<SkinCard>> official_;
    std::shared_ptr<msimeui::Container> officialHost_;
    std::shared_ptr<msimeui::Container> externalHost_;
    std::shared_ptr<CodeChip> directory_;

    // `.theme-surface-details` of the current skin, above the list.
    std::shared_ptr<Label> currentName_;
    std::shared_ptr<Label> currentHint_;
    PreviewStage currentStage_;
    std::string currentKey_;

    std::vector<std::shared_ptr<PillButton>> kindPills_;
    std::vector<std::shared_ptr<PillButton>> lightPills_;
    static constexpr int kLightPillValues[] = {-1, 1, 0}; // 跟随主题, 浅色, 深色

    skinpreview::CandidateFont font_;
    std::string contentKey_;
    std::string catalogKey_ = "\x01";
    std::string themeKey_;
    bool candLight_ = false;
    PreviewKind previewKind_ = PreviewKind::Horizontal;
    int previewLight_ = -1; // -1 follows the candidate theme, 0 dark, 1 light
    bool diagnosticsOpen_ = false;
};

std::shared_ptr<SkinCard> SkinPage::MakeCard(const std::string &id, const Json *external, bool official)
{
    auto card = std::make_shared<SkinCard>();
    card->id = id;

    std::wstring description;
    std::wstring meta;
    if (external)
    {
        card->name = Widen(JsonString(*external, "name"));
        card->layouts = StringList(external->contains("layouts") ? &(*external)["layouts"] : nullptr);
        card->themes = StringList(external->contains("themes") ? &(*external)["themes"] : nullptr);
        card->compatible = !(external->contains("compatible") && (*external)["compatible"].is_boolean() &&
                             !(*external)["compatible"].get<bool>());
        card->decoratedHorizontal = skinpreview::HasDecorationInLayout(external, true);
        card->decoratedVertical = skinpreview::HasDecorationInLayout(external, false);
        if (!official)
        {
            meta = Widen(id);
            const std::string version = JsonString(*external, "version");
            const std::string author = JsonString(*external, "author");
            if (!version.empty())
                meta += L" · v" + Widen(version);
            if (!author.empty())
                meta += L" · " + Widen(author);
        }
        if (card->compatible)
        {
            description = Widen(JsonString(*external, "description"));
            if (description.empty())
                description = L"基于 " + Widen(skinpreview::SkinBase(*model_, id));
        }
        else
        {
            description =
                L"当前布局或明暗模式不受支持（" + JoinList(card->layouts) + L"，" + JoinList(card->themes) + L"）";
        }
    }
    else
    {
        card->name = BuiltinName(*model_, id);
        description = BuiltinDescription(*model_, id);
    }

    card->description = MakeDescription(description);
    auto titles = std::make_shared<Column>(0.0f);
    // Built-in cards exist for the life of the page, so their titles can go into the search
    // index; external cards are rebuilt whenever the skin folder is rescanned, and an index
    // entry pointing at a discarded title would only produce a dead hit.
    if (external)
        card->title = std::make_shared<Label>(card->name, SectionTitleStyle());
    else
        card->title = MakeTitle(card->name, description);
    titles->Add(card->title);
    if (!meta.empty())
    {
        auto metaLabel = std::make_shared<Label>(std::move(meta), CaptionMuted(), TextRole::Muted);
        metaLabel->SetMargin({0.0f, 4.0f, 0.0f, 0.0f});
        titles->Add(metaLabel);
    }
    titles->Add(card->description);

    card->badge = std::make_shared<Label>(L"使用中", CaptionMuted(), TextRole::Accent);
    card->badgeHost = std::make_shared<Hideable>(card->badge, false);
    auto actions = std::make_shared<Column>(8.0f);
    actions->SetHorizontalContentAlignment(msimeui::HorizontalAlignment::Trailing);
    // Keep the slot while the badge is hidden: showing it would otherwise take width from
    // the titles and rewrap a description, and both cards would change height on selection.
    actions->SetMinWidth(44.0f);
    actions->Add(card->badgeHost);

    auto header = std::make_shared<Row>(16.0f);
    header->Add(titles, 1.0f);
    header->Add(actions);

    card->stage.band = std::make_shared<Band>(nullptr, Thickness{24.0f, 14.0f, 36.0f, 38.0f});
    BuildStage(card->stage);

    card->card = std::make_shared<PickCard>();
    card->card->SetClip(!(card->decoratedHorizontal || card->decoratedVertical));
    card->card->Add(std::make_shared<Band>(header, Thickness{24.0f, 20.0f, 24.0f, 20.0f}));
    card->card->Add(card->stage.band);
    card->card->SetSelectable(card->compatible);
    std::weak_ptr<SkinCard> weakCard = card;
    card->card->SetOnActivate([this, weakCard]() {
        if (auto target = weakCard.lock())
            Activate(target);
    });
    return card;
}

void SkinPage::BuildStage(PreviewStage &stage)
{
    stage.band->SetBackground([]() { return CurrentPalette().skinPreviewStageBg; });
    stage.unsupportedLabel = MakeCaption(L"");
    stage.unsupported = std::make_shared<Row>(0.0f);
    stage.unsupported->SetJustify(Row::Justify::Center);
    stage.unsupported->Add(stage.unsupportedLabel);
}

// `skin.ts resolvedPreviewTheme()`: the chosen preview theme wins when the skin has it,
// otherwise the skin's first theme, so a skin without that mode still shows itself.
bool SkinPage::RequestedLight() const
{
    return previewLight_ >= 0 ? previewLight_ == 1 : candLight_;
}

bool SkinPage::PreviewLight(const SkinCard &card) const
{
    const bool requested = RequestedLight();
    const char *name = requested ? "light" : "dark";
    if (card.themes.empty() || std::find(card.themes.begin(), card.themes.end(), name) != card.themes.end())
        return requested;
    return card.themes.front() == "light";
}

bool SkinPage::SupportsLayout(const std::vector<std::string> &layouts) const
{
    if (previewKind_ == PreviewKind::Toolbar || layouts.empty())
        return true;
    const char *layout = previewKind_ == PreviewKind::Vertical ? "vertical" : "horizontal";
    return std::find(layouts.begin(), layouts.end(), layout) != layouts.end();
}

void SkinPage::FillStage(PreviewStage &stage, const std::string &id, bool light,
                         const std::vector<std::string> &layouts, bool decoratedHorizontal, bool decoratedVertical)
{
    const auto show = [&stage](VisualPtr child, Thickness padding, bool decorated) {
        stage.band->SetBandPadding(padding);
        stage.band->SetClip(!decorated);
        stage.band->SetChild(std::move(child));
    };
    if (!SupportsLayout(layouts))
    {
        stage.unsupportedLabel->SetText(previewKind_ == PreviewKind::Vertical ? L"这个皮肤没有竖排的候选窗口"
                                                                              : L"这个皮肤没有横排的候选窗口");
        show(stage.unsupported, Thickness{24.0f, 14.0f, 36.0f, 38.0f}, false);
        return;
    }
    switch (previewKind_)
    {
    case PreviewKind::Horizontal:
    case PreviewKind::Vertical: {
        const bool horizontal = previewKind_ == PreviewKind::Horizontal;
        auto &preview = horizontal ? stage.horizontal : stage.vertical;
        if (!preview)
            preview = std::make_shared<skinpreview::CandidatePreview>();
        skinpreview::CandidatePreview::Options options;
        options.horizontal = horizontal;
        preview->Set(*model_, id, light, font_, options);
        show(preview, Thickness{24.0f, 14.0f, 36.0f, 38.0f}, horizontal ? decoratedHorizontal : decoratedVertical);
        break;
    }
    case PreviewKind::Toolbar:
        if (!stage.toolbar)
            stage.toolbar = std::make_shared<skinpreview::ToolbarPreview>();
        stage.toolbar->Set(skinpreview::ResolveToolbarLook(*model_, id, light), {});
        show(stage.toolbar, Thickness{24.0f, 9.0f, 24.0f, 9.0f}, decoratedHorizontal || decoratedVertical);
        break;
    }
}

void SkinPage::ApplyCardState(SkinCard &card)
{
    const bool light = PreviewLight(card);
    const bool active = model_->GetString("appearance.candidate_skin", "fluent") == card.id;

    std::wstring title = card.name;
    // Only say which theme a card is in when it cannot follow the page's choice.
    if (light != RequestedLight())
        title += light ? L" · 浅色" : L" · 深色";
    card.title->SetText(std::move(title));
    card.badgeHost->SetVisible(active);
    card.card->SetActive(active);

    if (card.applied && card.appliedLight == light && card.appliedContentKey == contentKey_)
        return;
    card.applied = true;
    card.appliedLight = light;
    card.appliedContentKey = contentKey_;
    FillStage(card.stage, card.id, light, card.layouts, card.decoratedHorizontal, card.decoratedVertical);
}

void SkinPage::RefreshCards()
{
    font_ = skinpreview::ReadCandidateFont(*model_);
    contentKey_ = ContentKey(font_);
    for (auto *cards : {&builtin_, &official_, &external_})
    {
        for (auto &card : *cards)
            ApplyCardState(*card);
    }
}

std::string SkinPage::ContentKey(const skinpreview::CandidateFont &font) const
{
    // Everything a preview reads besides the skin manifest itself: the catalog, the preview
    // layout, the candidate font and the sample the input scheme produces.
    std::string key = catalogKey_;
    key += "\x1f";
    key += std::to_string(static_cast<int>(previewKind_));
    key += "\x1f" + Narrow(font.primary);
    for (const auto &family : font.fallbacks)
        key += "\x1e" + Narrow(family);
    key += "\x1f" + model_->GetString("input.mode", "chinese");
    key += "\x1e" + model_->GetString("input.schema", "shuangpin");
    key += "\x1e" + model_->GetString("input.shuangpin_schema", "xiaohe");
    return key;
}

const SkinCard *SkinPage::FindCard(const std::string &id) const
{
    for (auto *cards : {&builtin_, &official_, &external_})
    {
        for (const auto &card : *cards)
        {
            if (card->id == id)
                return card.get();
        }
    }
    return nullptr;
}

void SkinPage::UpdateCurrentSkin()
{
    static const std::vector<std::string> kNoLayouts;
    const std::string active = model_->GetString("appearance.candidate_skin", "fluent");
    const SkinCard *card = FindCard(active);
    const bool light = card ? PreviewLight(*card) : RequestedLight();
    currentName_->SetText(card ? card->name : Widen(active));

    std::wstring hint;
    if (card && !card->compatible)
        hint = L"当前布局或明暗不受该皮肤支持，候选窗会回退到 Fluent";
    else if (previewLight_ < 0)
    {
        hint = std::wstring(L"预览跟随候选窗口主题：") + (light ? L"浅色" : L"深色");
        if (light != RequestedLight())
            hint += light ? L"，这个皮肤只有浅色" : L"，这个皮肤只有深色";
    }
    else
        hint = std::wstring(L"正在预览") + (light ? L"浅色" : L"深色") + L"，不影响主题设置";
    currentHint_->SetText(std::move(hint));

    std::string key = contentKey_;
    key += '\x1f';
    key += active;
    key += light ? 'L' : 'D';
    if (key == currentKey_)
        return;
    currentKey_ = std::move(key);
    FillStage(currentStage_, active, light, card ? card->layouts : kNoLayouts, card && card->decoratedHorizontal,
              card && card->decoratedVertical);
}

void SkinPage::SyncPreviewControls()
{
    for (size_t i = 0; i < kindPills_.size(); ++i)
        kindPills_[i]->SetActive(static_cast<PreviewKind>(i) == previewKind_);
    for (size_t i = 0; i < lightPills_.size(); ++i)
        lightPills_[i]->SetActive(kLightPillValues[i] == previewLight_);
}

void SkinPage::SetPreviewKind(PreviewKind kind)
{
    if (previewKind_ == kind)
        return;
    previewKind_ = kind;
    SyncPreviewControls();
    RefreshCards();
    UpdateCurrentSkin();
}

void SkinPage::SetPreviewLight(int value)
{
    if (previewLight_ == value)
        return;
    previewLight_ = value;
    SyncPreviewControls();
    RefreshCards();
    UpdateCurrentSkin();
}

void SkinPage::Activate(const std::shared_ptr<SkinCard> &card)
{
    if (!card->compatible || model_->GetString("appearance.candidate_skin", "fluent") == card->id)
        return;
    model_->Update("appearance.candidate_skin", card->id);
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
                             "appearance.candidate_skin_directory", "appearance.candidate_skin_catalog_scanned",
                             "appearance.candidate_skin_catalog_revision"})
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
    if (themeKey != themeKey_)
    {
        themeKey_ = themeKey;
        previewLight_ = -1;
    }
    SyncPreviewControls();
    RefreshCards();
    UpdateCurrentSkin();
}

VisualPtr SkinPage::Build(PageContext &)
{
    PageBuilder page(L"皮肤");

    // 当前皮肤: the page's summary of what is in use, with the preview controls. The controls
    // only change what this page draws; the real theme is on the appearance page.
    {
        auto block = std::make_shared<FlushCard>();
        auto titles = std::make_shared<Column>(0.0f);
        titles->Add(MakeCaption(L"当前皮肤"));
        currentName_ = std::make_shared<Label>(L"", SectionTitleStyle());
        currentName_->SetMargin({0.0f, 2.0f, 0.0f, 0.0f});
        titles->Add(currentName_);
        currentHint_ = MakeCaption(L"");
        currentHint_->SetMargin({0.0f, 4.0f, 0.0f, 0.0f});
        titles->Add(currentHint_);

        auto controls = std::make_shared<Column>(6.0f);
        controls->SetHorizontalContentAlignment(msimeui::HorizontalAlignment::Trailing);
        auto kindRow = std::make_shared<Row>(6.0f);
        kindRow->Add(MakeCaption(L"布局"));
        const wchar_t *kindLabels[] = {L"横排", L"竖排", L"工具栏"};
        for (int i = 0; i < 3; ++i)
        {
            auto pill = std::make_shared<PillButton>(kindLabels[i]);
            const PreviewKind kind = static_cast<PreviewKind>(i);
            pill->SetOnClick([this, kind]() { SetPreviewKind(kind); });
            kindPills_.push_back(pill);
            kindRow->Add(pill);
        }
        controls->Add(kindRow);
        auto lightRow = std::make_shared<Row>(6.0f);
        lightRow->Add(MakeCaption(L"明暗"));
        const wchar_t *lightLabels[] = {L"跟随主题", L"浅色", L"深色"};
        for (int i = 0; i < 3; ++i)
        {
            auto pill = std::make_shared<PillButton>(lightLabels[i]);
            const int value = kLightPillValues[i];
            pill->SetOnClick([this, value]() { SetPreviewLight(value); });
            lightPills_.push_back(pill);
            lightRow->Add(pill);
        }
        controls->Add(lightRow);

        auto header = std::make_shared<Row>(16.0f);
        header->Add(titles, 1.0f);
        header->Add(controls);
        block->Add(std::make_shared<Band>(header, Thickness{24.0f, 20.0f, 24.0f, 20.0f}));

        currentStage_.band = std::make_shared<Band>(nullptr, Thickness{24.0f, 14.0f, 36.0f, 38.0f});
        BuildStage(currentStage_);
        block->Add(currentStage_.band);
        page.AddRaw(block, 14.0f);
    }

    page.AddRaw(std::make_shared<Band>(
                    MakeDescription(L"点击下面的卡片即可切换皮肤。布局和明暗只影响本页的预览，不会改动外观设置。"),
                    Thickness{4.0f, 0.0f, 4.0f, 0.0f}),
                10.0f);

    page.AddRaw(std::make_shared<Band>(MakeTitle(L"内置皮肤"), Thickness{4.0f, 0.0f, 4.0f, 0.0f}), 10.0f);
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
