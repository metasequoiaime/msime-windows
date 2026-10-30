#include "settings/native/native_page_help_widgets.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Scene.h"
#include "msimeui/Window.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

// 荣誉室：把本地统计结算成一墙成就。页面本身不采集任何数据，只是 stats.db 的展示层。
// 总开关默认关闭，关着时连数据库都不打开，只显示开启引导。
namespace native_settings
{
namespace
{
constexpr float kTileHeight = 100.0f;
constexpr float kTileGap = 12.0f;
constexpr float kGridBottom = 18.0f;
constexpr float kTwoColumnWidth = 480.0f;
constexpr double kPollMs = 5000.0;
constexpr double kMinIntervalMs = 1000.0;
constexpr double kStaleMs = 15000.0;

int64_t JsonInt(const Json &object, const char *key)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number())
        return 0;
    return it->is_number_float() ? static_cast<int64_t>(it->get<double>()) : it->get<int64_t>();
}

std::wstring FormatNumber(int64_t value)
{
    std::wstring digits = std::to_wstring(value);
    const bool negative = !digits.empty() && digits.front() == L'-';
    if (negative)
        digits.erase(digits.begin());
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
        digits.insert(static_cast<size_t>(i), 1, L',');
    return negative ? L"-" + digits : digits;
}

std::wstring FormatDayKey(int64_t day_key)
{
    if (day_key <= 0)
        return L"";
    const int year = static_cast<int>(day_key / 10000);
    const int month = static_cast<int>((day_key / 100) % 100);
    const int day = static_cast<int>(day_key % 100);
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"%04d-%02d-%02d", year, month, day);
    return buffer;
}

// 千分比转百分比：874 -> 87.4%，整数结尾不留 .0。
std::wstring FormatPermille(int64_t permille)
{
    const int64_t whole = permille / 10;
    const int64_t fraction = permille % 10;
    if (fraction == 0)
        return std::to_wstring(whole) + L"%";
    return std::to_wstring(whole) + L"." + std::to_wstring(fraction) + L"%";
}

std::wstring FormatValue(int64_t value, const std::string &display)
{
    if (display == "percent")
        return FormatPermille(value);
    if (display == "speed")
        return FormatNumber(value) + L" 字/分";
    if (display == "days")
        return FormatNumber(value) + L" 天";
    return FormatNumber(value) + L" 字";
}

std::wstring TierGlyph(const std::string &tier)
{
    if (tier == "branch")
        return L"枝";
    if (tier == "ring")
        return L"轮";
    if (tier == "crown")
        return L"参";
    return L"新";
}

TextStyle TitleStyle()
{
    TextStyle style = DocStyle(kFontBody, DWRITE_FONT_WEIGHT_MEDIUM, 0.0f, false);
    return style;
}

struct Tile
{
    std::string tier;
    std::wstring glyph;
    std::wstring title;
    std::wstring note;
    TextLayout description;
    int64_t value = 0;
    int64_t target = 1;
    bool unlocked = false;
    bool sample_ok = true;
    std::string display = "count";
};

// 成就墙。列数跟着内容宽度走，窄窗口退成单列；进度条只在未解锁的格子里出现。
class HonorGrid : public msimeui::Visual
{
  public:
    HonorGrid()
    {
        SetMargin({0.0f, 0.0f, 0.0f, kGridBottom});
    }

    void SetTiles(std::vector<Tile> tiles)
    {
        tiles_ = std::move(tiles);
        InvalidateMeasure();
        InvalidateVisual();
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        const int columns = availableSize.width >= kTwoColumnWidth ? 2 : 1;
        const size_t rows =
            tiles_.empty() ? 0 : (tiles_.size() + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns);
        const float height =
            rows == 0 ? 0.0f : static_cast<float>(rows) * kTileHeight + static_cast<float>(rows - 1) * kTileGap;
        return {availableSize.width, height};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        // 和其他统计卡片一样从 bounds_ 现算格子，不缓存 Arrange 的结果：页面被索引提前构建、
        // 稍后才挂到窗口时，缓存的格子会在第一次绘制时还是空的。
        const int columns = bounds_.width >= kTwoColumnWidth ? 2 : 1;
        const float columnWidth = (bounds_.width - (columns - 1) * kTileGap) / static_cast<float>(std::max(columns, 1));
        for (size_t i = 0; i < tiles_.size(); ++i)
        {
            const size_t row = i / static_cast<size_t>(std::max(columns, 1));
            const size_t column = i % static_cast<size_t>(std::max(columns, 1));
            const RectF box{bounds_.x + column * (columnWidth + kTileGap), bounds_.y + row * (kTileHeight + kTileGap),
                            columnWidth, kTileHeight};
            RenderTile(dr, tiles_[i], box);
        }
    }

  private:
    static void RenderTile(msimeui::DeviceResources &dr, Tile &tile, const RectF &box)
    {
        const Palette &p = CurrentPalette();
        const D2D1_COLOR_F background =
            tile.unlocked ? Over(WithAlpha(p.accent, 0.08f), p.cardBgSolid) : p.surfaceSubtle;
        const D2D1_COLOR_F border = tile.unlocked ? p.accentSoftBorder : p.border;
        FillRounded(dr, box, 10.0f, background);
        StrokeRounded(dr, box, 10.0f, border, 1.0f);

        // 左侧档位徽章：新 / 枝 / 轮 / 参，解锁后填成强调色。
        const PointF center{box.x + 32.0f, box.y + 34.0f};
        const RectF badge{center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
        FillRounded(dr, badge, 16.0f, tile.unlocked ? p.accent : p.surfaceSunken);
        if (tile.unlocked)
            StrokeRounded(dr, badge, 16.0f, WithAlpha(p.accentStrong, 0.6f), 1.0f);
        TextStyle glyphStyle = DocStyle(13.0f, DWRITE_FONT_WEIGHT_MEDIUM);
        glyphStyle.align = DWRITE_TEXT_ALIGNMENT_CENTER;
        DrawTextInRect(dr, tile.glyph, glyphStyle, badge, tile.unlocked ? p.chromeBg : p.textMuted, true);

        const float textLeft = box.x + 58.0f;
        const float textWidth = std::max(box.width - 58.0f - 14.0f, 1.0f);
        DrawTextInRect(dr, tile.title, TitleStyle(), {textLeft, box.y + 8.0f, textWidth, 20.0f},
                       tile.unlocked ? p.text : p.textSecondary);

        tile.description.Draw(dr, {textLeft, box.y + 30.0f}, tile.unlocked ? p.textMuted : p.textSubtle, textWidth);

        // 状态行右对齐：解锁显示日期，未解锁显示当前进度。数字和进度条各占一行，窄栏下也不会互相挤掉。
        const RectF noteRect{textLeft, box.y + 66.0f, textWidth, 18.0f};
        if (tile.unlocked)
        {
            DrawTextInRect(dr, tile.note, DocStyle(kFontCaption), noteRect, p.accent, true);
            return;
        }
        DrawTextInRect(dr, tile.note, DocStyle(kFontCaption), noteRect, tile.sample_ok ? p.textMuted : p.dangerText,
                       true);
        const RectF track{textLeft, box.y + 88.0f, textWidth, 5.0f};
        FillRounded(dr, track, 2.5f, p.surfaceSunken);
        const float fraction =
            tile.target > 0 ? std::min(static_cast<float>(tile.value) / static_cast<float>(tile.target), 1.0f) : 0.0f;
        if (fraction > 0.0f)
            FillRounded(dr, {track.x, track.y, std::max(track.width * fraction, 5.0f), track.height}, 2.5f, p.accent);
    }

    std::vector<Tile> tiles_;
};

struct HonorState
{
    SettingsModel *model = nullptr;
    HostServices *host = nullptr;
    msimeui::Visual *root = nullptr;
    bool enabled = false;
    bool statistics_enabled = false;
    bool limited = false;
    bool has_records = false;
    bool loaded = false;
    bool has_data = false;
    std::string signature;
    bool pending = false;
    double last_request_at = -1e12;
    bool was_visible = false;

    std::shared_ptr<ToggleSwitch> toggle;
    std::shared_ptr<Label> status;
    std::shared_ptr<Hideable> status_holder;
    std::shared_ptr<Hideable> disabled_empty;
    std::shared_ptr<Hideable> limited_empty;
    std::shared_ptr<Hideable> no_data_empty;
    std::shared_ptr<Hideable> content;
    std::shared_ptr<Label> summary;
    std::shared_ptr<HonorGrid> grids[4];
    std::shared_ptr<Hideable> tier_holders[4];
    std::shared_ptr<Label> tier_counts[4];
    std::shared_ptr<Hideable> footer;
    std::unique_ptr<UiTimer> timer;

    void RenderVisibility()
    {
        disabled_empty->SetVisible(!enabled);
        const bool statistics_ready = enabled && statistics_enabled && !limited;
        limited_empty->SetVisible(enabled && !statistics_ready);
        no_data_empty->SetVisible(statistics_ready && loaded && !has_records);
        content->SetVisible(statistics_ready && (!loaded || has_records));
        footer->SetVisible(enabled);
    }

    bool IsVisible() const
    {
        const HWND hwnd = host->Hwnd();
        return root && root->GetParentVisual() && hwnd && IsWindowVisible(hwnd) && !IsIconic(hwnd);
    }

    void Post()
    {
        const std::string request_id = model->NewRequestId("honor");
        pending = true;
        last_request_at = msimeui::NowMs();
        model->Post("honorRequest", Json{{"requestId", request_id}, {"action", "overview"}});
    }

    void Refresh()
    {
        if (!enabled || !statistics_enabled || !IsVisible())
            return;
        const double now = msimeui::NowMs();
        // 丢一条响应不能让页面永久停摆：超过 kStaleMs 就当作这次请求失败了。
        if (pending && now - last_request_at < kStaleMs)
            return;
        if (now - last_request_at < kMinIntervalMs)
            return;
        Post();
    }

    void Tick()
    {
        const bool visible = IsVisible();
        if (visible && (!was_visible || msimeui::NowMs() - last_request_at >= kPollMs))
            Refresh();
        was_visible = visible;
    }

    void ApplyAchievements(const Json &message)
    {
        // 只用稳定部分去重：requestId 和提示语每次都变，带上就等于永不去重。
        const Json empty = Json::array();
        const Json &achievements =
            message.contains("achievements") && message["achievements"].is_array() ? message["achievements"] : empty;
        const std::string next_signature = achievements.dump() + "|" + std::to_string(JsonInt(message, "total")) + "|" +
                                           std::to_string(JsonInt(message, "unlocked"));
        const bool unchanged = has_data && next_signature == signature;
        signature = next_signature;
        has_data = true;
        if (unchanged)
            return;

        static const char *kTiers[4] = {"sprout", "branch", "ring", "crown"};
        std::map<std::string, std::vector<Tile>> grouped;
        for (const Json &entry : achievements)
        {
            Tile tile;
            tile.tier = entry.value("tier", std::string());
            tile.glyph = TierGlyph(tile.tier);
            tile.title = Widen(entry.value("title", std::string()));
            tile.description.Set(Widen(entry.value("description", std::string())),
                                 DocStyle(kFontCaption, DWRITE_FONT_WEIGHT_NORMAL, 1.5f, true));
            tile.value = JsonInt(entry, "value");
            tile.target = std::max<int64_t>(JsonInt(entry, "target"), 1);
            tile.unlocked = entry.value("unlocked", false);
            tile.sample_ok = entry.value("sampleOk", true);
            tile.display = entry.value("display", std::string("count"));
            tile.note = tile.unlocked
                            ? (JsonInt(entry, "unlockedDayKey") > 0
                                   ? L"已解锁 · " + FormatDayKey(JsonInt(entry, "unlockedDayKey"))
                                   : L"已解锁")
                            : FormatValue(tile.value, tile.display) + L" / " + FormatValue(tile.target, tile.display) +
                                  (tile.sample_ok ? L"" : L" · 样本不足");
            grouped[tile.tier].push_back(std::move(tile));
        }

        const int64_t total = JsonInt(message, "total");
        const int64_t unlocked = JsonInt(message, "unlocked");
        const int64_t percent = total > 0 ? (unlocked * 100 + total / 2) / total : 0;
        summary->SetText(L"已解锁 " + std::to_wstring(unlocked) + L" / " + std::to_wstring(total) + L" 项 · " +
                         std::to_wstring(percent) + L"%");
        for (int i = 0; i < 4; ++i)
        {
            const auto &group = grouped[kTiers[i]];
            const int64_t tier_total = static_cast<int64_t>(group.size());
            int64_t tier_unlocked = 0;
            for (const Tile &tile : group)
            {
                if (tile.unlocked)
                    ++tier_unlocked;
            }
            tier_counts[i]->SetText(std::to_wstring(tier_unlocked) + L" / " + std::to_wstring(tier_total));
            const bool has_tiles = !group.empty();
            grids[i]->SetTiles(std::move(grouped[kTiers[i]]));
            tier_holders[i]->SetVisible(has_tiles);
        }
    }

    void ShowStatus(const std::wstring &message)
    {
        status->SetText(message);
        status_holder->SetVisible(!message.empty());
    }
};

CssButton::Style HonorButtonStyle(CssButton::Look look)
{
    CssButton::Style style;
    style.look = look;
    style.fontSize = 13.0f;
    style.paddingX = 16.0f;
    style.paddingY = 7.0f;
    style.radius = 6.0f;
    style.controlBorder = true;
    style.transitionMs = 200.0;
    return style;
}

TextStyle EmptyTitleStyle()
{
    TextStyle style = SectionTitleStyle();
    style.align = DWRITE_TEXT_ALIGNMENT_CENTER;
    return style;
}

TextStyle EmptyTextStyle()
{
    TextStyle style = DocStyle(kFontCaption, DWRITE_FONT_WEIGHT_NORMAL, 1.6f, true);
    style.align = DWRITE_TEXT_ALIGNMENT_CENTER;
    return style;
}

std::shared_ptr<Column> EmptyColumn(const std::wstring &title, const std::wstring &text, VisualPtr action)
{
    auto column = std::make_shared<Column>(0.0f);
    auto heading = std::make_shared<Label>(title, EmptyTitleStyle(), TextRole::Primary);
    heading->SetHorizontalAlignment(msimeui::HorizontalAlignment::Center);
    heading->SetMargin({0.0f, 0.0f, 0.0f, 8.0f});
    column->Add(heading);
    auto body = std::make_shared<Label>(text, EmptyTextStyle(), TextRole::Muted);
    body->SetMaxWidth(520.0f);
    body->SetHorizontalAlignment(msimeui::HorizontalAlignment::Center);
    body->SetMargin({0.0f, 0.0f, 0.0f, 16.0f});
    column->Add(body);
    if (action)
    {
        action->SetHorizontalAlignment(msimeui::HorizontalAlignment::Center);
        column->Add(action);
    }
    return column;
}
} // namespace

VisualPtr BuildHonorPage(PageContext &context)
{
    auto state = std::make_shared<HonorState>();
    state->model = &context.model;
    state->host = &context.host;
    HonorState *s = state.get();

    PageBuilder page(L"荣誉室");

    // 总开关
    state->toggle = std::make_shared<ToggleSwitch>(false);
    state->toggle->SetOnChanged([s](bool on) {
        s->enabled = on;
        s->model->Update("honor.enabled", on);
        s->status->SetText(on ? L"荣誉室已开启，正在结算…" : L"荣誉室已关闭，成就记录仍然保留。");
        s->RenderVisibility();
    });
    page.AddSection(MakeRow(
        L"启用荣誉室", L"把本机输入统计结算成一墙成就；默认关闭，只读取本机数据，不新增采集、不联网", state->toggle));

    state->status = std::make_shared<Label>(L"", DocStyle(kFontCaption), TextRole::Muted);
    state->status->SetMargin({0.0f, 0.0f, 0.0f, 12.0f});
    state->status_holder = std::make_shared<Hideable>(state->status, false);
    page.AddRaw(state->status_holder, 0.0f);

    // 未开启：只显示引导，不读数据库
    {
        auto enable = std::make_shared<CssButton>(L"开启荣誉室", HonorButtonStyle(CssButton::Look::StatsPrimary));
        enable->SetOnClick([s]() {
            s->enabled = true;
            s->toggle->SetChecked(true);
            s->model->Update("honor.enabled", true);
            s->status->SetText(L"正在结算成就…");
            s->RenderVisibility();
        });
        auto section = std::make_shared<PaddedSection>(
            EmptyColumn(L"荣誉室还没开门",
                        L"开启后，这里按累计字数、连续天数、首选命中率等指标结算成就。首次开启会把已有统计一次性结算，"
                        L"之后每次解锁都会记下当天的日期。",
                        enable),
            Thickness{24.0f, 36.0f, 24.0f, 36.0f});
        section->SetMargin({0.0f, 0.0f, 0.0f, 18.0f});
        state->disabled_empty = std::make_shared<Hideable>(section, true);
        page.AddRaw(state->disabled_empty, 0.0f);
    }

    // 荣誉室开了但统计没开
    {
        auto open_stats = std::make_shared<CssButton>(L"开启输入统计", HonorButtonStyle(CssButton::Look::StatsPrimary));
        open_stats->SetOnClick([s]() {
            s->statistics_enabled = true;
            s->model->Update("statistics.enabled", true);
            s->status->SetText(L"已开启输入统计，正常打字后成就开始累积。");
            s->RenderVisibility();
        });
        auto visit_stats = std::make_shared<CssButton>(L"打开统计页", HonorButtonStyle(CssButton::Look::Secondary));
        visit_stats->SetOnClick([s]() { s->host->NavigateTo("stats"); });
        auto actions = std::make_shared<Row>(10.0f);
        actions->SetJustify(Row::Justify::Center);
        actions->Add(open_stats);
        actions->Add(visit_stats);
        auto section = std::make_shared<PaddedSection>(
            EmptyColumn(L"还差一步",
                        L"荣誉室只读取本机输入统计，统计关着就没有可结算的内容。开启统计后，"
                        L"这里会按现有记录补齐已经达成的成就。",
                        actions),
            Thickness{24.0f, 36.0f, 24.0f, 36.0f});
        section->SetMargin({0.0f, 0.0f, 0.0f, 18.0f});
        state->limited_empty = std::make_shared<Hideable>(section, false);
        page.AddRaw(state->limited_empty, 0.0f);
    }

    // 荣誉室开着、统计也开着，但还没攒下可结算的记录
    {
        auto visit_stats = std::make_shared<CssButton>(L"打开统计页", HonorButtonStyle(CssButton::Look::Secondary));
        visit_stats->SetOnClick([s]() { s->host->NavigateTo("stats"); });
        auto section = std::make_shared<PaddedSection>(
            EmptyColumn(L"还没有可以结算的记录",
                        L"正常打字几天后再回来。第一次结算会把已经达成的成就一次性补上，并把解锁日期记成当天。",
                        visit_stats),
            Thickness{24.0f, 36.0f, 24.0f, 36.0f});
        section->SetMargin({0.0f, 0.0f, 0.0f, 18.0f});
        state->no_data_empty = std::make_shared<Hideable>(section, false);
        page.AddRaw(state->no_data_empty, 0.0f);
    }

    // 成就墙
    {
        auto column = std::make_shared<Column>(0.0f);
        state->summary = std::make_shared<Label>(L"正在加载成就…", DocStyle(kFontCaption), TextRole::Muted);
        state->summary->SetMargin({0.0f, 0.0f, 0.0f, 14.0f});
        column->Add(state->summary);

        static const wchar_t *kTierTitles[4] = {L"新芽", L"枝叶", L"年轮", L"参天"};
        static const wchar_t *kTierNotes[4] = {L"刚刚开始写字", L"日常已经顺手", L"写字成了习惯", L"很多年的积累"};
        for (int i = 0; i < 4; ++i)
        {
            auto tier_column = std::make_shared<Column>(0.0f);
            auto header = std::make_shared<Row>(8.0f);
            auto title = std::make_shared<Label>(std::wstring(kTierTitles[i]) + L" · " + kTierNotes[i],
                                                 SectionTitleStyle(), TextRole::Primary);
            IndexSettingTitle(title, kTierTitles[i]);
            header->Add(title, 1.0f);
            state->tier_counts[i] = std::make_shared<Label>(L"", DocStyle(kFontCaption), TextRole::Muted);
            header->Add(state->tier_counts[i]);
            header->SetMargin({0.0f, 0.0f, 0.0f, 12.0f});
            tier_column->Add(header);
            state->grids[i] = std::make_shared<HonorGrid>();
            tier_column->Add(state->grids[i]);
            auto section = std::make_shared<Section>(tier_column);
            section->SetMargin({0.0f, 0.0f, 0.0f, 18.0f});
            state->tier_holders[i] = std::make_shared<Hideable>(section, false);
            column->Add(state->tier_holders[i]);
        }
        state->content = std::make_shared<Hideable>(column, false);
        page.AddRaw(state->content, 0.0f);
    }

    // 说明
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(MakeTitle(L"数据与隐私"));
        column->Add(
            MakeDescription(L"成就依据本机 stats.db 里的累计字数、活跃时长与首选命中率结算，不读取输入内容，不联网。"
                            L"保留策略清理旧统计后，已解锁的成就仍然保留；成就本身也只是一条编号和解锁日期。"));
        auto section = std::make_shared<Section>(column);
        section->SetMargin({0.0f, 0.0f, 0.0f, 18.0f});
        state->footer = std::make_shared<Hideable>(section, false);
        page.AddRaw(state->footer, 0.0f);
    }

    std::weak_ptr<HonorState> weak = state;
    context.OnConfig([weak]() {
        auto self = weak.lock();
        if (!self)
            return;
        self->enabled = self->model->GetBool("honor.enabled");
        self->statistics_enabled = self->model->GetBool("statistics.enabled");
        self->toggle->SetChecked(self->enabled);
        self->RenderVisibility();
        self->Refresh();
    });

    context.OnMessage("honorResponse", [weak](const Json &message) {
        auto self = weak.lock();
        if (!self)
            return;
        self->pending = false;
        if (!message.value("ok", false))
        {
            self->ShowStatus(Widen(message.value("message", std::string("荣誉室加载失败"))));
            return;
        }
        self->limited = message.value("limited", false);
        self->has_records = message.value("hasData", false);
        self->loaded = true;
        self->RenderVisibility();
        // 成功时页面本身已经说明状态，不再占一行显示“已加载荣誉室”。
        self->ShowStatus(L"");
        if (self->has_records)
            self->ApplyAchievements(message);
    });

    auto root = std::make_shared<OwnerBox>(page.Build(), state);
    state->root = root.get();
    state->timer = std::make_unique<UiTimer>(context.host.Hwnd(), 1000, [s]() { s->Tick(); });
    return root;
}
} // namespace native_settings
