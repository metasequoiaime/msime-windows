#include "skin/candidate_skin_catalog.h"

#include <toml++/toml.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>

namespace CandidateSkinCatalog
{
namespace
{
// 共享基底来自 msime 的固定主题（crates/client-core/src/theme.rs，上游调整时需同步）；
// Windows 只复用 Fluent 的结构，配色在加载时补进现有槽位。system 跟随宿主，没有调色板。
struct SharedBase
{
    const char *id;
    const char *theme;
    const char *surface;
    const char *text;
    const char *accent;
    const char *number;
};

const SharedBase *FindSharedBase(const std::string &id)
{
    static constexpr SharedBase bases[] = {
        {"system", nullptr, nullptr, nullptr, nullptr, nullptr},
        {"shuishan", "dark", "#2A2B27", "#FFFFFF", "#7FE08E", "#9FB5A3"},
        {"light", "light", "#FFFFFF", "#1A1A1A", "#005FB8", "#6A6F76"},
        {"paper", "light", "#F7F5F0", "#1A1E1B", "#2C7A4B", "#6E6A5E"},
        {"night", "dark", "#16262F", "#E6F1F4", "#4FD1C5", "#86A6B0"},
        {"ink", "dark", "#1A1A1A", "#9A9A9A", "#FFFFFF", "#9A9A9A"},
    };
    for (const auto &base : bases)
    {
        if (id == base.id)
        {
            return &base;
        }
    }
    return nullptr;
}

void FillColor(std::string &color, const std::string &fallback)
{
    if (color.empty())
    {
        color = fallback;
    }
}

// 给十六进制颜色换上指定透明度；清单里的 rgba()、颜色名等无法直接拼接的写法退回基底原色。
std::string WithAlpha(const std::string &color, const char *fallback, const char *alpha)
{
    const bool hex = color.size() > 1 && color.front() == '#' &&
                     std::all_of(color.begin() + 1, color.end(), [](unsigned char ch) { return std::isxdigit(ch); });
    if (hex && (color.size() == 7 || color.size() == 9))
    {
        return color.substr(0, 7) + alpha;
    }
    if (hex && (color.size() == 4 || color.size() == 5))
    {
        return std::string{'#', color[1], color[1], color[2], color[2], color[3], color[3]} + alpha;
    }
    return std::string(fallback) + alpha;
}

void AdaptSharedBase(Package &package)
{
    const SharedBase *base = FindSharedBase(package.base);
    if (!base)
    {
        return;
    }
    // 所有使用方（含设置预览）仍按 Fluent 的结构处理。
    package.base = "fluent";
    if (!base->theme)
    {
        return;
    }
    package.themes.erase(std::remove_if(package.themes.begin(), package.themes.end(),
                                        [base](const std::string &theme) { return theme != base->theme; }),
                         package.themes.end());
    CandidateColors &colors = std::string_view(base->theme) == "light" ? package.light : package.dark;
    FillColor(colors.surface, base->surface);
    FillColor(colors.border, "#0000001F");
    FillColor(colors.text, base->text);
    FillColor(colors.number, base->number);
    FillColor(colors.accent, base->accent);
    // 与上游一致，派生槽位跟随清单覆盖后的 accent / text，而不是基底原色。
    FillColor(colors.selected, WithAlpha(colors.accent, base->accent, "24"));
    FillColor(colors.hover, WithAlpha(colors.text, base->text, "0F"));
    FillColor(colors.selectedText, colors.accent);
    FillColor(colors.selectedNumber, colors.number);
    FillColor(colors.translation, colors.number);
    // 菜单与工具栏默认跟随最终候选配色，各自显式设置的颜色始终优先。
    FillColor(colors.menuBackground, colors.surface);
    FillColor(colors.menuBorder, colors.border);
    FillColor(colors.menuText, colors.text);
    FillColor(colors.menuHover, colors.hover);
    ToolbarColors &toolbar = std::string_view(base->theme) == "light" ? package.toolbarLight : package.toolbarDark;
    FillColor(toolbar.background, colors.surface);
    FillColor(toolbar.border, colors.border);
    FillColor(toolbar.handle, colors.accent);
    FillColor(toolbar.divider, colors.border);
    FillColor(toolbar.icon, colors.text);
    FillColor(toolbar.hover, colors.hover);
}

void SetError(std::string *error, const std::string &message)
{
    if (error)
    {
        *error = message;
    }
}

bool IsSafeRelativeResource(const std::string &name)
{
    if (name.empty() || name.size() > 256 || name.front() == '/' || name.front() == '\\' ||
        name.find('\\') != std::string::npos)
    {
        return false;
    }
    if (!std::all_of(name.begin(), name.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '/' || ch == '.' || ch == '_' || ch == '-';
        }))
    {
        return false;
    }
    std::filesystem::path path(name);
    if (path.is_absolute())
    {
        return false;
    }
    for (const auto &part : path)
    {
        if (part == ".." || part == "." || part.empty())
        {
            return false;
        }
    }
    return true;
}

bool ReadString(const toml::table &table, const char *key, std::string &out, size_t maximum, bool required)
{
    const toml::node *node = table.get(key);
    if (!node)
    {
        return !required;
    }
    const auto *value = node->as_string();
    if (!value)
    {
        return false;
    }
    out = value->get();
    return (!required || !out.empty()) && out.size() <= maximum;
}

bool ReadEnumArray(const toml::table &table, const char *key, const std::vector<std::string> &allowed,
                   std::vector<std::string> &out)
{
    const toml::node *node = table.get(key);
    if (!node)
    {
        return false;
    }
    const auto *array = node->as_array();
    if (!array || array->empty())
    {
        return false;
    }
    for (const auto &item : *array)
    {
        const auto *value = item.as_string();
        if (!value)
        {
            return false;
        }
        const std::string text = value->get();
        if (std::find(allowed.begin(), allowed.end(), text) == allowed.end() ||
            std::find(out.begin(), out.end(), text) != out.end())
        {
            return false;
        }
        out.push_back(text);
    }
    return true;
}

double BoundedNumber(const toml::table &table, const char *key, double maximum)
{
    const toml::node *node = table.get(key);
    if (!node)
    {
        return 0.0;
    }
    if (const auto *floating = node->as_floating_point())
    {
        const double value = floating->get();
        return std::isfinite(value) && value >= 0.0 && value <= maximum ? value : -1.0;
    }
    if (const auto *integer = node->as_integer())
    {
        const double value = static_cast<double>(integer->get());
        return value >= 0.0 && value <= maximum ? value : -1.0;
    }
    return -1.0;
}

// 只覆盖表里写了的边，没写的边保留 out 里的原值；越界或类型不对返回 false。
bool ReadInsetSide(const toml::table &table, const char *key, double maximum, double &out)
{
    if (!table.contains(key))
    {
        return true;
    }
    const double value = BoundedNumber(table, key, maximum);
    if (value < 0.0)
    {
        return false;
    }
    out = value;
    return true;
}

bool ReadEnum(const toml::table &table, const char *key, const std::vector<std::string> &allowed, std::string &out)
{
    if (!table.contains(key))
    {
        return true;
    }
    std::string text;
    if (!ReadString(table, key, text, 32, true) || std::find(allowed.begin(), allowed.end(), text) == allowed.end())
    {
        return false;
    }
    out = text;
    return true;
}

bool ReadResource(const toml::table &table, const char *key, std::string &out)
{
    return !table.contains(key) || (ReadString(table, key, out, 256, true) && IsSafeRelativeResource(out));
}

// 颜色按 CSS 颜色写法解析，只放行颜色值会用到的字符，挡住 `;`、`{}` 之类的写法：同一份 skin.toml
// 在各平台通用，这里不能比别处宽松。
bool ReadCssColor(const toml::table &table, const char *key, std::string &out)
{
    return ReadString(table, key, out, 80, false) && std::all_of(out.begin(), out.end(), [](unsigned char ch) {
               return std::isalnum(ch) || ch == '#' || ch == '(' || ch == ')' || ch == ',' || ch == '.' || ch == '%' ||
                      ch == ' ' || ch == '-' || ch == '/';
           });
}

bool ReadOptionalBounded(const toml::table &table, const char *key, double maximum, std::optional<double> &out);

bool ReadColors(const toml::table *table, CandidateColors &out)
{
    if (!table)
    {
        return true;
    }
    if (!ReadCssColor(*table, "accent", out.accent) || !ReadCssColor(*table, "selected", out.selected) ||
        !ReadCssColor(*table, "hover", out.hover) || !ReadCssColor(*table, "surface", out.surface) ||
        !ReadCssColor(*table, "border", out.border) || !ReadCssColor(*table, "text", out.text) ||
        !ReadCssColor(*table, "number", out.number) || !ReadCssColor(*table, "translation", out.translation) ||
        !ReadCssColor(*table, "translation_pos", out.translationPos) ||
        !ReadCssColor(*table, "translation_separator", out.translationSeparator) ||
        !ReadCssColor(*table, "candidate_text", out.candidateText) ||
        !ReadCssColor(*table, "preedit_text", out.preeditText) ||
        !ReadCssColor(*table, "preedit_caret", out.preeditCaret) ||
        !ReadCssColor(*table, "selected_text", out.selectedText) ||
        !ReadCssColor(*table, "selected_number", out.selectedNumber) ||
        !ReadCssColor(*table, "selected_translation", out.selectedTranslation) ||
        !ReadCssColor(*table, "selected_translation_pos", out.selectedTranslationPos) ||
        !ReadCssColor(*table, "selected_translation_separator", out.selectedTranslationSeparator) ||
        !ReadCssColor(*table, "selected_bar", out.selectedBar) ||
        !ReadCssColor(*table, "preedit_background", out.preeditBackground) ||
        !ReadCssColor(*table, "preedit_divider", out.preeditDivider) ||
        !ReadCssColor(*table, "page_arrow_divider", out.pageArrowDivider))
    {
        return false;
    }
    if (const toml::node *menuNode = table->get("menu"))
    {
        const auto *menu = menuNode->as_table();
        if (!menu || !ReadCssColor(*menu, "background", out.menuBackground) ||
            !ReadCssColor(*menu, "border", out.menuBorder) || !ReadCssColor(*menu, "text", out.menuText) ||
            !ReadCssColor(*menu, "hover", out.menuHover))
        {
            return false;
        }
    }
    if (const toml::node *bar = table->get("show_selected_bar"))
    {
        const auto *flag = bar->as_boolean();
        if (!flag)
        {
            return false;
        }
        out.showSelectedBar = flag->get();
    }
    return ReadOptionalBounded(*table, "backdrop_opacity", 1.0, out.backdropOpacity) &&
           ReadOptionalBounded(*table, "backdrop_luminosity", 1.0, out.backdropLuminosity);
}

// 字体族挡住引号、反斜杠、分号、花括号、尖括号和控制字符，与各平台共用的 skin.toml 规则一致；
// 字体名可以是中文，所以非 ASCII 字节一律放行。
bool ReadFontFamily(const toml::table &table, const char *key, std::string &out)
{
    if (!table.contains(key))
    {
        return true;
    }
    if (!ReadString(table, key, out, 64, true))
    {
        return false;
    }
    return std::all_of(out.begin(), out.end(), [](unsigned char ch) {
        return ch >= 0x80 || (ch >= 0x20 && ch != 0x7f &&
                              std::string_view("\"'\\,;{}<>`").find(static_cast<char>(ch)) == std::string_view::npos);
    });
}

bool ReadOptionalBounded(const toml::table &table, const char *key, double maximum, std::optional<double> &out)
{
    if (!table.contains(key))
    {
        return true;
    }
    const double value = BoundedNumber(table, key, maximum);
    if (value < 0.0)
    {
        return false;
    }
    out = value;
    return true;
}

// v2 的四边数值（切片、边宽）：1、2 或 4 个 0–500 的数，按 CSS 简写展开。键不存在时什么也不做。
bool ReadBoxValues(const toml::table &table, const char *key, std::vector<double> &out)
{
    const toml::node *node = table.get(key);
    if (!node)
    {
        return true;
    }
    const auto *array = node->as_array();
    if (!array || (array->size() != 1 && array->size() != 2 && array->size() != 4))
    {
        return false;
    }
    for (const auto &item : *array)
    {
        const auto number = item.value<double>();
        if (!number || !std::isfinite(*number) || *number < 0.0 || *number > 500.0)
        {
            return false;
        }
        out.push_back(*number);
    }
    return true;
}

// 可正可负的可选数值，缺省保持 out 不变。
bool ReadSignedNumber(const toml::table &table, const char *key, double limit, double &out)
{
    const toml::node *node = table.get(key);
    if (!node)
    {
        return true;
    }
    double value = 0.0;
    if (const auto *floating = node->as_floating_point())
    {
        value = floating->get();
    }
    else if (const auto *integer = node->as_integer())
    {
        value = static_cast<double>(integer->get());
    }
    else
    {
        return false;
    }
    if (!std::isfinite(value) || std::fabs(value) > limit)
    {
        return false;
    }
    out = value;
    return true;
}

bool ReadOptionalBool(const toml::table &table, const char *key, std::optional<bool> &out);

// border_image 的平铺方式：同 CSS border-image-repeat，1 个或 2 个值「横向 纵向」，stretch（默认）/ repeat。
bool ReadBorderImageRepeat(const toml::table &table, BorderImage &out)
{
    const toml::node *node = table.get("repeat");
    if (!node)
    {
        return true;
    }
    std::vector<std::string> values;
    if (const auto *single = node->as_string())
    {
        values.push_back(single->get());
    }
    else if (const auto *array = node->as_array())
    {
        for (const auto &item : *array)
        {
            const auto *value = item.as_string();
            if (!value)
            {
                return false;
            }
            values.push_back(value->get());
        }
    }
    else
    {
        return false;
    }
    if (values.empty() || values.size() > 2)
    {
        return false;
    }
    const auto parse = [](const std::string &value, bool &repeat) {
        if (value == "stretch")
        {
            repeat = false;
            return true;
        }
        if (value == "repeat")
        {
            repeat = true;
            return true;
        }
        return false;
    };
    bool repeatX = false;
    bool repeatY = false;
    if (!parse(values[0], repeatX))
    {
        return false;
    }
    if (values.size() == 1)
    {
        repeatY = repeatX;
    }
    else if (!parse(values[1], repeatY))
    {
        return false;
    }
    out.repeatX = repeatX;
    out.repeatY = repeatY;
    return true;
}

// 读一张九宫格表。base 非空时先继承它的值，表里写了哪项就覆盖哪项——
// [candidate_window.border_image.vertical] 用它只写与横排不同的部分。
bool ReadBorderImage(const toml::table &table, const BorderImage *base, BorderImage &out, std::string *error,
                     const char *where)
{
    if (base)
    {
        out = *base;
    }
    if (table.contains("source") && (!ReadResource(table, "source", out.source) || !IsAnimatableImagePath(out.source)))
    {
        SetError(error, std::string(where) + ".source 无效（需为 .png、.apng、.gif 或 .webp）");
        return false;
    }
    if (out.source.empty())
    {
        SetError(error, std::string(where) + " 缺少 source");
        return false;
    }
    if (table.contains("light_source") &&
        (!ReadResource(table, "light_source", out.lightSource) || !IsAnimatableImagePath(out.lightSource)))
    {
        SetError(error, std::string(where) + ".light_source 无效（需为 .png、.apng、.gif 或 .webp）");
        return false;
    }
    if (table.contains("slice_dip"))
    {
        out.sliceDip.clear();
        if (!ReadBoxValues(table, "slice_dip", out.sliceDip))
        {
            SetError(error, std::string(where) + ".slice_dip 必须是 1、2 或 4 个 0–500 的数值");
            return false;
        }
    }
    if (out.sliceDip.empty())
    {
        SetError(error, std::string(where) + " 缺少 slice_dip");
        return false;
    }
    if (table.contains("stretch_dip"))
    {
        out.stretchDip.clear();
        if (!ReadBoxValues(table, "stretch_dip", out.stretchDip))
        {
            SetError(error, std::string(where) + ".stretch_dip 必须是 1、2 或 4 个 0–500 的数值");
            return false;
        }
    }
    if (table.contains("width_dip"))
    {
        out.widthDip.clear();
        if (!ReadBoxValues(table, "width_dip", out.widthDip))
        {
            SetError(error, std::string(where) + ".width_dip 必须是 1、2 或 4 个 0–500 的数值");
            return false;
        }
    }
    std::optional<bool> fill;
    if (!ReadOptionalBool(table, "fill", fill))
    {
        SetError(error, std::string(where) + ".fill 必须是 true 或 false");
        return false;
    }
    if (fill)
    {
        out.fill = *fill;
    }
    if (!ReadBorderImageRepeat(table, out))
    {
        SetError(error, std::string(where) + ".repeat 必须是 stretch 或 repeat，1 个或 2 个值");
        return false;
    }
    return true;
}

bool ReadOptionalBool(const toml::table &table, const char *key, std::optional<bool> &out)
{
    const toml::node *node = table.get(key);
    if (!node)
    {
        return true;
    }
    const auto *flag = node->as_boolean();
    if (!flag)
    {
        return false;
    }
    out = flag->get();
    return true;
}

// Read via the wide path and parse the text. toml::parse_file(manifest.string()) would run the
// path through the ANSI code page: skins live under the user profile, so a non-ASCII (e.g.
// Chinese) user name corrupts it, and on a code page that cannot represent the characters
// path::string() throws right past the callers' toml handlers.
std::optional<toml::table> ParseManifest(const std::filesystem::path &manifest)
{
    std::ifstream input(manifest, std::ios::binary);
    if (!input)
    {
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return toml::parse(text);
}

bool ReadToolbarColors(const toml::node *node, ToolbarColors &out)
{
    if (!node)
    {
        return true;
    }
    const auto *table = node->as_table();
    return table && ReadCssColor(*table, "background", out.background) && ReadCssColor(*table, "border", out.border) &&
           ReadCssColor(*table, "handle", out.handle) && ReadCssColor(*table, "divider", out.divider) &&
           ReadCssColor(*table, "icon", out.icon) && ReadCssColor(*table, "hover", out.hover);
}

// [candidate_window.decoration] 里的 candidate_overlay_* 一组键；candidate_overlay 不是 true 时其余键不读。
bool ReadDecorationCandidate(const toml::table &decoration, Package &package, std::string *error)
{
    std::optional<bool> enabled;
    if (!ReadOptionalBool(decoration, "candidate_overlay", enabled))
    {
        SetError(error, "candidate_window.decoration.candidate_overlay 必须是 true 或 false");
        return false;
    }
    if (!enabled.value_or(false))
    {
        return true;
    }
    DecorationCandidate overlay;
    std::optional<double> fontSize;
    std::optional<double> minFontSize;
    std::optional<double> maxWidth;
    if (!ReadOptionalBounded(decoration, "candidate_overlay_font_size_dip", 200.0, fontSize) ||
        !ReadOptionalBounded(decoration, "candidate_overlay_min_font_size_dip", 200.0, minFontSize) ||
        (fontSize && *fontSize <= 0.0) || (minFontSize && *minFontSize <= 0.0))
    {
        SetError(error, "candidate_window.decoration 的 candidate_overlay_font_size_dip 与 "
                        "candidate_overlay_min_font_size_dip 必须是大于 0、不超过 200 的数值");
        return false;
    }
    if (!ReadOptionalBounded(decoration, "candidate_overlay_max_width_dip", 2000.0, maxWidth))
    {
        SetError(error, "candidate_window.decoration.candidate_overlay_max_width_dip 必须是 0–2000 的数值");
        return false;
    }
    if (!ReadCssColor(decoration, "candidate_overlay_color", overlay.color))
    {
        SetError(error, "candidate_window.decoration.candidate_overlay_color 无效");
        return false;
    }
    if (!ReadEnum(decoration, "candidate_overlay_align_h", {"left", "center", "right"}, overlay.alignH) ||
        !ReadEnum(decoration, "candidate_overlay_align_v", {"top", "center", "bottom"}, overlay.alignV))
    {
        SetError(error, "candidate_window.decoration 的 candidate_overlay_align_h 只能是 left、center 或 right，"
                        "candidate_overlay_align_v 只能是 top、center 或 bottom");
        return false;
    }
    if (!ReadSignedNumber(decoration, "candidate_overlay_offset_x_dip", kDecorationOffsetLimitDip,
                          overlay.offsetXDip) ||
        !ReadSignedNumber(decoration, "candidate_overlay_offset_y_dip", kDecorationOffsetLimitDip, overlay.offsetYDip))
    {
        SetError(error,
                 "candidate_window.decoration 的 candidate_overlay_offset_x_dip / candidate_overlay_offset_y_dip "
                 "必须在 -2000–2000 之间");
        return false;
    }
    std::optional<bool> stripBadge;
    if (!ReadOptionalBool(decoration, "candidate_overlay_strip_badge", stripBadge))
    {
        SetError(error, "candidate_window.decoration.candidate_overlay_strip_badge 必须是 true 或 false");
        return false;
    }
    if (const toml::node *indexNode = decoration.get("candidate_overlay_index"))
    {
        const auto *index = indexNode->as_integer();
        if (!index || index->get() < 0 || index->get() > 100)
        {
            SetError(error, "candidate_window.decoration.candidate_overlay_index 必须是 0–100 的整数");
            return false;
        }
        overlay.index = static_cast<int>(index->get());
    }
    overlay.fontSizeDip = fontSize.value_or(overlay.fontSizeDip);
    overlay.minFontSizeDip = minFontSize.value_or(overlay.minFontSizeDip);
    overlay.maxWidthDip = maxWidth.value_or(overlay.maxWidthDip);
    overlay.stripBadge = stripBadge.value_or(true);
    package.decorationCandidate = overlay;
    return true;
}

bool IsToolbarImagePath(const std::string &path)
{
    return IsSafeRelativeResource(path) && IsAnimatableImagePath(path);
}

// 按钮框的宽高、图标字号与底图：[toolbar.buttons.<key>] 与 [toolbar.button_defaults] 共用这几项。
bool ReadToolbarButtonLook(const toml::table &table, const std::string &where, ToolbarButton &button,
                           std::string *error)
{
    if (!ReadOptionalBounded(table, "width_dip", 500.0, button.widthDip) ||
        !ReadOptionalBounded(table, "height_dip", 500.0, button.heightDip) ||
        (button.widthDip && *button.widthDip <= 0.0) || (button.heightDip && *button.heightDip <= 0.0))
    {
        SetError(error, where + " 的 width_dip / height_dip 必须是大于 0、不超过 500 的数值");
        return false;
    }
    if (!ReadOptionalBounded(table, "icon_size_dip", 500.0, button.iconSizeDip) ||
        (button.iconSizeDip && *button.iconSizeDip <= 0.0))
    {
        SetError(error, where + ".icon_size_dip 必须是大于 0、不超过 500 的数值");
        return false;
    }
    if (table.contains("background") &&
        (!ReadString(table, "background", button.background, 256, true) || !IsToolbarImagePath(button.background)))
    {
        SetError(error, where + ".background 无效（需为皮肤目录里的 .png、.apng、.gif 或 .webp）");
        return false;
    }
    return true;
}

// [toolbar.buttons.<key>] 的一项。未知的状态名记一条提示后忽略，不让整份皮肤读不出来。
bool ReadToolbarButton(const toml::node &node, const std::string &key, const std::vector<std::string> &states,
                       Package &package, std::string *error)
{
    const std::string where = "toolbar.buttons." + key;
    const auto *table = node.as_table();
    if (!table)
    {
        SetError(error, where + " 必须是表");
        return false;
    }
    ToolbarButton button;
    if (!ReadOptionalBool(*table, "visible", button.visible))
    {
        SetError(error, where + ".visible 必须是 true 或 false");
        return false;
    }
    if (!ReadOptionalBounded(*table, "x_dip", 2000.0, button.xDip) ||
        !ReadOptionalBounded(*table, "y_dip", 2000.0, button.yDip))
    {
        SetError(error, where + " 的 x_dip / y_dip 必须是 0–2000 的数值");
        return false;
    }
    if (!ReadToolbarButtonLook(*table, where, button, error))
    {
        return false;
    }
    if (table->contains("image") &&
        (!ReadString(*table, "image", button.image, 256, true) || !IsToolbarImagePath(button.image)))
    {
        SetError(error, where + ".image 无效（需为皮肤目录里的 .png、.apng、.gif 或 .webp）");
        return false;
    }
    if (const toml::node *imagesNode = table->get("images"))
    {
        const auto *images = imagesNode->as_table();
        if (!images)
        {
            SetError(error, where + ".images 必须是表");
            return false;
        }
        if (states.empty())
        {
            SetError(error, where + " 没有状态之分，只能写 image，不能写 images");
            return false;
        }
        for (const auto &[stateKey, pathNode] : *images)
        {
            const std::string state(stateKey.str());
            const auto *path = pathNode.as_string();
            if (!path || !IsToolbarImagePath(path->get()))
            {
                SetError(error, where + ".images." + state + " 无效（需为皮肤目录里的 .png、.apng、.gif 或 .webp）");
                return false;
            }
            if (std::find(states.begin(), states.end(), state) == states.end())
            {
                package.notices.push_back(where + ".images 里的 " + state + " 不是这个按钮的状态，已忽略");
                continue;
            }
            button.images[state] = path->get();
        }
    }
    package.toolbarButtons[key] = std::move(button);
    return true;
}

// [toolbar.button_flow]：x_dip、y_dip、direction、gap_dip。
bool ReadToolbarButtonFlow(const toml::node &node, Package &package, std::string *error)
{
    const auto *table = node.as_table();
    if (!table)
    {
        SetError(error, "toolbar.button_flow 必须是表");
        return false;
    }
    ToolbarButtonFlow flow;
    std::string direction = "horizontal";
    if (!ReadSignedNumber(*table, "x_dip", 2000.0, flow.xDip) ||
        !ReadSignedNumber(*table, "y_dip", 2000.0, flow.yDip) || flow.xDip < 0.0 || flow.yDip < 0.0)
    {
        SetError(error, "toolbar.button_flow 的 x_dip / y_dip 必须是 0–2000 的数值");
        return false;
    }
    if (!ReadEnum(*table, "direction", {"horizontal", "vertical"}, direction))
    {
        SetError(error, "toolbar.button_flow.direction 只能是 horizontal 或 vertical");
        return false;
    }
    if (!ReadSignedNumber(*table, "gap_dip", 500.0, flow.gapDip) || flow.gapDip < 0.0)
    {
        SetError(error, "toolbar.button_flow.gap_dip 必须是 0–500 的数值");
        return false;
    }
    flow.vertical = direction == "vertical";
    package.toolbarButtonFlow = flow;
    return true;
}

// [toolbar] 的图片模式：mode、image、image_width_dip、image_height_dip 与 [toolbar.buttons.<key>]。
bool ReadToolbarImageMode(const toml::table &toolbar, Package &package, std::string *error)
{
    if (!ReadEnum(toolbar, "mode", {"bar", "image"}, package.toolbarMode))
    {
        SetError(error, "toolbar.mode 只能是 bar 或 image");
        return false;
    }
    if (toolbar.contains("image") &&
        (!ReadString(toolbar, "image", package.toolbarImage, 256, true) || !IsToolbarImagePath(package.toolbarImage)))
    {
        SetError(error, "toolbar.image 无效（需为皮肤目录里的 .png、.apng、.gif 或 .webp）");
        return false;
    }
    std::optional<double> width;
    std::optional<double> height;
    if (!ReadOptionalBounded(toolbar, "image_width_dip", 2000.0, width) ||
        !ReadOptionalBounded(toolbar, "image_height_dip", 2000.0, height))
    {
        SetError(error, "toolbar.image_width_dip / image_height_dip 必须是 0–2000 的数值");
        return false;
    }
    package.toolbarImageWidthDip = width.value_or(0.0);
    package.toolbarImageHeightDip = height.value_or(0.0);
    if (const toml::node *buttonsNode = toolbar.get("buttons"))
    {
        const auto *buttons = buttonsNode->as_table();
        if (!buttons)
        {
            SetError(error, "toolbar.buttons 必须是表");
            return false;
        }
        for (const auto &[keyNode, value] : *buttons)
        {
            const std::string key(keyNode.str());
            const std::vector<std::string> *states = ToolbarButtonStates(key);
            if (!states)
            {
                package.notices.push_back("没有 toolbar.buttons." + key + " 这个按钮，已忽略");
                continue;
            }
            if (!ReadToolbarButton(value, key, *states, package, error))
            {
                return false;
            }
        }
    }
    if (const toml::node *defaultsNode = toolbar.get("button_defaults"))
    {
        const auto *defaults = defaultsNode->as_table();
        if (!defaults)
        {
            SetError(error, "toolbar.button_defaults 必须是表");
            return false;
        }
        if (!ReadToolbarButtonLook(*defaults, "toolbar.button_defaults", package.toolbarButtonDefaults, error))
        {
            return false;
        }
    }
    if (const toml::node *flowNode = toolbar.get("button_flow"))
    {
        if (!ReadToolbarButtonFlow(*flowNode, package, error))
        {
            return false;
        }
    }
    if (package.toolbarMode != "image")
    {
        if (!package.toolbarImage.empty() || !package.toolbarButtons.empty() || package.toolbarButtonFlow ||
            toolbar.contains("button_defaults"))
        {
            package.notices.push_back(
                "toolbar 的 image、buttons、button_defaults 与 button_flow 只在 mode = \"image\" 时生效，已忽略");
        }
        return true;
    }
    // 渲染端按这两个尺寸开窗口，不去读图片的像素大小。
    if (package.toolbarImage.empty() || package.toolbarImageWidthDip <= 0.0 || package.toolbarImageHeightDip <= 0.0)
    {
        SetError(error, "toolbar.mode = \"image\" 需要同时写 image、image_width_dip 与 image_height_dip");
        return false;
    }
    // 没有 button_flow 时，坐标不全的按钮没地方放。
    if (!package.toolbarButtonFlow)
    {
        for (const auto &[key, button] : package.toolbarButtons)
        {
            if (button.xDip.has_value() != button.yDip.has_value())
            {
                package.notices.push_back("toolbar.buttons." + key +
                                          " 只写了 x_dip、y_dip 其中一个，又没有 button_flow，这个按钮不会显示");
            }
        }
    }
    return true;
}

// 长度：0–maximum 的 DIP 数值，或 "<数值>em"（0–maximumEm 倍字号）。键不存在时什么也不做。
bool ReadLength(const toml::table &table, const char *key, double maximum, double maximumEm, std::optional<Length> &out)
{
    const toml::node *node = table.get(key);
    if (!node)
    {
        return true;
    }
    if (const auto *text = node->as_string())
    {
        const std::string &value = text->get();
        if (value.size() < 3 || value.compare(value.size() - 2, 2, "em") != 0)
        {
            return false;
        }
        double number = 0.0;
        const char *first = value.data();
        const char *last = value.data() + value.size() - 2;
        const auto [end, ec] = std::from_chars(first, last, number);
        if (ec != std::errc() || end != last || !std::isfinite(number) || number < 0.0 || number > maximumEm)
        {
            return false;
        }
        out = Length{number, true};
        return true;
    }
    const double number = BoundedNumber(table, key, maximum);
    if (number < 0.0)
    {
        return false;
    }
    out = Length{number, false};
    return true;
}

bool ReadGeometry(const toml::table &table, CandidateGeometry &out)
{
    if (!ReadLength(table, "padding", 64.0, 4.0, out.padding) ||
        !ReadLength(table, "item_gap", 64.0, 4.0, out.itemGap) ||
        !ReadLength(table, "item_extra_height", 64.0, 4.0, out.itemExtraHeight) ||
        !ReadLength(table, "number_inset", 64.0, 4.0, out.numberInset) ||
        !ReadLength(table, "item_padding_right", 64.0, 4.0, out.itemPaddingRight) ||
        !ReadLength(table, "item_min_width", 500.0, 40.0, out.itemMinWidth) ||
        !ReadLength(table, "number_gap", 64.0, 4.0, out.numberGap) ||
        !ReadLength(table, "selected_bar_width", 16.0, 1.0, out.selectedBarWidth) ||
        !ReadOptionalBounded(table, "number_scale", 2.0, out.numberScale) ||
        !ReadOptionalBounded(table, "selected_bar_height_em", 2.0, out.selectedBarHeightEm) ||
        !ReadOptionalBool(table, "selected_bar_inside", out.selectedBarInside) ||
        !ReadOptionalBool(table, "item_corners_follow_card", out.itemCornersFollowCard) ||
        !ReadOptionalBool(table, "justify_rows", out.justifyRows) ||
        !ReadOptionalBool(table, "trim_hidden_preedit_gap", out.trimHiddenPreeditGap) ||
        !ReadEnum(table, "page_arrow_style", {"chevron", "triangle"}, out.pageArrowStyle))
    {
        return false;
    }
    std::vector<double> margin;
    if (!ReadBoxValues(table, "preedit_margin_dip", margin))
    {
        return false;
    }
    if (!margin.empty())
    {
        // 按 CSS 简写展开：1 个值四边相同，2 个值是上下、左右，4 个值是上右下左。
        const double top = margin[0];
        const double right = margin.size() > 1 ? margin[1] : top;
        const double bottom = margin.size() > 2 ? margin[2] : top;
        const double left = margin.size() > 3 ? margin[3] : right;
        out.preeditMarginDip = InsetsDip{top, right, bottom, left};
    }
    return true;
}

// 外部皮肤与内置皮肤清单共用的外观键：[candidate_window] 里的圆角、线宽、阴影与版式（含 horizontal /
// vertical 子表），[candidate.*] 配色，[toolbar] 配色与圆角。只覆盖写了的键，所以内置清单可以叠在出厂外观上读。
bool ReadLook(const toml::table &root, Package &package, std::string *error)
{
    if (const auto *window = root["candidate_window"].as_table())
    {
        if (window->contains("corner_radius_dip"))
        {
            const double radius = BoundedNumber(*window, "corner_radius_dip", 32.0);
            if (radius < 0.0)
            {
                SetError(error, "candidate_window.corner_radius_dip 超出范围");
                return false;
            }
            package.cornerRadiusDip = radius;
        }
        if (!ReadOptionalBounded(*window, "border_width_dip", 4.0, package.borderWidthDip))
        {
            SetError(error, "candidate_window.border_width_dip 超出范围");
            return false;
        }
        if (!ReadOptionalBounded(*window, "item_corner_radius_dip", 16.0, package.itemCornerRadiusDip))
        {
            SetError(error, "candidate_window.item_corner_radius_dip 超出范围");
            return false;
        }
        if (!ReadEnum(*window, "shadow", {"none", "soft", "strong"}, package.shadow))
        {
            SetError(error, "candidate_window.shadow 只能是 none、soft 或 strong");
            return false;
        }
        if (const toml::node *rowsNode = window->get("expand_rows"))
        {
            const auto *rows = rowsNode->as_integer();
            if (!rows || rows->get() < kMinExpandRows || rows->get() > kMaxExpandRows)
            {
                SetError(error, "candidate_window.expand_rows 必须是 2–8 的整数");
                return false;
            }
            package.expandRows = static_cast<int>(rows->get());
        }
        const toml::node *horizontal = window->get("horizontal");
        const toml::node *vertical = window->get("vertical");
        if (!ReadGeometry(*window, package.geometry) || (horizontal && !horizontal->as_table()) ||
            (vertical && !vertical->as_table()) ||
            (horizontal && !ReadGeometry(*horizontal->as_table(), package.horizontalGeometry)) ||
            (vertical && !ReadGeometry(*vertical->as_table(), package.verticalGeometry)))
        {
            SetError(error, "candidate_window 的版式数值无效");
            return false;
        }
    }
    const auto *candidate = root["candidate"].as_table();
    if (candidate && (!ReadColors((*candidate)["dark"].as_table(), package.dark) ||
                      !ReadColors((*candidate)["light"].as_table(), package.light)))
    {
        SetError(error, "candidate 配色无效");
        return false;
    }
    if (const toml::node *toolbarNode = root.get("toolbar"))
    {
        const auto *toolbar = toolbarNode->as_table();
        if (!toolbar || !ReadToolbarColors(toolbar->get("dark"), package.toolbarDark) ||
            !ReadToolbarColors(toolbar->get("light"), package.toolbarLight))
        {
            SetError(error, "toolbar 配色无效");
            return false;
        }
        if (toolbar->contains("corner_radius_dip"))
        {
            const double radius = BoundedNumber(*toolbar, "corner_radius_dip", 32.0);
            if (radius < 0.0)
            {
                SetError(error, "toolbar.corner_radius_dip 超出范围");
                return false;
            }
            package.toolbarCornerRadiusDip = radius;
        }
        if (!ReadOptionalBounded(*toolbar, "icon_corner_radius_dip", 16.0, package.toolbarIconCornerRadiusDip))
        {
            SetError(error, "toolbar.icon_corner_radius_dip 超出范围");
            return false;
        }
    }
    return true;
}

// 内置清单的开关：page_arrows、fit_content、expand_panel。只覆盖写了的键。
bool ReadSwitches(const toml::table &root, DefaultSkin &skin, std::string *error)
{
    const toml::node *windowNode = root.get("candidate_window");
    if (!windowNode)
    {
        return true;
    }
    const auto *window = windowNode->as_table();
    if (!window || !ReadOptionalBool(*window, "page_arrows", skin.pageArrows))
    {
        SetError(error, "candidate_window.page_arrows 必须是 true 或 false");
        return false;
    }
    if (!ReadOptionalBool(*window, "fit_content", skin.fitContent))
    {
        SetError(error, "candidate_window.fit_content 必须是 true 或 false");
        return false;
    }
    if (!ReadOptionalBool(*window, "expand_panel", skin.expandPanel))
    {
        SetError(error, "candidate_window.expand_panel 必须是 true 或 false");
        return false;
    }
    return true;
}

// 出厂清单：构建时从仓库的 skins/default/<id>/skin.toml 生成（见 server/CMakeLists.txt），内容与安装包里的一致。
struct FactoryManifest
{
    const char *id;
    const char *text;
};

constexpr FactoryManifest kFactoryManifests[] = {
#include "builtin_skin_manifests.inc"
};

struct FactoryEntry
{
    Package look;
    DefaultSkin switches;
};

const std::vector<std::pair<std::string, FactoryEntry>> &FactoryEntries()
{
    static const std::vector<std::pair<std::string, FactoryEntry>> entries = [] {
        std::vector<std::pair<std::string, FactoryEntry>> parsed;
        for (const FactoryManifest &manifest : kFactoryManifests)
        {
            FactoryEntry entry;
            entry.switches.id = manifest.id;
            try
            {
                const toml::table root = toml::parse(std::string_view(manifest.text));
                (void)ReadString(root, "name", entry.switches.name, 80, false);
                (void)ReadString(root, "description", entry.switches.description, 500, false);
                // 出厂清单由测试保证有效；万一读不出来，那一项保持渲染器的缺省值，不影响其他皮肤。
                Package look;
                if (ReadLook(root, look, nullptr))
                    entry.look = std::move(look);
                DefaultSkin switches = entry.switches;
                if (ReadSwitches(root, switches, nullptr))
                    entry.switches = std::move(switches);
            }
            catch (const std::exception &)
            {
            }
            entry.look.id = manifest.id;
            entry.look.base = manifest.id;
            parsed.emplace_back(manifest.id, std::move(entry));
        }
        return parsed;
    }();
    return entries;
}

const FactoryEntry *FindFactoryEntry(const std::string &id)
{
    for (const auto &[entryId, entry] : FactoryEntries())
    {
        if (entryId == id)
            return &entry;
    }
    return nullptr;
}

const FactoryEntry &FactoryEntryOrFluent(const std::string &id)
{
    if (const FactoryEntry *entry = FindFactoryEntry(id))
        return *entry;
    if (const FactoryEntry *fluent = FindFactoryEntry("fluent"))
        return *fluent;
    static const FactoryEntry empty;
    return empty;
}
} // namespace

float ResolveLength(const Length &length, float fontSize)
{
    return length.em ? std::round(fontSize * static_cast<float>(length.value)) : static_cast<float>(length.value);
}

bool HasFactoryLook(const std::string &id)
{
    return FindFactoryEntry(id) != nullptr;
}

const Package &FactoryLook(const std::string &id)
{
    return FactoryEntryOrFluent(id).look;
}

Package ResolveBuiltInLook(const std::filesystem::path &skinsRoot, const std::string &id)
{
    const std::string key = HasFactoryLook(id) ? id : std::string("fluent");
    Package look = FactoryLook(key);
    const std::filesystem::path manifest =
        skinsRoot / std::filesystem::u8path(kDefaultSkinsFolder) / std::filesystem::u8path(key) / L"skin.toml";
    try
    {
        const std::optional<toml::table> parsed = ParseManifest(manifest);
        if (!parsed)
        {
            return look;
        }
        // 叠在副本上读，数据目录里的清单哪一项写错了就整份不用，回到出厂外观。
        Package overlaid = look;
        if (ReadLook(*parsed, overlaid, nullptr))
        {
            return overlaid;
        }
    }
    catch (const std::exception &)
    {
    }
    return look;
}

DefaultSkin ResolveBuiltInInfo(const std::filesystem::path &skinsRoot, const std::string &id)
{
    DefaultSkin info = FactoryEntryOrFluent(id).switches;
    if (const std::optional<DefaultSkin> data = LoadDefault(skinsRoot, id))
    {
        if (!data->name.empty())
            info.name = data->name;
        if (!data->description.empty())
            info.description = data->description;
    }
    return info;
}

const std::vector<std::string> &BuiltInIds()
{
    static const std::vector<std::string> ids = {"fluent",       "wechat",           "wechat_scroll", "graphite",
                                                 "willow_green", "autumn_osmanthus", "microsoft"};
    return ids;
}

const std::vector<std::string> &OfficialPackageIds()
{
    static const std::vector<std::string> ids = {"green_leaf"};
    return ids;
}

bool IsOfficialPackage(const std::string &id)
{
    const auto &ids = OfficialPackageIds();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

const std::string &BorderImageSource(const BorderImage &borderImage, bool light)
{
    return light && !borderImage.lightSource.empty() ? borderImage.lightSource : borderImage.source;
}

bool IsCanvasBase(const std::string &base)
{
    return base == kCanvasBase;
}

bool IsAnimatableImagePath(const std::string &path)
{
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos || path.find('/', dot) != std::string::npos)
    {
        return false;
    }
    std::string extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return extension == ".png" || extension == ".apng" || extension == ".gif" || extension == ".webp";
}

const std::vector<std::string> *ToolbarButtonStates(const std::string &key)
{
    static const std::vector<std::string> status = {"cn", "en", "ja", "caps", "en-candidate"};
    static const std::vector<std::string> fullwidth = {"full", "half"};
    static const std::vector<std::string> punctuation = {"cn", "en"};
    static const std::vector<std::string> characterSet = {"simplified", "traditional"};
    static const std::vector<std::string> stateless;
    if (key == "status")
        return &status;
    if (key == "fullwidth")
        return &fullwidth;
    if (key == "punctuation")
        return &punctuation;
    if (key == "character_set")
        return &characterSet;
    if (key == "emoji" || key == "screen_keyboard" || key == "settings")
        return &stateless;
    return nullptr;
}

bool IsBuiltIn(const std::string &id)
{
    const auto &ids = BuiltInIds();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool IsSafeId(const std::string &id)
{
    if (id.empty() || id.size() > 64 || !std::isalnum(static_cast<unsigned char>(id.front())))
    {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return std::islower(ch) || std::isdigit(ch) || ch == '.' || ch == '_' || ch == '-';
    });
}

bool IsBackupFolder(const std::string &folder)
{
    constexpr std::string_view kSuffix = ".bak";
    if (folder.size() < kSuffix.size())
    {
        return false;
    }
    return std::equal(kSuffix.begin(), kSuffix.end(), folder.end() - kSuffix.size(),
                      [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); });
}

bool Supports(const Package &package, const std::string &layout, const std::string &theme)
{
    return std::find(package.layouts.begin(), package.layouts.end(), layout) != package.layouts.end() &&
           std::find(package.themes.begin(), package.themes.end(), theme) != package.themes.end();
}

std::optional<Package> LoadSupported(const std::filesystem::path &skinsRoot, const std::string &id,
                                     const std::string &layout, const std::string &theme)
{
    std::optional<Package> package = Load(skinsRoot, id);
    if (package && !Supports(*package, layout, theme))
    {
        return std::nullopt;
    }
    return package;
}

std::optional<Package> Load(const std::filesystem::path &skinsRoot, const std::string &id, std::string *error)
{
    if (!IsSafeId(id) || IsBuiltIn(id) || id == kDefaultSkinsFolder)
    {
        SetError(error, "目录名不是有效的外部皮肤 ID");
        return std::nullopt;
    }
    const std::filesystem::path directory = skinsRoot / std::filesystem::u8path(id);
    const std::filesystem::path manifest = directory / L"skin.toml";
    try
    {
        const std::optional<toml::table> parsed = ParseManifest(manifest);
        if (!parsed)
        {
            SetError(error, "缺少或无法解析 skin.toml");
            return std::nullopt;
        }
        const toml::table &root = *parsed;
        const int schemaVersion = static_cast<int>(root["schema_version"].value_or(0));
        if (schemaVersion != 1 && schemaVersion != 2)
        {
            SetError(error, "仅支持 schema_version 1 或 2");
            return std::nullopt;
        }
        Package package;
        package.schemaVersion = schemaVersion;
        if (!ReadString(root, "id", package.id, 64, true) || package.id != id ||
            !ReadString(root, "name", package.name, 80, true) ||
            !ReadString(root, "version", package.version, 32, true) ||
            !ReadString(root, "author", package.author, 120, false) ||
            !ReadString(root, "description", package.description, 500, false) ||
            !ReadString(root, "base", package.base, 32, true) ||
            (!IsBuiltIn(package.base) && !FindSharedBase(package.base) && !IsCanvasBase(package.base)))
        {
            SetError(error, "manifest 的基本信息无效");
            return std::nullopt;
        }
        // canvas 没有自己的面板，只有 v2 的九宫格与挂件能让它可见；v1 皮肤用它只会得到一张透明卡片。
        if (IsCanvasBase(package.base) && schemaVersion < 2)
        {
            SetError(error, "base = \"canvas\" 只能用于 schema_version = 2 的皮肤");
            return std::nullopt;
        }
        // 九宫格、挂件和几处内边距只在 canvas 上生效，其他 base 的卡片几何与它们互相打架：这种皮肤仍可使用，按 v1
        // 处理，写了这些键时读出 candidate_window 后再提示。
        //
        // [animation] 和装饰图 motion 是各平台共用的 v2 键，对任何 base 都合法；Windows 不播放它们、按静态显示，
        // 属于正常降级，不提示。
        if (schemaVersion >= 2 && !IsCanvasBase(package.base))
        {
            package.schemaVersion = 1;
        }
        const auto *supports = root["supports"].as_table();
        if (!supports || !ReadEnumArray(*supports, "layouts", {"horizontal", "vertical"}, package.layouts) ||
            !ReadEnumArray(*supports, "themes", {"dark", "light"}, package.themes))
        {
            SetError(error, "supports.layouts 或 supports.themes 无效");
            return std::nullopt;
        }
        const auto *window = root["candidate_window"].as_table();
        if (!window)
        {
            SetError(error, "缺少 candidate_window");
            return std::nullopt;
        }
        if (schemaVersion >= 2 && !IsCanvasBase(package.base))
        {
            std::string ignored;
            for (const char *key :
                 {"border_image", "content_insets_dip", "outer_insets_dip", "line_gap_dip", "decorations"})
            {
                if (window->contains(key))
                {
                    ignored += ignored.empty() ? "candidate_window." : "、candidate_window.";
                    ignored += key;
                }
            }
            if (!ignored.empty())
            {
                package.notices.push_back(ignored + " 只在 base = \"canvas\" 时生效，已忽略");
            }
        }
        package.minWidthDip = BoundedNumber(*window, "min_width_dip", 1000.0);
        if (package.minWidthDip < 0.0)
        {
            SetError(error, "candidate_window.min_width_dip 超出范围");
            return std::nullopt;
        }
        // 装饰图是可选的：没有这张表就没有装饰；有这张表时图片和两个尺寸都必须给出。
        if (const toml::node *decorationNode = window->get("decoration"))
        {
            const auto *decoration = decorationNode->as_table();
            if (!decoration || !decoration->contains("image") ||
                !ReadResource(*decoration, "image", package.decorationImage) ||
                !ReadEnum(*decoration, "align", {"left", "center", "right"}, package.decorationAlign))
            {
                SetError(error, "candidate_window.decoration 无效");
                return std::nullopt;
            }
            package.decorationTopDip = BoundedNumber(*decoration, "top_inset_dip", 500.0);
            package.decorationWidthDip = BoundedNumber(*decoration, "width_dip", 1000.0);
            if (package.decorationTopDip <= 0.0 || package.decorationWidthDip <= 0.0)
            {
                SetError(error, "candidate_window.decoration 尺寸无效");
                return std::nullopt;
            }
            if (!ReadDecorationCandidate(*decoration, package, error))
            {
                return std::nullopt;
            }
        }
        if (!ReadEnum(*window, "backdrop", {"none", "acrylic", "mica"}, package.backdrop))
        {
            SetError(error, "candidate_window.backdrop 只能是 none、acrylic 或 mica");
            return std::nullopt;
        }
        if (!ReadFontFamily(*window, "font_family", package.fontFamily))
        {
            SetError(error, "candidate_window.font_family 无效");
            return std::nullopt;
        }
        if (!ReadOptionalBool(*window, "page_arrows", package.pageArrows))
        {
            SetError(error, "candidate_window.page_arrows 必须是 true 或 false");
            return std::nullopt;
        }
        if (!ReadOptionalBool(*window, "fit_content", package.fitContent))
        {
            SetError(error, "candidate_window.fit_content 必须是 true 或 false");
            return std::nullopt;
        }
        if (!ReadOptionalBool(*window, "expand_panel", package.expandPanel))
        {
            SetError(error, "candidate_window.expand_panel 必须是 true 或 false");
            return std::nullopt;
        }
        if (const toml::node *backgroundNode = window->get("background"))
        {
            const auto *background = backgroundNode->as_table();
            if (!background || !background->contains("image") ||
                !ReadResource(*background, "image", package.backgroundImage) ||
                !ReadEnum(*background, "fit", {"cover", "contain", "stretch"}, package.backgroundFit))
            {
                SetError(error, "candidate_window.background 无效");
                return std::nullopt;
            }
            if (background->contains("opacity"))
            {
                package.backgroundOpacity = BoundedNumber(*background, "opacity", 1.0);
                if (package.backgroundOpacity < 0.0)
                {
                    SetError(error, "candidate_window.background.opacity 超出范围");
                    return std::nullopt;
                }
            }
        }
        if (package.schemaVersion >= 2)
        {
            if (const toml::node *borderImageNode = window->get("border_image"))
            {
                const auto *borderImageTable = borderImageNode->as_table();
                BorderImage bi;
                if (!borderImageTable)
                {
                    SetError(error, "candidate_window.border_image 必须是表");
                    return std::nullopt;
                }
                if (!ReadBorderImage(*borderImageTable, nullptr, bi, error, "candidate_window.border_image"))
                {
                    return std::nullopt;
                }
                // 竖排覆盖：只写与横排不同的键，其余继承上面那份。
                if (const toml::node *verticalNode = borderImageTable->get("vertical"))
                {
                    const auto *verticalTable = verticalNode->as_table();
                    if (!verticalTable)
                    {
                        SetError(error, "candidate_window.border_image.vertical 必须是表");
                        return std::nullopt;
                    }
                    BorderImage vertical;
                    if (!ReadBorderImage(*verticalTable, &bi, vertical, error,
                                         "candidate_window.border_image.vertical"))
                    {
                        return std::nullopt;
                    }
                    // 浅色主题的图不跟着继承：横排那张是按横排几何画的，套到竖排会错位。
                    if (!verticalTable->contains("light_source"))
                    {
                        vertical.lightSource.clear();
                    }
                    package.borderImageVertical = vertical;
                }
                package.borderImage = bi;
            }

            if (const toml::node *insetsNode = window->get("content_insets_dip"))
            {
                const auto *insetsTable = insetsNode->as_table();
                if (!insetsTable)
                {
                    SetError(error, "candidate_window.content_insets_dip 必须是表");
                    return std::nullopt;
                }
                InsetsDip insets;
                insets.top = BoundedNumber(*insetsTable, "top", 200.0);
                insets.right = BoundedNumber(*insetsTable, "right", 500.0);
                insets.bottom = BoundedNumber(*insetsTable, "bottom", 200.0);
                insets.left = BoundedNumber(*insetsTable, "left", 500.0);
                if (insets.top < 0.0 || insets.right < 0.0 || insets.bottom < 0.0 || insets.left < 0.0)
                {
                    SetError(error, "candidate_window.content_insets_dip 数值超出范围");
                    return std::nullopt;
                }
                package.contentInsetsDip = insets;
                if (const toml::node *verticalNode = insetsTable->get("vertical"))
                {
                    const auto *verticalTable = verticalNode->as_table();
                    if (!verticalTable)
                    {
                        SetError(error, "candidate_window.content_insets_dip.vertical 必须是表");
                        return std::nullopt;
                    }
                    // 竖排只覆盖它写了的边，其余与横排共用。
                    InsetsDip vertical = insets;
                    const bool valid = ReadInsetSide(*verticalTable, "top", 200.0, vertical.top) &&
                                       ReadInsetSide(*verticalTable, "right", 500.0, vertical.right) &&
                                       ReadInsetSide(*verticalTable, "bottom", 200.0, vertical.bottom) &&
                                       ReadInsetSide(*verticalTable, "left", 500.0, vertical.left);
                    if (!valid)
                    {
                        SetError(error, "candidate_window.content_insets_dip.vertical 数值超出范围");
                        return std::nullopt;
                    }
                    package.contentInsetsDipVertical = vertical;
                }
            }

            if (!ReadOptionalBounded(*window, "line_gap_dip", 100.0, package.lineGapDip))
            {
                SetError(error, "candidate_window.line_gap_dip 超出范围");
                return std::nullopt;
            }

            if (const toml::node *outerNode = window->get("outer_insets_dip"))
            {
                const auto *outerTable = outerNode->as_table();
                if (!outerTable)
                {
                    SetError(error, "candidate_window.outer_insets_dip 必须是表");
                    return std::nullopt;
                }
                InsetsDip outer;
                outer.top = BoundedNumber(*outerTable, "top", 500.0);
                outer.right = BoundedNumber(*outerTable, "right", 500.0);
                outer.bottom = BoundedNumber(*outerTable, "bottom", 500.0);
                outer.left = BoundedNumber(*outerTable, "left", 500.0);
                if (outer.top < 0.0 || outer.right < 0.0 || outer.bottom < 0.0 || outer.left < 0.0)
                {
                    SetError(error, "candidate_window.outer_insets_dip 数值超出范围");
                    return std::nullopt;
                }
                package.outerInsetsDip = outer;
            }

            if (const toml::node *decorationsNode = window->get("decorations"))
            {
                const auto *decorationsArr = decorationsNode->as_array();
                if (!decorationsArr)
                {
                    SetError(error, "candidate_window.decorations 必须是数组");
                    return std::nullopt;
                }
                if (decorationsArr->size() > kMaxFreeDecorations)
                {
                    SetError(error, "candidate_window.decorations 最多 32 层");
                    return std::nullopt;
                }
                for (const auto &decNode : *decorationsArr)
                {
                    const auto *decTable = decNode.as_table();
                    FreeDecoration dec;
                    if (!decTable || !decTable->contains("image") || !ReadResource(*decTable, "image", dec.image) ||
                        !IsAnimatableImagePath(dec.image))
                    {
                        SetError(error, "candidate_window.decorations.image 无效（需为 .png、.apng、.gif 或 .webp）");
                        return std::nullopt;
                    }
                    if (decTable->contains("anchor") &&
                        !ReadEnum(*decTable, "anchor",
                                  {"top-left", "top-right", "bottom-left", "bottom-right", "left", "right", "top",
                                   "bottom", "center"},
                                  dec.anchor))
                    {
                        SetError(error, "candidate_window.decorations.anchor 无效");
                        return std::nullopt;
                    }
                    // offset_x_dip、offset_y_dip、z_index 可选；写了就必须是范围内的数（z_index 为整数）。
                    if (!ReadSignedNumber(*decTable, "offset_x_dip", kDecorationOffsetLimitDip, dec.offsetXDip) ||
                        !ReadSignedNumber(*decTable, "offset_y_dip", kDecorationOffsetLimitDip, dec.offsetYDip))
                    {
                        SetError(error, "candidate_window.decorations 的 offset_x_dip / offset_y_dip 必须在 "
                                        "-2000–2000 之间");
                        return std::nullopt;
                    }
                    if (const toml::node *zNode = decTable->get("z_index"))
                    {
                        const auto *zValue = zNode->as_integer();
                        if (!zValue || zValue->get() < -kDecorationZIndexLimit ||
                            zValue->get() > kDecorationZIndexLimit)
                        {
                            SetError(error, "candidate_window.decorations.z_index 必须是 -1000–1000 的整数");
                            return std::nullopt;
                        }
                        dec.zIndex = static_cast<int>(zValue->get());
                    }
                    dec.widthDip = BoundedNumber(*decTable, "width_dip", 2000.0);
                    dec.heightDip = BoundedNumber(*decTable, "height_dip", 2000.0);
                    if (dec.widthDip <= 0.0 || dec.heightDip <= 0.0)
                    {
                        SetError(error, "candidate_window.decorations 的 width_dip 与 height_dip 必须大于 0");
                        return std::nullopt;
                    }
                    package.freeDecorations.push_back(dec);
                }
            }
        }

        if (!ReadLook(root, package, error))
        {
            return std::nullopt;
        }
        // 工具栏图片模式的贴图都在皮肤目录里，所以只有外部皮肤能用，不放进内置清单共用的 ReadLook。
        if (const auto *toolbar = root["toolbar"].as_table())
        {
            if (!ReadToolbarImageMode(*toolbar, package, error))
            {
                return std::nullopt;
            }
        }
        std::error_code ec;
        if (!package.decorationImage.empty() &&
            !std::filesystem::is_regular_file(directory / std::filesystem::u8path(package.decorationImage), ec))
        {
            SetError(error, "找不到 candidate_window.decoration.image 文件");
            return std::nullopt;
        }
        if (!package.backgroundImage.empty() &&
            !std::filesystem::is_regular_file(directory / std::filesystem::u8path(package.backgroundImage), ec))
        {
            SetError(error, "找不到 candidate_window.background.image 文件");
            return std::nullopt;
        }
        if (package.borderImage && !package.borderImage->source.empty() &&
            !std::filesystem::is_regular_file(directory / std::filesystem::u8path(package.borderImage->source), ec))
        {
            SetError(error, "找不到 candidate_window.border_image.source 文件");
            return std::nullopt;
        }
        if (package.borderImage && !package.borderImage->lightSource.empty() &&
            !std::filesystem::is_regular_file(directory / std::filesystem::u8path(package.borderImage->lightSource),
                                              ec))
        {
            SetError(error, "找不到 candidate_window.border_image.light_source 文件");
            return std::nullopt;
        }
        if (package.borderImageVertical &&
            !std::filesystem::is_regular_file(directory / std::filesystem::u8path(package.borderImageVertical->source),
                                              ec))
        {
            SetError(error, "找不到 candidate_window.border_image.vertical.source 文件");
            return std::nullopt;
        }
        if (package.borderImageVertical && !package.borderImageVertical->lightSource.empty() &&
            !std::filesystem::is_regular_file(
                directory / std::filesystem::u8path(package.borderImageVertical->lightSource), ec))
        {
            SetError(error, "找不到 candidate_window.border_image.vertical.light_source 文件");
            return std::nullopt;
        }
        for (const auto &dec : package.freeDecorations)
        {
            if (!dec.image.empty() &&
                !std::filesystem::is_regular_file(directory / std::filesystem::u8path(dec.image), ec))
            {
                SetError(error, "找不到 candidate_window.decorations.image 文件");
                return std::nullopt;
            }
        }
        // bar 模式用不到这些图，不去检查。
        if (package.toolbarMode == "image")
        {
            const auto exists = [&](const std::string &path) {
                return std::filesystem::is_regular_file(directory / std::filesystem::u8path(path), ec);
            };
            if (!exists(package.toolbarImage))
            {
                SetError(error, "找不到 toolbar.image 文件");
                return std::nullopt;
            }
            if (!package.toolbarButtonDefaults.background.empty() && !exists(package.toolbarButtonDefaults.background))
            {
                SetError(error, "找不到 toolbar.button_defaults.background 文件");
                return std::nullopt;
            }
            for (const auto &[key, button] : package.toolbarButtons)
            {
                if (!button.image.empty() && !exists(button.image))
                {
                    SetError(error, "找不到 toolbar.buttons." + key + ".image 文件");
                    return std::nullopt;
                }
                if (!button.background.empty() && !exists(button.background))
                {
                    SetError(error, "找不到 toolbar.buttons." + key + ".background 文件");
                    return std::nullopt;
                }
                for (const auto &[state, path] : button.images)
                {
                    if (!exists(path))
                    {
                        SetError(error, "找不到 toolbar.buttons." + key + ".images." + state + " 文件");
                        return std::nullopt;
                    }
                }
            }
        }
        AdaptSharedBase(package);
        // 工具栏拖动条没单独写时跟候选的 accent 走，与选中竖条、预编辑光标同色；accent 也没写才沿用 base。
        FillColor(package.toolbarDark.handle, package.dark.accent);
        FillColor(package.toolbarLight.handle, package.light.accent);
        return package;
    }
    catch (const toml::parse_error &)
    {
        SetError(error, "缺少或无法解析 skin.toml");
        return std::nullopt;
    }
    catch (const std::exception &)
    {
        SetError(error, "skin.toml 不是有效的 TOML manifest");
        return std::nullopt;
    }
}

ScanResult Scan(const std::filesystem::path &skinsRoot)
{
    ScanResult result;
    std::error_code ec;
    if (!std::filesystem::exists(skinsRoot, ec))
    {
        return result;
    }
    for (std::filesystem::directory_iterator it(skinsRoot, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_directory(ec))
        {
            continue;
        }
        const std::string folder = it->path().filename().u8string();
        // <id>.bak 是安装包覆盖随包皮肤前留下的旧版本：manifest 的 id 与目录名对不上，
        // 不跳过就会在设置页被列成一条损坏的皮肤。
        if (folder == kDefaultSkinsFolder || IsBackupFolder(folder))
        {
            continue;
        }
        std::string error;
        auto package = Load(skinsRoot, folder, &error);
        if (package)
        {
            // 能用但有问题的皮肤照样列出来，问题另记一条，免得作者以为 v2 字段生效了。
            for (const auto &notice : package->notices)
            {
                result.issues.push_back({folder, notice, Issue::Kind::Noticed});
            }
            result.packages.push_back(std::move(*package));
        }
        else
        {
            result.issues.push_back({folder, error, Issue::Kind::LoadFailed});
        }
    }
    if (ec)
    {
        result.issues.push_back({"skins", "无法完整读取皮肤目录", Issue::Kind::ScanFailed});
    }
    std::sort(result.packages.begin(), result.packages.end(),
              [](const Package &a, const Package &b) { return a.name < b.name; });
    std::sort(result.issues.begin(), result.issues.end(),
              [](const Issue &a, const Issue &b) { return a.folder < b.folder; });
    return result;
}

std::optional<DefaultSkin> LoadDefault(const std::filesystem::path &skinsRoot, const std::string &id,
                                       std::string *error)
{
    if (!IsBuiltIn(id))
    {
        SetError(error, "不是内置皮肤 ID");
        return std::nullopt;
    }
    const std::filesystem::path manifest =
        skinsRoot / std::filesystem::u8path(kDefaultSkinsFolder) / std::filesystem::u8path(id) / L"skin.toml";
    try
    {
        const std::optional<toml::table> parsed = ParseManifest(manifest);
        if (!parsed)
        {
            SetError(error, "缺少或无法解析 skin.toml");
            return std::nullopt;
        }
        const toml::table &root = *parsed;
        DefaultSkin skin;
        if (root["schema_version"].value_or(0) != 1 || !ReadString(root, "id", skin.id, 64, true) || skin.id != id ||
            !ReadString(root, "name", skin.name, 80, false) ||
            !ReadString(root, "description", skin.description, 500, false))
        {
            SetError(error, "manifest 的基本信息无效");
            return std::nullopt;
        }
        if (!ReadSwitches(root, skin, error))
        {
            return std::nullopt;
        }
        return skin;
    }
    catch (const std::exception &)
    {
        SetError(error, "skin.toml 不是有效的 TOML manifest");
        return std::nullopt;
    }
}

namespace
{
// 开关的取值顺序：数据目录里的内置清单 → 出厂清单 → 程序缺省值。
bool SwitchValue(const std::optional<DefaultSkin> &defaults, std::optional<bool> DefaultSkin::*field,
                 const std::string &base, bool fallback)
{
    if (defaults && (*defaults).*field)
        return *((*defaults).*field);
    const FactoryEntry &factory = FactoryEntryOrFluent(IsBuiltIn(base) ? base : "fluent");
    return (factory.switches.*field).value_or(fallback);
}
} // namespace

bool ResolvePageArrows(const std::filesystem::path &skinsRoot, const std::string &skinId, const Package *package)
{
    if (package && package->pageArrows)
    {
        return *package->pageArrows;
    }
    const std::string base = package ? package->base : skinId;
    // 配置里的皮肤既不是内置、也没能作为外部皮肤加载时，渲染端回退到 fluent，这里跟着回退。
    const auto defaults = LoadDefault(skinsRoot, IsBuiltIn(base) ? base : "fluent");
    return SwitchValue(defaults, &DefaultSkin::pageArrows, base, kDefaultPageArrows);
}

bool ResolveFitContent(const std::filesystem::path &skinsRoot, const std::string &skinId, const Package *package)
{
    if (package && package->fitContent)
    {
        return *package->fitContent;
    }
    const std::string base = package ? package->base : skinId;
    const auto defaults = LoadDefault(skinsRoot, IsBuiltIn(base) ? base : "fluent");
    return SwitchValue(defaults, &DefaultSkin::fitContent, base, kDefaultFitContent);
}

bool ResolveExpandPanel(const std::filesystem::path &skinsRoot, const std::string &skinId, const Package *package)
{
    if (package && package->expandPanel)
    {
        return *package->expandPanel;
    }
    const std::string base = package ? package->base : skinId;
    const auto defaults = LoadDefault(skinsRoot, IsBuiltIn(base) ? base : "fluent");
    return SwitchValue(defaults, &DefaultSkin::expandPanel, base, kDefaultExpandPanel);
}
} // namespace CandidateSkinCatalog
