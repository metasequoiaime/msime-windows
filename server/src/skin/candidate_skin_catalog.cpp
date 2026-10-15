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

// 皮肤颜色会被拼进 WebView2 的 CSS 声明，只放行颜色值会用到的字符，挡住 `;`、`{}` 之类能跳出声明的写法。
bool ReadCssColor(const toml::table &table, const char *key, std::string &out)
{
    return ReadString(table, key, out, 80, false) && std::all_of(out.begin(), out.end(), [](unsigned char ch) {
               return std::isalnum(ch) || ch == '#' || ch == '(' || ch == ')' || ch == ',' || ch == '.' || ch == '%' ||
                      ch == ' ' || ch == '-' || ch == '/';
           });
}

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
    return true;
}

// 字体族会被拼进 WebView2 的 font-family（加引号）与脚本字符串，挡住引号、反斜杠、分号、花括号、尖括号和控制字符；
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
} // namespace

const std::vector<std::string> &BuiltInIds()
{
    static const std::vector<std::string> ids = {"fluent",       "wechat",           "graphite",
                                                 "willow_green", "autumn_osmanthus", "microsoft"};
    return ids;
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
            !ReadString(root, "base", package.base, 32, true) || !IsBuiltIn(package.base))
        {
            SetError(error, "manifest 的基本信息无效");
            return std::nullopt;
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
                    !ReadResource(*borderImageTable, "source", bi.source))
                {
                    SetError(error, "candidate_window.border_image.source 无效");
                    return std::nullopt;
                }
                const auto *sliceArr = (*borderImageTable)["slice_dip"].as_array();
                if (!sliceArr || (sliceArr->size() != 1 && sliceArr->size() != 2 && sliceArr->size() != 4))
                {
                    SetError(error, "candidate_window.border_image.slice_dip 必须包含 1、2 或 4 个数值");
                    return std::nullopt;
                }
                for (const auto &val : *sliceArr)
                {
                    const auto num = val.value<double>();
                    if (!num || *num < 0.0 || *num > 500.0)
                    {
                        SetError(error, "candidate_window.border_image.slice_dip 超出范围");
                        return std::nullopt;
                    }
                    bi.sliceDip.push_back(*num);
                }
                if (const auto *stretchArr = (*borderImageTable)["stretch_dip"].as_array())
                {
                    if (stretchArr->size() != 1 && stretchArr->size() != 2 && stretchArr->size() != 4)
                    {
                        SetError(error, "candidate_window.border_image.stretch_dip 必须包含 1、2 或 4 个数值");
                        return std::nullopt;
                    }
                    for (const auto &val : *stretchArr)
                    {
                        const auto num = val.value<double>();
                        if (!num || *num < 0.0 || *num > 500.0)
                        {
                            SetError(error, "candidate_window.border_image.stretch_dip 超出范围");
                            return std::nullopt;
                        }
                        bi.stretchDip.push_back(*num);
                    }
                }
                if (const auto *widthArr = (*borderImageTable)["width_dip"].as_array())
                {
                    if (widthArr->size() != 1 && widthArr->size() != 2 && widthArr->size() != 4)
                    {
                        SetError(error, "candidate_window.border_image.width_dip 必须包含 1、2 或 4 个数值");
                        return std::nullopt;
                    }
                    for (const auto &val : *widthArr)
                    {
                        const auto num = val.value<double>();
                        if (!num || *num < 0.0 || *num > 500.0)
                        {
                            SetError(error, "candidate_window.border_image.width_dip 超出范围");
                            return std::nullopt;
                        }
                        bi.widthDip.push_back(*num);
                    }
                }
                bi.fill = (*borderImageTable)["fill"].value_or(true);
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
                for (const auto &decNode : *decorationsArr)
                {
                    const auto *decTable = decNode.as_table();
                    FreeDecoration dec;
                    if (!decTable || !decTable->contains("image") || !ReadResource(*decTable, "image", dec.image))
                    {
                        SetError(error, "candidate_window.decorations.image 无效");
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
                    // 这三个键可选，缺省时 table::get 返回空指针，必须经 node_view 取值
                    dec.offsetXDip = (*decTable)["offset_x_dip"].value_or(0.0);
                    dec.offsetYDip = (*decTable)["offset_y_dip"].value_or(0.0);
                    dec.widthDip = BoundedNumber(*decTable, "width_dip", 2000.0);
                    dec.heightDip = BoundedNumber(*decTable, "height_dip", 2000.0);
                    dec.zIndex = static_cast<int>((*decTable)["z_index"].value_or(int64_t{2}));
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
