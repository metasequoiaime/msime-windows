#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace CandidateSkinCatalog
{
struct CandidateColors
{
    std::string accent;
    std::string selected;
    std::string hover;
    std::string surface;
    std::string border;
    std::string text;
    std::string number;
    // 候选后的翻译文本。留空时沿用候选文字色并降到 62% 不透明度；设置后按原值绘制，选中行也不改。
    std::string translation;
    // 翻译里的词性（n. / adj. …）与义项分隔符（; / ；）。留空时与同一行的翻译同色。
    // 包写了 translation 而没写这两项时，基础皮肤自带的词性/分隔符配色也一并作废，跟随新的翻译色。
    std::string translationPos;
    std::string translationSeparator;
    std::optional<bool> showSelectedBar;
    // 以下各项留空时的回退：candidate_text / preedit_text 用 text；preedit_caret / selected_bar 用 accent；
    // selected_text / selected_number 用基础皮肤的选中行配色；selected_translation 用 translation，
    // 再不行就是选中行文字色 × 0.62；preedit_background 透明；preedit_divider 不画。
    std::string candidateText;
    std::string preeditText;
    std::string preeditCaret;
    std::string selectedText;
    std::string selectedNumber;
    std::string selectedTranslation;
    // 选中行的词性与分隔符。留空时用 translation_pos / translation_separator；包写了 selected_translation
    // 而没写这两项时跟随 selected_translation。
    std::string selectedTranslationPos;
    std::string selectedTranslationSeparator;
    std::string selectedBar;
    std::string preeditBackground;
    std::string preeditDivider;
    // 候选框右键菜单；留空的项沿用基础皮肤。
    std::string menuBackground;
    std::string menuBorder;
    std::string menuText;
    std::string menuHover;
};

// 悬浮工具栏的配色覆盖，D2D 与 WebView2 两个渲染器都读这一份。留空的项沿用基础皮肤。
struct ToolbarColors
{
    std::string background;
    std::string border;
    std::string handle;
    std::string divider;
    std::string icon;
    std::string hover;
};

struct BorderImage
{
    std::string source;
    // 四边切片大小（DIP）：top, right, bottom, left
    std::vector<double> sliceDip;
    // 可拉伸区（上右下左，从面板外缘量起）：给出时覆盖 sliceDip。
    // 面板图里画死的装饰必须落在可拉伸区之外，否则会被横向拉长。
    std::vector<double> stretchDip;
    // 四边宽度（DIP）：top, right, bottom, left；若为空则沿用 sliceDip
    std::vector<double> widthDip;
    // 是否填充中间内容区域
    bool fill = true;
};

struct InsetsDip
{
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double left = 0.0;
};

struct FreeDecoration
{
    std::string image;
    // 锚定基准：top-left, top-right, bottom-left, bottom-right, left, right, top, bottom, center
    std::string anchor = "right";
    double offsetXDip = 0.0;
    double offsetYDip = 0.0;
    double widthDip = 0.0;
    double heightDip = 0.0;
    int zIndex = 2;
};

struct Package
{
    // 皮肤规范版本：1 为通用现代卡片皮肤，2 为支持九宫格与自由挂件的高级皮肤。
    int schemaVersion = 1;
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    // 渲染用的本地基底；共享基底在 Load 中转为 Fluent，并补齐缺省配色与有效 themes。
    std::string base = "fluent";
    std::vector<std::string> layouts;
    std::vector<std::string> themes;
    double minWidthDip = 0.0;
    // 覆盖基础皮肤的候选框外框圆角；未设置时沿用基础皮肤。
    std::optional<double> cornerRadiusDip;
    // 候选框外框线宽（0–4）与候选项高亮圆角（0–16）；未设置时沿用基础皮肤。
    std::optional<double> borderWidthDip;
    std::optional<double> itemCornerRadiusDip;
    // 候选框卡片阴影：none / soft / strong；为空表示沿用基础皮肤。不影响右键菜单的阴影。
    std::string shadow;
    // 候选字体族，排在用户配置的字体之前，用户字体作为回退；为空表示不改。
    std::string fontFamily;
    // 候选框卡片内的背景图，绘制在底色之上、候选文字之下，按外框圆角裁剪。
    std::string backgroundImage;
    std::string backgroundFit = "cover"; // cover / contain / stretch
    double backgroundOpacity = 1.0;
    double decorationTopDip = 0.0;
    double decorationWidthDip = 0.0;
    // 卡片上方的装饰图；为空表示皮肤没有装饰。
    std::string decorationImage;
    std::string decorationAlign = "right"; // left / center / right，相对卡片
    // 候选框里的翻页箭头；未设置时沿用 base 默认皮肤的设置（见 ResolvePageArrows）。
    std::optional<bool> pageArrows;
    // 卡片严格按候选文字所需的宽度收紧，不再垫出厂最小宽度、也不在同一输入里只增不减；
    // min_width_dip 与装饰图宽度仍然生效。未设置时沿用 base 默认皮肤的设置（见 ResolveFitContent）。
    std::optional<bool> fitContent;
    CandidateColors dark;
    CandidateColors light;
    // 覆盖基础皮肤的悬浮工具栏外框圆角；未设置时沿用基础皮肤。
    std::optional<double> toolbarCornerRadiusDip;
    ToolbarColors toolbarDark;
    ToolbarColors toolbarLight;
    // v2 专有特性：九宫格贴图边框
    std::optional<BorderImage> borderImage;
    // v2 专有特性：面板内容/文字安全内边距，从面板外缘量起（渲染时扣掉边框宽度）。
    std::optional<InsetsDip> contentInsetsDip;
    // v2 专有特性：预编辑行与候选行之间的行距（DIP，0–100）；未设置时沿用 base 的行距。
    std::optional<double> lineGapDip;
    // v2 专有特性：面板之外需要额外预留的窗口空间（上右下左 DIP），给挂在卡片外沿的挂件用。
    // 不声明时窗口只按卡片大小开，挂到外面的立绘会被窗口边界裁掉。
    std::optional<InsetsDip> outerInsetsDip;
    // v2 专有特性：多层自由锚定挂件（支持动图）
    std::vector<FreeDecoration> freeDecorations;
};

struct Issue
{
    std::string folder;
    std::string reason;
};

struct ScanResult
{
    std::vector<Package> packages;
    std::vector<Issue> issues;
};

// 内置皮肤的设置清单：<skinsRoot>/default/<id>/skin.toml。外观仍由内置 CSS 与 D2D 配色决定，
// 清单只承载皮肤级的开关；外部皮肤没写的开关沿用它 base 的这份清单。
struct DefaultSkin
{
    std::string id;
    std::string name;
    std::optional<bool> pageArrows;
    std::optional<bool> fitContent;
};

// 默认皮肤清单所在的子目录名，Scan 跳过它，外部皮肤也不能占用这个 ID。
inline constexpr const char *kDefaultSkinsFolder = "default";
// 默认皮肤清单缺失或没写 page_arrows 时的取值，与出厂清单一致：翻页箭头默认关闭。
inline constexpr bool kDefaultPageArrows = false;
// 默认皮肤清单缺失或没写 fit_content 时的取值：卡片保留出厂最小宽度。
inline constexpr bool kDefaultFitContent = false;

// 六个内置皮肤的 ID，按设置页的展示顺序。
const std::vector<std::string> &BuiltInIds();
bool IsBuiltIn(const std::string &id);
bool IsSafeId(const std::string &id);
// 安装包覆盖同名随包皮肤前，把旧目录改名成 <id>.bak（重名时 <id>.2.bak …）；Scan 跳过它们。
bool IsBackupFolder(const std::string &folder);
bool Supports(const Package &package, const std::string &layout, const std::string &theme);
std::optional<Package> Load(const std::filesystem::path &skinsRoot, const std::string &id,
                            std::string *error = nullptr);
// 读取外部皮肤，但只在它声明支持该布局与主题时才返回；不支持或读不出来时返回空，
// 调用方整份不套用、回退到 fluent（皮肤自己声明的 base 也不用）。
// 两个渲染后端与辅助浮层共用这一条判定，避免某个使用方只 Load 不校验。
std::optional<Package> LoadSupported(const std::filesystem::path &skinsRoot, const std::string &id,
                                     const std::string &layout, const std::string &theme);
ScanResult Scan(const std::filesystem::path &skinsRoot);
std::optional<DefaultSkin> LoadDefault(const std::filesystem::path &skinsRoot, const std::string &id,
                                       std::string *error = nullptr);
// skinId 是配置里的候选皮肤：内置皮肤读自己的默认清单；外部皮肤先看自己的 page_arrows，
// 没写再看 base 的默认清单。package 为外部皮肤已加载的清单，内置皮肤传 nullptr。
bool ResolvePageArrows(const std::filesystem::path &skinsRoot, const std::string &skinId, const Package *package);
// fit_content 的解析顺序同 ResolvePageArrows。
bool ResolveFitContent(const std::filesystem::path &skinsRoot, const std::string &skinId, const Package *package);
} // namespace CandidateSkinCatalog
