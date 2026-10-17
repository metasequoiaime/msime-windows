#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
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
    // candidate_window.backdrop 不是 none 时的材质参数，0–1。backdrop_opacity 是 surface 色调盖在材质上的不透明度，
    // 不写时取 surface 自带的 alpha；backdrop_luminosity 是亮度层的不透明度，越高越不受后面画面明暗影响，
    // 不写时亚克力取 0.85、云母取 1。
    std::optional<double> backdropOpacity;
    std::optional<double> backdropLuminosity;
};

// 悬浮工具栏的配色覆盖。留空的项沿用基础皮肤。
struct ToolbarColors
{
    std::string background;
    std::string border;
    std::string handle;
    std::string divider;
    std::string icon;
    std::string hover;
};

// 悬浮工具栏单个按钮的图标。单状态按钮用 image；有状态的按钮用 states（状态名 → 相对皮肤目录的路径）。
// 状态名与 D2D 端的状态判断一一对应：
//   status（语言）       : cn / en / caps / ja / en-candidate
//   fullwidth            : full / half
//   punctuation          : cn / en
//   character_set        : simplified / traditional
//   emoji / screen_keyboard / settings : 无状态（单图 image，或省略→矢量回退）
struct ToolbarButtonImages
{
    std::string image;
    std::map<std::string, std::string> states;
};

// 悬浮工具栏单个按钮的参数。全部可选：不写就沿用出厂值 / 用户设置。
struct ToolbarButton
{
    // 皮肤是否显示这个按钮。与用户在设置页的开关是「与」的关系：皮肤只能藏、不能强行显示用户关掉的项。
    std::optional<bool> visible;
    // 图标框尺寸（DIP）；不写沿用出厂。
    std::optional<double> sizeDip;
    // image 模式（把工具栏画成一张图 + 自由摆放的图标）下，图标左上角相对图片左上角的绝对坐标（DIP）。
    // 两个都给齐才按绝对坐标摆；缺一个（或整项没配）退回默认一行（从左到右、竖直居中）。
    std::optional<double> xDip;
    std::optional<double> yDip;
    ToolbarButtonImages images;
};

struct ToolbarButtons
{
    // 按钮键 → 参数。键名：status / fullwidth / punctuation / character_set / emoji / screen_keyboard / settings。
    std::map<std::string, ToolbarButton> items;
};

struct BorderImage
{
    std::string source;
    // 切片线位置，以源图像素计（同 CSS border-image-slice 的数值，键名沿用 _dip）：1/2/4 个值，按 CSS
    // 简写展开成上右下左。
    std::vector<double> sliceDip;
    // 可拉伸区（上右下左，同为源图像素，从图片外缘量起）：给出时覆盖 sliceDip。
    // 面板图里画死的装饰必须落在可拉伸区之外，否则会被横向拉长。
    std::vector<double> stretchDip;
    // 四边在屏幕上的绘制宽度（DIP，同 CSS border-image-width）；为空时取切片的数值当 DIP。
    // 给 2 倍图时把它写成切片的一半，面板在 200% 缩放下仍按原图像素清晰显示。
    std::vector<double> widthDip;
    // 是否画九宫格中心块（CSS border-image-slice 的 fill）
    bool fill = true;
};

struct InsetsDip
{
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double left = 0.0;
};

// 挂件 z_index 低于它的画在面板（卡片）之下，其余画在面板之上。面板自身相当于 z_index = 1，
// 与样例预览页 .container 的 z-index: 1 一致。
inline constexpr int kDecorationFrontZIndex = 1;
inline constexpr int kDecorationZIndexLimit = 1000;
inline constexpr double kDecorationOffsetLimitDip = 2000.0;
// 挂件层数上限，挡住把几百张大图塞进一个皮肤的清单。
inline constexpr size_t kMaxFreeDecorations = 32;

struct FreeDecoration
{
    std::string image;
    // 锚定基准：top-left, top-right, bottom-left, bottom-right, left, right, top, bottom, center
    std::string anchor = "right";
    double offsetXDip = 0.0;
    double offsetYDip = 0.0;
    double widthDip = 0.0;
    double heightDip = 0.0;
    // -1000–1000；低于 kDecorationFrontZIndex 的在面板之下。同层按声明顺序叠放。
    int zIndex = 2;
};

struct Package
{
    // 皮肤规范版本：1 为通用现代卡片皮肤，2 为支持九宫格与自由挂件的高级皮肤。v2 必须以 canvas 为 base，
    // 声明 2 而 base 不是 canvas 的按 1 处理（v2 字段全部忽略），并在 notices 里记一条。
    int schemaVersion = 1;
    // 不妨碍加载的问题；Scan 把它们和加载失败一起列进 issues，设置页的诊断信息会显示。
    std::vector<std::string> notices;
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
    // 候选框卡片背后的系统材质：none / acrylic（模糊后面的窗口）/ mica（模糊桌面壁纸，Win11；不支持时按 acrylic）；
    // 为空等同 none。色调取 surface，参数见 CandidateColors::backdropOpacity。系统关闭透明效果时退回不透明底色。
    std::string backdrop;
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
    // 装饰图上叠加候选词（Sogou 风格）：把第 candidate_index 个候选词画在装饰图上。
    std::optional<bool> decorationCandidateOverlay;
    double decorationCandidateFontSizeDip = 16.0;
    std::string decorationCandidateColor;             // 空 = 默认白色
    std::string decorationCandidateAlignH = "center"; // left / center / right，相对装饰图
    std::string decorationCandidateAlignV = "center"; // top / center / bottom，相对装饰图
    double decorationCandidateOffsetXDip = 0.0;
    double decorationCandidateOffsetYDip = 0.0;
    bool decorationCandidateStripBadge = true;      // 过滤模型名等后缀，只显示词本身
    double decorationCandidateMaxWidthDip = 0.0;    // 候选词最大宽度；<=0 用装饰图宽度
    double decorationCandidateMinFontSizeDip = 8.0; // 自动缩字号的下限
    int decorationCandidateIndex = 1; // 显示第几个候选词：0=跟随选中，1=第一个，2=第二个……
    // 候选框里的翻页箭头；未设置时沿用 base 默认皮肤的设置（见 ResolvePageArrows）。
    std::optional<bool> pageArrows;
    // 卡片严格按候选文字所需的宽度收紧，不再垫出厂最小宽度、也不在同一输入里只增不减；
    // min_width_dip 与装饰图宽度仍然生效。未设置时沿用 base 默认皮肤的设置（见 ResolveFitContent）。
    std::optional<bool> fitContent;
    CandidateColors dark;
    CandidateColors light;
    // 覆盖基础皮肤的悬浮工具栏外框圆角；未设置时沿用基础皮肤。image 模式忽略。
    std::optional<double> toolbarCornerRadiusDip;
    ToolbarColors toolbarDark;
    ToolbarColors toolbarLight;
    // 悬浮工具栏画法：
    //   "bar"（缺省）= 现有的矩形条：底色 + 描边 + 圆角 + 矢量图标排成一行。
    //   "image"      = 自由图片模式：整张 toolbarImage 当背景，图标按各自 x_dip/y_dip 摆上去，
    //                  不画底色/描边/圆角/阴影/把手/分隔线，整张图都可拖。
    // 只认这两个值（写别的判皮肤非法）；"image" 还要同时给 toolbarImage 与两个尺寸，否则皮肤非法。
    std::string toolbarMode = "bar";
    // 工具栏背景图（相对皮肤目录）；image 模式用。
    std::string toolbarImage;
    double toolbarImageWidthDip = 0.0;
    double toolbarImageHeightDip = 0.0;
    // 逐按钮参数（显隐、尺寸、绝对坐标、逐状态换图）。
    ToolbarButtons toolbarButtons;
    // v2 专有特性：九宫格贴图边框
    std::optional<BorderImage> borderImage;
    // v2 专有特性：面板内容/文字安全内边距，从面板外缘量起；某一边小于九宫格边框宽度时按边框宽度算。
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

// 内置皮肤的设置清单：<skinsRoot>/default/<id>/skin.toml。外观仍由内置的 D2D 配色决定，
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

// 全贴图基底：透明面板、无描边、无圆角、无阴影，外观完全来自九宫格面板图与挂件，是 v2 与自定义皮肤的地基。
// 它只能作为外部皮肤的 base，不能单独选用（没有贴图时什么也看不见），也不在 BuiltInIds 里。
inline constexpr const char *kCanvasBase = "canvas";
bool IsCanvasBase(const std::string &base);
// v2 图片（border_image.source、decorations.image）允许的扩展名：.png、.apng、.gif、.webp，不分大小写。
bool IsAnimatableImagePath(const std::string &path);

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
