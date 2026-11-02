#include "settings/native/native_settings_pages.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Fonts.h"
#include "msimeui/Window.h"

#include <dwrite_1.h>

#include <algorithm>
#include <iterator>

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

// `.section-header-*-helpcode { margin-top: 12px }`
VisualPtr Spaced(VisualPtr row)
{
    row->SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
    return row;
}

// Single choice drawn as one 30px box split into segments, the chosen one filled with the accent.
// A struck option cannot stay on together with what the other choice of the page has now: its
// label is crossed out with a backslash, and choosing it still goes through (the page then turns
// the other side off).
class SegmentedChoice : public msimeui::Visual
{
  public:
    using SelectHandler = std::function<void(size_t index)>;

    explicit SegmentedChoice(std::vector<std::wstring> labels)
        : labels_(std::move(labels)), struck_(labels_.size(), false)
    {
    }

    void SetOnSelect(SelectHandler handler)
    {
        onSelect_ = std::move(handler);
    }

    void SetSelected(size_t index)
    {
        if (selected_ == index)
            return;
        selected_ = index;
        InvalidateVisual();
    }

    void SetStruck(size_t index, bool struck)
    {
        if (index >= struck_.size() || struck_[index] == struck)
            return;
        struck_[index] = struck;
        InvalidateVisual();
    }

    SizeF Measure(const SizeF &) override
    {
        float width = 0.0f;
        for (const std::wstring &label : labels_)
            width += SegmentWidth(label);
        return {width, 30.0f};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        FillRounded(dr, bounds_, kRadius, p.controlBg);
        for (size_t i = 0; i < labels_.size(); ++i)
        {
            const RectF segment = SegmentRect(i);
            if (i == selected_)
                FillSegment(dr, i, segment, p.accent);
            else if (i == hovered_)
                FillSegment(dr, i, segment, p.buttonSecondaryHover);
            if (i > 0)
                FillRect(dr, {segment.x, bounds_.y, CssBorder(), bounds_.height}, p.border);
            const D2D1_COLOR_F color = i == selected_ ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)
                                       : struck_[i]   ? p.textMuted
                                                      : p.text;
            DrawTextInRect(dr, labels_[i], Style(), segment, color, true);
            if (struck_[i])
            {
                // A backslash through the label: falling to the right, 12 degrees off level.
                const float half = MeasureTextWidth(labels_[i], Style()) * 0.5f + 2.0f;
                const float fall = half * 0.2126f;
                const float cx = segment.x + segment.width * 0.5f;
                const float cy = segment.y + segment.height * 0.5f;
                DrawLine(dr, {cx - half, cy - fall}, {cx + half, cy + fall}, color, 1.5f);
            }
        }
        StrokeRounded(dr, bounds_, kRadius, p.border, CssBorder());
    }

    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        pressed_ = HitSegment(point);
        return pressed_ != kNone;
    }

    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        const size_t hit = HitSegment(point);
        const bool clicked = hit != kNone && hit == pressed_;
        pressed_ = kNone;
        if (clicked && hit != selected_ && onSelect_)
        {
            // The handler rewrites the config, which syncs the selection back here.
            const auto handler = onSelect_;
            handler(hit);
        }
        return true;
    }

    bool OnMouseMove(const POINT &point, WPARAM) override
    {
        const size_t hit = HitSegment(point);
        if (hit != hovered_)
        {
            hovered_ = hit;
            InvalidateVisual();
        }
        return true;
    }

    void OnMouseLeave() override
    {
        if (hovered_ == kNone)
            return;
        hovered_ = kNone;
        InvalidateVisual();
    }

    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, IDC_HAND);
    }

  private:
    static constexpr size_t kNone = static_cast<size_t>(-1);
    static constexpr float kRadius = 6.0f;

    static TextStyle Style()
    {
        return ControlStyle();
    }

    static float SegmentWidth(const std::wstring &label)
    {
        return MeasureTextWidth(label, Style()) + 28.0f;
    }

    RectF SegmentRect(size_t index) const
    {
        float x = bounds_.x;
        for (size_t i = 0; i < index; ++i)
            x += SegmentWidth(labels_[i]);
        return {x, bounds_.y, SegmentWidth(labels_[index]), bounds_.height};
    }

    // Rounded only on the outer corners of the box.
    void FillSegment(msimeui::DeviceResources &dr, size_t index, const RectF &segment, const D2D1_COLOR_F &color)
    {
        FillRounded(dr, segment, kRadius, color);
        const float half = segment.width * 0.5f;
        if (index > 0)
            FillRect(dr, {segment.x, segment.y, half, segment.height}, color);
        if (index + 1 < labels_.size())
            FillRect(dr, {segment.x + half, segment.y, segment.width - half, segment.height}, color);
    }

    size_t HitSegment(const POINT &point) const
    {
        if (!window_)
            return kNone;
        const PointF dip = window_->ClientPixelsToDips(point);
        for (size_t i = 0; i < labels_.size(); ++i)
        {
            if (Contains(SegmentRect(i), dip))
                return i;
        }
        return kNone;
    }

    std::vector<std::wstring> labels_;
    std::vector<bool> struck_;
    size_t selected_ = kNone;
    size_t hovered_ = kNone;
    size_t pressed_ = kNone;
    SelectHandler onSelect_;
};

// The shuangpin helpcode settings. `helpcode.shuangpin_helpcode` switches the end helpcode on; the
// two mid-sentence ways do not depend on it, nor do the helpcode hints of the candidate window,
// which show while any of the three is on.
constexpr const char *kEndEnabled = "helpcode.shuangpin_helpcode";
constexpr const char *kEndDirect = "helpcode.shuangpin_helpcode_direct";
constexpr const char *kMidTrigger = "helpcode.shuangpin_mid_sentence_helpcode";
constexpr const char *kMidDirect = "helpcode.shuangpin_direct_helpcode";
constexpr const char *kDirectSlash = "helpcode.shuangpin_direct_helpcode_slash";
constexpr const char *kDirectUppercase = "helpcode.shuangpin_direct_helpcode_uppercase";
constexpr const char *kDirectEndTrigger = "helpcode.shuangpin_direct_helpcode_end_trigger";

// 末尾辅助码 and 句中辅助码, in the order of their segments.
enum class EndMode
{
    Off,
    Trailing,
    Trigger,
};

enum class MidMode
{
    Off,
    Direct,
    Trigger,
};

struct HelpcodeModes
{
    // What the end helpcode settings say.
    EndMode storedEnd = EndMode::Off;
    // What the page shows: the engine ignores the end helpcode while the direct one is on.
    EndMode end = EndMode::Off;
    MidMode mid = MidMode::Off;
    // 句中直接紧跟式下用引导键进入末尾辅助码：末尾引导键（反引号、分号）这时也有用。
    bool directEndTrigger = false;
};

HelpcodeModes ReadModes(const SettingsModel &model)
{
    HelpcodeModes modes;
    if (model.GetBool(kEndEnabled))
        modes.storedEnd = model.GetBool(kEndDirect) ? EndMode::Trailing : EndMode::Trigger;
    if (model.GetBool(kMidDirect))
        modes.mid = MidMode::Direct;
    else if (model.GetBool(kMidTrigger))
        modes.mid = MidMode::Trigger;
    modes.end = modes.mid == MidMode::Direct ? EndMode::Off : modes.storedEnd;
    modes.directEndTrigger = modes.mid == MidMode::Direct && model.GetBool(kDirectEndTrigger);
    return modes;
}

// 反引号、分号能不能当末尾引导键：末尾辅助码是引导键式，或句中直接紧跟式下开了用引导键进入末尾辅助码。
bool EndTriggerKeysInUse(const HelpcodeModes &modes)
{
    return modes.end == EndMode::Trigger || modes.directEndTrigger;
}

// The one pair that may be on together: the end helpcode read from the input's shape and the
// mid-sentence one opened by a trigger key. Older versions also let both sides use trigger keys;
// such a config keeps working as it is until the user changes either side here.
bool CanCoexist(EndMode end, MidMode mid)
{
    return end == EndMode::Off || mid == MidMode::Off || (end == EndMode::Trailing && mid == MidMode::Trigger);
}

// For notices; the segments themselves are shorter.
std::wstring EndName(EndMode mode)
{
    return mode == EndMode::Trailing ? L"末尾紧跟式" : mode == EndMode::Trigger ? L"末尾引导键式" : L"末尾辅助码";
}

std::wstring MidName(MidMode mode)
{
    return mode == MidMode::Direct ? L"句中直接紧跟式" : mode == MidMode::Trigger ? L"句中引导键式" : L"句中辅助码";
}

// Posts `value` unless the local snapshot already has it.
void Set(SettingsModel &model, const char *path, bool value)
{
    if (model.GetBool(path) != value)
        model.Update(path, value);
}

// Switches the end helpcode to `target`. When the mid-sentence one cannot stay on with it, that
// one goes off first and `notice` says so; it is filled before any write, so the sync after the
// first write already shows it.
void ChooseEnd(SettingsModel &model, EndMode target, std::wstring &notice)
{
    const HelpcodeModes modes = ReadModes(model);
    notice.clear();
    if (!CanCoexist(target, modes.mid))
    {
        notice = MidName(modes.mid) + L"不能和" + EndName(target) + L"同时开，已关闭句中辅助码。";
        Set(model, kMidDirect, false);
        Set(model, kMidTrigger, false);
    }
    Set(model, kEndEnabled, target != EndMode::Off);
    if (target != EndMode::Off)
        Set(model, kEndDirect, target == EndMode::Trailing);
}

// Switches the mid-sentence helpcode to `target`; see ChooseEnd. The host turns the direct and the
// trigger one off when the other goes on, the page writes the same so that it agrees at once.
void ChooseMid(SettingsModel &model, MidMode target, std::wstring &notice)
{
    const HelpcodeModes modes = ReadModes(model);
    notice.clear();
    // Checked against the stored end helpcode: the one hidden by the direct helpcode comes back
    // once that is off.
    if (!CanCoexist(modes.storedEnd, target))
    {
        if (modes.end != EndMode::Off)
            notice = MidName(target) + L"不能和" + EndName(modes.end) + L"同时开，已关闭末尾辅助码。";
        // The direct helpcode overrides the end one by itself; its setting stays for when the
        // direct one goes off again.
        if (target == MidMode::Trigger)
            Set(model, kEndEnabled, false);
    }
    switch (target)
    {
    case MidMode::Off:
        Set(model, kMidDirect, false);
        Set(model, kMidTrigger, false);
        break;
    case MidMode::Direct:
        Set(model, kMidDirect, true);
        Set(model, kMidTrigger, false);
        break;
    case MidMode::Trigger:
        Set(model, kMidTrigger, true);
        Set(model, kMidDirect, false);
        break;
    }
}

std::wstring EndHint(const HelpcodeModes &modes)
{
    if (modes.directEndTrigger)
        return L"句中直接紧跟式开着时，末尾的辅码也由直接紧跟式识别；在输入末尾敲末尾引导键，可以先整理成纯双拼，再按"
               L"末尾辅助码筛候选。";
    if (modes.mid == MidMode::Direct)
        return L"句中直接紧跟式开着时末尾辅助码不生效，末尾的辅码也由直接紧跟式识别。";
    switch (modes.end)
    {
    case EndMode::Trailing:
        return L"音节后直接接辅码：完整音节后多敲一个键是单码（如 ulpbx），一码重排；末尾两键第二个大写是双码（如 "
               L"ulpbxY），两码筛选。";
    case EndMode::Trigger:
        return L"在输入末尾敲引导键，再接一到两个辅码，大小写均可（如 ulpb;xy），一码重排、两码筛选。不敲引导键时"
               L"末尾的字母一律按拼音读。";
    default:
        return L"输入末尾的字母一律按拼音读。";
    }
}

std::wstring MidHint(MidMode mode)
{
    switch (mode)
    {
    case MidMode::Direct:
        return L"不用引导键：三码（如 uia）字优先，四码（如 uiab）词优先，单独敲时直接定字。句中的四码要按下面的"
               L"标记结束，否则会让位给普通切分。";
    case MidMode::Trigger:
        return L"在句中某个完整音节后敲引导键，后面的字母筛这个音节上的字（如 ulpb`xihfa）：第一码大小写均可，第二码"
               L"要用大写（如 ulpb`xYihfa），之后接着打。整句候选会带着锁住的字重新打分。";
    default:
        return L"不给句中的音节加辅码。";
    }
}

// One row of 按键分配. Backtick, semicolon and uppercase exist on both sides as trigger keys, and a
// key belongs to one side at most, which the host keeps too (SetHelpcodeTrigger). Uppercase also
// is the second code of the end double code and can mark the direct helpcode's four-key codes,
// as the slash can.
enum class KeyKind
{
    Backtick,
    Semicolon,
    Uppercase,
    Slash,
};

struct KeySpec
{
    KeyKind kind;
    const wchar_t *label;
    const char *endPath;
    const char *midPath;
};

const KeySpec kKeys[] = {
    {KeyKind::Backtick, L"` 反引号", "helpcode.shuangpin_helpcode_trigger_backtick",
     "helpcode.shuangpin_mid_sentence_helpcode_backtick"},
    {KeyKind::Semicolon, L"; 分号", "helpcode.shuangpin_helpcode_trigger_semicolon",
     "helpcode.shuangpin_mid_sentence_helpcode_semicolon"},
    {KeyKind::Uppercase, L"A 大写字母", "helpcode.shuangpin_helpcode_trigger_uppercase",
     "helpcode.shuangpin_mid_sentence_helpcode_uppercase"},
    {KeyKind::Slash, L"/ 斜杠", nullptr, nullptr},
};

DropdownOption KeyOption(const std::string &value)
{
    std::wstring label = L"不用";
    if (value == "end")
        label = L"末尾引导键";
    else if (value == "mid")
        label = L"句中引导键";
    else if (value == "double")
        label = L"末尾双码的第二码（ulpbxY）";
    else if (value == "four")
        label = L"直接紧跟式四码的第二位（uiaB）";
    else if (value == "slash")
        label = L"直接紧跟式四码结束标记（uiab/）";
    return {std::move(label), value, false, {}};
}

struct KeyChoices
{
    std::vector<DropdownOption> options;
    std::string value;
};

int EndTriggerCount(const SettingsModel &model)
{
    int count = 0;
    for (const KeySpec &key : kKeys)
    {
        if (key.endPath && model.GetBool(key.endPath))
            ++count;
    }
    return count;
}

bool IsDirectMarker(const KeySpec &key, const HelpcodeModes &modes)
{
    return key.kind == KeyKind::Slash || (key.kind == KeyKind::Uppercase && modes.mid == MidMode::Direct);
}

// A row shows when the key can be changed, or when it is held to the one use it has: the last end
// trigger key, or the last way to end a four-key code. A key that can only do nothing, or only be
// the second code of the end double code, needs no row.
bool IsShown(const KeyChoices &choices)
{
    return choices.options.size() > 1 ||
           (choices.options.size() == 1 && choices.value != "none" && choices.value != "double");
}

// What a key can do with the current choices, and what it does. Only uses that apply now are
// listed.
KeyChoices ChoicesFor(const SettingsModel &model, const HelpcodeModes &modes, const KeySpec &key)
{
    KeyChoices choices;
    if (IsDirectMarker(key, modes))
    {
        if (modes.mid != MidMode::Direct)
            return choices;
        const bool slash = key.kind == KeyKind::Slash;
        const bool mine = model.GetBool(slash ? kDirectSlash : kDirectUppercase);
        const bool other = model.GetBool(slash ? kDirectUppercase : kDirectSlash);
        const std::string use = slash ? "slash" : "four";
        choices.value = mine ? use : "none";
        // At least one way to end a four-key code stays, as the host requires.
        if (!mine || other)
            choices.options.push_back(KeyOption("none"));
        choices.options.push_back(KeyOption(use));
        return choices;
    }
    const bool end = EndTriggerKeysInUse(modes) && model.GetBool(key.endPath);
    const bool mid = modes.mid == MidMode::Trigger && model.GetBool(key.midPath);
    const bool doubleCode = key.kind == KeyKind::Uppercase && modes.end == EndMode::Trailing;
    choices.value = mid ? "mid" : end ? "end" : doubleCode ? "double" : "none";
    if (doubleCode)
        choices.options.push_back(KeyOption("double"));
    // The host keeps one end trigger key at least: the last one can only move to the other side.
    else if (!end || EndTriggerCount(model) > 1)
        choices.options.push_back(KeyOption("none"));
    if (EndTriggerKeysInUse(modes))
        choices.options.push_back(KeyOption("end"));
    if (modes.mid == MidMode::Trigger)
        choices.options.push_back(KeyOption("mid"));
    return choices;
}

void ApplyKeyChoice(SettingsModel &model, const KeySpec &key, const std::string &value)
{
    const HelpcodeModes modes = ReadModes(model);
    if (IsDirectMarker(key, modes))
    {
        Set(model, key.kind == KeyKind::Slash ? kDirectSlash : kDirectUppercase, value != "none");
        return;
    }
    // Turning a key on for one side releases it on the other at the host; the second write only
    // brings the local snapshot along and is never refused.
    if (value == "end")
    {
        Set(model, key.endPath, true);
        Set(model, key.midPath, false);
        return;
    }
    if (value == "mid")
    {
        Set(model, key.midPath, true);
        Set(model, key.endPath, false);
        return;
    }
    Set(model, key.midPath, false);
    // An end trigger key that does nothing now stays as it is.
    if (value == "none" && EndTriggerKeysInUse(modes))
        Set(model, key.endPath, false);
}

std::wstring KeyNotes(const HelpcodeModes &modes, const std::vector<KeyChoices> &keys)
{
    std::vector<std::string> values;
    bool lockedEnd = false;
    bool lockedMarker = false;
    for (const KeyChoices &key : keys)
    {
        values.push_back(key.value);
        if (key.options.size() == 1 && key.value == "end")
            lockedEnd = true;
        if (key.options.size() == 1 && (key.value == "slash" || key.value == "four"))
            lockedMarker = true;
    }
    std::wstring notes;
    const auto add = [&notes](const wchar_t *text) {
        if (!notes.empty())
            notes += L"\n";
        notes += text;
    };
    if (lockedEnd)
        add(L"末尾引导键至少要留一个。");
    if (lockedMarker)
        add(L"句中四码的结束标记至少要留一个。");
    const std::string &semicolon = values[static_cast<size_t>(KeyKind::Semicolon)];
    const std::string &uppercase = values[static_cast<size_t>(KeyKind::Uppercase)];
    if (semicolon == "end" || semicolon == "mid")
        add(L"分号在微软、搜狗、紫光双拼里也是 ing 韵母，只在完整音节后才当引导键。");
    if (uppercase == "mid")
        add(modes.end == EndMode::Trailing
                ? L"大写字母当句中引导键：完整音节后直接敲大写的第一码，相当于反引号加这个字母（如 ulpbXihfa），第二码"
                  L"仍要大写。末尾双码（ulpbxY）随之不可用。"
                : L"大写字母当句中引导键：完整音节后直接敲大写的第一码，相当于反引号加这个字母（如 ulpbXihfa），第二码"
                  L"仍要大写。");
    if (uppercase == "end")
        add(L"大写字母当末尾引导键：第一码敲成大写（如 ulpbXy）。");
    if (uppercase == "four")
        add(L"大写字母只当四码的第二位辅码。");
    if (values[static_cast<size_t>(KeyKind::Slash)] == "slash")
        add(L"斜杠分给直接紧跟式后不再当标点。");
    return notes;
}

std::wstring KeyWarnings(const HelpcodeModes &modes, const std::vector<KeyChoices> &keys)
{
    const auto assigned = [&keys](const char *use) {
        return std::any_of(keys.begin(), keys.end(), [use](const KeyChoices &key) { return key.value == use; });
    };
    std::wstring warnings;
    if (EndTriggerKeysInUse(modes) && !assigned("end"))
        warnings += L"没有键分给末尾引导键，末尾辅助码不会生效。";
    if (modes.mid == MidMode::Trigger && !assigned("mid"))
        warnings += L"没有键分给句中引导键，句中辅助码不会生效。";
    return warnings;
}

void SyncOptions(Dropdown &dropdown, std::vector<DropdownOption> options)
{
    const auto &current = dropdown.GetOptions();
    const bool same = current.size() == options.size() &&
                      std::equal(current.begin(), current.end(), options.begin(),
                                 [](const DropdownOption &a, const DropdownOption &b) { return a.value == b.value; });
    if (!same)
        dropdown.SetOptions(std::move(options));
}

std::shared_ptr<Label> Hint(TextRole role = TextRole::Muted)
{
    auto label = MakeDescription(L"");
    label->SetRole(role);
    return label;
}

// 双拼辅助码: scheme and hints, then the end and the mid-sentence helpcode as two single choices
// that may be on together only as CanCoexist says, then the keys both sides share.
VisualPtr ShuangpinHelpcodeSection(PageContext &context)
{
    SettingsModel *model = &context.model;
    auto column = std::make_shared<Column>(0.0f);
    column->Add(MakeTitleBlock(L"双拼辅助码"));
    column->Add(Spaced(MakeRow(L"双拼辅助码方案", L"", SchemeDropdown(context, "helpcode.shuangpin_helpcode_schema"))));
    column->Add(Spaced(MakeRow(L"在候选窗口中显示双拼辅助码", L"",
                               BindToggle(context, "helpcode.show_sp_helpcode_in_candidate_window"))));
    column->Add(std::make_shared<Divider>());

    // The page and its controls live as long as the window; the sync below simply shares them.
    struct Controls
    {
        std::shared_ptr<SegmentedChoice> end;
        std::shared_ptr<SegmentedChoice> mid;
        std::shared_ptr<Label> endHint;
        std::shared_ptr<Label> midHint;
        std::shared_ptr<Hideable> struckNote;
        std::shared_ptr<Label> notice;
        std::shared_ptr<Hideable> noticeBox;
        std::shared_ptr<Label> legacy;
        std::shared_ptr<Hideable> legacyBox;
        std::shared_ptr<Hideable> keys;
        std::vector<std::shared_ptr<Hideable>> keyRows;
        std::vector<std::shared_ptr<Dropdown>> keyDropdowns;
        std::shared_ptr<Label> keyNotes;
        std::shared_ptr<Hideable> keyNotesBox;
        std::shared_ptr<Label> keyWarnings;
        std::shared_ptr<Hideable> keyWarningsBox;
        // 用引导键进入末尾辅助码：只在句中辅助码选直接紧跟式时生效，其余时候置灰。
        std::shared_ptr<Dimmable> endTrigger;
        // Shown after a choice turned the other side off, until the next choice.
        std::wstring noticeText;
    };
    auto c = std::make_shared<Controls>();

    c->end = std::make_shared<SegmentedChoice>(std::vector<std::wstring>{L"关", L"末尾紧跟式", L"引导键式"});
    c->mid = std::make_shared<SegmentedChoice>(std::vector<std::wstring>{L"关", L"直接紧跟式", L"引导键式"});
    Controls *self = c.get();
    c->end->SetOnSelect(
        [model, self](size_t index) { ChooseEnd(*model, static_cast<EndMode>(index), self->noticeText); });
    c->mid->SetOnSelect(
        [model, self](size_t index) { ChooseMid(*model, static_cast<MidMode>(index), self->noticeText); });

    const auto spacedHint = [](const std::shared_ptr<Label> &label) {
        label->SetMargin({0.0f, 4.0f, 0.0f, 0.0f});
        return label;
    };
    c->endHint = spacedHint(Hint());
    c->midHint = spacedHint(Hint());
    column->Add(MakeRow(L"末尾辅助码", L"", c->end));
    column->Add(c->endHint);
    column->Add(Spaced(MakeRow(L"句中辅助码", L"", c->mid)));
    column->Add(c->midHint);

    const auto box = [&column](const std::shared_ptr<Label> &label) {
        auto hideable = std::make_shared<Hideable>(label, false);
        hideable->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
        column->Add(hideable);
        return hideable;
    };
    auto struck = Hint();
    struck->SetText(L"带反斜杠的选项不能和另一边当前的方式同时开，选它会把另一边切成「关」。");
    c->struckNote = box(struck);
    c->notice = Hint(TextRole::Accent);
    c->noticeBox = box(c->notice);
    c->legacy = Hint(TextRole::Accent);
    c->legacyBox = box(c->legacy);

    // 按键分配
    auto keys = std::make_shared<Column>(0.0f);
    keys->Add(std::make_shared<Divider>());
    keys->Add(MakeRow(L"按键分配", L"末尾和句中共用，每个键只能有一种用途", nullptr));
    for (const KeySpec &key : kKeys)
    {
        Dropdown::Options settings;
        settings.minWidth = 110.0f;
        settings.paddingLeft = 9.0f;
        auto dropdown = std::make_shared<Dropdown>(std::vector<DropdownOption>{KeyOption("none")}, settings);
        const KeySpec *spec = &key;
        dropdown->SetOnChanged(
            [model, spec](size_t, const DropdownOption &option) { ApplyKeyChoice(*model, *spec, option.value); });
        auto row = std::make_shared<Hideable>(MakeRow(key.label, L"", dropdown, true), false);
        row->SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
        keys->Add(row);
        c->keyRows.push_back(row);
        c->keyDropdowns.push_back(dropdown);
    }
    c->keyNotes = Hint();
    c->keyNotesBox = std::make_shared<Hideable>(c->keyNotes, false);
    c->keyNotesBox->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
    keys->Add(c->keyNotesBox);
    c->keyWarnings = Hint(TextRole::Danger);
    c->keyWarningsBox = std::make_shared<Hideable>(c->keyWarnings, false);
    c->keyWarningsBox->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
    keys->Add(c->keyWarningsBox);
    c->keys = std::make_shared<Hideable>(keys, false);
    column->Add(c->keys);

    auto endTrigger = std::make_shared<Column>(0.0f);
    endTrigger->Add(std::make_shared<Divider>());
    endTrigger->Add(MakeRow(L"用引导键进入末尾辅助码",
                            L"句中辅助码选直接紧跟式时，在输入末尾敲末尾引导键（在上面的按键分配里把反引号或分号设成"
                            L"末尾引导键）会删掉各音节上的辅码、整理成纯双拼（如 uixulpb 敲 ; 变成 uiulpb;），再接一到"
                            L"两个辅码按末尾辅助码筛候选；退格删掉这个引导键时恢复原来的输入。前面每一节都要能看成"
                            L"「完整双拼 + 0~2 个辅码」，否则引导键仍当标点。微软、搜狗、紫光双拼里分号跟在字母后面时是"
                            L" ing 韵母，请改用反引号",
                            BindToggle(context, "helpcode.shuangpin_direct_helpcode_end_trigger")));
    c->endTrigger = std::make_shared<Dimmable>(endTrigger);
    column->Add(c->endTrigger);

    context.OnConfig([model, c]() {
        const HelpcodeModes modes = ReadModes(*model);
        c->end->SetSelected(static_cast<size_t>(modes.end));
        c->mid->SetSelected(static_cast<size_t>(modes.mid));
        bool anyStruck = false;
        for (size_t i = 0; i < 3; ++i)
        {
            const bool endStruck = !CanCoexist(static_cast<EndMode>(i), modes.mid);
            const bool midStruck = !CanCoexist(modes.end, static_cast<MidMode>(i));
            c->end->SetStruck(i, endStruck);
            c->mid->SetStruck(i, midStruck);
            anyStruck = anyStruck || endStruck || midStruck;
        }
        c->endHint->SetText(EndHint(modes));
        c->midHint->SetText(MidHint(modes.mid));
        c->struckNote->SetVisible(anyStruck);
        c->notice->SetText(c->noticeText);
        c->noticeBox->SetVisible(!c->noticeText.empty());

        // Only a config from an older version gets here; it keeps working until either side changes.
        const bool legacy = !CanCoexist(modes.end, modes.mid);
        if (legacy)
            c->legacy->SetText(L"沿用旧版本的组合：" + EndName(modes.end) + L"和" + MidName(modes.mid) +
                               L"同时开着，继续按原样生效。现在句中引导键式只能搭配末尾紧跟式，改动末尾或句中任意"
                               L"一边之后，另一边会切成「关」。");
        c->legacyBox->SetVisible(legacy);

        std::vector<KeyChoices> keys;
        bool anyKey = false;
        for (size_t i = 0; i < std::size(kKeys); ++i)
        {
            keys.push_back(ChoicesFor(*model, modes, kKeys[i]));
            const KeyChoices &choices = keys.back();
            const bool shown = IsShown(choices);
            c->keyRows[i]->SetVisible(shown);
            anyKey = anyKey || shown;
            if (!shown)
                continue;
            SyncOptions(*c->keyDropdowns[i], choices.options);
            c->keyDropdowns[i]->SetValue(choices.value);
        }
        c->keys->SetVisible(anyKey);
        const std::wstring notes = KeyNotes(modes, keys);
        c->keyNotes->SetText(notes);
        c->keyNotesBox->SetVisible(!notes.empty());
        const std::wstring warnings = KeyWarnings(modes, keys);
        c->keyWarnings->SetText(warnings);
        c->keyWarningsBox->SetVisible(!warnings.empty());
        c->endTrigger->SetDisabled(modes.mid != MidMode::Direct);
    });
    return column;
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

    page.AddSection(ShuangpinHelpcodeSection(context));

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
