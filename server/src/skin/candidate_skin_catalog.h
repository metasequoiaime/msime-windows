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
    // 横排三角翻页箭头前面那条分隔线（page_arrow_style = "triangle" 时才画）；留空不画。
    std::string pageArrowDivider;
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

// 图片模式（[toolbar] mode = "image"）下一个按钮的摆法与贴图：[toolbar.buttons.<key>]。
struct ToolbarButton
{
    // false 时皮肤把它藏起来；皮肤不能让用户在设置页关掉的按钮重新出现。
    std::optional<bool> visible;
    // 按钮框左上角相对背景图左上角的位置（DIP）。两个都写了就固定在这里；否则按 [toolbar.button_flow]
    // 排，皮肤没写 button_flow 时这个按钮不画。
    std::optional<double> xDip;
    std::optional<double> yDip;
    // 按钮框的宽高（DIP）；没写时取 [toolbar.button_defaults]，再没有为 kDefaultToolbarButtonSizeDip。
    std::optional<double> widthDip;
    std::optional<double> heightDip;
    // 没有贴图时画的内置图标（Segoe Fluent Icons）的字号（DIP）；没写时取 button_defaults，
    // 再没有为按钮框短边的 0.82 倍。
    std::optional<double> iconSizeDip;
    // 按钮自己的底图（如圆底），铺满按钮框、画在图标或按钮图下面，随按钮一起显示和隐藏；
    // 没写时取 button_defaults。
    std::string background;
    // 各状态共用的图；images 里没有当前状态的图时也用它。两者都没有就画内置的矢量图标。
    std::string image;
    // 状态名 → 图，只对有状态的按钮有效，状态名见 ToolbarButtonStates。
    std::map<std::string, std::string> images;
};

inline constexpr double kDefaultToolbarButtonSizeDip = 26.0;

// [toolbar.button_flow]：没写坐标的按钮从 (x_dip, y_dip) 起按工具栏的顺序依次排开，只排当前显示的按钮，
// 所以设置页关掉一个按钮后面的就往前补，打开一个就排进队里。写了坐标的按钮不参与排列。
struct ToolbarButtonFlow
{
    double xDip = 0.0;
    double yDip = 0.0;
    bool vertical = false;
    double gapDip = 0.0;
};

// 装饰图上叠加一个候选词（[candidate_window.decoration] 的 candidate_overlay_* 一组键）。
struct DecorationCandidate
{
    // 叠加哪一个候选：0 跟随高亮，1 起为当前页的第几个。
    int index = 1;
    double fontSizeDip = 16.0;
    // 词太宽时字号往下缩，最小缩到这么大，再宽就让它超出。
    double minFontSizeDip = 8.0;
    // 词最多占多宽（DIP）；0 为装饰框的宽度。
    double maxWidthDip = 0.0;
    // CSS 颜色；为空是白色。
    std::string color;
    std::string alignH = "center"; // left / center / right，相对装饰框
    std::string alignV = "center"; // top / center / bottom，相对装饰框
    double offsetXDip = 0.0;
    double offsetYDip = 0.0;
    // 只画词本身，不带候选后面的标注（如整句模型名）。
    bool stripBadge = true;
};

struct BorderImage
{
    std::string source;
    // 浅色主题换用的面板图（light_source），切片、边宽和 fill 与 source 共用，两张图要按同一套几何画；
    // 为空时深浅主题都用 source。
    std::string lightSource;
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
    // 平铺方式，语义同 CSS border-image-repeat：1 个或 2 个值「横向 纵向」，stretch（默认）/ repeat。
    // repeat 按源图原尺寸平铺，用来画可以任意加长的花纹边框（如竖向候选窗的侧边）。
    bool repeatX = false;
    bool repeatY = false;
};

struct InsetsDip
{
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double left = 0.0;
};

// 候选框里的一段长度：清单里写 DIP 数值，或写成 "0.3em" 按候选字号缩放（四舍五入到整 DIP）。
struct Length
{
    double value = 0.0;
    bool em = false;
};
float ResolveLength(const Length &length, float fontSize);

// 候选框的版式。[candidate_window] 里写的对两种排列都生效，[candidate_window.horizontal] /
// [candidate_window.vertical] 里写的只对那一种生效、并且优先。没写的项沿用 base，再没有就用渲染器的缺省值。
struct CandidateGeometry
{
    // 卡片内边距
    std::optional<Length> padding;
    // 候选项之间的空隙
    std::optional<Length> itemGap;
    // 候选项比 1.35 倍字号多出的高度，上下各一半
    std::optional<Length> itemExtraHeight;
    // 高亮左缘到序号左端的距离（不小于 5）
    std::optional<Length> numberInset;
    // 候选文字后面的留白
    std::optional<Length> itemPaddingRight;
    // 横排每项的最小宽度，0 为不限
    std::optional<Length> itemMinWidth;
    // 序号与候选之间的空
    std::optional<Length> numberGap;
    // 序号字号相对候选字号的比例
    std::optional<double> numberScale;
    // 选中竖条的宽度与高度（高度以字号的倍数计），以及是否落在高亮左缘之内
    std::optional<Length> selectedBarWidth;
    std::optional<double> selectedBarHeightEm;
    std::optional<bool> selectedBarInside;
    // 落在卡片四角上的高亮角跟卡片同一圆角
    std::optional<bool> itemCornersFollowCard;
    // 横排时把卡片多出的宽度均分给同一行各项，高亮铺满到卡片右边
    std::optional<bool> justifyRows;
    // 隐藏预编辑行时抵掉它与候选之间的 2 DIP 行距
    std::optional<bool> trimHiddenPreeditGap;
    // 翻页箭头样式：chevron（折线）/ triangle（实心三角，横排前面隔一条分隔线）；为空沿用 base
    std::string pageArrowStyle;
    // 预编辑行的外边距（上右下左 DIP）
    std::optional<InsetsDip> preeditMarginDip;
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
    // 皮肤规范版本：1 为通用现代卡片皮肤，2 在 canvas 上支持九宫格与自由挂件。声明 2 而 base 不是 canvas 的按 1
    // 处理（九宫格、挂件等 canvas 字段全部忽略），清单写了这些字段时在 notices 里列出；只写了 [animation] 或装饰图
    // motion 的不记，Windows 按静态显示。
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
    // 叠加在装饰图上的候选词；没开 candidate_overlay 时为空。
    std::optional<DecorationCandidate> decorationCandidate;
    // 候选框里的翻页箭头；未设置时沿用 base 默认皮肤的设置（见 ResolvePageArrows）。
    std::optional<bool> pageArrows;
    // 卡片严格按候选文字所需的宽度收紧，不再垫出厂最小宽度、也不在同一输入里只增不减；
    // min_width_dip 与装饰图宽度仍然生效。未设置时沿用 base 默认皮肤的设置（见 ResolveFitContent）。
    std::optional<bool> fitContent;
    // 卷轴展开：横排候选行右端放一个展开按钮，点开后在候选行下面展开后续候选的网格，右侧细滚动条，
    // 滚轮上下滚动而不是翻页。未设置时沿用 base 默认皮肤的设置（见 ResolveExpandPanel）。
    std::optional<bool> expandPanel;
    // 展开后网格一屏显示的行数（2–8）；未设置时为 kDefaultExpandRows。
    std::optional<int> expandRows;
    CandidateColors dark;
    CandidateColors light;
    // 候选框版式：两种排列共用的，以及只对横排 / 竖排生效的。
    CandidateGeometry geometry;
    CandidateGeometry horizontalGeometry;
    CandidateGeometry verticalGeometry;
    // 覆盖基础皮肤的悬浮工具栏外框圆角与图标悬停底色的圆角；未设置时沿用基础皮肤。
    std::optional<double> toolbarCornerRadiusDip;
    std::optional<double> toolbarIconCornerRadiusDip;
    ToolbarColors toolbarDark;
    ToolbarColors toolbarLight;
    // 悬浮工具栏的画法：bar（缺省）是矩形条；image 是一整张背景图，按钮按 toolbarButtons 的坐标摆在图上，
    // 不画底色、描边、阴影、把手与分隔线，窗口按图片的透明区域裁成异形，透明处点得穿。
    std::string toolbarMode = "bar";
    // image 模式的背景图（相对皮肤目录）与它的显示尺寸（DIP）。
    std::string toolbarImage;
    double toolbarImageWidthDip = 0.0;
    double toolbarImageHeightDip = 0.0;
    // 按钮键 → 摆法与贴图，只在 image 模式生效。键见 ToolbarButtonStates。
    std::map<std::string, ToolbarButton> toolbarButtons;
    // [toolbar.button_defaults]：各按钮没写的 width_dip、height_dip、icon_size_dip、background 取这里的。
    ToolbarButton toolbarButtonDefaults;
    std::optional<ToolbarButtonFlow> toolbarButtonFlow;
    // v2 专有特性：九宫格贴图边框
    std::optional<BorderImage> borderImage;
    // 竖排（layout == "vertical"）替换用的九宫格；来自 [candidate_window.border_image.vertical]，
    // 只覆盖它写了的键，其余沿用 borderImage。为空时竖排也用 borderImage。
    std::optional<BorderImage> borderImageVertical;
    // v2 专有特性：面板内容/文字安全内边距，从面板外缘量起；某一边小于九宫格边框宽度时按边框宽度算。
    std::optional<InsetsDip> contentInsetsDip;
    // 竖排替换用的内容内边距（[candidate_window.content_insets_dip.vertical]），
    // 只覆盖它写了的边，其余沿用 contentInsetsDip。为空时竖排也用 contentInsetsDip。
    std::optional<InsetsDip> contentInsetsDipVertical;
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
    // 这条 issue 的性质，设置页据此把加载失败的目录和带提示的皮肤分开计数。不看 issue 的 folder 能否在
    // packages 里找到：官方包在设置页单独成组，目录名读不出时 folder 也不是皮肤 id。
    enum class Kind
    {
        // 目录加载失败，不会出现在可选列表里。
        LoadFailed,
        // 皮肤能用，只是有些字段被忽略。
        Noticed,
        // 皮肤目录本身读不全，folder 指的是 skinsRoot。
        ScanFailed,
    };

    std::string folder;
    std::string reason;
    Kind kind = Kind::LoadFailed;
};

struct ScanResult
{
    std::vector<Package> packages;
    std::vector<Issue> issues;
};

// 内置皮肤的清单：<skinsRoot>/default/<id>/skin.toml，同时承载皮肤级的开关与完整外观。出厂清单编进程序，
// 安装时整份覆盖数据目录里的那一份；数据目录里的清单写了的键覆盖出厂值（技术上能改，但不建议，升级会冲掉）。
// 外部皮肤没写的开关与外观都沿用它 base 的这份清单。
struct DefaultSkin
{
    std::string id;
    std::string name;
    // 设置页皮肤卡片上的一句说明。
    std::string description;
    std::optional<bool> pageArrows;
    std::optional<bool> fitContent;
    std::optional<bool> expandPanel;
};

// 默认皮肤清单所在的子目录名，Scan 跳过它，外部皮肤也不能占用这个 ID。
inline constexpr const char *kDefaultSkinsFolder = "default";
// 默认皮肤清单缺失或没写 page_arrows 时的取值，与出厂清单一致：翻页箭头默认关闭。
inline constexpr bool kDefaultPageArrows = false;
// 默认皮肤清单缺失或没写 fit_content 时的取值：卡片保留出厂最小宽度。
inline constexpr bool kDefaultFitContent = false;
// 默认皮肤清单缺失或没写 expand_panel 时的取值：用翻页而不是卷轴展开。
inline constexpr bool kDefaultExpandPanel = false;
// 卷轴展开后一屏的网格行数，及皮肤可写的范围。
inline constexpr int kDefaultExpandRows = 4;
inline constexpr int kMinExpandRows = 2;
inline constexpr int kMaxExpandRows = 8;

// 全贴图基底：透明面板、无描边、无圆角、无阴影，外观完全来自九宫格面板图与挂件，是 v2 与自定义皮肤的地基。
// 它只能作为外部皮肤的 base，不能单独选用（没有贴图时什么也看不见），也不在 BuiltInIds 里。
inline constexpr const char *kCanvasBase = "canvas";
bool IsCanvasBase(const std::string &base);
// v2 图片（border_image.source、decorations.image）允许的扩展名：.png、.apng、.gif、.webp，不分大小写。
bool IsAnimatableImagePath(const std::string &path);

// 悬浮工具栏按钮键（status、fullwidth、punctuation、character_set、emoji、screen_keyboard、settings）的
// 状态名，即 images 表的键；无状态的按钮是空表。不是按钮键时返回 nullptr。
//   status: cn / en / ja / caps / en-candidate；fullwidth: full / half；punctuation: cn / en；
//   character_set: simplified / traditional
const std::vector<std::string> *ToolbarButtonStates(const std::string &key);

// 指定主题下实际要画的面板图：浅色主题优先 light_source，没写就用 source。
const std::string &BorderImageSource(const BorderImage &borderImage, bool light);

// 内置皮肤的 ID，按设置页的展示顺序。每个都有 skins/default/<id>/skin.toml 出厂清单。
const std::vector<std::string> &BuiltInIds();
bool IsBuiltIn(const std::string &id);
// 官方贴图皮肤：本身是 skins\<id> 下的外部皮肤包，随安装包装到数据目录、照外部皮肤加载和渲染，
// 只是设置页把它们列在内置皮肤之后，而不是「外部皮肤」列表里。按设置页的展示顺序。
const std::vector<std::string> &OfficialPackageIds();
bool IsOfficialPackage(const std::string &id);
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
// expand_panel 的解析顺序同 ResolvePageArrows。
bool ResolveExpandPanel(const std::filesystem::path &skinsRoot, const std::string &skinId, const Package *package);

// 有出厂清单的基底：六个内置皮肤与 canvas。
bool HasFactoryLook(const std::string &id);
// 编进程序的出厂清单解析出的外观（候选配色、版式、工具栏）。没有出厂清单的 id 用 fluent 的。
const Package &FactoryLook(const std::string &id);
// 实际生效的内置外观：出厂外观上叠加数据目录 skins/default/<id>/skin.toml 里写了的键；
// 那份清单缺失或读不出来时就是出厂外观。没有出厂清单的 id 用 fluent 的。
Package ResolveBuiltInLook(const std::filesystem::path &skinsRoot, const std::string &id);
// 内置皮肤的名称与说明（设置页用）：数据目录里的清单写了就用它的，否则用出厂清单的。
DefaultSkin ResolveBuiltInInfo(const std::filesystem::path &skinsRoot, const std::string &id);
} // namespace CandidateSkinCatalog
