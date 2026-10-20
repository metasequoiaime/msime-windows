#include "settings/native/native_page_dictionary_panel.h"

#include "msimeui/Compat.h"
#include "msimeui/DeviceResources.h"
#include "msimeui/Window.h"

#include <shobjidl.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace native_settings
{
namespace
{
constexpr size_t kNone = static_cast<size_t>(-1);
constexpr size_t kPageSize = 100; // DICTIONARY_PAGE_SIZE

// Table cells inherit the body's 16px.
TextStyle CellStyle()
{
    TextStyle style;
    style.size = kFontDefault;
    return style;
}

TextStyle HeaderStyle()
{
    TextStyle style = CellStyle();
    style.weight = DWRITE_FONT_WEIGHT_MEDIUM;
    return style;
}

// `.dict-row-action` is a <button> without `font: inherit`: system font at 13.333px.
TextStyle ActionStyle()
{
    TextStyle style = ControlStyle();
    style.size = 13.333f;
    return style;
}

// Single-line text clipped with an ellipsis (text-overflow: ellipsis).
void DrawCell(msimeui::DeviceResources &dr, const std::wstring &text, const TextStyle &style, const RectF &rect,
              const D2D1_COLOR_F &color, bool center)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    IDWriteFactory *factory = DWriteFactory();
    if (!target || !factory || text.empty() || rect.width <= 0.0f)
        return;
    ComPtr<IDWriteTextFormat> format;
    const std::wstring &family = style.family.empty() ? UiFontFamily() : style.family;
    if (FAILED(factory->CreateTextFormat(family.c_str(), nullptr, style.weight, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, style.size, L"zh-cn", format.GetAddressOf())))
        return;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ComPtr<IDWriteInlineObject> ellipsis;
    if (!center && SUCCEEDED(factory->CreateEllipsisTrimmingSign(format.Get(), ellipsis.GetAddressOf())))
    {
        DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        format->SetTrimming(&trimming, ellipsis.Get());
    }
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), rect.width,
                                         rect.height, layout.GetAddressOf())))
        return;
    if (ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(color))
        target->DrawTextLayout(D2D1::Point2F(rect.x, rect.y), layout.Get(), brush,
                               D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

msimeui::ScrollViewer::Appearance TableScrollbar()
{
    const Palette &p = CurrentPalette();
    msimeui::ScrollViewer::Appearance appearance;
    appearance.reservedWidth = 8.0f;
    appearance.trackWidth = 8.0f;
    appearance.thumbWidth = 8.0f;
    appearance.cornerRadius = 2.0f;
    appearance.drawTrack = false;
    appearance.thumbMinLength = 24.0f;
    appearance.thumbColor = p.scrollbarThumb;
    appearance.thumbHoverColor = p.scrollbarThumbHover;
    appearance.hoverThumbOnly = true;
    appearance.wheelStep = 100.0f;
    appearance.smoothScrollMs = 260.0;
    return appearance;
}

std::wstring DecodeDictionaryBytes(const std::string &bytes, bool &ok)
{
    ok = true;
    const auto widen = [&](const char *data, int size, UINT codePage, DWORD flags, std::wstring &out) {
        if (size == 0)
        {
            out.clear();
            return true;
        }
        const int length = MultiByteToWideChar(codePage, flags, data, size, nullptr, 0);
        if (length <= 0)
            return false;
        out.assign(static_cast<size_t>(length), L'\0');
        return MultiByteToWideChar(codePage, flags, data, size, out.data(), length) == length;
    };
    const unsigned char *raw = reinterpret_cast<const unsigned char *>(bytes.data());
    const int size = static_cast<int>(bytes.size());
    std::wstring text;
    if (size >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF)
    {
        ok = widen(bytes.data() + 3, size - 3, CP_UTF8, MB_ERR_INVALID_CHARS, text);
        return text;
    }
    if (size >= 2 && ((raw[0] == 0xFF && raw[1] == 0xFE) || (raw[0] == 0xFE && raw[1] == 0xFF)))
    {
        const bool bigEndian = raw[0] == 0xFE;
        for (int i = 2; i + 1 < size; i += 2)
        {
            const wchar_t unit = bigEndian ? static_cast<wchar_t>((raw[i] << 8) | raw[i + 1])
                                           : static_cast<wchar_t>(raw[i] | (raw[i + 1] << 8));
            text.push_back(unit);
        }
        return text;
    }
    if (widen(bytes.data(), size, CP_UTF8, MB_ERR_INVALID_CHARS, text))
        return text;
    if (widen(bytes.data(), size, 54936 /* GB18030 */, 0, text))
        return text;
    widen(bytes.data(), size, CP_UTF8, 0, text);
    return text;
}

std::wstring Trim(const std::wstring &text)
{
    const size_t first = text.find_first_not_of(L" \t\r\n　");
    if (first == std::wstring::npos)
        return {};
    const size_t last = text.find_last_not_of(L" \t\r\n　");
    return text.substr(first, last - first + 1);
}

// Number(value) of the page: empty is 0, anything that is not an integer is not sent.
std::optional<int64_t> ParseWeight(const std::wstring &text)
{
    const std::wstring trimmed = Trim(text);
    if (trimmed.empty())
        return 0;
    try
    {
        size_t used = 0;
        const double value = std::stod(trimmed, &used);
        if (used != trimmed.size() || !std::isfinite(value) || std::floor(value) != value)
            return std::nullopt;
        return static_cast<int64_t>(value);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::wstring DictionaryLabel(const std::string &dictionary)
{
    if (dictionary == "quanpin")
        return L"全拼词库";
    if (dictionary == "wubi")
        return L"五笔词库";
    if (dictionary == "english")
        return L"英文词库";
    return {};
}
} // namespace

// ---------------------------------------------------------------------------------
// PageCard
// ---------------------------------------------------------------------------------
PageCard::PageCard(VisualPtr child, Style style, Thickness padding)
    : msimeui::Container(std::move(child)), style_(style), inner_(padding)
{
}

SizeF PageCard::Measure(const SizeF &availableSize)
{
    const float border = CssBorder();
    padding_ = {inner_.left + border, inner_.top + border, inner_.right + border, inner_.bottom + border};
    return msimeui::Container::Measure(availableSize);
}

void PageCard::Render(msimeui::DeviceResources &dr)
{
    const Palette &p = CurrentPalette();
    const bool dict = style_ == Style::DictCard;
    const float radius = dict ? 12.0f : 8.0f;
    msimeui::ShadowPass passes[2] = {};
    for (int i = 0; i < p.cardShadowCount; ++i)
        passes[i] = {p.cardShadow[i].sigma, p.cardShadow[i].alpha, 0.0f, p.cardShadow[i].offsetY};
    shadow_.Render(dr.GetRenderTarget(), bounds_, radius, passes, static_cast<size_t>(p.cardShadowCount));
    FillRounded(dr, bounds_, radius, dict ? p.cardBgSolid : Over(p.cardBg, p.chromeBg));
    StrokeRounded(dr, bounds_, radius, dict ? p.borderStrong : p.border, 1.0f);
    msimeui::Container::Render(dr);
}

// ---------------------------------------------------------------------------------
// DictInput
// ---------------------------------------------------------------------------------
DictInput::DictInput(float fontSize, std::wstring placeholder)
{
    box_ = std::make_shared<msimeui::TextBox>(42.0f, std::move(placeholder));
    box_->SetMultiline(false);
    box_->SetFontSize(fontSize);
    box_->SetPlaceholderFontSize(fontSize);
    SetChild(box_);
}

std::wstring DictInput::GetText() const
{
    return box_->GetText();
}

void DictInput::SetText(const std::wstring &text)
{
    box_->SetText(text);
}

void DictInput::SetPlaceholder(std::wstring placeholder)
{
    box_->SetPlaceholderText(std::move(placeholder));
}

void DictInput::SetOnSubmit(std::function<void()> handler)
{
    box_->SetOnSubmit(std::move(handler));
}

void DictInput::ApplyAppearance(bool focused)
{
    const Palette &p = CurrentPalette();
    const int state = (focused ? 2 : 0) | (p.light ? 1 : 0);
    if (state == appliedState_)
        return;
    appliedState_ = state;
    msimeui::TextBox::Appearance appearance;
    appearance.cornerRadius = 8.0f;
    appearance.borderWidth = CssBorder();
    // padding: 0 13px inside the 1px border
    appearance.paddingX = 13.0f + CssBorder();
    appearance.paddingY = 0.0f;
    appearance.fill = focused ? p.controlBg : p.surfaceSunken;
    appearance.border = p.controlBorder;
    appearance.focusBorder = p.accent;
    appearance.text = p.text;
    appearance.selection = WithAlpha(p.accent, 0.45f);
    appearance.placeholder = p.textSubtle;
    appearance.fontFamily = UiFontFamily();
    box_->SetAppearance(appearance);
}

SizeF DictInput::Measure(const SizeF &availableSize)
{
    box_->MeasureInLayout(availableSize);
    return {availableSize.width, 42.0f};
}

void DictInput::Render(msimeui::DeviceResources &dr)
{
    const bool focused = box_->IsFocused();
    ApplyAppearance(focused);
    if (focused)
    {
        // box-shadow: 0 0 0 2px color-mix(in srgb, var(--accent-color) 22%, transparent)
        FillRounded(dr, Inflate(bounds_, 2.0f), 10.0f, WithAlpha(CurrentPalette().accent, 0.22f));
    }
    box_->Render(dr);
}

std::shared_ptr<Label> MakeDialogTitle(const std::wstring &title)
{
    TextStyle style;
    style.size = 20.0f;
    style.weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
    auto label = std::make_shared<Label>(title, style);
    // margin-bottom 20px collapses with the first field's 14px margin-top.
    label->SetMargin({0.0f, 0.0f, 0.0f, 6.0f});
    return label;
}

VisualPtr MakeDialogField(const std::wstring &label, const std::shared_ptr<DictInput> &input)
{
    auto column = std::make_shared<Column>(7.0f);
    column->SetMargin({0.0f, 14.0f, 0.0f, 0.0f});
    column->Add(std::make_shared<Label>(label, BodyStyle(), TextRole::Secondary));
    column->Add(input);
    return column;
}

// ---------------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------------
std::array<float, 6> DictColumns::Edges(float width) const
{
    const float percent = (percents[0] + percents[1] + percents[2]) * width;
    const float leftover = std::max(width - 64.0f - 124.0f - percent, 0.0f);
    const float index = 64.0f + leftover * 64.0f / 188.0f;
    const float action = 124.0f + leftover * 124.0f / 188.0f;
    std::array<float, 6> edges{};
    edges[0] = 0.0f;
    edges[1] = index;
    edges[2] = edges[1] + percents[0] * width;
    edges[3] = edges[2] + percents[1] * width;
    edges[4] = edges[3] + percents[2] * width;
    edges[5] = std::max(edges[4] + action, width);
    return edges;
}

DictTableHeader::DictTableHeader(DictColumns columns) : columns_(columns)
{
}

void DictTableHeader::SetLabels(std::array<std::wstring, 4> labels)
{
    labels_ = std::move(labels);
    InvalidateVisual();
}

void DictTableHeader::SetScrollbarReserve(float width)
{
    if (reserve_ == width)
        return;
    reserve_ = width;
    InvalidateVisual();
}

SizeF DictTableHeader::Measure(const SizeF &availableSize)
{
    // border-top of the wrap + 13px padding + line + 13px padding + border-bottom
    return {availableSize.width, CssBorder() + 13.0f + LineHeight(HeaderStyle()) + 13.0f + CssBorder()};
}

void DictTableHeader::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void DictTableHeader::Render(msimeui::DeviceResources &dr)
{
    const Palette &p = CurrentPalette();
    const float border = CssBorder();
    FillRect(dr, bounds_, p.tableHeaderBg);
    FillRect(dr, {bounds_.x, bounds_.y, bounds_.width, border}, p.border);
    FillRect(dr, {bounds_.x, bounds_.y + bounds_.height - border, bounds_.width - reserve_, border}, p.tableBorder);
    const float width = std::max(bounds_.width - reserve_, 0.0f);
    const auto edges = columns_.Edges(width);
    const RectF row = {bounds_.x, bounds_.y + border, width, bounds_.height - border * 2.0f};
    DrawCell(dr, L"No.", HeaderStyle(), {row.x + edges[0] + 10.0f, row.y, edges[1] - edges[0] - 20.0f, row.height},
             p.textMuted, true);
    for (size_t i = 0; i < 4; ++i)
    {
        // th:last-child { padding-left: 26px }
        const float left = row.x + edges[i + 1] + (i == 3 ? 26.0f : 18.0f);
        const float right = row.x + edges[i + 2] - 18.0f;
        DrawCell(dr, labels_[i], HeaderStyle(), {left, row.y, std::max(right - left, 0.0f), row.height}, p.textMuted,
                 false);
    }
}

DictTableBody::DictTableBody(DictColumns columns) : columns_(columns)
{
}

void DictTableBody::SetRows(std::vector<std::array<std::wstring, 3>> rows, size_t firstIndex)
{
    rows_ = std::move(rows);
    firstIndex_ = firstIndex;
    hovered_ = pressed_ = {kNone, false};
    InvalidateMeasure();
    InvalidateVisual();
}

void DictTableBody::SetEmpty(std::wstring message)
{
    rows_.clear();
    empty_ = std::move(message);
    hovered_ = pressed_ = {kNone, false};
    InvalidateMeasure();
    InvalidateVisual();
}

void DictTableBody::SetOnAction(ActionHandler handler)
{
    onAction_ = std::move(handler);
}

float DictTableBody::RowHeight() const
{
    return 13.0f + LineHeight(CellStyle()) + 13.0f + CssBorder();
}

SizeF DictTableBody::Measure(const SizeF &availableSize)
{
    if (rows_.empty())
        return {availableSize.width, 48.0f + LineHeight(CellStyle()) + 48.0f + CssBorder()};
    return {availableSize.width, RowHeight() * static_cast<float>(rows_.size())};
}

void DictTableBody::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

RectF DictTableBody::ActionRect(size_t row, bool remove) const
{
    const auto edges = columns_.Edges(bounds_.width);
    const float lineHeight = LineHeight(ActionStyle());
    const float editWidth = MeasureTextWidth(L"编辑", ActionStyle()) + 16.0f;
    const float removeWidth = MeasureTextWidth(L"删除", ActionStyle()) + 16.0f;
    const float height = lineHeight + 8.0f;
    const float top = bounds_.y + RowHeight() * static_cast<float>(row);
    const float y = top + (RowHeight() - CssBorder() - height) * 0.5f;
    const float x = bounds_.x + edges[4] + 18.0f + (remove ? editWidth : 0.0f);
    return {x, y, remove ? removeWidth : editWidth, height};
}

void DictTableBody::Render(msimeui::DeviceResources &dr)
{
    const Palette &p = CurrentPalette();
    const float border = CssBorder();
    if (rows_.empty())
    {
        const float height = bounds_.height;
        DrawCell(dr, empty_, CellStyle(), {bounds_.x + 48.0f, bounds_.y, bounds_.width - 96.0f, height - border},
                 p.textSubtle, true);
        FillRect(dr, {bounds_.x, bounds_.y + height - border, bounds_.width, border}, p.tableBorder);
        return;
    }
    const auto edges = columns_.Edges(bounds_.width);
    const float rowHeight = RowHeight();
    for (size_t r = 0; r < rows_.size(); ++r)
    {
        const RectF row = {bounds_.x, bounds_.y + rowHeight * static_cast<float>(r), bounds_.width, rowHeight};
        if (!dr.IsVisible(row))
            continue;
        const float textHeight = rowHeight - border;
        DrawCell(dr, std::to_wstring(firstIndex_ + r + 1), CellStyle(),
                 {row.x + edges[0] + 10.0f, row.y, edges[1] - edges[0] - 20.0f, textHeight}, p.textMuted, true);
        for (size_t c = 0; c < 3; ++c)
        {
            const float left = row.x + edges[c + 1] + 18.0f;
            const float right = row.x + edges[c + 2] - 18.0f;
            DrawCell(dr, rows_[r][c], CellStyle(), {left, row.y, std::max(right - left, 0.0f), textHeight}, p.text,
                     false);
        }
        for (bool remove : {false, true})
        {
            const RectF action = ActionRect(r, remove);
            const bool hot = hovered_.first == r && hovered_.second == remove;
            D2D1_COLOR_F color = p.textMuted;
            if (hot)
                color = remove ? p.dangerHover : p.text;
            DrawCell(dr, remove ? L"删除" : L"编辑", ActionStyle(), action, color, true);
        }
        FillRect(dr, {row.x, row.y + rowHeight - border, row.width, border}, p.tableBorder);
    }
}

std::pair<size_t, bool> DictTableBody::HitAction(const PointF &point) const
{
    if (rows_.empty() || !Contains(bounds_, point))
        return {kNone, false};
    const size_t row = static_cast<size_t>((point.y - bounds_.y) / RowHeight());
    if (row >= rows_.size())
        return {kNone, false};
    for (bool remove : {false, true})
    {
        if (Contains(ActionRect(row, remove), point))
            return {row, remove};
    }
    return {kNone, false};
}

bool DictTableBody::OnMouseMove(const POINT &point, WPARAM)
{
    if (!window_)
        return false;
    const auto hit = HitAction(window_->ClientPixelsToDips(point));
    if (hit != hovered_)
    {
        hovered_ = hit;
        InvalidateVisual();
    }
    return true;
}

void DictTableBody::OnMouseLeave()
{
    if (hovered_.first != kNone)
    {
        hovered_ = {kNone, false};
        InvalidateVisual();
    }
}

bool DictTableBody::OnMouseDown(const POINT &point, WPARAM)
{
    if (!window_)
        return false;
    pressed_ = HitAction(window_->ClientPixelsToDips(point));
    return pressed_.first != kNone;
}

bool DictTableBody::OnMouseUp(const POINT &point, WPARAM)
{
    if (pressed_.first == kNone || !window_)
        return false;
    const auto hit = HitAction(window_->ClientPixelsToDips(point));
    const auto pressed = pressed_;
    pressed_ = {kNone, false};
    if (hit == pressed && onAction_)
        onAction_(pressed.first, pressed.second);
    return true;
}

HCURSOR DictTableBody::GetCursor() const
{
    return LoadCursor(nullptr, hovered_.first != kNone ? IDC_HAND : IDC_ARROW);
}

DictTableScroll::DictTableScroll(std::shared_ptr<DictTableBody> body, HostServices &host)
    : body_(std::move(body)), host_(host)
{
    scroll_ = std::make_shared<msimeui::ScrollViewer>(body_);
    scroll_->SetAppearance(TableScrollbar());
    AdoptChild(scroll_);
}

float DictTableScroll::MaxHeight() const
{
    // max-height: clamp(180px, calc(100vh - 350px), 550px)
    RECT client{};
    GetClientRect(host_.Hwnd(), &client);
    const float scale = static_cast<float>(msimeui::compat::GetWindowDpi(host_.Hwnd())) / 96.0f;
    const float viewport = static_cast<float>(client.bottom - client.top) / std::max(scale, 0.01f);
    return std::clamp(viewport - 350.0f, 180.0f, 550.0f);
}

bool DictTableScroll::HasScrollbar() const
{
    return contentHeight_ > bounds_.height + 0.5f;
}

void DictTableScroll::ScrollToTop()
{
    scroll_->SetScrollOffset(0.0f);
}

SizeF DictTableScroll::Measure(const SizeF &availableSize)
{
    contentHeight_ = body_->MeasureInLayout({availableSize.width, 100000.0f}).height;
    const float height = std::min(contentHeight_, MaxHeight());
    // Only when the theme changed: SetAppearance invalidates the measure pass that is running.
    if (appliedLight_ != static_cast<int>(CurrentPalette().light))
    {
        appliedLight_ = static_cast<int>(CurrentPalette().light);
        scroll_->SetAppearance(TableScrollbar());
    }
    scroll_->MeasureInLayout({availableSize.width, height});
    return {availableSize.width, height};
}

void DictTableScroll::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    scroll_->ArrangeInLayout(finalRect);
}

void DictTableScroll::Render(msimeui::DeviceResources &dr)
{
    scroll_->Render(dr);
}

void DictTableScroll::Attach(msimeui::Window *window)
{
    msimeui::Visual::Attach(window);
    scroll_->Attach(window);
}

msimeui::Visual *DictTableScroll::FindVisualAt(const PointF &point)
{
    return Contains(bounds_, point) ? scroll_->FindVisualAt(point) : nullptr;
}

msimeui::Visual *DictTableScroll::FindFocusableAt(const PointF &point)
{
    return Contains(bounds_, point) ? scroll_->FindFocusableAt(point) : nullptr;
}

// ---------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------
std::optional<std::wstring> PickAndReadTextFile(HWND owner, bool allowYaml, bool &readFailed)
{
    readFailed = false;
    std::filesystem::path selected;
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return std::nullopt;
    const COMDLG_FILTERSPEC withYaml[] = {{L"词库文件 (*.txt;*.yaml;*.yml)", L"*.txt;*.yaml;*.yml"},
                                          {L"所有文件", L"*.*"}};
    const COMDLG_FILTERSPEC textOnly[] = {{L"文本文件 (*.txt)", L"*.txt"}, {L"所有文件", L"*.*"}};
    if (allowYaml)
        dialog->SetFileTypes(ARRAYSIZE(withYaml), withYaml);
    else
        dialog->SetFileTypes(ARRAYSIZE(textOnly), textOnly);
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    ComPtr<IShellItem> item;
    if (FAILED(dialog->Show(owner)) || FAILED(dialog->GetResult(&item)))
        return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return std::nullopt;
    selected = path;
    CoTaskMemFree(path);

    std::ifstream file(selected, std::ios::binary);
    if (!file)
    {
        readFailed = true;
        return std::wstring();
    }
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    bool ok = true;
    std::wstring text = DecodeDictionaryBytes(bytes, ok);
    if (!ok)
    {
        readFailed = true;
        return std::wstring();
    }
    return text;
}

bool SaveTextFile(HWND owner, const std::wstring &filename, const std::string &content)
{
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return false;
    const COMDLG_FILTERSPEC filters[] = {{L"文本文件 (*.txt)", L"*.txt"}, {L"所有文件", L"*.*"}};
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    dialog->SetDefaultExtension(L"txt");
    dialog->SetFileName(filename.c_str());
    ComPtr<IShellItem> item;
    if (FAILED(dialog->Show(owner)) || FAILED(dialog->GetResult(&item)))
        return false;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return false;
    const std::filesystem::path target = path;
    CoTaskMemFree(path);
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write("\xEF\xBB\xBF", 3);
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(file);
}

// ---------------------------------------------------------------------------------
// DictionaryPanel
// ---------------------------------------------------------------------------------
DictionaryPanel::DictionaryPanel(PageContext &context, Kind kind)
    : context_(context), kind_(kind), dictionary_(kind == Kind::Quick ? "quick" : "quanpin"),
      prefix_(kind == Kind::Quick ? "quick-" : "dict-")
{
    DictColumns columns;
    if (kind_ == Kind::Quick)
        columns.percents = {0.20f, 0.34f, 0.15f};
    search_ = std::make_shared<DictInput>(kFontDefault, kind_ == Kind::Quick ? L"输入编码前缀，留空可查看全部" : L"");
    search_->SetOnSubmit([this]() { Query(0, true); });
    TextStyle hint;
    hint.size = 13.0f;
    hint.wrap = true;
    hint_ = std::make_shared<Label>(
        kind_ == Kind::Quick ? L"编码只能包含英文字母；触发键 K "
                               L"不属于编码内容。批量导入格式为：编码<Tab>短语<Tab>权重（兼容无权重的两列文件）"
                             : L"",
        hint, TextRole::Muted);
    // .dict-hint { padding: 0 22px 16px }
    hint_->SetMargin({22.0f, 0.0f, 22.0f, 16.0f});
    header_ = std::make_shared<DictTableHeader>(columns);
    body_ = std::make_shared<DictTableBody>(columns);
    body_->SetOnAction([this](size_t row, bool remove) {
        if (row >= rows_.size())
            return;
        const Entry data = rows_[row];
        if (!remove)
        {
            OpenDialog(&data);
            return;
        }
        const std::string rowDictionary = dictionary_;
        context_.host.Confirm(L"确定删除“" + Widen(data.word) + L"”吗？", [this, data, rowDictionary](bool confirmed) {
            if (!confirmed)
                return;
            if (rowDictionary == "english")
            {
                Post("delete", {{"dictionary", rowDictionary},
                                {"oldWord", data.word},
                                {"oldDisplay", data.display.empty() ? data.word : data.display},
                                {"word", data.word},
                                {"display", data.display.empty() ? data.word : data.display}});
                return;
            }
            Json payload = {{"dictionary", rowDictionary},
                            {"oldCode", data.code},
                            {"oldWord", data.word},
                            {"code", data.code},
                            {"word", data.word}};
            if (data.hasWeight)
                payload["weight"] = data.weight;
            Post("delete", std::move(payload));
        });
    });
    scroll_ = std::make_shared<DictTableScroll>(body_, context_.host);

    TextStyle status;
    status.size = kFontDefault;
    status_ = std::make_shared<Label>(L"", status);
    previous_ = std::make_shared<Button>(L"上一页", ButtonKind::Neutral);
    next_ = std::make_shared<Button>(L"下一页", ButtonKind::Neutral);
    previous_->SetOnClick([this]() {
        scroll_->ScrollToTop();
        Query(offset_ >= kPageSize ? offset_ - kPageSize : 0, false);
    });
    next_->SetOnClick([this]() {
        scroll_->ScrollToTop();
        Query(offset_ + kPageSize, false);
    });

    context_.OnMessage("dictionaryResponse", [this](const Json &message) { OnResponse(message); });

    if (kind_ == Kind::Quick)
    {
        header_->SetLabels(HeaderLabels());
        body_->SetEmpty(L"点击查询后查看快捷短语");
        SyncPager();
    }
    else
    {
        SetDictionary("quanpin");
    }
}

void DictionaryPanel::AddTo(Column &column)
{
    auto toolbar = std::make_shared<Row>(10.0f);
    // .dict-toolbar { padding: 20px 22px 10px }
    toolbar->SetPadding({22.0f, 20.0f, 22.0f, 10.0f});
    toolbar->Add(search_, 1.0f);
    auto searchButton = std::make_shared<Button>(L"查询", ButtonKind::Neutral);
    searchButton->SetOnClick([this]() { Query(0, true); });
    auto importButton = std::make_shared<Button>(L"批量导入", ButtonKind::Neutral);
    importButton->SetOnClick([this]() { Import("import"); });
    auto addButton = std::make_shared<Button>(kind_ == Kind::Quick ? L"新增短语" : L"新增词条", ButtonKind::Primary);
    addButton->SetOnClick([this]() { OpenDialog(nullptr); });
    toolbar->Add(searchButton);
    toolbar->Add(importButton);
    toolbar->Add(addButton);
    column.Add(toolbar);
    column.Add(hint_);
    column.Add(header_);
    column.Add(scroll_);

    // .dict-pagination { gap: 8px; margin: 12px 0; padding: 0 18px }
    auto pager = std::make_shared<Row>(8.0f);
    pager->SetPadding({18.0f, 0.0f, 18.0f, 0.0f});
    pager->SetMargin({0.0f, 12.0f, 0.0f, 12.0f});
    pager->Add(status_, 1.0f);
    pager->Add(previous_);
    pager->Add(next_);
    column.Add(pager);
}

std::array<std::wstring, 4> DictionaryPanel::HeaderLabels() const
{
    if (kind_ == Kind::Quick)
        return {L"编码", L"快捷短语", L"权重", L"操作"};
    if (dictionary_ == "english")
        return {L"单词", L"显示内容", L"权重", L"操作"};
    return {L"编码", L"词条", L"权重", L"操作"};
}

void DictionaryPanel::SetDictionary(const std::string &dictionary)
{
    dictionary_ = dictionary;
    latestQueryId_.clear();
    lastQuery_.clear();
    offset_ = 0;
    hasMore_ = false;
    loading_ = false;
    status_->SetText(L"");
    SyncPager();
    const bool english = dictionary_ == "english";
    const bool quanpin = dictionary_ == "quanpin";
    search_->SetText(L"");
    search_->SetPlaceholder(english   ? L"输入英文前缀，例如 meta"
                            : quanpin ? L"输入完整全拼，例如 nihao"
                                      : L"输入五笔编码前缀");
    hint_->SetText(
        english ? L"按英文前缀查询；批量导入格式为：单词<Tab>显示内容<Tab>权重（兼容无权重的两列文件）"
        : quanpin ? L"全拼新增会校验拼音合法性、汉字数量和重复词条；批量导入格式为：词语<Tab>全拼[<Tab>权重]（兼容 "
                    L"Rime userdb.txt / dict.yaml，全拼可用空格或 ' 分音节）"
                  : L"管理 86 五笔编码、词条及权重；批量导入格式为：词语<Tab>五笔编码[<Tab>权重]（兼容 Rime "
                    L"dict.yaml）");
    header_->SetLabels(HeaderLabels());
    rows_.clear();
    body_->SetEmpty(L"输入查询条件后查看词条");
    if (onModeChanged_)
        onModeChanged_();
}

void DictionaryPanel::SetOnModeChanged(std::function<void()> handler)
{
    onModeChanged_ = std::move(handler);
}

void DictionaryPanel::Post(const std::string &action, Json data)
{
    const std::string requestId = prefix_ + std::to_string(++counter_);
    std::string target = dictionary_;
    if (data.contains("dictionary") && data["dictionary"].is_string())
        target = data["dictionary"].get<std::string>();
    pending_[requestId] = {action, target};
    if (action == "query")
    {
        latestQueryId_ = requestId;
        loading_ = true;
        status_->SetText(L"查询中…");
        SyncPager();
    }
    data["requestId"] = requestId;
    data["action"] = action;
    data["dictionary"] = target;
    context_.model.Post("dictionaryRequest", std::move(data));
}

void DictionaryPanel::Query(size_t offset, bool fromInput)
{
    if (fromInput || offset == 0)
        lastQuery_ = Trim(search_->GetText());
    if (kind_ == Kind::Dictionary && lastQuery_.empty())
    {
        context_.host.ShowToast(L"请输入查询内容", false);
        return;
    }
    PostQuery(offset);
}

void DictionaryPanel::PostQuery(size_t offset)
{
    Json data = {{"offset", offset}, {"limit", kPageSize}};
    if (kind_ == Kind::Dictionary && dictionary_ == "english")
        data["word"] = Narrow(lastQuery_);
    else
        data["code"] = Narrow(lastQuery_);
    Post("query", std::move(data));
}

void DictionaryPanel::RefreshQuery()
{
    if (kind_ == Kind::Quick)
    {
        PostQuery(offset_);
        return;
    }
    if (!lastQuery_.empty())
        PostQuery(offset_);
}

void DictionaryPanel::SyncPager()
{
    previous_->SetDisabled(loading_ || offset_ == 0);
    next_->SetDisabled(loading_ || !hasMore_);
}

void DictionaryPanel::Import(const std::string &action)
{
    const std::string target = dictionary_;
    bool failed = false;
    const auto content =
        PickAndReadTextFile(context_.host.Hwnd(), kind_ == Kind::Dictionary && action == "import", failed);
    if (!content)
        return;
    if (failed)
    {
        context_.host.ShowToast(L"读取文件失败", false);
        return;
    }
    if (Trim(*content).empty())
    {
        context_.host.ShowToast(L"文件内容为空", false);
        return;
    }
    Post(action, {{"dictionary", target}, {"content", Narrow(*content)}});
}

void DictionaryPanel::Export(const std::string &dictionary)
{
    Post("export", {{"dictionary", dictionary}});
}

void DictionaryPanel::OpenDialog(const Entry *row)
{
    editingExisting_ = row != nullptr;
    editing_ = row ? std::optional<Entry>(*row) : std::nullopt;
    editingDictionary_ = dictionary_;
    const bool english = dictionary_ == "english";
    const bool quick = kind_ == Kind::Quick;
    std::wstring title;
    if (quick)
        title = row ? L"编辑快捷短语" : L"新增快捷短语";
    else
        title = row ? L"编辑词条" : L"新增词条";
    // Labels inherit the dialog's 14px, and so do the inputs (font: inherit).
    dialogCode_ = std::make_shared<DictInput>(kFontBody, L"");
    dialogWord_ = std::make_shared<DictInput>(kFontBody, L"");
    dialogWeight_ = std::make_shared<DictInput>(kFontBody, L"");
    std::wstring codeLabel = L"编码";
    std::wstring wordLabel = quick ? L"快捷短语" : L"词条";
    if (!quick)
    {
        codeLabel = english ? L"单词" : dictionary_ == "quanpin" ? L"全拼" : L"五笔编码";
        wordLabel = english ? L"显示内容" : L"词条";
    }
    if (row)
    {
        dialogCode_->SetText(Widen(english ? row->word : row->code));
        dialogWord_->SetText(Widen(english ? (row->display.empty() ? row->word : row->display) : row->word));
        dialogWeight_->SetText(row->hasWeight ? std::to_wstring(row->weight) : L"10");
    }
    else
    {
        dialogWeight_->SetText(L"10");
    }
    dialogCode_->SetOnSubmit([this]() { SaveDialog(); });
    dialogWord_->SetOnSubmit([this]() { SaveDialog(); });
    dialogWeight_->SetOnSubmit([this]() { SaveDialog(); });

    auto column = std::make_shared<Column>(0.0f);
    column->Add(MakeDialogTitle(title));
    column->Add(MakeDialogField(codeLabel, dialogCode_));
    column->Add(MakeDialogField(wordLabel, dialogWord_));
    column->Add(MakeDialogField(L"权重", dialogWeight_));
    auto actions = std::make_shared<Row>(10.0f);
    actions->SetJustify(Row::Justify::End);
    actions->SetMargin({0.0f, 24.0f, 0.0f, 0.0f});
    auto cancel = std::make_shared<Button>(L"取消", ButtonKind::Neutral);
    auto save = std::make_shared<Button>(L"保存", ButtonKind::Primary);
    cancel->SetOnClick([this]() {
        dialogOpen_ = false;
        context_.host.CloseModal();
    });
    save->SetOnClick([this]() { SaveDialog(); });
    actions->Add(cancel);
    actions->Add(save);
    column->Add(actions);
    dialogOpen_ = true;
    context_.host.ShowModal(column, [this]() { dialogOpen_ = false; });
}

void DictionaryPanel::SaveDialog()
{
    if (!dialogOpen_)
        return;
    const std::wstring code = Trim(dialogCode_->GetText());
    const std::wstring word = Trim(dialogWord_->GetText());
    const auto weight = ParseWeight(dialogWeight_->GetText());
    if (kind_ == Kind::Quick && word.size() > 199)
    {
        context_.host.ShowToast(L"快捷短语不能超过 199 个 wchar 字符", false);
        return;
    }
    // Number('abc') is NaN, which the page posts and the host contract rejects.
    if (!weight)
        return;
    const std::string action = editingExisting_ ? "update" : "create";
    Json data;
    data["dictionary"] = editingDictionary_;
    if (editingDictionary_ == "english")
    {
        data["word"] = Narrow(code);
        data["display"] = Narrow(word);
        data["weight"] = *weight;
        if (editing_)
        {
            data["oldWord"] = editing_->word;
            data["oldDisplay"] = editing_->display.empty() ? editing_->word : editing_->display;
        }
    }
    else
    {
        data["code"] = Narrow(code);
        data["word"] = Narrow(word);
        data["weight"] = *weight;
        if (editing_)
        {
            data["oldCode"] = editing_->code;
            data["oldWord"] = editing_->word;
        }
    }
    Post(action, std::move(data));
}

void DictionaryPanel::RenderRows()
{
    std::vector<std::array<std::wstring, 3>> cells;
    cells.reserve(rows_.size());
    const bool english = kind_ == Kind::Dictionary && dictionary_ == "english";
    for (const Entry &row : rows_)
    {
        const std::wstring weight = std::to_wstring(row.weight);
        if (english)
            cells.push_back({Widen(row.word), Widen(row.display.empty() ? row.word : row.display), weight});
        else
            cells.push_back({Widen(row.code), Widen(row.word), weight});
    }
    if (cells.empty())
        body_->SetEmpty(kind_ == Kind::Quick ? L"没有找到快捷短语" : L"没有找到词条");
    else
        body_->SetRows(std::move(cells), offset_);
}

void DictionaryPanel::OnResponse(const Json &message)
{
    const std::string requestId = message.value("requestId", std::string());
    if (requestId.rfind(prefix_, 0) != 0)
        return;
    const auto found = pending_.find(requestId);
    if (found == pending_.end())
        return;
    const Pending context = found->second;
    pending_.erase(found);
    if (context.action == "query" && requestId != latestQueryId_)
        return;
    if (kind_ == Kind::Dictionary && context.action == "query" && context.dictionary != dictionary_)
        return;
    const bool ok = message.value("ok", false);
    if (context.action == "query")
    {
        loading_ = false;
        if (ok)
        {
            const size_t start = message.contains("offset") && message["offset"].is_number_integer()
                                     ? message["offset"].get<size_t>()
                                     : 0;
            const size_t count = message.contains("rows") && message["rows"].is_array() ? message["rows"].size() : 0;
            offset_ = start;
            hasMore_ = message.value("hasMore", false);
            status_->SetText(count ? L"第 " + std::to_wstring(start + 1) + L"–" + std::to_wstring(start + count) +
                                         L" 条" + (hasMore_ ? L"，后面还有结果" : L"")
                                   : L"没有更多结果");
        }
        else
        {
            status_->SetText(L"查询失败，请重试");
        }
        SyncPager();
    }

    const bool isImport = context.action == "import" || context.action == "importHans";
    const bool isExport = context.action == "export";
    (void)isImport;
    if (isExport && ok && message.contains("content") && message["content"].is_string() &&
        message.contains("filename") && message["filename"].is_string())
    {
        SaveTextFile(context_.host.Hwnd(), Widen(message["filename"].get<std::string>()),
                     message["content"].get<std::string>());
    }
    std::wstring text = message.contains("message") && message["message"].is_string()
                            ? Widen(message["message"].get<std::string>())
                            : (ok ? L"操作成功" : L"操作失败");
    if (kind_ == Kind::Dictionary && context.action != "query" && context.dictionary != dictionary_)
    {
        const std::wstring label = DictionaryLabel(context.dictionary);
        if (!label.empty())
            text = label + L"：" + text;
    }
    context_.host.ShowToast(text, ok);

    if (context.action == "query" && ok)
    {
        rows_.clear();
        for (const Json &item : message["rows"])
        {
            Entry row;
            row.word = item.value("word", std::string());
            row.code = item.value("code", std::string());
            row.display = item.value("display", std::string());
            if (item.contains("weight") && item["weight"].is_number())
            {
                row.weight = item["weight"].get<int64_t>();
                row.hasWeight = true;
            }
            // Quick phrases without a code or weight are not shown (tools-settings.ts).
            if (kind_ == Kind::Quick && (!item.contains("code") || !row.hasWeight))
                continue;
            rows_.push_back(std::move(row));
        }
        RenderRows();
    }
    if (ok && context.action != "query" && !isExport)
    {
        if (kind_ == Kind::Quick)
        {
            if (dialogOpen_)
            {
                dialogOpen_ = false;
                context_.host.CloseModal();
            }
            PostQuery(offset_);
            return;
        }
        const bool fromDialog = context.action == "create" || context.action == "update";
        if (fromDialog && context.dictionary == editingDictionary_ && dialogOpen_)
        {
            dialogOpen_ = false;
            context_.host.CloseModal();
        }
        if (context.dictionary == dictionary_)
            RefreshQuery();
    }
}
} // namespace native_settings
