#include "settings/native/native_settings_pages.h"

#include "config/candidate_panel_move_key.h"
#include "engine/contracts/candidate_action_key.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Window.h"

#include <algorithm>
#include <map>
#include <regex>

// partials/shortcut.html + modules/shortcut.ts + modules/shortcut-recorder.ts +
// styles/modules/shortcut.css
namespace native_settings
{
namespace
{
// ---------------------------------------------------------------------------------
// Candidate translation expand-key recorder (moved from the input page)
// ---------------------------------------------------------------------------------
std::string KeyCodeForBinding(UINT vk)
{
    if (vk >= 'A' && vk <= 'Z')
        return std::string("Key") + static_cast<char>(vk);
    if (vk >= '0' && vk <= '9')
        return std::string("Digit") + static_cast<char>(vk);
    if (vk >= VK_F1 && vk <= VK_F9)
        return std::string("F") + static_cast<char>('1' + (vk - VK_F1));
    if (vk >= VK_F10 && vk <= VK_F12)
        return std::string("F1") + static_cast<char>('0' + (vk - VK_F10));
    for (const auto &entry : FanyImeKeyChord::NamedKeys)
        if (entry.key == vk)
            return std::string(entry.name);
    return {};
}

std::wstring WideFromAscii(std::string_view text)
{
    std::wstring wide;
    for (const char ch : text)
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    return wide;
}

std::wstring ExpandKeyLabel(const std::string &binding)
{
    if (binding.empty())
        return L"未绑定（功能关闭）";
    FanyImeKeyChord::Binding parsed;
    if (!FanyImeCandidateActionKey::Parse(binding, parsed) || parsed.key == 0)
        return L"无效配置";
    std::wstring label;
    if (parsed.modifiers & FanyImeKeyChord::Control)
        label += L"Ctrl + ";
    if (parsed.modifiers & FanyImeKeyChord::Shift)
        label += L"Shift + ";
    if (parsed.key == 0xc0)
        label += L"`";
    else if ((parsed.key >= 'A' && parsed.key <= 'Z') || (parsed.key >= '0' && parsed.key <= '9'))
        label += static_cast<wchar_t>(parsed.key);
    else if (parsed.key >= VK_F1 && parsed.key <= VK_F12)
        label += L"F" + std::to_wstring(parsed.key - VK_F1 + 1);
    else
    {
        for (const auto &entry : FanyImeKeyChord::NamedKeys)
            if (entry.key == parsed.key)
                label += WideFromAscii(entry.label);
    }
    return label;
}

// `.shortcut-actions button`-styled small button; the record one also captures keys while recording.
class ExpandKeyButton : public msimeui::Visual
{
  public:
    explicit ExpandKeyButton(std::wstring text) : text_(std::move(text))
    {
    }

    void SetText(std::wstring text)
    {
        if (text == text_)
            return;
        text_ = std::move(text);
        InvalidateMeasure();
        InvalidateVisual();
    }

    void SetDisabled(bool disabled)
    {
        disabled_ = disabled;
    }

    void SetOnClick(std::function<void()> handler)
    {
        onClick_ = std::move(handler);
    }

    void SetOnKey(std::function<bool(WPARAM, LPARAM)> handler)
    {
        onKey_ = std::move(handler);
    }

    void SetOnBlur(std::function<void()> handler)
    {
        onBlur_ = std::move(handler);
    }

    SizeF Measure(const SizeF &) override
    {
        const float border = CssBorder();
        const TextStyle style = CaptionStyle();
        return {MeasureTextWidth(text_, style) + 20.0f + border * 2.0f, LineHeight(style) + 12.0f + border * 2.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        FillRounded(dr, bounds_, 6.0f, p.controlBg);
        StrokeRounded(dr, bounds_, 6.0f, p.border, 1.0f);
        DrawTextInRect(dr, text_, CaptionStyle(), bounds_, p.text, true);
    }

    bool IsFocusable() const override
    {
        return !disabled_;
    }

    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        pressed_ = !disabled_ && window_ && HitTest(window_->ClientPixelsToDips(point));
        return pressed_;
    }

    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        const bool clicked = pressed_ && window_ && HitTest(window_->ClientPixelsToDips(point));
        pressed_ = false;
        if (clicked && onClick_ && !disabled_)
        {
            // A handler may tear down the visual that owns onClick_ while it runs.
            const auto handler = onClick_;
            handler();
        }
        return true;
    }

    bool OnKeyDown(WPARAM key, LPARAM lParam) override
    {
        if (!onKey_)
            return false;
        const auto handler = onKey_;
        return handler(key, lParam);
    }

    void OnFocusChanged(bool focused) override
    {
        if (!focused && onBlur_)
        {
            const auto handler = onBlur_;
            handler();
        }
    }

    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, disabled_ ? IDC_ARROW : IDC_HAND);
    }

  private:
    static TextStyle CaptionStyle()
    {
        TextStyle style;
        style.size = kFontCaption;
        return style;
    }

    std::wstring text_;
    bool disabled_ = false;
    bool pressed_ = false;
    std::function<void()> onClick_;
    std::function<bool(WPARAM, LPARAM)> onKey_;
    std::function<void()> onBlur_;
};

// The expand-key recorder: bare ` only, or Ctrl[+Shift] + letter/digit/F1-F12/punctuation/Space;
// clearing disables the feature. The binding lives in keybindings.candidate_expansion_key.
class ExpandKeyEditor : public msimeui::Container
{
  public:
    explicit ExpandKeyEditor(SettingsModel *model) : model_(model)
    {
        record_ = std::make_shared<ExpandKeyButton>(L"未绑定（功能关闭）");
        record_->SetOnClick([this] { StartRecording(); });
        record_->SetOnKey([this](WPARAM key, LPARAM lParam) { return OnRecordKey(key, lParam); });
        record_->SetOnBlur([this] { StopRecording(); });
        clear_ = std::make_shared<ExpandKeyButton>(L"清除");
        clear_->SetOnClick([this] { Save(std::string(), L"已清除，展开键功能已关闭。"); });
        reset_ = std::make_shared<ExpandKeyButton>(L"恢复默认");
        reset_->SetOnClick([this] { Save(std::string(FanyImeCandidateActionKey::Default), L"已恢复为默认的 ` 键。"); });

        auto actions = std::make_shared<Row>(6.0f);
        actions->SetWrap(true, 6.0f);
        actions->SetAlign(Row::Align::Start);
        actions->Add(record_);
        actions->Add(clear_);
        actions->Add(reset_);
        TextStyle statusStyle;
        statusStyle.size = kFontCaption;
        statusStyle.wrap = true;
        status_ = std::make_shared<Label>(L"", statusStyle, TextRole::Muted);
        statusBox_ = std::make_shared<Hideable>(status_, false);
        auto column = std::make_shared<Column>(0.0f);
        column->Add(actions);
        column->Add(statusBox_);
        SetChild(column);
    }

    void Sync(const std::string &binding)
    {
        binding_ = binding;
        if (!recording_)
            record_->SetText(ExpandKeyLabel(binding));
    }

    void SetDisabled(bool disabled)
    {
        record_->SetDisabled(disabled);
        clear_->SetDisabled(disabled);
        reset_->SetDisabled(disabled);
    }

  private:
    void StartRecording()
    {
        if (recording_)
            return;
        recording_ = true;
        record_->SetText(L"请按下新按键…");
        SetStatus(L"按 Esc 取消。除 ` 以外的键需要按住 Ctrl，可再加 Shift。");
    }

    void StopRecording()
    {
        if (!recording_)
            return;
        recording_ = false;
        record_->SetText(ExpandKeyLabel(binding_));
    }

    void SetStatus(std::wstring text)
    {
        status_->SetText(std::move(text));
        statusBox_->SetVisible(!text.empty());
    }

    void Save(std::string binding, std::wstring status)
    {
        StopRecording();
        binding_ = binding;
        model_->Update("keybindings.candidate_expansion_key", binding);
        SetStatus(std::move(status));
    }

    bool OnRecordKey(WPARAM vk, LPARAM lParam)
    {
        if (!recording_)
            return false;
        if (vk == VK_ESCAPE || vk == VK_TAB)
        {
            StopRecording();
            SetStatus(L"");
            return vk == VK_ESCAPE;
        }
        if ((lParam & (1L << 30)) != 0 || vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN ||
            vk == VK_RWIN)
            return true;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        const bool win = (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;
        if (alt || win)
        {
            SetStatus(L"不支持 Alt / Win 组合。");
            return true;
        }
        const std::string code = KeyCodeForBinding(static_cast<UINT>(vk));
        if (code.empty())
        {
            SetStatus(L"请选择 `、Ctrl + 字母、数字、F1–F12、标点或 Space。");
            return true;
        }
        std::string binding;
        if (!ctrl && !shift && vk == VK_OEM_3)
            binding = "Backquote";
        else if (ctrl)
            binding = "Ctrl+" + (shift ? std::string("Shift+") : std::string()) + code;
        else
        {
            SetStatus(L"除 ` 以外的键需要按住 Ctrl。");
            return true;
        }
        FanyImeKeyChord::Binding parsed;
        if (!FanyImeCandidateActionKey::Parse(binding, parsed))
        {
            SetStatus(L"这个键不能用作展开键。");
            return true;
        }
        Save(binding, L"已保存。");
        return true;
    }

    SettingsModel *model_ = nullptr;
    std::string binding_;
    bool recording_ = false;
    std::shared_ptr<ExpandKeyButton> record_, clear_, reset_;
    std::shared_ptr<Label> status_;
    std::shared_ptr<Hideable> statusBox_;
};

// The panel move-key recorders: bare arrow keys / letters / digits / F1-F12 / punctuation /
// Space, optionally with Ctrl; clearing unbinds that direction. Bindings live in
// keybindings.candidate_expansion_move_prev_key and ..._next_key.
std::string MoveKeyCodeForBinding(UINT vk)
{
    switch (vk)
    {
    case VK_UP:
        return "Up";
    case VK_DOWN:
        return "Down";
    case VK_LEFT:
        return "Left";
    case VK_RIGHT:
        return "Right";
    default:
        return KeyCodeForBinding(vk);
    }
}

std::wstring MoveKeyLabel(const std::string &binding)
{
    if (binding.empty())
        return L"未绑定（不移动）";
    FanyImeKeyChord::Binding parsed;
    if (!CandidatePanelMoveKey::Parse(binding, parsed) || parsed.key == 0)
        return L"无效配置";
    std::wstring label;
    if (parsed.modifiers & FanyImeKeyChord::Control)
        label += L"Ctrl + ";
    switch (parsed.key)
    {
    case 0x26:
        return label + L"↑";
    case 0x28:
        return label + L"↓";
    case 0x27:
        return label + L"←";
    case 0x29:
        return label + L"→";
    default:
        break;
    }
    if ((parsed.key >= 'A' && parsed.key <= 'Z') || (parsed.key >= '0' && parsed.key <= '9'))
        label += static_cast<wchar_t>(parsed.key);
    else if (parsed.key >= VK_F1 && parsed.key <= VK_F12)
        label += L"F" + std::to_wstring(parsed.key - VK_F1 + 1);
    else
    {
        for (const auto &entry : FanyImeKeyChord::NamedKeys)
            if (entry.key == parsed.key)
                label += WideFromAscii(entry.label);
    }
    return label;
}

class MoveKeyEditor : public msimeui::Container
{
  public:
    MoveKeyEditor(SettingsModel *model, std::string path, std::string fallback)
        : model_(model), path_(std::move(path)), fallback_(std::move(fallback))
    {
        record_ = std::make_shared<ExpandKeyButton>(L"未绑定（不移动）");
        record_->SetOnClick([this] { StartRecording(); });
        record_->SetOnKey([this](WPARAM key, LPARAM lParam) { return OnRecordKey(key, lParam); });
        record_->SetOnBlur([this] { StopRecording(); });
        clear_ = std::make_shared<ExpandKeyButton>(L"清除");
        clear_->SetOnClick([this] { Save(std::string(), L"已清除，该方向不再移动展开候选。"); });
        reset_ = std::make_shared<ExpandKeyButton>(L"恢复默认");
        reset_->SetOnClick([this] { Save(fallback_, L"已恢复默认方向键。"); });

        auto actions = std::make_shared<Row>(6.0f);
        actions->SetWrap(true, 6.0f);
        actions->SetAlign(Row::Align::Start);
        actions->Add(record_);
        actions->Add(clear_);
        actions->Add(reset_);
        TextStyle statusStyle;
        statusStyle.size = kFontCaption;
        statusStyle.wrap = true;
        status_ = std::make_shared<Label>(L"", statusStyle, TextRole::Muted);
        statusBox_ = std::make_shared<Hideable>(status_, false);
        auto column = std::make_shared<Column>(0.0f);
        column->Add(actions);
        column->Add(statusBox_);
        SetChild(column);
    }

    void Sync(const std::string &binding)
    {
        binding_ = binding;
        if (!recording_)
            record_->SetText(MoveKeyLabel(binding));
    }

  private:
    void StartRecording()
    {
        if (recording_)
            return;
        recording_ = true;
        record_->SetText(L"请按下新按键…");
        SetStatus(L"按 Esc 取消。方向键、字母、数字、F1–F12、标点或 Space 可直接按，也可按住 Ctrl。");
    }

    void StopRecording()
    {
        if (!recording_)
            return;
        recording_ = false;
        record_->SetText(MoveKeyLabel(binding_));
    }

    void SetStatus(std::wstring text)
    {
        status_->SetText(std::move(text));
        statusBox_->SetVisible(!text.empty());
    }

    void Save(std::string binding, std::wstring status)
    {
        StopRecording();
        binding_ = binding;
        model_->Update(path_, binding);
        SetStatus(std::move(status));
    }

    bool OnRecordKey(WPARAM vk, LPARAM lParam)
    {
        if (!recording_)
            return false;
        if (vk == VK_ESCAPE || vk == VK_TAB)
        {
            StopRecording();
            SetStatus(L"");
            return vk == VK_ESCAPE;
        }
        if ((lParam & (1L << 30)) != 0 || vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN ||
            vk == VK_RWIN)
            return true;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        const bool win = (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;
        if (alt || win)
        {
            SetStatus(L"不支持 Alt / Win 组合。");
            return true;
        }
        const std::string code = MoveKeyCodeForBinding(static_cast<UINT>(vk));
        if (code.empty())
        {
            SetStatus(L"请选择方向键、字母、数字、F1–F12、标点或 Space。");
            return true;
        }
        const std::string binding = ctrl ? "Ctrl+" + code : code;
        FanyImeKeyChord::Binding parsed;
        if (!CandidatePanelMoveKey::Parse(binding, parsed))
        {
            SetStatus(L"这个键不能用作移动键。");
            return true;
        }
        Save(binding, L"已保存。");
        return true;
    }

    SettingsModel *model_ = nullptr;
    std::string path_;
    std::string fallback_;
    std::string binding_;
    bool recording_ = false;
    std::shared_ptr<ExpandKeyButton> record_, clear_, reset_;
    std::shared_ptr<Label> status_;
    std::shared_ptr<Hideable> statusBox_;
};

TextStyle Font(float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL, float lineHeight = 0.0f,
               bool wrap = false)
{
    TextStyle style;
    style.size = size;
    style.weight = weight;
    style.lineHeight = lineHeight;
    style.wrap = wrap;
    return style;
}

// `.section.shortcut-section { padding: 0; overflow: hidden }`: the card without the
// standard 20px 24px padding; rows draw edge to edge.
class FlatSection : public msimeui::Container
{
  public:
    explicit FlatSection(VisualPtr child) : msimeui::Container(std::move(child))
    {
        SetMargin({0.0f, 0.0f, 0.0f, 18.0f});
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        const float border = CssBorder();
        padding_ = {border, border, border, border};
        return msimeui::Container::Measure(availableSize);
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        constexpr float kRadius = 8.0f;
        msimeui::ShadowPass passes[2] = {};
        for (int i = 0; i < p.cardShadowCount; ++i)
            passes[i] = {p.cardShadow[i].sigma, p.cardShadow[i].alpha, 0.0f, p.cardShadow[i].offsetY};
        shadow_.Render(dr.GetRenderTarget(), bounds_, kRadius, passes, static_cast<size_t>(p.cardShadowCount));
        FillRounded(dr, bounds_, kRadius, Over(p.cardBg, p.chromeBg));
        msimeui::Container::Render(dr);
        // The border paints over the rows' top borders at the card edge.
        StrokeRounded(dr, bounds_, kRadius, p.border, 1.0f);
    }

  private:
    msimeui::ShadowCache shadow_;
};

// A block with a 1px top border in --border-color (`.shortcut-list`, `.shortcut-row + .shortcut-row`).
class TopRule : public msimeui::Container
{
  public:
    explicit TopRule(VisualPtr child, bool rule = true) : msimeui::Container(std::move(child)), rule_(rule)
    {
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        padding_ = {0.0f, rule_ ? CssBorder() : 0.0f, 0.0f, 0.0f};
        return msimeui::Container::Measure(availableSize);
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        if (rule_)
            FillRect(dr, {bounds_.x, bounds_.y, bounds_.width, CssBorder()}, CurrentPalette().border);
        msimeui::Container::Render(dr);
    }

  private:
    bool rule_ = true;
};

// `.shortcut-row`: label on the left, keys or an editor on the right, `gap: 24px`,
// `padding: 10px 22px`, `min-height: 56px`. Both sides start at their max-content
// width and shrink in proportion to it when they do not fit (flex-shrink: 1); the
// editor does not shrink (`flex-shrink: 0`).
class ShortcutRow : public msimeui::Panel
{
  public:
    ShortcutRow(VisualPtr left, VisualPtr right, bool rightShrinks)
        : left_(left), right_(right), rightShrinks_(rightShrinks)
    {
        AddChild(left_);
        AddChild(right_);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        const float inner = std::max(availableSize.width - 44.0f, 0.0f);
        const float leftMax = left_->MeasureInLayout({inner, availableSize.height}).width;
        const float rightMax = right_->MeasureInLayout({inner, availableSize.height}).width;
        float leftWidth = leftMax;
        float rightWidth = rightMax;
        const float overflow = leftMax + rightMax + kGap - inner;
        if (overflow > 0.0f)
        {
            if (rightShrinks_ && leftMax + rightMax > 0.0f)
            {
                leftWidth = leftMax - overflow * leftMax / (leftMax + rightMax);
                rightWidth = rightMax - overflow * rightMax / (leftMax + rightMax);
            }
            else
            {
                leftWidth = std::max(inner - rightMax - kGap, 0.0f);
            }
        }
        leftSize_ = left_->MeasureInLayout({std::max(leftWidth, 0.0f), availableSize.height});
        rightSize_ = right_->MeasureInLayout({std::max(rightWidth, 0.0f), availableSize.height});
        leftSize_.width = std::max(leftWidth, 0.0f);
        rightSize_.width = std::max(rightWidth, 0.0f);
        // min-height: 56px is the border box, which includes the 1px top border the row
        // gets after the first one (TopRule draws it outside this visual).
        const float minHeight = 56.0f - (topBorder_ ? CssBorder() : 0.0f);
        return {availableSize.width, std::max(minHeight, std::max(leftSize_.height, rightSize_.height) + 20.0f)};
    }

    void SetTopBorder(bool topBorder)
    {
        topBorder_ = topBorder;
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        const float centerY = finalRect.y + finalRect.height * 0.5f;
        left_->ArrangeInLayout(
            {finalRect.x + 22.0f, centerY - leftSize_.height * 0.5f, leftSize_.width, leftSize_.height});
        right_->ArrangeInLayout({finalRect.x + finalRect.width - 22.0f - rightSize_.width,
                                 centerY - rightSize_.height * 0.5f, rightSize_.width, rightSize_.height});
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        left_->Render(dr);
        right_->Render(dr);
    }

  private:
    static constexpr float kGap = 24.0f;
    VisualPtr left_;
    VisualPtr right_;
    bool rightShrinks_ = true;
    bool topBorder_ = false;
    SizeF leftSize_ = {};
    SizeF rightSize_ = {};
};

// A `.shortcut-row` in a list; rows after the first carry the 1px top border.
VisualPtr RuledRow(VisualPtr left, VisualPtr right, bool rightShrinks, bool rule)
{
    auto row = std::make_shared<ShortcutRow>(std::move(left), std::move(right), rightShrinks);
    row->SetTopBorder(rule);
    return std::make_shared<TopRule>(row, rule);
}

// <kbd> in `.shortcut-keys`: 12px/1.25 Segoe UI Variable, padding 4px 8px, 1px border
// with a darker bottom, 5px radius, `box-shadow: 0 1px 0 var(--kbd-shadow)`.
class Kbd : public msimeui::Visual
{
  public:
    explicit Kbd(std::wstring text) : text_(std::move(text))
    {
    }

    SizeF Measure(const SizeF &) override
    {
        const float border = CssBorder();
        return {std::max(28.0f, MeasureTextWidth(text_, Style()) + 16.0f + border * 2.0f),
                15.0f + 8.0f + border * 2.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        const float border = CssBorder();
        const RectF box = bounds_;
        FillRounded(dr, {box.x, box.y + 1.0f, box.width, box.height}, 5.0f, p.kbdShadow);
        FillRounded(dr, box, 5.0f, p.kbdBorderBottom);
        FillRounded(dr, {box.x, box.y, box.width, box.height - border}, 5.0f, p.kbdBorder);
        FillRounded(dr, {box.x + border, box.y + border, box.width - border * 2.0f, box.height - border * 2.0f},
                    5.0f - border, p.kbdBg);
        DrawTextInRect(dr, text_, Style(), box, p.kbdText, true);
    }

  private:
    static TextStyle Style()
    {
        static const std::wstring family = IsFontInstalled(L"Segoe UI Variable")        ? L"Segoe UI Variable"
                                           : IsFontInstalled(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text"
                                                                                        : L"Segoe UI";
        TextStyle style = Font(12.0f, DWRITE_FONT_WEIGHT_NORMAL, 1.25f);
        style.family = family;
        return style;
    }

    std::wstring text_;
};

// `.shortcut-keys`: caps and separators, `gap: 5px`, right-aligned, wrapping. Tokens
// starting with '|' are <i> separators, with '~' <em> ones (3px side margins, muted).
VisualPtr Keys(std::initializer_list<const wchar_t *> tokens)
{
    auto row = std::make_shared<Row>(5.0f);
    row->SetWrap(true, 5.0f);
    row->SetJustify(Row::Justify::End);
    for (const wchar_t *token : tokens)
    {
        if (token[0] == L'|' || token[0] == L'~')
        {
            const bool em = token[0] == L'~';
            auto label = std::make_shared<Label>(token + 1, Font(11.0f), em ? TextRole::Muted : TextRole::Subtle);
            if (em)
                label->SetMargin({3.0f, 0.0f, 3.0f, 0.0f});
            row->Add(label);
        }
        else
        {
            row->Add(std::make_shared<Kbd>(token));
        }
    }
    return row;
}

// A shortcut's name; indexed for search like MakeTitle.
std::shared_ptr<Label> RowLabel(std::wstring text, TextRole role = TextRole::Primary)
{
    auto label = std::make_shared<Label>(text, Font(kFontBody, DWRITE_FONT_WEIGHT_NORMAL, 0.0f, true), role);
    IndexSettingTitle(label, std::move(text));
    return label;
}

// `.shortcut-section-heading`: title + description, optional badge, `padding: 18px 22px`.
VisualPtr Heading(std::wstring title, std::wstring description, VisualPtr badge = nullptr)
{
    auto row = std::make_shared<Row>(16.0f);
    row->SetPadding({22.0f, 18.0f, 22.0f, 18.0f});
    row->SetJustify(Row::Justify::SpaceBetween);
    auto text = std::make_shared<Column>(0.0f);
    if (!title.empty())
        text->Add(MakeTitle(title, description));
    if (!description.empty())
    {
        auto desc = MakeDescription(std::move(description));
        if (text->GetBounds().width == 0.0f && title.empty())
            desc->SetMargin({0.0f, 4.0f, 0.0f, 0.0f});
        text->Add(desc);
    }
    row->Add(text, 1.0f);
    if (badge)
        row->Add(badge);
    return row;
}

// `.shortcut-badge`
class Badge : public msimeui::Visual
{
  public:
    explicit Badge(std::wstring text) : text_(std::move(text))
    {
    }

    SizeF Measure(const SizeF &) override
    {
        const float border = CssBorder();
        return {MeasureTextWidth(text_, Font(11.0f)) + 18.0f + border * 2.0f,
                LineHeight(Font(11.0f)) + 6.0f + border * 2.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        FillRounded(dr, bounds_, bounds_.height * 0.5f, p.accentSoft);
        StrokeRounded(dr, bounds_, bounds_.height * 0.5f, p.accentSoftBorder, 1.0f);
        DrawTextInRect(dr, text_, Font(11.0f), bounds_, p.accentStrong, true);
    }

  private:
    std::wstring text_;
};

// `.shortcut-section .content-page-option`: margin-top 8px, padding 0 24px 4px.
VisualPtr Options(const std::vector<VisualPtr> &checks)
{
    auto column = std::make_shared<Column>(0.0f);
    column->SetPadding({24.0f, 0.0f, 24.0f, 4.0f});
    column->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
    for (size_t i = 0; i < checks.size(); ++i)
    {
        if (i > 0)
            column->Add(std::make_shared<Divider>(Divider::Kind::Option));
        column->Add(checks[i]);
    }
    return column;
}

// `.shortcut-system-guide`: an ordered list of steps in a subtle box.
class SystemGuide : public msimeui::Container
{
  public:
    SystemGuide()
    {
        auto column = std::make_shared<Column>(0.0f);
        auto title = std::make_shared<Label>(L"修改或关闭 Ctrl+Space（系统）",
                                             Font(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1.7f, true));
        title->SetMargin({0.0f, 0.0f, 0.0f, 4.0f});
        IndexSettingTitle(title, L"修改或关闭 Ctrl+Space（系统）");
        column->Add(title);
        column->Add(std::make_shared<Label>(L"请前往 Windows 系统设置修改“输入法 / 非输入法切换”的按键顺序：", Body(),
                                            TextRole::Secondary));
        const wchar_t *steps[] = {L"打开“设置”，进入“时间和语言” → “输入”。", L"选择“高级键盘设置” → “输入语言热键”。",
                                  L"选中“中文（简体）输入法 - 输入法 / 非输入法切换”，点击“更改按键顺序”。",
                                  L"关闭该按键顺序，或将 Ctrl+Space 改为其他不常用组合。"};
        auto list = std::make_shared<Column>(0.0f);
        // ol { margin: 9px 0 8px; padding-left: 22px }, li + li { margin-top: 3px }
        list->SetMargin({0.0f, 9.0f, 0.0f, 8.0f});
        for (size_t i = 0; i < std::size(steps); ++i)
        {
            auto item = std::make_shared<ListItem>(static_cast<int>(i + 1), steps[i]);
            if (i > 0)
                item->SetMargin({0.0f, 3.0f, 0.0f, 0.0f});
            list->Add(item);
        }
        column->Add(list);
        column->Add(std::make_shared<Label>(
            L"不同 Windows 版本的选项名称可能略有差异；修改后如未立即生效，请重新登录或重启电脑。", Body(),
            TextRole::Muted));
        SetChild(column);
        // margin: 16px 22px 20px
        SetMargin({22.0f, 16.0f, 22.0f, 20.0f});
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        // padding: 16px 18px inside the 1px border
        const float border = CssBorder();
        padding_ = {18.0f + border, 16.0f + border, 18.0f + border, 16.0f + border};
        return msimeui::Container::Measure(availableSize);
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        FillRounded(dr, bounds_, 8.0f, p.surfaceSubtle);
        StrokeRounded(dr, bounds_, 8.0f, p.border, 1.0f);
        msimeui::Container::Render(dr);
    }

  private:
    static TextStyle Body()
    {
        return Font(12.0f, DWRITE_FONT_WEIGHT_NORMAL, 1.7f, true);
    }

    // <li> with its "N." marker hanging in the list's 22px left padding.
    class ListItem : public msimeui::Visual
    {
      public:
        ListItem(int number, std::wstring text) : marker_(std::to_wstring(number) + L".")
        {
            text_.Set(std::move(text), Body());
        }

        SizeF Measure(const SizeF &availableSize) override
        {
            const SizeF text = text_.Measure(std::max(availableSize.width - 22.0f, 1.0f));
            return {availableSize.width, text.height};
        }

        void Arrange(const RectF &finalRect) override
        {
            bounds_ = finalRect;
        }

        void Render(msimeui::DeviceResources &dr) override
        {
            const D2D1_COLOR_F color = CurrentPalette().textSecondary;
            const float textX = bounds_.x + 22.0f;
            // The marker ends a space before the text.
            TextStyle marker = Body();
            marker.wrap = false;
            const float markerWidth = MeasureTextWidth(marker_ + L" ", marker);
            TextLayout layout;
            layout.Set(marker_, marker);
            layout.Draw(dr, {textX - markerWidth, bounds_.y}, color, 1000.0f);
            text_.Draw(dr, {textX, bounds_.y}, color, bounds_.width - 22.0f);
        }

      private:
        std::wstring marker_;
        TextLayout text_;
    };
};

// ---------------------------------------------------------------------------------
// Input-state shortcut editors (shortcut-recorder.ts)
// ---------------------------------------------------------------------------------
struct ShortcutAction
{
    std::string key;
    std::wstring label;
    std::string editor;
    std::string defaultBinding;
    std::string binding;
    std::wstring problem;
};

struct ShortcutRule
{
    std::string binding;
    std::wstring label;
};

struct ShortcutKey
{
    std::string code;
    int vk = 0;
    std::wstring label;
};

struct ShortcutMetadata
{
    std::vector<ShortcutAction> actions;
    std::vector<ShortcutRule> reserved;
    std::vector<ShortcutRule> warnings;
    std::vector<ShortcutKey> keys;
    std::string raw;
};

class ShortcutEditor;

struct RecorderState
{
    SettingsModel *model = nullptr;
    bool hasMetadata = false;
    ShortcutMetadata metadata;
    std::vector<ShortcutEditor *> editors;
    ShortcutEditor *active = nullptr;
    bool saving = false;
    UINT_PTR timer = 0;

    const ShortcutAction *Action(const std::string &key) const
    {
        for (const auto &action : metadata.actions)
        {
            if (action.key == key)
                return &action;
        }
        return nullptr;
    }

    std::wstring Format(const std::string &binding) const
    {
        if (binding.empty())
            return L"未绑定";
        std::wstring result;
        size_t start = 0;
        while (start <= binding.size())
        {
            const size_t plus = binding.find('+', start);
            std::string part = binding.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
            std::wstring label;
            for (const auto &key : metadata.keys)
            {
                if (key.code == part)
                {
                    label = key.label;
                    break;
                }
            }
            if (label.empty())
            {
                if (part.rfind("Key", 0) == 0)
                    part = part.substr(3);
                else if (part.rfind("Digit", 0) == 0)
                    part = part.substr(5);
                label = Widen(part);
            }
            if (!result.empty())
                result += L" + ";
            result += label;
            if (plus == std::string::npos)
                break;
            start = plus + 1;
        }
        return result;
    }

    std::wstring Conflict(const ShortcutAction &action, const std::string &binding) const
    {
        if (binding.empty())
            return {};
        for (const auto &rule : metadata.reserved)
        {
            if (rule.binding == binding)
                return rule.label;
        }
        for (const auto &other : metadata.actions)
        {
            if (other.key != action.key && other.binding == binding)
                return other.label;
        }
        return {};
    }

    void EndSaving();
    void RenderAll();
};

RecorderState &Recorder()
{
    static RecorderState state;
    return state;
}

void CALLBACK SaveTimeout(HWND, UINT, UINT_PTR id, DWORD);

// `.shortcut-actions button`: padding 6px 10px, 1px border, 6px radius, --control-bg,
// 12px text inherited from `.shortcut-editor` (`font: inherit`, page font).
class ActionButton : public msimeui::Visual
{
  public:
    explicit ActionButton(std::wstring text) : text_(std::move(text))
    {
    }

    void SetText(std::wstring text)
    {
        if (text == text_)
            return;
        text_ = std::move(text);
        InvalidateMeasure();
        InvalidateVisual();
    }

    void SetDisabled(bool disabled)
    {
        disabled_ = disabled;
    }

    void SetOnClick(std::function<void()> handler)
    {
        onClick_ = std::move(handler);
    }

    SizeF Measure(const SizeF &) override
    {
        const float border = CssBorder();
        return {MeasureTextWidth(text_, Style()) + 20.0f + border * 2.0f, LineHeight(Style()) + 12.0f + border * 2.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        FillRounded(dr, bounds_, 6.0f, p.controlBg);
        StrokeRounded(dr, bounds_, 6.0f, p.border, 1.0f);
        DrawTextInRect(dr, text_, Style(), bounds_, p.text, true);
    }

    bool IsFocusable() const override
    {
        return !disabled_;
    }

    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        pressed_ = !disabled_ && window_ && HitTest(window_->ClientPixelsToDips(point));
        return pressed_;
    }

    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        const bool clicked = pressed_ && window_ && HitTest(window_->ClientPixelsToDips(point));
        pressed_ = false;
        if (clicked && onClick_ && !disabled_)
        {
            // A handler may tear down the visual that owns onClick_ while it runs.
            const auto handler = onClick_;
            handler();
        }
        return true;
    }

    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, disabled_ ? IDC_ARROW : IDC_HAND);
    }

    static TextStyle Style()
    {
        return Font(kFontCaption);
    }

  protected:
    std::wstring text_;
    bool disabled_ = false;
    bool pressed_ = false;
    std::function<void()> onClick_;
};

// The binding button: click to record, then press the chord.
class RecordButton : public ActionButton
{
  public:
    explicit RecordButton(std::wstring text) : ActionButton(std::move(text))
    {
    }

    void SetOnKey(std::function<bool(WPARAM, LPARAM)> handler)
    {
        onKey_ = std::move(handler);
    }

    void SetOnBlur(std::function<void()> handler)
    {
        onBlur_ = std::move(handler);
    }

    bool OnKeyDown(WPARAM key, LPARAM lParam) override
    {
        if (!onKey_)
            return false;
        // The handler may rebuild the editor list and destroy this button.
        const auto handler = onKey_;
        return handler(key, lParam);
    }

    void OnFocusChanged(bool focused) override
    {
        if (!focused && onBlur_)
        {
            const auto handler = onBlur_;
            handler();
        }
    }

  private:
    std::function<bool(WPARAM, LPARAM)> onKey_;
    std::function<void()> onBlur_;
};

// `.shortcut-editor`: the three buttons, the status line and the confirmation buttons.
class ShortcutEditor : public msimeui::Container
{
  public:
    ShortcutEditor(std::string editorId, std::wstring initialBinding) : editorId_(std::move(editorId))
    {
        record_ = std::make_shared<RecordButton>(std::move(initialBinding));
        clear_ = std::make_shared<ActionButton>(L"清空");
        reset_ = std::make_shared<ActionButton>(L"恢复默认");
        confirm_ = std::make_shared<ActionButton>(L"仍然绑定");
        cancel_ = std::make_shared<ActionButton>(L"取消");
        for (ActionButton *button : {static_cast<ActionButton *>(record_.get()), clear_.get(), reset_.get()})
            button->SetDisabled(true);

        auto actions = std::make_shared<Row>(6.0f);
        actions->SetWrap(true, 6.0f);
        actions->SetAlign(Row::Align::Start);
        actions->Add(record_);
        actions->Add(clear_);
        actions->Add(reset_);
        status_ =
            std::make_shared<Label>(L"", Font(kFontCaption, DWRITE_FONT_WEIGHT_NORMAL, 0.0f, true), TextRole::Muted);
        statusBox_ = std::make_shared<Hideable>(status_, false);
        auto confirmation = std::make_shared<Row>(6.0f);
        confirmation->SetWrap(true, 6.0f);
        confirmation->SetAlign(Row::Align::Start);
        confirmation->Add(confirm_);
        confirmation->Add(cancel_);
        confirmationBox_ = std::make_shared<Hideable>(confirmation, false);
        auto column = std::make_shared<Column>(0.0f);
        column->Add(actions);
        column->Add(statusBox_);
        column->Add(confirmationBox_);
        actions_ = actions;
        SetChild(column);

        record_->SetOnClick([this]() { OnRecordClick(); });
        record_->SetOnBlur([this]() {
            if (recording_)
                Finish();
        });
        record_->SetOnKey([this](WPARAM key, LPARAM lParam) { return OnKey(key, lParam); });
        confirm_->SetOnClick([this]() {
            if (pending_)
                Save(*pending_, true);
        });
        cancel_->SetOnClick([this]() { Finish(); });
        clear_->SetOnClick([this]() { Save(std::string()); });
        reset_->SetOnClick([this]() {
            if (const ShortcutAction *action = Action())
                Save(action->defaultBinding);
        });
    }

    const std::string &EditorId() const
    {
        return editorId_;
    }

    // max-content: the buttons or the status text, whichever is wider (flex-shrink: 0).
    SizeF Measure(const SizeF &availableSize) override
    {
        const SizeF buttons = actions_->MeasureInLayout({100000.0f, availableSize.height});
        float width = buttons.width;
        if (statusBox_->IsVisible())
            width = std::max(width, MeasureTextWidth(status_->GetText(), Font(kFontCaption)));
        width = std::min(width, availableSize.width);
        const SizeF size = GetChildSize(width, availableSize.height);
        return {width, size.height};
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        msimeui::Container::Render(dr);
    }

    void Finish(const std::wstring &message = {})
    {
        RecorderState &state = Recorder();
        pending_.reset();
        if (state.active == this)
            state.active = nullptr;
        confirmationBox_->SetVisible(false);
        recording_ = false;
        Render();
        SetStatus(message);
    }

    void Render()
    {
        RecorderState &state = Recorder();
        if (const ShortcutAction *action = Action(); action && !recording_)
            record_->SetText(state.Format(action->binding));
        const bool disabled = state.saving || !state.hasMetadata;
        for (ActionButton *button : {static_cast<ActionButton *>(record_.get()), clear_.get(), reset_.get()})
            button->SetDisabled(disabled);
        InvalidateVisual();
    }

  private:
    SizeF GetChildSize(float width, float height)
    {
        return child_ ? child_->MeasureInLayout({width, height}) : SizeF{};
    }

    const ShortcutAction *Action() const
    {
        const RecorderState &state = Recorder();
        for (const auto &action : state.metadata.actions)
        {
            if (action.editor == editorId_)
                return &action;
        }
        return nullptr;
    }

    void SetStatus(const std::wstring &message)
    {
        status_->SetText(message);
        statusBox_->SetVisible(!message.empty());
        InvalidateMeasure();
    }

    void OnRecordClick()
    {
        RecorderState &state = Recorder();
        if (state.saving || !state.hasMetadata)
            return;
        if (recording_)
        {
            Finish();
            return;
        }
        if (state.active && state.active != this)
            state.active->Finish();
        Finish();
        state.active = this;
        recording_ = true;
        record_->SetText(L"请按下快捷键");
        SetStatus(L"Ctrl + 字母、数字、F1–F12、标点、Space，可加 Shift；Esc 取消，Tab 离开。");
    }

    // KeyboardEvent.code for the key, like eventCode(): '' for keys that never bind.
    std::string Code(WPARAM vk, LPARAM lParam) const
    {
        const bool extended = (lParam & (1 << 24)) != 0;
        if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN)
            return "Modifier";
        if (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE)
            return {};
        if (vk == VK_RETURN && extended)
            return {};
        if (vk >= 'A' && vk <= 'Z')
            return std::string("Key") + static_cast<char>(vk);
        if (vk >= '0' && vk <= '9')
            return std::string("Digit") + static_cast<char>(vk);
        if (vk >= VK_F1 && vk <= VK_F12)
            return "F" + std::to_string(vk - VK_F1 + 1);
        for (const auto &key : Recorder().metadata.keys)
        {
            if (key.vk == static_cast<int>(vk))
                return key.code;
        }
        if (vk == VK_TAB)
            return "Tab";
        if (vk == VK_ESCAPE)
            return "Escape";
        return {};
    }

    bool OnKey(WPARAM vk, LPARAM lParam)
    {
        const std::string code = Code(vk, lParam);
        if (pending_ && code == "Escape")
        {
            Finish();
            return true;
        }
        if (!recording_)
            return false;
        if (code == "Tab")
        {
            // Tab leaves the button, which ends the recording.
            Finish();
            return false;
        }
        if (code == "Escape")
        {
            Finish();
            return true;
        }
        const bool repeat = (lParam & (1 << 30)) != 0;
        if (repeat || code == "Modifier")
            return true;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        const bool meta = (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;
        static const std::regex simple("^(Key[A-Z]|Digit[0-9]|F([1-9]|1[0-2]))$");
        bool named = false;
        for (const auto &key : Recorder().metadata.keys)
            named = named || key.code == code;
        if (meta || !ctrl || alt || code.empty() || (!std::regex_match(code, simple) && !named))
        {
            SetStatus(L"请选择 Ctrl + 字母、数字、F1–F12、标点、Space，可加 Shift。");
            return true;
        }
        Save(std::string("Ctrl+") + (shift ? "Shift+" : "") + code);
        return true;
    }

    void Save(const std::string &binding, bool confirmed = false)
    {
        RecorderState &state = Recorder();
        if (state.saving)
            return;
        const ShortcutAction *action = Action();
        if (!action)
            return;
        if (state.active && state.active != this)
            state.active->Finish();
        const std::wstring owner = state.Conflict(*action, binding);
        if (!owner.empty())
        {
            pending_.reset();
            confirmationBox_->SetVisible(false);
            SetStatus(state.Format(binding) + L" 用于" + owner + L"。");
            return;
        }
        std::wstring warning;
        for (const auto &rule : state.metadata.warnings)
        {
            if (rule.binding == binding)
                warning = rule.label;
        }
        if (!warning.empty() && !confirmed)
        {
            Finish(state.Format(binding) + L" 与常用的“" + warning +
                   L"”快捷键冲突，绑定后会覆盖当前软件的这项操作。仍要绑定吗？");
            pending_ = binding;
            state.active = this;
            confirmationBox_->SetVisible(true);
            return;
        }
        const std::string key = action->key;
        Finish(L"正在保存…");
        state.saving = true;
        // The host echoes a snapshot after every write; without one, give the editors back.
        state.timer = SetTimer(nullptr, 0, 5000, &SaveTimeout);
        state.RenderAll();
        // Not SettingsModel::Update: its local write would echo straight back as if it
        // were the host's snapshot and end the save at once.
        state.model->Post("configUpdate", Json{{"path", "keybindings." + key}, {"value", binding}});
    }

    std::string editorId_;
    std::shared_ptr<RecordButton> record_;
    std::shared_ptr<ActionButton> clear_;
    std::shared_ptr<ActionButton> reset_;
    std::shared_ptr<ActionButton> confirm_;
    std::shared_ptr<ActionButton> cancel_;
    std::shared_ptr<Row> actions_;
    std::shared_ptr<Label> status_;
    std::shared_ptr<Hideable> statusBox_;
    std::shared_ptr<Hideable> confirmationBox_;
    std::optional<std::string> pending_;
    bool recording_ = false;
};

void RecorderState::EndSaving()
{
    saving = false;
    if (timer)
        KillTimer(nullptr, timer);
    timer = 0;
}

void RecorderState::RenderAll()
{
    for (ShortcutEditor *editor : editors)
        editor->Render();
}

void CALLBACK SaveTimeout(HWND, UINT, UINT_PTR id, DWORD)
{
    KillTimer(nullptr, id);
    RecorderState &state = Recorder();
    if (state.timer != id)
        return;
    state.timer = 0;
    state.EndSaving();
    state.RenderAll();
    for (ShortcutEditor *editor : state.editors)
    {
        // finish() in the page runs on the editor that saved; only it shows a status.
        (void)editor;
    }
    if (state.active)
        state.active->Finish(L"保存未完成，请重试。");
}

std::wstring Problem(const ShortcutAction &action)
{
    return L"此快捷键已停用：" + action.problem + L"。请重新绑定。";
}

bool ParseMetadata(const Json &value, ShortcutMetadata &out)
{
    if (!value.is_object())
        return false;
    auto str = [](const Json &object, const char *key) {
        return object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : std::string();
    };
    out = {};
    out.raw = value.dump();
    if (value.contains("actions") && value["actions"].is_array())
    {
        for (const Json &item : value["actions"])
        {
            out.actions.push_back({str(item, "key"), Widen(str(item, "label")), str(item, "editor"),
                                   str(item, "defaultBinding"), str(item, "binding"), Widen(str(item, "problem"))});
        }
    }
    for (const char *list : {"reserved", "warnings"})
    {
        if (!value.contains(list) || !value[list].is_array())
            continue;
        for (const Json &item : value[list])
            (std::string(list) == "reserved" ? out.reserved : out.warnings)
                .push_back({str(item, "binding"), Widen(str(item, "label"))});
    }
    if (value.contains("keys") && value["keys"].is_array())
    {
        for (const Json &item : value["keys"])
        {
            out.keys.push_back({str(item, "code"),
                                item.contains("vk") && item["vk"].is_number() ? item["vk"].get<int>() : 0,
                                Widen(str(item, "label"))});
        }
    }
    return true;
}
} // namespace

VisualPtr BuildShortcutPage(PageContext &context)
{
    PageBuilder page(L"快捷键");
    SettingsModel *model = &context.model;

    // .shortcut-intro: 13px / 1.6, muted, margin -4px 8px 18px
    auto intro = std::make_shared<Label>(L"各快捷键的生效范围见下方说明。",
                                         Font(13.0f, DWRITE_FONT_WEIGHT_NORMAL, 1.6f, true), TextRole::Muted);
    page.AddRaw(intro, 18.0f);
    intro->SetMargin({8.0f, -4.0f, 8.0f, 18.0f});

    // 简繁切换
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(Heading(L"简繁切换", L"仅在中文输入状态生效，切换时保留正在输入的内容"));
        column->Add(
            Options({BindCheck(context, L"Ctrl + Shift + F", "keybindings.toggle_character_set_ctrl_shift_f")}));
        page.AddRaw(std::make_shared<FlatSection>(column), 18.0f);
    }

    // 中英文切换
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(Heading(L"中英文切换", L"勾选启用对应快捷键；Ctrl+Space 由 Windows 系统管理，此处不控制"));
        column->Add(Options({BindCheck(context, L"Shift", "keybindings.switch_language_shift"),
                             BindCheck(context, L"Ctrl", "keybindings.switch_language_ctrl"),
                             BindCheck(context, L"Ctrl + Alt + Space", "keybindings.switch_language_ctrl_alt_space")}));
        auto cycle = Heading(L"", L"开启后，上面勾选的快捷键按 中文 → 日文 → 英文 → 中文 "
                                  L"依次切换；有输入时先上屏原始字母，中文和日文分别保留各自的输入方案");
        cycle->SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
        column->Add(cycle);
        column->Add(Options({BindCheck(context, L"允许中日英循环切换", "keybindings.trilingual_cycle")}));
        auto guide = std::make_shared<TopRule>(std::make_shared<SystemGuide>());
        guide->SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
        column->Add(guide);
        page.AddRaw(std::make_shared<FlatSection>(column), 18.0f);
    }

    // 输入状态切换
    {
        RecorderState &state = Recorder();
        state = RecorderState{};
        state.model = model;
        auto column = std::make_shared<Column>(0.0f);
        column->Add(Heading(L"输入状态切换",
                            L"支持 Ctrl 或 Ctrl+Shift 组合。英文候选模式仅在中文输入状态生效，再按一次回到拼音输入"));
        auto list = std::make_shared<Column>(0.0f);
        const std::pair<const char *, const wchar_t *> rows[] = {{"englishCandidatesHotkey", L"切换英文候选模式"},
                                                                 {"fullWidthHotkey", L"全角 / 半角切换"},
                                                                 {"punctuationHotkey", L"中文 / 英文标点切换"}};
        const wchar_t *defaults[] = {L"Ctrl + Shift + E", L"Ctrl + Shift + Space", L"Ctrl + ."};
        for (size_t i = 0; i < std::size(rows); ++i)
        {
            auto editor = std::make_shared<ShortcutEditor>(rows[i].first, defaults[i]);
            state.editors.push_back(editor.get());
            list->Add(RuledRow(RowLabel(rows[i].second), editor, false, i > 0));
        }
        column->Add(std::make_shared<TopRule>(list));
        column->Add(MakeDescription(L"全半角和标点快捷键在中英文状态下均生效。绑定后会占用当前软件的同名快捷键；"
                                    L"标点模式锁定时，仍接收标点快捷键，标点模式保持锁定。"));
        page.AddRaw(std::make_shared<FlatSection>(column), 18.0f);

        // applyInputStateShortcuts()
        context.OnConfig([model]() {
            RecorderState &state = Recorder();
            const Json *value = model->Find("keybindings.input_state_shortcuts");
            ShortcutMetadata metadata;
            if (!value || !ParseMetadata(*value, metadata))
                return;
            const bool changed = !state.hasMetadata || state.metadata.raw != metadata.raw;
            state.metadata = std::move(metadata);
            state.hasMetadata = true;
            state.EndSaving();
            if (changed && state.active)
                state.active->Finish();
            for (ShortcutEditor *editor : state.editors)
            {
                const ShortcutAction *action = nullptr;
                for (const auto &item : state.metadata.actions)
                {
                    if (item.editor == editor->EditorId())
                        action = &item;
                }
                if (!action)
                    continue;
                if (!action->problem.empty())
                    editor->Finish(Problem(*action));
                else if (changed || !state.active)
                    editor->Finish();
                else
                    editor->Render();
            }
        });
    }

    // 提交
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(Heading(L"提交", L"双拼方案下，Enter 提交原始输入，Shift + Enter 提交转换后的全拼。"
                                     L"敲大写字母时 Shift 常常没松开就按了回车，容易误触，可在此关闭"));
        column->Add(
            Options({BindCheck(context, L"Shift + Enter 提交转换后的全拼", "keybindings.commit_pinyin_shift_enter")}));
        page.AddRaw(std::make_shared<FlatSection>(column), 18.0f);
    }

    // 翻页方式
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(Heading(L"翻页方式", L"勾选启用对应的候选翻页键。左 / 右只在候选窗为横排时生效：光标在拼音末尾时"
                                         L"右键移动高亮，左键回到第一个候选后再按才移动光标。滚轮翻页依赖系统设置"
                                         L"「当我悬停在非活动窗口上方时滚动它们」，该项关闭时滚轮不会到达候选窗口"));
        column->Add(Options(
            {BindCheck(context, L"- / =", "general.paging_minus_equal"),
             BindCheck(context, L", / .", "general.paging_comma_period"),
             BindCheck(context, L"[ / ]", "general.paging_brackets"),
             BindCheck(context, L"Shift+Tab / Tab", "general.paging_tab"),
             BindCheck(context, L"PageUp / PageDown", "general.paging_page_up_down"),
             BindCheck(context, L"上 / 下（移动候选项）", "general.candidate_arrow_navigation"),
             BindCheck(context, L"左 / 右（横排时移动候选项）", "general.candidate_horizontal_arrow_navigation"),
             BindCheck(context, L"鼠标滚轮", "general.paging_mouse_wheel")}));
        page.AddRaw(std::make_shared<FlatSection>(column), 18.0f);
    }

    // 候选操作
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(Heading(L"候选操作", L"输入和选取候选词时使用"));
        auto list = std::make_shared<Column>(0.0f);
        struct Entry
        {
            const wchar_t *label;
            VisualPtr keys;
        };
        const std::vector<Entry> entries = {
            {L"选择候选（小键盘数字同样可用）", Keys({L"Space", L"~或", L"1", L"|–", L"9"})},
            {L"向前 / 向后翻页", Keys({L"-", L"|/", L"="})},
            {L"向前 / 向后翻页", Keys({L",", L"|/", L"."})},
            {L"向前 / 向后翻页", Keys({L"Shift", L"|+", L"Tab", L"|/", L"Tab"})},
            {L"向前 / 向后翻页", Keys({L"Page Up", L"|/", L"Page Down"})},
            {L"移动候选项", Keys({L"↑", L"|/", L"↓"})},
            {L"编辑输入串", Keys({L"←", L"|/", L"→", L"~、", L"Backspace"})},
            {L"按音节删除 / 按音节移动光标",
             Keys({L"Ctrl", L"|+", L"Backspace", L"~、", L"Ctrl", L"|+", L"←", L"|/", L"→"})},
            {L"删除光标后的字母", Keys({L"Delete"})},
            {L"手动分隔拼音（如 xi'an）", Keys({L"'"})},
            {L"以词定字：上屏高亮候选的首字 / 末字（可在“输入”中改为 - / =）", Keys({L"[", L"|/", L"]"})},
            {L"上屏高亮候选的译文（需开启候选中英互译；有多条释义时再选一条）", Keys({L"Ctrl", L"|+", L"Enter"})},
            {L"上屏指定序号候选的译文（需开启候选中英互译）", Keys({L"Ctrl", L"|+", L"1", L"|–", L"9"})},
            {L"展开 / 收起候选译义面板（可在下方“候选译义面板”改键或关闭）", Keys({L"`"})},
            {L"提交原始输入 / 取消输入", Keys({L"Enter", L"|/", L"Esc"})},
            {L"提交拼音（双拼方案下提交转换后的全拼，可在上方关闭）", Keys({L"Shift", L"|+", L"Enter"})},
        };
        for (size_t i = 0; i < entries.size(); ++i)
            list->Add(RuledRow(RowLabel(entries[i].label), entries[i].keys, true, i > 0));
        column->Add(std::make_shared<TopRule>(list));
        page.AddRaw(std::make_shared<FlatSection>(column), 18.0f);
    }

    // 候选展开面板（展开键与移动展开候选键，从「输入」页迁移而来）
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(Heading(L"候选展开面板", L"按展开键在高亮候选旁弹出面板"));
        auto expandEditor = std::make_shared<ExpandKeyEditor>(model);
        auto prevEditor = std::make_shared<MoveKeyEditor>(model, "keybindings.candidate_expansion_move_prev_key",
                                                          std::string(CandidatePanelMoveKey::DefaultPrev));
        auto nextEditor = std::make_shared<MoveKeyEditor>(model, "keybindings.candidate_expansion_move_next_key",
                                                          std::string(CandidatePanelMoveKey::DefaultNext));

        auto list = std::make_shared<Column>(0.0f);
        list->Add(RuledRow(RowLabel(L"展开键"), expandEditor, false, false));
        list->Add(RuledRow(RowLabel(L"上一个展开候选词"), prevEditor, false, true));
        list->Add(RuledRow(RowLabel(L"下一个展开候选词"), nextEditor, false, true));
        column->Add(std::make_shared<TopRule>(list));
        auto moveHint =
            MakeDescription(L"移动键只在面板打开时移动展开候选，平时仍照常参与拼音输入；清除后该方向不再移动");
        moveHint->SetMargin({24.0f, 8.0f, 24.0f, 0.0f});
        column->Add(moveHint);
        page.AddRaw(std::make_shared<FlatSection>(column), 18.0f);

        context.OnConfig([model, expandEditor, prevEditor, nextEditor]() {
            if (const Json *expand = model->Find("keybindings.candidate_expansion_key"); expand && expand->is_string())
                expandEditor->Sync(expand->get<std::string>());
            if (const Json *prev = model->Find("keybindings.candidate_expansion_move_prev_key");
                prev && prev->is_string())
                prevEditor->Sync(prev->get<std::string>());
            if (const Json *next = model->Find("keybindings.candidate_expansion_move_next_key");
                next && next->is_string())
                nextEditor->Sync(next->get<std::string>());
        });
    }

    // 全局快捷键
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(Heading(L"全局快捷键", L"程序运行时全局生效；重启、退出和清缓存主要用于维护与调试",
                            std::make_shared<Badge>(L"高级")));
        auto list = std::make_shared<Column>(0.0f);
        struct Entry
        {
            const wchar_t *label;
            VisualPtr keys;
            bool danger;
        };
        const std::vector<Entry> entries = {
            {L"打开屏幕键盘", Keys({L"Ctrl", L"|+", L"Shift", L"|+", L"Win", L"|+", L"K"}), false},
            {L"删除当前候选窗口中的第 1–8 项", Keys({L"Ctrl", L"|+", L"Shift", L"|+", L"Alt", L"|+", L"1–8"}), false},
            {L"清除输入法引擎缓存", Keys({L"Ctrl", L"|+", L"Shift", L"|+", L"Alt", L"|+", L"C"}), false},
            {L"重启输入法服务", Keys({L"Ctrl", L"|+", L"Shift", L"|+", L"Alt", L"|+", L"R"}), false},
            {L"立即退出输入法服务", Keys({L"Ctrl", L"|+", L"Shift", L"|+", L"Alt", L"|+", L"T"}), true},
        };
        for (size_t i = 0; i < entries.size(); ++i)
            list->Add(RuledRow(RowLabel(entries[i].label, entries[i].danger ? TextRole::Danger : TextRole::Primary),
                               entries[i].keys, true, i > 0));
        column->Add(std::make_shared<TopRule>(list));
        page.AddRaw(std::make_shared<FlatSection>(column), 18.0f);
    }

    VisualPtr content = page.Build();
    // .shortcut-settings { padding-bottom: 24px }
    content->SetPadding({12.0f, 14.0f, 12.0f, 24.0f});
    return content;
}
} // namespace native_settings
