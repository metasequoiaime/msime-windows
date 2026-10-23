#include "settings/native/native_page_help_widgets.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Window.h"

// partials/feedback-settings.html + modules/feedback-settings.ts; styles in
// styles/modules/document-settings.css (.document-hero, .feedback-*, .document-note).
namespace native_settings
{
namespace
{

// `.feedback-icon`: 38px accent-soft tile with a two-letter Consolas label.
class FeedbackIcon : public msimeui::Visual
{
  public:
    explicit FeedbackIcon(std::wstring text) : text_(std::move(text))
    {
    }
    SizeF Measure(const SizeF &) override
    {
        return {38.0f, 38.0f};
    }
    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }
    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        FillRounded(dr, bounds_, 8.0f, p.accentSoft);
        DrawTextInRect(dr, text_, MonoStyle(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), bounds_, p.accentStrong, true);
    }

  private:
    std::wstring text_;
};

// `.feedback-link` / `.feedback-body code`: 12px Consolas, single line with ellipsis.
class MonoLine : public msimeui::Visual
{
  public:
    MonoLine(std::wstring text, std::function<void()> onClick) : onClick_(std::move(onClick))
    {
        layout_.Set(std::move(text), MonoStyle(12.0f));
        SetMargin({0.0f, 9.0f, 0.0f, 0.0f});
    }
    SizeF Measure(const SizeF &availableSize) override
    {
        return {availableSize.width, LineHeight(MonoStyle(12.0f))};
    }
    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }
    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        const bool hot = hovered_ && onClick_;
        IDWriteTextLayout *layout = layout_.Get(bounds_.width);
        if (!layout)
            return;
        const DWRITE_TEXT_RANGE all = {0, static_cast<UINT32>(layout_.Text().size())};
        layout->SetUnderline(hot ? TRUE : FALSE, all);
        // text-overflow: ellipsis
        layout->SetMaxWidth(std::max(bounds_.width, 1.0f));
        if (!trimming_)
        {
            DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            IDWriteInlineObject *sign = nullptr;
            if (SUCCEEDED(DWriteFactory()->CreateEllipsisTrimmingSign(layout, &sign)))
            {
                layout->SetTrimming(&trimming, sign);
                sign->Release();
            }
        }
        ID2D1RenderTarget *target = dr.GetRenderTarget();
        target->PushAxisAlignedClip(ToD2D(bounds_), D2D1_ANTIALIAS_MODE_ALIASED);
        layout_.Draw(dr, {bounds_.x, bounds_.y}, hot ? p.accentStrong : p.textMuted, bounds_.width);
        target->PopAxisAlignedClip();
    }
    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        pressed_ = onClick_ && window_ && HitTest(window_->ClientPixelsToDips(point));
        return pressed_;
    }
    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        if (!pressed_)
            return false;
        pressed_ = false;
        if (window_ && HitTest(window_->ClientPixelsToDips(point)) && onClick_)
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
        return LoadCursor(nullptr, onClick_ ? IDC_HAND : IDC_ARROW);
    }

  private:
    TextLayout layout_;
    std::function<void()> onClick_;
    bool hovered_ = false;
    bool pressed_ = false;
    bool trimming_ = false;
};

CssButton::Style FeedbackActionStyle()
{
    // .feedback-action: padding 7px 11px, radius 6px, 12px inherited font.
    CssButton::Style style;
    style.paddingX = 11.0f;
    style.paddingY = 7.0f;
    style.fontSize = 12.0f;
    return style;
}

// `.document-note`: bold label, then the muted text wrapping beside it (gap 16px).
class DocumentNote : public msimeui::Panel
{
  public:
    DocumentNote(std::wstring title, std::wstring text)
    {
        TextStyle titleStyle = DocStyle(12.0f, DWRITE_FONT_WEIGHT_BOLD, 1.6f);
        title_ = MakeLabel(std::move(title), titleStyle, TextRole::Primary);
        text_ = MakeLabel(std::move(text), DocStyle(12.0f, DWRITE_FONT_WEIGHT_NORMAL, 1.6f, true), TextRole::Muted);
        AddChild(title_);
        AddChild(text_);
    }
    SizeF Measure(const SizeF &availableSize) override
    {
        titleSize_ = title_->MeasureInLayout(availableSize);
        textSize_ = text_->MeasureInLayout(
            {std::max(availableSize.width - titleSize_.width - 16.0f, 0.0f), availableSize.height});
        return {availableSize.width, std::max(titleSize_.height, textSize_.height)};
    }
    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        title_->ArrangeInLayout({finalRect.x, finalRect.y, titleSize_.width, titleSize_.height});
        text_->ArrangeInLayout({finalRect.x + titleSize_.width + 16.0f, finalRect.y,
                                std::max(finalRect.width - titleSize_.width - 16.0f, 0.0f), textSize_.height});
    }
    void Render(msimeui::DeviceResources &dr) override
    {
        title_->Render(dr);
        text_->Render(dr);
    }

  private:
    std::shared_ptr<Label> title_;
    std::shared_ptr<Label> text_;
    SizeF titleSize_ = {};
    SizeF textSize_ = {};
};
} // namespace

VisualPtr BuildFeedbackPage(PageContext &context)
{
    PageBuilder page(L"反馈");
    page.SetTitleRole(TextRole::Secondary);
    SettingsModel *model = &context.model;
    const auto openUrl = [model](const std::string &url) { model->Post("openExternalUrl", Json(url)); };

    auto hero = std::make_shared<PaddedSection>(
        MakeDocumentHero(L"反馈与交流", L"告诉我们你的想法", L"遇到问题或有功能建议时，可以通过以下渠道提交和交流。"),
        Thickness{28.0f, 26.0f, 28.0f, 26.0f});
    page.AddRaw(hero);

    struct Channel
    {
        const wchar_t *icon;
        const wchar_t *title;
        const wchar_t *text;
        const wchar_t *detail;
        const char *url;
        const wchar_t *action;
        const char *copy;
    };
    const Channel channels[] = {
        {L"GH", L"GitHub Issues", L"适合提交可复现的问题、功能建议和开发讨论。",
         L"github.com/metasequoiaime/MSIME-Windows/issues", "https://github.com/metasequoiaime/MSIME-Windows/issues",
         L"查看 Issues", nullptr},
        {L"QQ", L"QQ 交流群", L"适合中文用户进行日常交流、测试反馈和使用讨论。", L"群号：829919142", nullptr,
         L"复制群号", "829919142"},
        {L"TG", L"Telegram 群组", L"面向国际用户和开发者的即时讨论频道。", L"t.me/msimegroup",
         "https://t.me/msimegroup", L"打开群组", nullptr},
    };
    for (const Channel &channel : channels)
    {
        auto body = std::make_shared<Column>(0.0f);
        TextStyle titleStyle = DocStyle(13.0f, static_cast<DWRITE_FONT_WEIGHT>(550));
        body->Add(MakeLabel(channel.title, titleStyle, TextRole::Primary));
        auto paragraph =
            MakeLabel(channel.text, DocStyle(12.0f, DWRITE_FONT_WEIGHT_NORMAL, 1.7f, true), TextRole::Muted);
        paragraph->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
        body->Add(paragraph);
        std::function<void()> linkClick;
        if (channel.url)
        {
            const std::string url = channel.url;
            linkClick = [openUrl, url]() { openUrl(url); };
        }
        body->Add(std::make_shared<MonoLine>(channel.detail, linkClick));

        auto action = std::make_shared<CssButton>(channel.action, FeedbackActionStyle());
        if (channel.copy)
        {
            const std::string copy = channel.copy;
            const std::wstring label = channel.action;
            std::weak_ptr<CssButton> weak = action;
            HWND hwnd = context.host.Hwnd();
            auto timer = std::make_shared<std::unique_ptr<UiTimer>>();
            action->SetOnClick([model, copy, label, weak, hwnd, timer]() {
                model->Post("copyText", Json(copy));
                if (auto button = weak.lock())
                    button->SetText(L"已复制");
                *timer = std::make_unique<UiTimer>(hwnd, 1600, [weak, label, timer]() {
                    if (auto button = weak.lock())
                        button->SetText(label);
                    // One shot; the running tick is a copy, so the timer can go now.
                    timer->reset();
                });
            });
        }
        else
        {
            action->SetOnClick(linkClick);
        }

        auto row = std::make_shared<Row>(18.0f);
        row->Add(std::make_shared<FeedbackIcon>(channel.icon));
        row->Add(body, 1.0f);
        row->Add(action);
        page.AddSection(row);
    }

    page.AddSection(std::make_shared<DocumentNote>(
        L"提交问题时建议附上", L"系统版本、输入方案、复现步骤、相关截图，以及 Debug 输出中的关键日志。"));
    return page.Build();
}
} // namespace native_settings
