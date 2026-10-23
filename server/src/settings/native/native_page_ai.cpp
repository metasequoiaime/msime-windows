#include "settings/native/native_page_voice_fields.h"
#include "settings/native/native_settings_pill.h"

#include "ai/jev_client.h"
#include "config/ai_provider.h"
#include "voice-input/voice_providers.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Scene.h"
#include "msimeui/Window.h"

#include <algorithm>
#include <map>

// 移植自旧网页设置页的 partials/ai-settings.html 与 modules/ai-settings.ts、
// ai-assistant-state.ts、model-fetch.ts、credential-test.ts，结构已按三块重排：
// 状态、联想模式与提供商、提示词。
namespace native_settings
{
using namespace voice_fields;

namespace
{
// ---------------------------------------------------------------------------------
// Model combobox: `.dropdown-toggle.font-combobox.ai-model-combobox` with the model
// list (`#aiModelMenu.font-dropdown-menu.compact-scroll-menu`) that 查询模型 fills.
// ---------------------------------------------------------------------------------
class ModelCombo;

class ModelList : public msimeui::Visual
{
  public:
    static constexpr float kItem = 28.0f;
    static constexpr float kGap = 2.0f;
    static constexpr float kInset = 2.0f;

    explicit ModelList(ModelCombo *owner) : owner_(owner)
    {
    }

    void SetItems(std::vector<std::wstring> items)
    {
        items_ = std::move(items);
        hovered_ = kNone;
        pressed_ = kNone;
        scrollbar_.Reset();
        LayoutScrollbar();
        InvalidateMeasure();
    }

    bool Empty() const
    {
        return items_.empty();
    }

    float ContentHeight() const
    {
        const float rows = static_cast<float>(items_.size());
        return kInset * 2.0f + rows * kItem + std::max(rows - 1.0f, 0.0f) * kGap;
    }

    SizeF Measure(const SizeF &) override
    {
        // 6 rows visible (border box 182px).
        const float visible = 6.0f * 28.0f + 5.0f * 2.0f + 2.0f * 2.0f - 2.0f;
        // No intrinsic width: the popup matches the combobox (max of this and the anchor).
        return {0.0f, std::min(ContentHeight(), visible)};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        LayoutScrollbar();
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        ID2D1RenderTarget *target = dr.GetRenderTarget();
        target->PushAxisAlignedClip(ToD2D(bounds_), D2D1_ANTIALIAS_MODE_ALIASED);
        for (size_t i = 0; i < items_.size(); ++i)
        {
            const RectF item = ItemRect(i);
            if (item.y + item.height < bounds_.y || item.y > bounds_.y + bounds_.height)
                continue;
            if (i == hovered_)
                FillRounded(dr, item, 8.0f, p.dropdownItemHover);
            // white-space: nowrap; text-overflow: ellipsis
            TextLayout text;
            TextStyle style = BodyStyle();
            text.Set(items_[i], style);
            const float width = item.width - 16.0f;
            IDWriteTextLayout *layout = text.Get(width);
            if (layout)
            {
                layout->SetMaxWidth(width);
                DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
                DWriteFactory()->CreateEllipsisTrimmingSign(layout, ellipsis.GetAddressOf());
                layout->SetTrimming(&trimming, ellipsis.Get());
                DWRITE_TEXT_METRICS metrics{};
                layout->GetMetrics(&metrics);
                if (ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(i == hovered_ ? p.text : p.dropdownItem))
                    target->DrawTextLayout(D2D1::Point2F(item.x + 8.0f, item.y + (item.height - metrics.height) * 0.5f),
                                           layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
        target->PopAxisAlignedClip();
        // This list's thumb does not light up under the pointer.
        scrollbar_.Render(dr, p.scrollbarThumb, p.scrollbarThumb);
    }

    // The list takes no keyboard focus, but a press on it must not reach a focusable
    // field the popup covers: Window sends the press to whatever FindFocusableAt returns.
    msimeui::Visual *FindFocusableAt(const PointF &point) override
    {
        return HitTest(point) ? this : nullptr;
    }

    bool OnMouseMove(const POINT &point, WPARAM keyState) override
    {
        if (!window_)
            return true;
        const PointF dip = window_->ClientPixelsToDips(point);
        bool changed = scrollbar_.Move(dip, keyState);
        if (!scrollbar_.IsDragging())
        {
            const size_t hit = scrollbar_.IsOnScrollbar(dip) ? kNone : HitRow(dip);
            changed = changed || hit != hovered_;
            hovered_ = hit;
        }
        if (changed)
            InvalidateVisual();
        return true;
    }

    void OnMouseLeave() override
    {
        scrollbar_.Leave();
        hovered_ = kNone;
        InvalidateVisual();
    }

    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        pressed_ = kNone;
        if (!window_)
            return true;
        const PointF dip = window_->ClientPixelsToDips(point);
        if (scrollbar_.IsOnScrollbar(dip))
        {
            scrollbar_.Press(dip);
            hovered_ = kNone;
            InvalidateVisual();
            return true;
        }
        pressed_ = HitRow(dip);
        return true;
    }

    bool OnMouseUp(const POINT &point, WPARAM) override;

    bool OnMouseWheel(const POINT &, short delta, WPARAM) override
    {
        if (scrollbar_.Wheel(delta))
            InvalidateVisual();
        return true;
    }

  private:
    static constexpr size_t kNone = static_cast<size_t>(-1);

    RectF ItemRect(size_t i) const
    {
        const float scroll = scrollbar_.GetPlacedOffset(window_ ? window_->GetDpi() : 96.0f);
        return {bounds_.x + kInset, bounds_.y + kInset + static_cast<float>(i) * (kItem + kGap) - scroll,
                bounds_.width - kInset * 2.0f, kItem};
    }

    size_t HitRow(const PointF &point) const
    {
        if (!Contains(bounds_, point))
            return kNone;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (Contains(ItemRect(i), point))
                return i;
        }
        return kNone;
    }

    void LayoutScrollbar()
    {
        scrollbar_.Layout(ContentHeight(), bounds_.height, bounds_);
    }

    ModelCombo *owner_ = nullptr;
    std::vector<std::wstring> items_;
    size_t hovered_ = kNone;
    size_t pressed_ = kNone;
    CompactMenuScrollbar scrollbar_;
};

class ModelPopup : public msimeui::Popup
{
  public:
    explicit ModelPopup(const std::shared_ptr<ModelList> &list) : msimeui::Popup(list), list_(list.get())
    {
        SetPadding(1.0f);
        SetPlacement(msimeui::PopupPlacement::BelowLeading);
        SetCornerRadius(10.0f);
        SetShadowEnabled(true);
        SetConstrainToViewport(true);
        SetMatchAnchorWidth(true);
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        ScopedTextAntialias grayscale(dr, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        msimeui::Popup::Render(dr);
    }

    bool OnMouseWheel(const POINT &point, short delta, WPARAM keyState) override
    {
        if (!window_ || !HitTest(window_->ClientPixelsToDips(point)))
            return false;
        list_->OnMouseWheel(point, delta, keyState);
        return true;
    }

  private:
    ModelList *list_ = nullptr;
};

class ModelCombo : public msimeui::Visual
{
  public:
    using Handler = std::function<void(const std::wstring &)>;

    ModelCombo()
    {
        InputBox::Options options;
        options.bare = true;
        options.font = FieldTextStyle(kFontBody);
        input_ = std::make_shared<InputBox>(options);
        AdoptChild(input_);
        list_ = std::make_shared<ModelList>(this);
        popup_ = std::make_shared<ModelPopup>(list_);
    }

    std::shared_ptr<InputBox> Input() const
    {
        return input_;
    }

    void SetOnPicked(Handler handler)
    {
        onPicked_ = std::move(handler);
    }

    // Arrow click with no list yet: same as 查询模型.
    void SetOnFetch(std::function<void()> handler)
    {
        onFetch_ = std::move(handler);
    }

    void SetModels(std::vector<std::wstring> models)
    {
        Close();
        list_->SetItems(std::move(models));
    }

    bool HasModels() const
    {
        return !list_->Empty();
    }

    void Open()
    {
        msimeui::Scene *scene = window_ ? window_->GetScene() : nullptr;
        if (open_ || !scene || list_->Empty())
            return;
        scene->ClearPopups(true);
        const Palette &p = CurrentPalette();
        popup_->SetBackgroundFill(p.dropdownMenuBg);
        popup_->SetBorderColor(p.dropdownMenuBorder);
        popup_->SetTopHighlight(p.dropdownMenuHighlight);
        popup_->SetShadowPasses({{p.menuShadow[0].sigma, p.menuShadow[0].alpha, 0.0f, p.menuShadow[0].offsetY},
                                 {p.menuShadow[1].sigma, p.menuShadow[1].alpha, 0.0f, p.menuShadow[1].offsetY}});
        popup_->SetAnchorRect(bounds_);
        popup_->SetOffset(0.0f, 2.0f);
        scene->AddPopup(popup_, [this]() {
            open_ = false;
            InvalidateVisual();
        });
        popup_->BeginAppearAnimation(140.0, -3.0f, 0.985f);
        open_ = true;
        window_->Invalidate();
    }

    void Close()
    {
        if (!open_)
            return;
        if (msimeui::Scene *scene = window_ ? window_->GetScene() : nullptr)
            scene->RemovePopup(popup_.get(), false);
        open_ = false;
        InvalidateVisual();
    }

    void Pick(const std::wstring &model)
    {
        Close();
        input_->SetText(model);
        if (onPicked_)
        {
            const auto handler = onPicked_;
            handler(model);
        }
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        input_->MeasureInLayout({std::max(availableSize.width - 12.0f - 6.0f - 18.0f - 2.0f, 0.0f), 40.0f});
        return {availableSize.width, 40.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        popup_->SetAnchorRect(bounds_);
        const float border = CssBorder();
        const float lineHeight = LineHeight(FieldTextStyle(kFontBody)) + 2.0f;
        input_->ArrangeInLayout({finalRect.x + border + 12.0f, finalRect.y + (finalRect.height - lineHeight) * 0.5f,
                                 std::max(finalRect.width - border * 2.0f - 12.0f - 6.0f - 18.0f, 0.0f), lineHeight});
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        const bool focused = input_->IsFocused();
        // :focus-within ring and border, the dropdown look otherwise.
        if (focused || open_)
            FillRounded(dr, Inflate(bounds_, 2.0f), 10.0f, p.accentSoft);
        FillRounded(dr, bounds_, 8.0f, hovered_ || open_ ? p.surfaceRaised : p.dropdownBg);
        StrokeRounded(dr, bounds_, 8.0f, focused || open_ ? p.accentSoftBorder : (hovered_ ? p.borderStrong : p.border),
                      1.0f);
        input_->Render(dr);
        static const std::string arrow = LoadAssetSvg(L"arrow.svg");
        static const std::string lightArrow = InvertSvgColors(arrow, 0.25f);
        const RectF a = ArrowRect();
        DrawSvg(dr, p.light ? lightArrow : arrow, a);
    }

    void Attach(msimeui::Window *window) override
    {
        msimeui::Visual::Attach(window);
        input_->Attach(window);
        popup_->Attach(window);
    }

    msimeui::Visual *FindVisualAt(const PointF &point) override
    {
        if (!HitTest(point))
            return nullptr;
        if (Contains(ArrowRect(), point))
            return this;
        return input_->FindVisualAt(point) ? input_->FindVisualAt(point) : this;
    }

    msimeui::Visual *FindFocusableAt(const PointF &point) override
    {
        if (!HitTest(point) || Contains(ArrowRect(), point))
            return nullptr;
        return input_->FindFocusableAt(point);
    }

    bool KeepsPopupsOpenOnClick() const override
    {
        return true;
    }

    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        pressed_ = window_ && HitTest(window_->ClientPixelsToDips(point));
        return pressed_;
    }

    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        if (!pressed_ || !window_)
            return false;
        pressed_ = false;
        const PointF dip = window_->ClientPixelsToDips(point);
        if (!HitTest(dip))
            return true;
        if (Contains(ArrowRect(), dip) && list_->Empty())
        {
            if (onFetch_)
            {
                // A handler may tear down the visual that owns onFetch_ while it runs.
                const auto handler = onFetch_;
                handler();
            }
            return true;
        }
        open_ ? Close() : Open();
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
    RectF ArrowRect() const
    {
        return {bounds_.x + bounds_.width - CssBorder() - 6.0f - 18.0f, bounds_.y + (bounds_.height - 18.0f) * 0.5f,
                18.0f, 18.0f};
    }

    std::shared_ptr<InputBox> input_;
    std::shared_ptr<ModelList> list_;
    std::shared_ptr<ModelPopup> popup_;
    Handler onPicked_;
    std::function<void()> onFetch_;
    bool hovered_ = false;
    bool pressed_ = false;
    bool open_ = false;
};

bool ModelList::OnMouseUp(const POINT &point, WPARAM)
{
    if (scrollbar_.IsDragging())
    {
        // The press belonged to the scrollbar, so releasing over a row picks nothing.
        scrollbar_.Release(window_ ? window_->ClientPixelsToDips(point) : PointF{});
        pressed_ = kNone;
        return true;
    }
    const size_t hit = window_ ? HitRow(window_->ClientPixelsToDips(point)) : kNone;
    if (hit != kNone && hit == pressed_)
        owner_->Pick(items_[hit]);
    pressed_ = kNone;
    return true;
}

// An input in the 40px `.ai-grid` look.
std::shared_ptr<InputBox> GridInput(bool token = false)
{
    InputBox::Options options;
    options.height = 40.0f;
    if (token)
    {
        options.font = MonospaceStyle();
        options.letterSpacing = MonospaceTokenSpacing();
        options.password = true;
        options.reveal = true;
    }
    return std::make_shared<InputBox>(options);
}

std::shared_ptr<Dropdown> GridDropdown(std::vector<DropdownOption> options)
{
    // `.ai-grid .dropdown-toggle`: full width and 40px tall; the menu stays right-aligned
    // and shrink-to-fit.
    Dropdown::Options settings;
    // Inside an `.ai-field`, whose font-weight: 500 the button and menu inherit.
    settings.weight = DWRITE_FONT_WEIGHT_MEDIUM;
    auto dropdown = std::make_shared<Dropdown>(std::move(options), settings);
    dropdown->SetHeight(40.0f);
    return dropdown;
}

std::string Trimmed(const std::string &text)
{
    return Narrow(Trim(Widen(text)));
}

// 注册表里某个服务商的字段；找不到返回 nullptr。
const Json *FindProviderEntry(const Json *providers, const std::string &id, const char *field)
{
    if (!providers || !providers->is_array())
        return nullptr;
    for (const Json &entry : *providers)
    {
        if (!entry.is_object() || entry.value("id", std::string()) != id)
            continue;
        const auto found = entry.find(field);
        return found == entry.end() || !found->is_string() ? nullptr : &*found;
    }
    return nullptr;
}

struct AiPage : std::enable_shared_from_this<AiPage>
{
    explicit AiPage(PageContext &context) : context(context)
    {
    }

    PageContext &context;
    int localWrites = 0;

    std::string mode = "generate";
    std::string provider = "deepseek";
    std::string promptId = "custom_1";
    std::map<std::string, std::string> customPrompts = {{"custom_1", ""}, {"custom_2", ""}, {"custom_3", ""}};

    std::shared_ptr<RadioGroup> modeGroup;
    std::shared_ptr<StatusPill> pill;
    std::shared_ptr<Label> statusText;
    std::shared_ptr<Dropdown> providerDropdown;
    std::shared_ptr<ModelCombo> model;
    std::shared_ptr<InputBox> token;
    std::shared_ptr<Hideable> tokenField;
    std::shared_ptr<InputBox> limit;
    std::shared_ptr<InputBox> endpoint;
    std::shared_ptr<Hideable> providerField;
    std::shared_ptr<Dimmable> limitDimmable;
    std::shared_ptr<Dimmable> thinkingDimmable;
    std::shared_ptr<Hideable> promptSection;
    std::shared_ptr<Dropdown> promptSlot;
    std::shared_ptr<InputBox> prompt;
    std::shared_ptr<PlainButton> fetchButton;
    std::shared_ptr<StatusText> fetchStatus;
    std::unique_ptr<CredentialTest> test;
    std::string pendingFetch;
    // 切换模式或服务商后，配置要等宿主写盘并推回快照才算落地。这期间状态栏说「保存中」，
    // 免得「界面已经切好、后端还是旧服务商」的那几百毫秒被当成已经可用。
    bool saving = false;

    void Write(const std::string &path, Json value)
    {
        ++localWrites;
        context.model.Update(path, std::move(value));
        --localWrites;
    }

    // endpoint/model 按服务商写槽位键：设置宿主据此判断单值键要不要跟着改，页面不必知道
    // 后端此刻的活动服务商是哪一家。
    void WriteProviderValue(const std::string &field, const std::string &value)
    {
        ++localWrites;
        context.model.Update("ai_assistant." + field + "@" + provider, value);
        --localWrites;
    }

    // Credential test and model list share one request config; empty values fall back to
    // the provider defaults.
    Json CurrentConfig() const
    {
        const std::string endpointValue = Trimmed(Narrow(endpoint->GetText()));
        const std::string modelValue = Trimmed(Narrow(model->Input()->GetText()));
        return Json{{"provider", provider},
                    {"token", Trimmed(Narrow(token->GetText()))},
                    {"endpoint", endpointValue},
                    {"model", modelValue}};
    }

    void RefreshChoices()
    {
        const auto choices = AiProviderChoices(context.model.Find("ai_assistant.providers"), mode);
        std::vector<DropdownOption> options;
        options.reserve(choices.size());
        for (const auto &[id, name] : choices)
            options.push_back({name, id});
        providerDropdown->SetOptions(options);
    }

    void ApplyProviderFields()
    {
        const Json *providers = context.model.Find("ai_assistant.providers");
        const Json *endpoints = context.model.Find("ai_assistant.endpoints");
        const Json *models = context.model.Find("ai_assistant.models");
        const auto slot = [this, providers](const Json *map, const char *field) {
            std::string value;
            if (map && map->is_object())
            {
                const auto found = map->find(provider);
                if (found != map->end() && found->is_string())
                    value = Trimmed(found->get<std::string>());
            }
            if (value.empty())
            {
                // 用户自建的服务商把地址与模型放在注册表里。
                if (const Json *entry = FindProviderEntry(providers, provider, field); entry)
                    value = Trimmed(entry->get<std::string>());
            }
            return value;
        };
        endpoint->SetText(Widen(slot(endpoints, "endpoint")));
        endpoint->SetPlaceholder(Widen(DefaultEndpoint()));
        model->Input()->SetText(Widen(slot(models, "model")));
        model->Input()->SetPlaceholder(Widen(DefaultModel()));
        if (tokenField)
            tokenField->SetVisible(AiAssistant::ProviderUsesToken(provider));

        std::string tokenValue;
        if (const Json *tokens = context.model.Find("ai_assistant.tokens"); tokens && tokens->is_object())
        {
            const auto found = tokens->find(provider);
            if (found != tokens->end() && found->is_string())
                tokenValue = Trimmed(found->get<std::string>());
        }
        // 与 ReadActiveAiConfig 同一顺序：注册表、最后才是给当前服务商兜底的单值 token。
        if (tokenValue.empty())
        {
            if (const Json *entry = FindProviderEntry(providers, provider, "token"); entry && entry->is_string())
                tokenValue = Trimmed(entry->get<std::string>());
        }
        if (tokenValue.empty() && context.model.GetString("ai_assistant.provider") == provider)
            tokenValue = Trimmed(context.model.GetString("ai_assistant.token"));
        token->SetText(Widen(tokenValue));
    }

    // 出厂默认值只用于占位符：空槽位在界面上显示灰色提示，保存时仍写空串。
    std::string DefaultEndpoint() const
    {
        return Narrow(AiDefaultEndpoint(provider));
    }

    std::string DefaultModel() const
    {
        return Narrow(AiDefaultModel(provider));
    }

    // 思考强度是否生效由端点域名决定，不看服务商标签。
    bool ThinkingAvailable() const
    {
        if (!endpoint)
            return false;
        const std::string current = Trimmed(Narrow(endpoint->GetText()));
        return VoiceInput::VendorForRequest(provider, current) == VoiceInput::ChatVendor::DeepSeek;
    }

    // 页首摘要与各字段的可用状态都从这里推，避免两处各写一遍条件。
    void SyncLayout()
    {
        // 控件在 Build 末尾才全部就绪；OnConfig 注册时会立刻跑一次 Sync，这里统一做空指针保护。
        const bool select = mode == "select";
        if (providerField)
            providerField->SetVisible(!select);
        // Jev 只吃 Token、模型与接口地址；候选数量与思考强度对它没有意义，置灰并挡住输入。
        for (const auto &dim : {limitDimmable, thinkingDimmable})
        {
            if (dim)
                dim->SetDisabled(select || (dim == thinkingDimmable && !ThinkingAvailable()));
        }
        if (promptSection)
            promptSection->SetVisible(!select && context.model.GetBool("ai_assistant.enabled"));
        // 选择器不自带模型清单：官方 TypeSafe 只认内置模型名，自建实现的模型名由用户填。
        // 这个按钮在 select 模式下只会返回一句「不提供模型列表」，不如直接关掉。
        if (fetchButton)
            fetchButton->SetDisabled(select);
        RefreshStatus();
    }

    void RefreshStatus()
    {
        if (mode == "select")
        {
            const std::string text = Trimmed(Narrow(token->GetText()));
            const bool set = !text.empty() && text.front() != '<';
            const std::wstring picked = Trim(model->Input()->GetText());
            const std::wstring shown = picked.empty() ? Widen(DefaultModel()) : picked;
            pill->Set(L"从候选里挑 · " + (saving ? std::wstring(L"保存中") : shown),
                      saving ? "pending" : (set ? "success" : "pending"));
            statusText->SetText(L"只把分段拼音、前文和最多 32 个本地候选发给选择器，由它挑一个；不生成新文字。"
                                L"官方 TypeSafe 之外，Laya、OpenJev 这类自建实现也照同一套协议，在下方填它们的地址。");
            return;
        }
        const std::wstring name = AiProviderLabel(provider, context.model.Find("ai_assistant.providers"));
        const std::wstring modelText = Trim(model->Input()->GetText());
        const std::wstring shown = modelText.empty() ? Widen(DefaultModel()) : modelText;
        const bool ready = context.model.GetBool("ai_assistant.enabled");
        pill->Set(name + L" · " +
                      (saving ? std::wstring(L"保存中") : (shown.empty() ? std::wstring(L"未选模型") : shown)),
                  saving ? "pending" : (ready ? "success" : "pending"));
        statusText->SetText(ready ? L"已启用。每次输入完整拼音后，拼音、前文和字数上限会发给上方服务商。"
                                  : L"尚未启用；下面的配置保存后，打开总开关才会生效。");
    }

    void ClearModelMenu()
    {
        pendingFetch.clear();
        fetchButton->SetDisabled(false);
        fetchButton->SetText(L"查询模型");
        model->SetModels({});
        fetchStatus->Clear();
    }

    void SwitchProvider(const std::string &next)
    {
        if (next == provider)
            return;
        provider = next;
        Write("ai_assistant.provider", provider);
        saving = true;
        providerDropdown->SetValue(provider);
        ApplyProviderFields();
        ClearModelMenu();
        SyncLayout();
    }

    void SwitchMode(const std::string &next)
    {
        if (next == mode)
            return;
        mode = next;
        const std::string target = AiProviderForMode(mode, provider);
        if (target != provider)
        {
            // 配置层在 provider 变更时自动记下离开的那家，页面不需要单独写 generate_provider。
            provider = target;
            Write("ai_assistant.provider", provider);
            saving = true;
        }
        RefreshChoices();
        providerDropdown->SetValue(provider);
        ApplyProviderFields();
        ClearModelMenu();
        SyncLayout();
    }

    void SwitchPrompt(const std::string &id)
    {
        customPrompts[promptId] = Narrow(prompt->GetText());
        Write("ai_assistant.prompt_" + promptId, customPrompts[promptId]);
        promptId = id;
        prompt->SetText(Widen(customPrompts[id]));
    }

    void Fetch()
    {
        if (!pendingFetch.empty())
            return;
        pendingFetch = context.model.NewRequestId("models");
        fetchButton->SetDisabled(true);
        fetchButton->SetText(L"获取中…");
        fetchStatus->Set(L"正在向服务商查询模型列表…", "pending");
        context.model.Post("apiModelList",
                           Json{{"requestId", pendingFetch}, {"service", "ai.assistant"}, {"config", CurrentConfig()}});
    }

    void OnModels(const Json &message)
    {
        if (pendingFetch.empty() || message.value("requestId", std::string()) != pendingFetch)
            return;
        pendingFetch.clear();
        fetchButton->SetDisabled(false);
        fetchButton->SetText(L"查询模型");
        if (!message.value("ok", false))
        {
            fetchStatus->Set(Widen(message.value("message", std::string())), "error");
            return;
        }
        std::vector<std::wstring> names;
        if (message.contains("models") && message["models"].is_array())
        {
            for (const auto &item : message["models"])
            {
                if (item.is_string() && !Trim(Widen(item.get<std::string>())).empty())
                    names.push_back(Widen(item.get<std::string>()));
            }
        }
        model->SetModels(names);
        if (names.empty())
        {
            const std::string text = message.value("message", std::string());
            fetchStatus->Set(text.empty() ? L"服务商没有返回可用模型" : Widen(text), "error");
            return;
        }
        fetchStatus->Set(L"获取到 " + std::to_wstring(names.size()) + L" 个模型", "success");
        model->Open();
    }

    void Sync()
    {
        if (localWrites > 0)
            return;
        // 首帧快照可能早于控件构建完成，和 SyncLayout 一样先确认控件已就绪。
        if (!modeGroup || !providerDropdown || !endpoint || !model || !token || !limit || !promptSlot || !prompt)
            return;
        const SettingsModel &m = context.model;
        // 快照回来了，说明上次的写入已经落地，状态栏可以显示最终结果。
        saving = false;
        const std::string stored = m.GetString("ai_assistant.provider", "deepseek");
        mode = AiModeOf(stored);
        modeGroup->SetValue(mode);
        RefreshChoices();
        // 生成式用 generate_provider 恢复上次选的服务商；它缺失或非法时退回 provider 本身。
        const std::string remembered = m.GetString("ai_assistant.generate_provider");
        provider = AiProviderForMode(mode, remembered.empty() ? stored : remembered);
        providerDropdown->SetValue(provider);
        ApplyProviderFields();
        limit->SetText(std::to_wstring(m.GetInt("ai_assistant.candidate_limit", 3)));
        promptId = m.GetString("ai_assistant.prompt_id", "custom_1");
        const Json *custom1 = m.Find("ai_assistant.prompt_custom_1");
        customPrompts["custom_1"] =
            custom1 && custom1->is_string() ? custom1->get<std::string>() : m.GetString("ai_assistant.prompt");
        customPrompts["custom_2"] = m.GetString("ai_assistant.prompt_custom_2");
        customPrompts["custom_3"] = m.GetString("ai_assistant.prompt_custom_3");
        promptSlot->SetValue(promptId);
        prompt->SetText(Widen(customPrompts[promptId]));
        SyncLayout();
    }

    VisualPtr BuildProviderSection()
    {
        auto column = std::make_shared<Column>(0.0f);
        auto heading = MakeTitle(L"服务商");
        heading->SetMargin({0.0f, 0.0f, 0.0f, 18.0f});
        column->Add(heading);
        auto description = MakeDescription(
            L"以下服务商都走 OpenAI 兼容的 Chat Completions 接口。接口地址与模型留空时使用各家的默认值；"
            L"凭据按服务商分开保存，切换服务商不会清掉其它家的 Token。Ollama 连接本机服务，不需要 API Token，"
            L"模型用「查询模型」从已下载的模型里选。");
        description->SetMargin({0.0f, -10.0f, 0.0f, 16.0f});
        column->Add(description);

        auto grid = std::make_shared<FormGrid>();
        providerDropdown = GridDropdown({});
        providerDropdown->SetOnChanged([this](size_t, const DropdownOption &option) { SwitchProvider(option.value); });
        providerField = std::make_shared<Hideable>(std::make_shared<Field>(L"服务提供商", providerDropdown));
        grid->Add(providerField);
        model = std::make_shared<ModelCombo>();
        model->SetOnPicked([this](const std::wstring &value) { WriteProviderValue("model", Narrow(value)); });
        model->SetOnFetch([this]() { Fetch(); });
        model->Input()->SetOnCommit([this](const std::wstring &text) {
            // 空值表示回到默认模型：写空串让后端补默认值，界面上显示占位符。
            WriteProviderValue("model", Trimmed(Narrow(text)));
        });
        fetchButton = std::make_shared<PlainButton>(L"查询模型", ModelFetchButtonStyle());
        fetchButton->SetOnClick([this]() { Fetch(); });
        auto modelRow = std::make_shared<Row>(8.0f);
        modelRow->SetAlign(Row::Align::Stretch);
        modelRow->Add(model, 1.0f);
        modelRow->Add(fetchButton);
        auto testButton = std::make_shared<PlainButton>(L"测试连接", CredentialButtonStyle());
        auto testStatus = std::make_shared<StatusText>();
        auto actions = std::make_shared<Row>(10.0f);
        actions->Add(testButton);
        actions->Add(testStatus, 1.0f);
        fetchStatus = std::make_shared<StatusText>();
        auto modelField = std::make_shared<Field>(L"模型", modelRow);
        auto tail = std::make_shared<Column>(0.0f);
        tail->Add(std::make_shared<MinHeight>(actions, 20.0f));
        fetchStatus->SetMargin({0.0f, 7.0f, 0.0f, 0.0f});
        tail->Add(fetchStatus);
        modelField->Add(tail);
        grid->Add(modelField);
        test = std::make_unique<CredentialTest>(
            context, testButton, testStatus, []() { return std::string("ai.assistant"); },
            [this]() { return CurrentConfig(); });
        context.OnMessage("apiModelListResult", [weak = weak_from_this()](const Json &message) {
            if (auto page = weak.lock())
                page->OnModels(message);
        });

        token = GridInput(true);
        token->SetOnCommit([this](const std::wstring &text) {
            // 不用 Token 的服务商不落盘，免得留下一个谁都不读的空槽或旧值。
            if (!AiAssistant::ProviderUsesToken(provider))
                return;
            Write("ai_assistant.token_" + provider, Trimmed(Narrow(text)));
            RefreshStatus();
        });
        // Ollama 不鉴权，选它时整个输入框收起来，不留一个填了也没用的框。
        tokenField = std::make_shared<Hideable>(std::make_shared<Field>(L"API Token", token));
        grid->Add(tokenField);

        limit = GridInput();
        limit->SetPlaceholder(L"1-10，默认 3");
        limit->SetOnCommit([this](const std::wstring &text) {
            int value = 3;
            try
            {
                value = std::stoi(Trimmed(Narrow(text)));
            }
            catch (...)
            {
                value = 3;
            }
            value = std::clamp(value, 1, 10);
            limit->SetText(std::to_wstring(value));
            Write("ai_assistant.candidate_limit", value);
        });
        auto limitField = std::make_shared<Field>(L"候选数量", limit);
        limitField->Add(MakeDescription(L"每次输入最多生成几个候选，范围 1-10。"));
        limitDimmable = std::make_shared<Dimmable>(limitField);
        grid->Add(limitDimmable);
        auto thinking =
            BindDropdown(context, "ai_assistant.thinking",
                         Options({{"disabled", L"关闭"}, {"low", L"低"}, {"high", L"高"}, {"max", L"最高"}}));
        thinking->SetHeight(40.0f);
        auto thinkingColumn = std::make_shared<Field>(L"思考强度", thinking);
        thinkingColumn->Add(MakeDescription(L"只对 DeepSeek 生效。开启后候选更准，但要多等几秒到几十秒。"));
        thinkingDimmable = std::make_shared<Dimmable>(thinkingColumn);
        grid->Add(thinkingDimmable);

        endpoint = GridInput();
        endpoint->SetOnCommit([this](const std::wstring &text) {
            WriteProviderValue("endpoint", Trimmed(Narrow(text)));
            // 端点变了，思考强度是否生效也跟着变（后端按域名判定）。
            SyncLayout();
        });
        auto endpointField = std::make_shared<Field>(L"接口地址", endpoint);
        endpointField->Add(
            MakeDescription(L"留空使用官方地址。Laya、OpenJev 这类自建实现照同一套协议，填它们的地址即可。"
                            L"这里填的地址会收到你的 Token，只填你信任的地址。"));
        grid->Add(endpointField, true);

        column->Add(grid);
        return column;
    }

    VisualPtr Build()
    {
        PageBuilder page(L"AI 与云联想");

        // 状态区：总开关、当前配置摘要、数据去向。
        {
            auto column = std::make_shared<Column>(0.0f);
            pill = std::make_shared<StatusPill>();
            std::weak_ptr<AiPage> weakPage = weak_from_this();
            auto toggle = BindToggle(context, "ai_assistant.enabled", [weakPage](bool) {
                if (auto page = weakPage.lock())
                    page->SyncLayout();
            });
            auto header = MakeRow(L"启用 AI 联想", L"关闭后不再向后端请求任何联想候选，已保存的配置保持不变。", toggle);
            header->SetMargin({0.0f, 0.0f, 0.0f, 14.0f});
            column->Add(header);
            auto pillRow = std::make_shared<Row>(10.0f);
            pillRow->SetAlign(Row::Align::Center);
            pillRow->Add(pill);
            column->Add(pillRow);

            TextStyle caption;
            caption.size = kFontCaption;
            statusText = std::make_shared<Label>(L"", caption, TextRole::Muted);
            statusText->SetMargin({0.0f, 10.0f, 0.0f, 0.0f});
            column->Add(statusText);

            auto privacy = MakeDescription(
                L"开启后，每次输入完整拼音都会把拼音、本焦点会话最近上屏的文本（最多 1024 字节）和候选数量发给你配置"
                L"的服务商，对方按自己的隐私条款处理。像密码、密钥的连续英文数字串会被剔除；中文正文原样发送。"
                L"宿主标记为敏感字段的输入框不发送。关闭总开关即可停止。详见 PRIVACY.md。");
            privacy->SetMargin({0.0f, 10.0f, 0.0f, 0.0f});
            column->Add(privacy);
            page.AddSection(column);
        }

        // 联想模式：生成式与从候选里挑是两种接入方式，属于同一个页面的两个层次。
        {
            auto column = std::make_shared<Column>(0.0f);
            auto heading = MakeTitle(L"联想模式");
            heading->SetMargin({0.0f, 0.0f, 0.0f, 16.0f});
            column->Add(heading);
            modeGroup = std::make_shared<RadioGroup>();
            auto generate = modeGroup->Add(L"生成式", "generate");
            generate->SetInnerPadding({4.0f, 8.0f, 4.0f, 8.0f});
            auto select = modeGroup->Add(L"从候选里挑", "select");
            select->SetInnerPadding({4.0f, 8.0f, 4.0f, 8.0f});
            modeGroup->SetValue("generate");
            modeGroup->SetOnChanged([this](const std::string &value) { SwitchMode(value); });
            auto group = std::make_shared<Column>(2.0f);
            group->Add(generate);
            group->Add(select);
            auto row = MakeRow(L"联想模式",
                               L"生成式让模型按拼音和前文写出新候选；从候选里挑只让服务商在本地候选里挑一个。", group);
            column->Add(row);
            page.AddSection(column);
        }

        page.AddSection(BuildProviderSection());

        // 提示词：跟服务商配置分开，只在生成式且已启用时才有意义。
        {
            auto column = std::make_shared<Column>(0.0f);
            auto heading = MakeTitle(L"自定义提示词");
            heading->SetMargin({0.0f, 0.0f, 0.0f, 16.0f});
            column->Add(heading);
            Dropdown::Options slotOptions;
            slotOptions.fixedWidth = 168.0f;
            slotOptions.weight = DWRITE_FONT_WEIGHT_MEDIUM;
            promptSlot = std::make_shared<Dropdown>(
                Options({{"custom_1", L"自定义一"}, {"custom_2", L"自定义二"}, {"custom_3", L"自定义三"}}),
                slotOptions);
            promptSlot->SetOnChanged([this](size_t, const DropdownOption &option) {
                Write("ai_assistant.prompt_id", option.value);
                SwitchPrompt(option.value);
            });
            auto header = std::make_shared<Field>(L"提示词槽位", promptSlot);
            header->SetWidth(168.0f);
            header->SetHorizontalAlignment(msimeui::HorizontalAlignment::Leading);
            header->SetMargin({0.0f, 0.0f, 0.0f, 16.0f});
            column->Add(header);
            InputBox::Options promptOptions;
            TextStyle font = BodyStyle();
            font.size = kFontDefault;
            promptOptions.font = font;
            promptOptions.multiline = true;
            promptOptions.height = 280.0f;
            promptOptions.lineHeight = 1.65f;
            promptOptions.scrollbar = InputBox::Options::Scrollbar::Thin;
            prompt = std::make_shared<InputBox>(promptOptions);
            prompt->SetMargin({0.0f, 0.0f, 0.0f, 4.5f});
            prompt->SetOnCommit([this](const std::wstring &text) {
                customPrompts[promptId] = Narrow(text);
                Write("ai_assistant.prompt_" + promptId, customPrompts[promptId]);
            });
            column->Add(prompt);
            auto hint = MakeDescription(
                L"三个槽位各存一份提示词，切换只改当前生效的那一个。自定义一为空时沿用旧版的提示词内容。");
            hint->SetMargin({0.0f, 6.0f, 0.0f, 0.0f});
            column->Add(hint);
            promptSection = std::make_shared<Hideable>(column);
        }
        page.AddRaw(promptSection, 18.0f);

        SyncLayout();
        context.OnConfig([weak = weak_from_this()]() {
            if (auto page = weak.lock())
                page->Sync();
        });
        return page.Build();
    }
};

std::vector<std::shared_ptr<AiPage>> &KeepAlive()
{
    static std::vector<std::shared_ptr<AiPage>> pages;
    return pages;
}
} // namespace

VisualPtr BuildAiPage(PageContext &context)
{
    auto page = std::make_shared<AiPage>(context);
    KeepAlive().push_back(page);
    return page->Build();
}
} // namespace native_settings
