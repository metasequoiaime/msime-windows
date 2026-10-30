#pragma once

#include "settings/native/native_settings_pages.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Pieces shared by the voice input and AI assistant pages: the `.voice-grid` /
// `.ai-grid` form (labelled fields in two columns), the sunken inputs with their reveal
// toggle, the small secondary buttons and the credential test row.
namespace native_settings::voice_fields
{
// Font of a grid field: `font: inherit` from the 12px / 500 field label.
TextStyle FieldTextStyle(float size = 12.0f, bool medium = true);
// `Consolas, "Cascadia Mono", monospace` of the token inputs.
TextStyle MonospaceStyle(float size = 12.0f);
// `letter-spacing: -0.1ch` of the token inputs; both monospace faces advance 0.55em.
inline float MonospaceTokenSpacing(float size = 12.0f)
{
    return -0.1f * 0.55f * size;
}

// Sunken input (`.voice-grid input`, `.ai-grid input`, `.voice-prompt`, `.ai-prompt`):
// --surface-sunken fill, --control-border, 6px radius; focused it turns --control-bg with
// an accent border and a 2px accent ring. Wraps msimeui's TSF text box.
class InputBox : public msimeui::Visual
{
  public:
    struct Options
    {
        TextStyle font = FieldTextStyle();
        // Border-box height; 0 derives it from the line height and the padding.
        float height = 0.0f;
        bool multiline = false;
        bool password = false;
        // `.voice-secret-toggle` eye button on the right.
        bool reveal = false;
        float paddingX = 12.0f;
        float paddingY = 10.0f;
        float radius = 6.0f;
        // CSS letter-spacing in DIPs.
        float letterSpacing = 0.0f;
        // Multi-line: CSS line-height as a multiple of the font size (0 = normal).
        float lineHeight = 0.0f;
        // Multi-line scrollbar while the text overflows: Chrome's default Fluent bar
        // (15px, arrow buttons) or the page's 8px `::-webkit-scrollbar` (.ai-prompt).
        enum class Scrollbar
        {
            Fluent,
            Thin,
        };
        Scrollbar scrollbar = Scrollbar::Fluent;
        // `resize: vertical`: a corner grip drags the height, never below `height`.
        bool resizable = false;
        // No own chrome: an input embedded in a combobox.
        bool bare = false;
        std::wstring placeholder;
    };

    explicit InputBox(Options options);

    void SetText(const std::wstring &text);
    std::wstring GetText() const;
    void SetPlaceholder(const std::wstring &placeholder);
    // Focus left or Enter pressed after an edit: the page's `change` event.
    void SetOnCommit(std::function<void(const std::wstring &)> handler);
    // User edits only; programmatic SetText calls do not trigger this handler.
    void SetOnEdited(std::function<void(const std::wstring &)> handler);
    bool IsFocused() const;
    void Focus();

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    void Attach(msimeui::Window *window) override;
    msimeui::Visual *FindVisualAt(const PointF &point) override;
    msimeui::Visual *FindFocusableAt(const PointF &point) override;
    msimeui::Visual *FindFirstFocusableDescendant() override;

  private:
    class Reveal;
    class Scroller;
    void ApplyAppearance();
    float Height() const;
    RectF RevealRect() const;

    Options options_;
    std::shared_ptr<msimeui::TextBox> box_;
    std::shared_ptr<Reveal> reveal_;
    std::shared_ptr<Scroller> scroller_;
    // Height the user dragged the resize grip to; 0 keeps options_.height.
    float resizedHeight_ = 0.0f;
    std::wstring committed_;
    std::function<void(const std::wstring &)> onCommit_;
    bool revealed_ = false;
    bool appliedLight_ = false;
    bool appearanceApplied_ = false;
};

// `display: flex; flex-direction: column; gap: 7px` field with its 12px / 500 label.
class Field : public Column
{
  public:
    Field(std::wstring label, VisualPtr control);
    void SetLabel(std::wstring label);

  private:
    std::shared_ptr<Label> label_;
};

// `.voice-grid` / `.ai-grid`: two equal columns, 16px gaps, auto placement; wide items
// span both columns and hidden (Hideable) items leave the flow.
class FormGrid : public msimeui::Panel
{
  public:
    explicit FormGrid(float gap = 16.0f);
    FormGrid &Add(VisualPtr child, bool wide = false);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;

  private:
    struct Placement
    {
        size_t row = 0;
        size_t column = 0;
        bool wide = false;
        bool visible = true;
    };
    void Place(float width);

    float gap_ = 16.0f;
    std::vector<bool> wide_;
    std::vector<Placement> placements_;
    std::vector<float> rowHeights_;
};

// A plain HTML button whose every property comes from its CSS rule.
class PlainButton : public msimeui::Visual
{
  public:
    struct Style
    {
        TextStyle font;
        float paddingX = 10.0f;
        float paddingY = 4.0f;
        float minWidth = 0.0f;
        // Border-box height; 0 derives it from the line height (lineHeight in font).
        float height = 0.0f;
        float radius = 5.0f;
        D2D1_COLOR_F (*fill)(const Palette &) = nullptr;
        D2D1_COLOR_F (*hoverFill)(const Palette &) = nullptr;
        D2D1_COLOR_F (*border)(const Palette &) = nullptr;
        D2D1_COLOR_F (*text)(const Palette &) = nullptr;
        D2D1_COLOR_F (*hoverText)(const Palette &) = nullptr;
        float disabledOpacity = 0.65f;
    };

    PlainButton(std::wstring text, Style style);
    void SetText(std::wstring text);
    const std::wstring &Text() const
    {
        return text_;
    }
    void SetDisabled(bool disabled);
    bool IsDisabled() const
    {
        return disabled_;
    }
    void SetOnClick(std::function<void()> handler);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    void OnMouseEnter() override;
    void OnMouseLeave() override;
    HCURSOR GetCursor() const override;

  private:
    std::wstring text_;
    Style style_;
    bool hovered_ = false;
    bool pressed_ = false;
    bool disabled_ = false;
    std::function<void()> onClick_;
};

// `.credential-test-button` (also `.ai-model-fetch-button` with a different height).
PlainButton::Style CredentialButtonStyle();
// `.ai-model-fetch-button`.
PlainButton::Style ModelFetchButtonStyle();
// `.voice-prompt-reset`.
PlainButton::Style PromptResetButtonStyle();

// `.credential-test-status` / `.ai-model-fetch-status`: 12px / 1.45, muted, green on
// success, red on error, display: none while empty.
class StatusText : public Hideable
{
  public:
    StatusText();
    void Set(const std::wstring &text, const std::string &kind);
    void Clear();
    const std::wstring &GetText() const
    {
        return text_;
    }
    const std::string &GetKind() const
    {
        return kind_;
    }
    void SetDisplayPrefix(const std::wstring &prefix);
    void SetOnChanged(std::function<void()> handler);

  private:
    std::shared_ptr<Label> label_;
    std::wstring text_;
    std::string kind_;
    std::wstring prefix_;
    std::function<void()> onChanged_;
};

// Runs `callback` once after `ms` on the UI thread; returns an id for CancelTimeout.
UINT_PTR SetTimeout(UINT ms, std::function<void()> callback);
void CancelTimeout(UINT_PTR id);

// `setupCredentialTest`: posts apiCredentialTest, shows 测试中…, the result, a 30s
// timeout and 配置已变更 when the settings changed while the test ran.
class CredentialTest
{
  public:
    CredentialTest(PageContext &context, std::shared_ptr<PlainButton> button, std::shared_ptr<StatusText> status,
                   std::function<std::string()> service, std::function<Json()> readConfig);
    // Invalidate UI callbacks and the timeout without cancelling the host's network task.
    void Reset();

  private:
    void Start();
    void Finish();

    PageContext *context_ = nullptr;
    std::shared_ptr<PlainButton> button_;
    std::shared_ptr<StatusText> status_;
    std::function<std::string()> service_;
    std::function<Json()> readConfig_;
    std::wstring idleLabel_;
    std::string pendingId_;
    std::string pendingService_;
    Json pendingSnapshot_;
    UINT_PTR timer_ = 0;
};

// Text + rotating chevron toggle without the section-title style (`.voice-sound-expand`).
class PlainExpander : public msimeui::Visual
{
  public:
    PlainExpander(std::wstring text, TextStyle style);
    void SetOnToggled(std::function<void(bool expanded)> handler);
    bool IsExpanded() const
    {
        return expanded_;
    }

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  private:
    TextLayout text_;
    bool expanded_ = false;
    bool pressed_ = false;
    msimeui::Tween rotation_{0.0f};
    std::function<void(bool)> onToggled_;
};

// Shows `child` with a minimum height (`min-height` of a flex row).
class MinHeight : public msimeui::Container
{
  public:
    MinHeight(VisualPtr child, float minHeight, Thickness padding = {});
    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;

  private:
    float minHeight_ = 0.0f;
};

// Snapshot helpers.
std::string JsonString(const Json *value);
// Provider -> secret map from `<base>s` (object), the legacy `<base>` value and the
// `<base>_<provider>` slots, like readTokenMap.
std::map<std::string, std::string> ReadProviderMap(const SettingsModel &model, const std::string &section,
                                                   const std::string &base, const std::string &provider,
                                                   const std::vector<std::string> &providers, bool slots);
std::wstring Trim(const std::wstring &text);

// AI 联想模式。`jev` 把拼音、前文和本地候选交给 TypeSafe Choice 接口，从已有候选里挑一个；
// 其余服务商走 OpenAI 兼容的 Chat Completions，自己生成候选文本。
std::string AiModeOf(const std::string &provider);
std::string AiProviderForMode(const std::string &mode, const std::string &provider);
// 候选词翻译复用的服务商：选择器模式（jev）下取生成式上次用的那家，其余情况就是 provider 本身。
std::string AiTranslationProvider(const SettingsModel &model, const std::string &provider);
// 出厂默认的接口地址与模型；未知服务商返回空串。空槽位在界面上显示它作占位符。
std::wstring AiDefaultEndpoint(const std::string &provider);
std::wstring AiDefaultModel(const std::string &provider);
// 内置服务商的显示名；用户自建的服务商读注册表里的 name，缺失时退回 id。
std::wstring AiProviderLabel(const std::string &provider, const Json *providers = nullptr);
// 名称与 id 的候选列表，供服务商下拉框使用。
std::vector<std::pair<std::string, std::wstring>> AiProviderChoices(const Json *providers, const std::string &mode);

// 当前服务商在这次快照里的生效配置。分槽优先（`tokens` / `endpoints` / `models`），
// 单值键只在它就是当前服务商时作数，最后回退到出厂默认值。
struct ActiveAiConfig
{
    std::string provider;
    std::string endpoint;
    std::string model;
    std::string token;
    bool tokenSet = false;
    bool usable = false; // 与后端 AiTranslation::IsUsableConfig 同一口径
};
ActiveAiConfig ReadActiveAiConfig(const SettingsModel &model);
bool AiProviderIsJev(const std::string &provider);
} // namespace native_settings::voice_fields
