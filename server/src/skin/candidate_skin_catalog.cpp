#include "skin/candidate_skin_catalog.h"

#include <toml++/toml.h>

#include <algorithm>
#include <cctype>
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
        !ReadCssColor(*table, "preedit_divider", out.preeditDivider))
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

// [toolbar.buttons]：按钮键 → 参数。键名见 ToolbarButtons 注释；未知键忽略（旧皮肤兼容）。
// 读 visible / size_dip / x_dip / y_dip，以及 images 表（状态 → 图）或单图 image。
bool ReadToolbarButtons(const toml::node *node, ToolbarButtons &out, std::string *error)
{
    if (!node)
    {
        return true;
    }
    const auto *table = node->as_table();
    if (!table)
    {
        return false;
    }
    for (const auto &[key, value] : *table)
    {
        const std::string buttonKey(key.str());
        const auto *button = value.as_table();
        if (!button || !ReadOptionalBool(*button, "visible", out.items[buttonKey].visible) ||
            !ReadOptionalBounded(*button, "size_dip", 200.0, out.items[buttonKey].sizeDip) ||
            !ReadOptionalBounded(*button, "x_dip", 2000.0, out.items[buttonKey].xDip) ||
            !ReadOptionalBounded(*button, "y_dip", 2000.0, out.items[buttonKey].yDip))
        {
            SetError(error, "toolbar.buttons." + buttonKey + " 参数无效");
            return false;
        }
        if (const toml::node *imagesNode = button->get("images"))
        {
            const auto *images = imagesNode->as_table();
            if (!images)
            {
                SetError(error, "toolbar.buttons." + buttonKey + ".images 必须是表");
                return false;
            }
            for (const auto &[state, path] : *images)
            {
                const std::string stateKey(state.str());
                const auto *pathValue = path.as_string();
                if (!pathValue || !IsSafeRelativeResource(pathValue->get()))
                {
                    SetError(error, "toolbar.buttons." + buttonKey + ".images." + stateKey + " 无效");
                    return false;
                }
                out.items[buttonKey].images.states[stateKey] = pathValue->get();
            }
        }
        else if (const toml::node *imageNode = button->get("image"))
        {
            const auto *imageValue = imageNode->as_string();
            if (!imageValue || !IsSafeRelativeResource(imageValue->get()))
            {
                SetError(error, "toolbar.buttons." + buttonKey + ".image 无效");
                return false;
            }
            out.items[buttonKey].images.image = imageValue->get();
        }
    }
    return true;
}
} // namespace

const std::vector<std::string> &BuiltInIds()
{
    static const std::vector<std::string> ids = {"fluent",       "wechat",           "graphite",
                                                 "willow_green", "autumn_osmanthus", "microsoft"};
    return ids;
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
        // v2 字段只在 canvas 上生效：其他 base 的卡片几何与九宫格/挂件互相打架。这种皮肤仍可使用，按 v1 处理。
        if (schemaVersion >= 2 && !IsCanvasBase(package.base))
        {
            package.schemaVersion = 1;
            package.notices.push_back("schema_version = 2 需要 base = \"canvas\"，已按 schema_version = 1 "
                                      "加载，九宫格、挂件等 v2 字段被忽略");
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
            // 装饰图上叠加候选词（可选）：candidate_overlay 开启后按 candidate_overlay_* 一组定位/排版。
            if (!ReadOptionalBool(*decoration, "candidate_overlay", package.decorationCandidateOverlay))
            {
                SetError(error, "candidate_window.decoration.candidate_overlay 必须是 true 或 false");
                return std::nullopt;
            }
            if (decoration->contains("candidate_overlay_font_size_dip"))
            {
                const double size = BoundedNumber(*decoration, "candidate_overlay_font_size_dip", 200.0);
                if (size < 0.0)
                {
                    SetError(error, "candidate_window.decoration.candidate_overlay_font_size_dip 超出范围");
                    return std::nullopt;
                }
                package.decorationCandidateFontSizeDip = size;
            }
            if (!ReadCssColor(*decoration, "candidate_overlay_color", package.decorationCandidateColor))
            {
                SetError(error, "candidate_window.decoration.candidate_overlay_color 无效");
                return std::nullopt;
            }
            if (!ReadEnum(*decoration, "candidate_overlay_align_h", {"left", "center", "right"},
                          package.decorationCandidateAlignH) ||
                !ReadEnum(*decoration, "candidate_overlay_align_v", {"top", "center", "bottom"},
                          package.decorationCandidateAlignV))
            {
                SetError(error, "candidate_window.decoration.candidate_overlay_align 无效");
                return std::nullopt;
            }
            if (!ReadSignedNumber(*decoration, "candidate_overlay_offset_x_dip", 2000.0,
                                  package.decorationCandidateOffsetXDip) ||
                !ReadSignedNumber(*decoration, "candidate_overlay_offset_y_dip", 2000.0,
                                  package.decorationCandidateOffsetYDip))
            {
                SetError(error, "candidate_window.decoration.candidate_overlay_offset 无效");
                return std::nullopt;
            }
            {
                std::optional<bool> stripBadge;
                if (!ReadOptionalBool(*decoration, "candidate_overlay_strip_badge", stripBadge))
                {
                    SetError(error, "candidate_window.decoration.candidate_overlay_strip_badge 必须是 true 或 false");
                    return std::nullopt;
                }
                package.decorationCandidateStripBadge = stripBadge.value_or(true);
            }
            if (decoration->contains("candidate_overlay_max_width_dip"))
            {
                const double width = BoundedNumber(*decoration, "candidate_overlay_max_width_dip", 2000.0);
                if (width < 0.0)
                {
                    SetError(error, "candidate_window.decoration.candidate_overlay_max_width_dip 超出范围");
                    return std::nullopt;
                }
                package.decorationCandidateMaxWidthDip = width;
            }
            if (decoration->contains("candidate_overlay_min_font_size_dip"))
            {
                const double size = BoundedNumber(*decoration, "candidate_overlay_min_font_size_dip", 200.0);
                if (size < 0.0)
                {
                    SetError(error, "candidate_window.decoration.candidate_overlay_min_font_size_dip 超出范围");
                    return std::nullopt;
                }
                package.decorationCandidateMinFontSizeDip = size;
            }
            if (decoration->contains("candidate_overlay_index"))
            {
                const double index = BoundedNumber(*decoration, "candidate_overlay_index", 1000.0);
                if (index < 0.0)
                {
                    SetError(error, "candidate_window.decoration.candidate_overlay_index 无效");
                    return std::nullopt;
                }
                package.decorationCandidateIndex = static_cast<int>(index);
            }
        }
        if (window->contains("corner_radius_dip"))
        {
            const double radius = BoundedNumber(*window, "corner_radius_dip", 32.0);
            if (radius < 0.0)
            {
                SetError(error, "candidate_window.corner_radius_dip 超出范围");
                return std::nullopt;
            }
            package.cornerRadiusDip = radius;
        }
        if (!ReadOptionalBounded(*window, "border_width_dip", 4.0, package.borderWidthDip))
        {
            SetError(error, "candidate_window.border_width_dip 超出范围");
            return std::nullopt;
        }
        if (!ReadOptionalBounded(*window, "item_corner_radius_dip", 16.0, package.itemCornerRadiusDip))
        {
            SetError(error, "candidate_window.item_corner_radius_dip 超出范围");
            return std::nullopt;
        }
        if (!ReadEnum(*window, "shadow", {"none", "soft", "strong"}, package.shadow))
        {
            SetError(error, "candidate_window.shadow 只能是 none、soft 或 strong");
            return std::nullopt;
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
                if (!borderImageTable || !borderImageTable->contains("source") ||
                    !ReadResource(*borderImageTable, "source", bi.source) || !IsAnimatableImagePath(bi.source))
                {
                    SetError(error, "candidate_window.border_image.source 无效（需为 .png、.apng、.gif 或 .webp）");
                    return std::nullopt;
                }
                if (!borderImageTable->contains("slice_dip") ||
                    !ReadBoxValues(*borderImageTable, "slice_dip", bi.sliceDip))
                {
                    SetError(error, "candidate_window.border_image.slice_dip 必须是 1、2 或 4 个 0–500 的数值");
                    return std::nullopt;
                }
                if (!ReadBoxValues(*borderImageTable, "stretch_dip", bi.stretchDip))
                {
                    SetError(error, "candidate_window.border_image.stretch_dip 必须是 1、2 或 4 个 0–500 的数值");
                    return std::nullopt;
                }
                if (!ReadBoxValues(*borderImageTable, "width_dip", bi.widthDip))
                {
                    SetError(error, "candidate_window.border_image.width_dip 必须是 1、2 或 4 个 0–500 的数值");
                    return std::nullopt;
                }
                std::optional<bool> fill;
                if (!ReadOptionalBool(*borderImageTable, "fill", fill))
                {
                    SetError(error, "candidate_window.border_image.fill 必须是 true 或 false");
                    return std::nullopt;
                }
                bi.fill = fill.value_or(true);
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

        const auto *candidate = root["candidate"].as_table();
        if (candidate && (!ReadColors((*candidate)["dark"].as_table(), package.dark) ||
                          !ReadColors((*candidate)["light"].as_table(), package.light)))
        {
            SetError(error, "candidate 配色无效");
            return std::nullopt;
        }
        if (const toml::node *toolbarNode = root.get("toolbar"))
        {
            const auto *toolbar = toolbarNode->as_table();
            if (!toolbar || !ReadToolbarColors(toolbar->get("dark"), package.toolbarDark) ||
                !ReadToolbarColors(toolbar->get("light"), package.toolbarLight))
            {
                SetError(error, "toolbar 配色无效");
                return std::nullopt;
            }
            if (toolbar->contains("corner_radius_dip"))
            {
                const double radius = BoundedNumber(*toolbar, "corner_radius_dip", 32.0);
                if (radius < 0.0)
                {
                    SetError(error, "toolbar.corner_radius_dip 超出范围");
                    return std::nullopt;
                }
                package.toolbarCornerRadiusDip = radius;
            }
            if (!ReadEnum(*toolbar, "mode", {"bar", "image"}, package.toolbarMode))
            {
                SetError(error, "toolbar.mode 只能是 bar 或 image");
                return std::nullopt;
            }
            if (!ReadResource(*toolbar, "image", package.toolbarImage))
            {
                SetError(error, "toolbar.image 无效");
                return std::nullopt;
            }
            if (toolbar->contains("image_width_dip"))
            {
                const double width = BoundedNumber(*toolbar, "image_width_dip", 2000.0);
                if (width < 0.0)
                {
                    SetError(error, "toolbar.image_width_dip 超出范围");
                    return std::nullopt;
                }
                package.toolbarImageWidthDip = width;
            }
            if (toolbar->contains("image_height_dip"))
            {
                const double height = BoundedNumber(*toolbar, "image_height_dip", 2000.0);
                if (height < 0.0)
                {
                    SetError(error, "toolbar.image_height_dip 超出范围");
                    return std::nullopt;
                }
                package.toolbarImageHeightDip = height;
            }
            if (!ReadToolbarButtons(toolbar->get("buttons"), package.toolbarButtons, error))
            {
                return std::nullopt;
            }
            // image 模式必须同时给图片和两个尺寸（渲染端不读图片自然尺寸）。
            if (package.toolbarMode == "image" &&
                (package.toolbarImage.empty() || package.toolbarImageWidthDip <= 0.0 ||
                 package.toolbarImageHeightDip <= 0.0))
            {
                SetError(error, "toolbar.mode=image 必须同时给 image、image_width_dip 与 image_height_dip");
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
        for (const auto &dec : package.freeDecorations)
        {
            if (!dec.image.empty() &&
                !std::filesystem::is_regular_file(directory / std::filesystem::u8path(dec.image), ec))
            {
                SetError(error, "找不到 candidate_window.decorations.image 文件");
                return std::nullopt;
            }
        }
        if (!package.toolbarImage.empty() &&
            !std::filesystem::is_regular_file(directory / std::filesystem::u8path(package.toolbarImage), ec))
        {
            SetError(error, "找不到 toolbar.image 文件");
            return std::nullopt;
        }
        for (const auto &[buttonKey, button] : package.toolbarButtons.items)
        {
            if (!button.images.image.empty() &&
                !std::filesystem::is_regular_file(directory / std::filesystem::u8path(button.images.image), ec))
            {
                SetError(error, "找不到 toolbar.buttons." + buttonKey + ".image 文件");
                return std::nullopt;
            }
            for (const auto &[state, path] : button.images.states)
            {
                if (!std::filesystem::is_regular_file(directory / std::filesystem::u8path(path), ec))
                {
                    SetError(error, "找不到 toolbar.buttons." + buttonKey + ".images." + state + " 文件");
                    return std::nullopt;
                }
            }
        }
        AdaptSharedBase(package);
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
                result.issues.push_back({folder, notice});
            }
            result.packages.push_back(std::move(*package));
        }
        else
        {
            result.issues.push_back({folder, error});
        }
    }
    if (ec)
    {
        result.issues.push_back({"skins", "无法完整读取皮肤目录"});
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
            !ReadString(root, "name", skin.name, 80, false))
        {
            SetError(error, "manifest 的基本信息无效");
            return std::nullopt;
        }
        if (const toml::node *windowNode = root.get("candidate_window"))
        {
            const auto *window = windowNode->as_table();
            if (!window || !ReadOptionalBool(*window, "page_arrows", skin.pageArrows))
            {
                SetError(error, "candidate_window.page_arrows 必须是 true 或 false");
                return std::nullopt;
            }
            if (!ReadOptionalBool(*window, "fit_content", skin.fitContent))
            {
                SetError(error, "candidate_window.fit_content 必须是 true 或 false");
                return std::nullopt;
            }
        }
        return skin;
    }
    catch (const std::exception &)
    {
        SetError(error, "skin.toml 不是有效的 TOML manifest");
        return std::nullopt;
    }
}

bool ResolvePageArrows(const std::filesystem::path &skinsRoot, const std::string &skinId, const Package *package)
{
    if (package && package->pageArrows)
    {
        return *package->pageArrows;
    }
    const std::string base = package ? package->base : skinId;
    // 配置里的皮肤既不是内置、也没能作为外部皮肤加载时，渲染端回退到 fluent，这里跟着回退。
    const auto defaults = LoadDefault(skinsRoot, IsBuiltIn(base) ? base : "fluent");
    return defaults && defaults->pageArrows ? *defaults->pageArrows : kDefaultPageArrows;
}

bool ResolveFitContent(const std::filesystem::path &skinsRoot, const std::string &skinId, const Package *package)
{
    if (package && package->fitContent)
    {
        return *package->fitContent;
    }
    const std::string base = package ? package->base : skinId;
    const auto defaults = LoadDefault(skinsRoot, IsBuiltIn(base) ? base : "fluent");
    return defaults && defaults->fitContent ? *defaults->fitContent : kDefaultFitContent;
}
} // namespace CandidateSkinCatalog
