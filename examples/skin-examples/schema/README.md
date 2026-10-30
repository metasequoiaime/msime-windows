# 外部皮肤 manifest（TOML）

每个皮肤目录需要 `skin.toml`，`id` 必须与文件夹名一致。`schema_version` 支持 `1` 与 `2`，同时也是 msime 各平台共用的清单版本：写在 `base = "canvas"` 上时由 Direct2D 候选窗完整绘制（九宫格面板、上下两层挂件、动图）；写在别的基底上时按 `schema_version = 1` 加载，皮肤照常可用，只是九宫格面板、自由挂件、`line_gap_dip` 这些 canvas 专属键不生效，清单里写了它们时设置页会列出来。

候选框和悬浮工具栏都由 `skin.toml` 声明，不要再提供 `cand.css` 或工具栏 CSS。所有图片路径都相对于皮肤目录，且不能指向目录之外。

## 顶层字段

- 必填：`schema_version`（`1` 或 `2`）、`id`、`name`、`version`、`base`（继承的内置皮肤：`fluent`、`wechat`、`graphite`、`willow_green`、`autumn_osmanthus`、`microsoft`；或与 msime 共享的基底：`system`、`shuishan`、`light`、`paper`、`night`、`ink`，按 `fluent` 的结构绘制并补上基底配色，固定明暗的基底只在对应的 `themes` 下生效；或全贴图基底 `canvas`，只用于 v2）
- `schema_version` 与 `base` 的搭配：
  - 下面标「v2」的表和键只在 `schema_version = 2` 且 `base = "canvas"` 时生效。写了 2 而 `base` 是别的基底时，皮肤仍可使用，按 `schema_version = 1` 加载，这些表和键全部忽略（连同其中的错误）；清单里确实写了它们时，设置页的诊断信息会列出被忽略的键。
  - `schema_version = 2` 也是 msime 各平台共用的清单版本，`[animation]` 和装饰图的 `motion`、`motion_period_ms` 对任何基底都合法。Windows 目前不播放它们，按静态显示，不算错误，也不提示。
  - `schema_version = 1` 不能用 `canvas`：没有面板贴图它只是一张透明卡片，这种皮肤不会出现在可选列表里。
- 可选：`author`、`description`
- `[supports]`：`layouts`（`horizontal` / `vertical`）、`themes`（`dark` / `light`）
- `[license]`：`code`、`assets`

## `[candidate_window]`

- `min_width_dip`：卡片最小宽度；实际最小宽度还会被装饰图的 `width_dip` 撑大
- `corner_radius_dip`：卡片圆角，`0`–`32`；不写则沿用 `base`
- `border_width_dip`：卡片外框线宽，`0`–`4`；颜色取 `border`，不写 `border` 时取 `base` 的边框色（杨柳青、秋桂没有外框，此时仍不可见）
- `item_corner_radius_dip`：选中 / 悬停高亮的圆角，`0`–`16`；贴着卡片四角的那几个角仍跟 `corner_radius_dip`
- `shadow`：卡片阴影，`none` | `soft` | `strong`；不写则沿用 `base`。只影响候选卡片，不影响右键菜单
- `backdrop`：卡片背后的系统材质，`none` | `acrylic` | `mica`；不写等于 `none`。`acrylic` 模糊卡片后面的窗口；`mica` 模糊桌面壁纸，只透出壁纸色调（需要 Windows 11，更早的系统按 `acrylic` 画）。材质按 Fluent 配方叠「模糊背景 → 亮度层 → 色调」，色调颜色取 `surface`，两个不透明度见下面配色表里的 `backdrop_opacity` / `backdrop_luminosity`。系统关闭「透明效果」或不支持（低于 Windows 10 1903）时，`surface` 按不透明画。写了 `border_image` 时不生效
- `font_family`：候选字体名，UTF-8 编码后最长 64 字节（约 21 个汉字），不能含引号、反斜杠、逗号、分号、尖括号、花括号和反引号。排在用户设置的字体前面，缺字时仍回落到用户字体；字号不由皮肤决定
- `page_arrows`：候选框里的翻页箭头，`true` | `false`。横排时是候选右侧贴着最后一行的一小列，竖排时是最后一行下方的一矮行、“‹”与序号左对齐；颜色取 `number`，悬停底色取 `hover`。不写则沿用 `base` 的默认皮肤设置（数据目录 `skins\default\<base>\skin.toml`，出厂为 `false`）
- `fit_content`：卡片是否严格按候选文字所需的宽度收紧，`true` | `false`。打开后不再垫出厂的最小宽度（160 DIP），同一次输入里后到的翻译等内容消失时宽度也立刻缩回；`min_width_dip` 和装饰图 `width_dip` 仍然生效。不写则沿用 `base` 的默认皮肤设置（出厂为 `false`）

### `[candidate_window.decoration]`（可选）

卡片上方的装饰图。写了这张表就必须同时给出 `image`、`top_inset_dip`、`width_dip`。

- `image`：图片路径
- `top_inset_dip` / `width_dip`：装饰框的高和宽，框贴在卡片上方，不与卡片重叠；图片在框内等比缩放
- `align`：`left` | `center` | `right`，相对卡片

装饰图上还可以叠加一个候选词（比如把首选词写在立绘的对话框里）。`candidate_overlay = true` 时下面这组键才生效，都可省略：

- `candidate_overlay`：`true` | `false`，默认 `false`
- `candidate_overlay_index`：叠加哪个候选，`0`–`100` 的整数。`0` 跟随高亮，`1` 是当前页第一个，依此类推，默认 `1`。当前页没有这一个时不画
- `candidate_overlay_font_size_dip`：字号，大于 `0`、不超过 `200`，默认 `16`。字体与候选相同
- `candidate_overlay_max_width_dip`：词最多占多宽，`0`–`2000`，默认 `0` 即装饰框的宽度。超出时按比例缩小字号
- `candidate_overlay_min_font_size_dip`：缩字号的下限，大于 `0`、不超过 `200`，默认 `8`。缩到下限仍放不下时就让它超出，不折行
- `candidate_overlay_color`：文字颜色，写法见[颜色格式](#颜色格式)，默认白色
- `candidate_overlay_align_h`：`left` | `center` | `right`；`candidate_overlay_align_v`：`top` | `center` | `bottom`。相对装饰框对齐，默认都是 `center`
- `candidate_overlay_offset_x_dip` / `candidate_overlay_offset_y_dip`：对齐之后再平移，`-2000`–`2000`，正数向右、向下
- `candidate_overlay_strip_badge`：`true` 时只画词本身，不带候选后面的标注（如整句模型名），默认 `true`

### `[candidate_window.background]`（可选）

卡片背景图，画在底色之上、文字之下，按圆角裁剪，对深浅两套主题都生效。

- `image`：图片路径
- `fit`：`cover`（铺满裁切）| `contain`（完整显示）| `stretch`（拉伸）
- `opacity`：`0`–`1`

### `canvas` 基底（v2）

全贴图皮肤的地基：面板透明、没有描边、直角、默认没有阴影，候选框长什么样完全由下面的九宫格面板图和挂件决定。它只能作为 `base`，不能在设置页单独选用。

- 文字与高亮用不依赖底色的中性色：深色主题近白文字（`#f2f2f2`）、白色半透明的选中 / 悬停底，浅色主题近黑文字（`#1a1a1a`）、黑色半透明的选中 / 悬停底；高亮圆角 4，可用 `item_corner_radius_dip` 改。贴图偏深或偏浅时用 `[candidate.dark]` / `[candidate.light]` 改配色。
- v1 的键照样可用：配色、`font_family`、`page_arrows`、`fit_content`、`min_width_dip`、`corner_radius_dip`、`border_width_dip`、`shadow`（写 `soft` / `strong` 才有阴影）、`[candidate_window.decoration]`、`[candidate_window.background]`、`[toolbar]`。写了 `border_image` 时，卡片的底色、描边、圆角、阴影和 `background` 背景图都让给面板图。
- 右键菜单没有贴图，沿用 `fluent` 的样式，可用 `[candidate.*.menu]` 改；悬浮工具栏同样沿用 `fluent`，可用 `[toolbar]` 改配色，或用[图片模式](#图片模式mode--image)整个换成贴图。翻页箭头、`fit_content` 的出厂值也取 `fluent` 的默认皮肤设置。

### `[candidate_window.border_image]`（v2 可选）

九宫格面板图，语义同 CSS `border-image`：四角按边宽缩放，四边与中心默认拉伸，写 `repeat` 时改为按源图原尺寸平铺。

- `source`：图片路径，`.png`、`.apng`、`.gif` 或 `.webp`
- `light_source`：可选，浅色主题换用的面板图，格式要求同 `source`。切片、边宽和 `fill` 与 `source` 共用，所以两张图要按同一套几何画；不写时深浅主题都用 `source`
- `slice_dip`：切片线位置，**按源图像素计**（同 CSS `border-image-slice` 的数值，键名里的 `_dip` 是历史写法），1、2 或 4 个 `0`–`500` 的数，按 CSS 简写展开成上右下左（1 个值四边相同，2 个值是「上下 左右」）
- `stretch_dip`：可选，同为源图像素，给出时代替 `slice_dip`。**面板图里画死的装饰必须落在切片之内（可拉伸区之外）**，否则会被拉长；例如帆船画在右侧，就把右侧切片放到船身左侧
- `width_dip`：可选，四边在屏幕上画多宽，**按 DIP 计**（同 CSS `border-image-width`），写法同上。不写时取切片的数值当 DIP。给 2 倍图时写成切片的一半，200% 缩放下逐像素清晰、100% 下是缩小一半的同一张图。两条对边加起来比面板还宽时四边按同一比例缩小
- `fill`：可选，布尔值，是否画中心块，默认 `true`；`false` 时中心留空、透出桌面
- `repeat`：可选，横向/纵向怎么铺，同 CSS `border-image-repeat`，`stretch`（默认）或 `repeat`。写 1 个值两轴相同，写 2 个值是「横向 纵向」。`repeat` 按源图原尺寸平铺，适合花纹边框（例如竖向候选窗两侧重复的铆钉）；四角永远不铺

面板图画在卡片的底色之上、文字之下，不按圆角裁剪（同 CSS）。动图面板也会播放，但面板一般建议用静态图，把动画留给挂件。

### `[candidate_window.border_image.vertical]`（v2 可选）

竖排（`layout == "vertical"`）换用的九宫格面板图。只写与横排不同的键，其余沿用 `[candidate_window.border_image]` 的值。横排和竖排的图尺寸不同时，`slice_dip` 一般要各写一份。`light_source` 不继承——横排那张浅色图是按横排几何画的，竖排要浅色变体就自己写一张。

```toml
[candidate_window.border_image]
source = "assets/horizontal.png"
slice_dip = [8, 45, 6, 66]

[candidate_window.border_image.vertical]
source = "assets/vertical.png"
slice_dip = [26, 10, 26, 10]
repeat = ["stretch", "repeat"]   # 横向拉伸，纵向平铺
```

### `[candidate_window.content_insets_dip]`（v2 可选）

候选框内容与文字的安全内边距，**从面板外缘量起**，用来避开贴图四周自带的花纹或立绘。某一边小于九宫格该边的边宽（`width_dip`）时按边宽算，文字不会压在边框上（同 CSS 的 `border + padding`）。不写时是边宽再加 5 DIP。

- `top` / `right` / `bottom` / `left`：四边内边距，`top` / `bottom` `0`–`200`，`left` / `right` `0`–`500`

竖排面板比横排窄很多时，右、左边距往往没法共用，可以另写一份 `[candidate_window.content_insets_dip.vertical]`：只写要覆盖的边，其余沿用上面的值。

```toml
[candidate_window.content_insets_dip]
left = 66
top = 10
right = 150
bottom = 12

[candidate_window.content_insets_dip.vertical]
left = 10
right = 12
```

### `[candidate_window]` 的 `line_gap_dip`（v2 可选）

预编辑行与候选之间的距离，`0`–`100`。只影响这一处，竖排时候选与翻页箭头之间的间距不变。不写则沿用 `base` 的行距。

### `[candidate_window.outer_insets_dip]`（v2 可选）

挂件挂在卡片外沿时，窗口需要额外预留的透明空白，从卡片四边往外量。

- `top` / `right` / `bottom` / `left`：四边预留量浮点数

**不声明时窗口只按卡片大小开，挂到卡片外面的立绘会被窗口边界裁掉。**

### `[[candidate_window.decorations]]`（v2 可选，数组表）

自由锚定的立绘挂件，可以叠加角色立绘与动图，最多 32 层。

- `image`：图片路径，`.png`、`.apng`、`.gif` 或 `.webp`
- `anchor`：挂件框贴着面板的哪个位置：`top-left` | `top-right` | `bottom-left` | `bottom-right` | `left` | `right` | `top` | `bottom` | `center`，默认 `right`
- `width_dip` / `height_dip`：挂件框的尺寸，`0`–`2000`（不含 0）；图片在框里等比缩放、居中（同 CSS `background-size: contain`）
- `offset_x_dip` / `offset_y_dip`：相对锚点的平移量，`-2000`–`2000`，正数向右、向下
- `z_index`：层级，`-1000`–`1000` 的整数，默认 `2`。面板自身相当于 `1`：**`z_index < 1` 的挂件画在面板之下**（只有伸出面板的部分看得见），其余画在面板与文字之上。同层按 `z_index` 从小到大、相同时按书写顺序叠放

挂件不参与布局，也不接收点击；伸到面板外面时要用 `outer_insets_dip` 给窗口留出空白。

### 动图（v2）

v2 皮肤里的图片可以是动图：APNG（按 [APNG 规范](https://wiki.mozilla.org/APNG_Specification) 解码）、GIF，以及系统 WebP 解码器能逐帧读出的动画 WebP（每帧 100 ms）。

- 帧时长与浏览器一致：`delay_den` 为 0 按 100 算，10 ms 及以下按 100 ms；APNG 的 `num_plays`、GIF 的 NETSCAPE 循环次数生效，播完停在最后一帧。
- 动画从候选窗出现时开始，同一次显示里打字不会让它重头开始；候选窗隐藏或换皮肤后，下一次显示从第一帧开始。
- 系统关闭了动画效果（设置 → 辅助功能 → 视觉效果 → 动画效果）时显示静态图：APNG 显示它的默认图（通常就是第一帧），GIF 显示第一帧。
- 一张动图超过 1024 帧或解码后超过 64 MB、文件损坏或不合规范时只显示第一帧（静态图）。所有动图的帧合计受 64 MB 显存预算约束。
- v1 皮肤里的图片始终是静态首帧，与以往一致。

## `[candidate.dark]` / `[candidate.light]`

`accent`、`selected`、`hover`、`surface`、`border`、`text`、`number`、`translation`、`show_selected_bar`

`candidate_window.backdrop` 不是 `none` 时还可以写两个 `0`–`1` 的数，深浅主题各自设置：

- `backdrop_opacity`：`surface` 色调盖在材质上的不透明度，越高越接近纯色。不写时取 `surface` 自带的 alpha
- `backdrop_luminosity`：亮度层的不透明度。越高，卡片明暗越不受后面画面影响，亮背景上的浅色字也能保住对比度；调低更通透，对比度跟着后面走。不写时 `acrylic` 取 `0.85`、`mica` 取 `1`

Windows 11 的默认值可以作参考：亚克力深色 `#2c2c2c` / `0.15` / `0.96`、浅色 `#fcfcfc` / `0` / `0.85`；云母深色 `#202020` / `0.8` / `1`、浅色 `#f3f3f3` / `0.5` / `1`（依次是 `surface`、`backdrop_opacity`、`backdrop_luminosity`）。

细分配色，都可省略，不写时按右列回落：

| 键 | 作用 | 不写时 |
|---|---|---|
| `candidate_text` | 普通候选的文字 | `text` |
| `preedit_text` | 预编辑（拼音）文字 | `text` |
| `preedit_caret` | 预编辑光标 | `accent` |
| `selected_text` | 选中候选的文字 | `base` 的选中行文字色 |
| `selected_number` | 选中候选的序号 | `base` 的选中行序号色 |
| `translation_pos` | 翻译里的词性（`n.`、`adj.`、`proper n.` 等） | 与翻译同色 |
| `translation_separator` | 翻译里义项之间的分隔符（`;`、`；`） | 与翻译同色 |
| `selected_translation` | 选中候选的翻译 | `base` 的选中行翻译色（微信、杨柳青、秋桂铺实色高亮，各有一组）；`base` 没有时用 `translation`，再不写则取选中文字色的 62% 不透明度 |
| `selected_translation_pos` | 选中候选翻译里的词性 | 写了 `selected_translation` 时与它同色；否则 `base` 的选中行词性色，再回落到 `translation_pos` |
| `selected_translation_separator` | 选中候选翻译里的分隔符 | 写了 `selected_translation` 时与它同色；否则 `base` 的选中行分隔符色，再回落到 `translation_separator` |
| `selected_bar` | 选中项左侧竖条 | `accent` |
| `preedit_background` | 预编辑行底色，圆角同高亮圆角 | 透明 |
| `preedit_divider` | 预编辑行下方 1px 分隔线 | 无 |

设置页里的「候选文字颜色」优先于 `text`、`candidate_text` 和 `preedit_text`。

内置皮肤的翻译用 [Notion 调色板](https://docs.super.so/notion-colors)：单词分别是 fluent 紫、微信蓝、杨柳青绿、石墨棕、微软粉，秋桂浅色橙、深色黄；词性换成相邻的另一个 Notion 色（紫配粉、蓝配绿、绿配蓝、棕配橙、橙配红、黄配橙、粉配紫）再加 `80` 透明度调淡，与单词区分开；分隔符是单词色加 `73` 透明度。包写了 `translation` 而没写 `translation_pos` / `translation_separator` 时，`base` 的词性和分隔符配色一并作废，跟随新的翻译色。想要同样的效果，词性取另一个色相、在色值后补两位十六进制透明度即可：

```toml
translation = "#529cca"
translation_pos = "#4dab9a80"
translation_separator = "#529cca73"
```

### `[candidate.dark.menu]` / `[candidate.light.menu]`（可选）

候选窗右键菜单：`background`、`border`、`text`、`hover`。

## `[toolbar]`

悬浮工具栏样式。所有键都可省略，不写的沿用 `base`。

- `corner_radius_dip`：`0`–`32`
- `[toolbar.dark]` / `[toolbar.light]`：`background`、`border`、`handle`（左侧拖动条）、`divider`、`icon`、`hover`。`handle` 不写时取同一主题的 `[candidate.*].accent`，与选中竖条、预编辑光标同色；`accent` 也没写才沿用 `base`。写的话也请与 `accent` 一致
- `mode`：`bar`（默认，矩形条）| `image`（图片模式，见下）

### 图片模式（`mode = "image"`）

工具栏整个画成一张图，按钮按坐标摆在图上，类似搜狗的状态栏皮肤。不画底色、描边、圆角、阴影、拖动条和分隔线；**窗口按图片的透明区域裁成异形**，完全透明的地方点击会落到后面的窗口上。按钮框总是算在窗口里，所以按钮也可以摆在背景图透明的地方（比如人物旁边的空白处），不必在背景图里给它垫底。图上除按钮以外的地方都能按住拖动。

```toml
[toolbar]
mode = "image"
image = "assets/toolbar.png"
image_width_dip = 160
image_height_dip = 40

[toolbar.buttons.status]
x_dip = 10
y_dip = 7
width_dip = 26
height_dip = 26

[toolbar.buttons.status.images]
cn = "assets/cn.png"
en = "assets/en.png"

[toolbar.buttons.settings]
x_dip = 124
y_dip = 7
image = "assets/settings.png"
```

- `image`：背景图，`.png`、`.apng`、`.gif` 或 `.webp`
- `image_width_dip` / `image_height_dip`：背景图的显示尺寸，大于 `0`、不超过 `2000`。图片拉伸到这个尺寸；窗口按背景图与所有按钮合起来的范围开，按钮排到图外时窗口跟着变大。给 2 倍图时写成像素的一半
- `[toolbar.buttons.<按钮>]`：每个按钮一张表，按钮名见下表，所有键都可省略
  - `x_dip` / `y_dip`：按钮框左上角相对背景图左上角的位置，`0`–`2000`。两个都写了就固定在这里；否则按下面的 `button_flow` 排，皮肤没写 `button_flow` 时这个按钮不画（设置仍可从托盘菜单打开）
  - `width_dip` / `height_dip`：按钮框的宽高，大于 `0`、不超过 `500`，默认 `26`
  - `background`：按钮自己的底图（比如圆底），拉满按钮框，画在按钮图或图标下面，跟着按钮一起显示、一起隐藏。**按钮的底图请画在这里，不要画进背景图**，否则用户关掉按钮后底图还留在图上
  - `icon_size_dip`：没有贴图时内置图标（Segoe Fluent Icons）的字号，大于 `0`、不超过 `500`。不写时是按钮框短边的 `0.82` 倍；按钮框画在一块圆底之类的图案里、又想让图标比框大或小时单独设它
  - `visible`：`false` 时把这个按钮藏起来。皮肤只能藏，用户在设置页关掉的按钮写 `true` 也不会出现
  - `image`：按钮图，在按钮框里等比缩放；各个状态共用，`images` 里缺某个状态时也用它
  - `[toolbar.buttons.<按钮>.images]`：状态名 → 按钮图，只有有状态的按钮能写
  - 既没有 `image` 也没有对应状态的图时，画内置的矢量图标，颜色取 `[toolbar.*].icon`。鼠标悬停和按下时按钮上盖一层 `[toolbar.*].hover`

所有按钮共用的设置写在 `[toolbar.button_defaults]`，每个按钮没写的 `width_dip`、`height_dip`、`icon_size_dip`、`background` 就取这里的。按钮多半是一排或一列，用 `[toolbar.button_flow]` 让工具栏自己排，不用逐个写坐标：

```toml
[toolbar.button_defaults]
width_dip = 30
height_dip = 30
icon_size_dip = 20
background = "assets/plate.png"

[toolbar.button_flow]
x_dip = 170          # 第一个按钮框的左上角
y_dip = 0
direction = "vertical"   # horizontal（默认）| vertical
gap_dip = 2
```

- `button_flow` 只排**当前显示**的按钮，按下表的顺序；写了 `x_dip` 和 `y_dip` 的按钮固定在原处，不占队里的位置；`visible = false` 的也不占
- `x_dip` / `y_dip`：`0`–`2000`；`gap_dip`：按钮之间的空隙，`0`–`500`
| 按钮 | 作用 | 状态名 |
|---|---|---|
| `status` | 中 / 英切换 | `cn`（中文）、`en`（英文）、`ja`（日文）、`caps`（大写锁定）、`en-candidate`（英文候选输入） |
| `fullwidth` | 全 / 半角 | `full`、`half` |
| `punctuation` | 中 / 英标点 | `cn`、`en` |
| `character_set` | 简 / 繁 | `simplified`、`traditional` |
| `emoji` | 表情面板 | 无 |
| `screen_keyboard` | 屏幕键盘 | 无 |
| `settings` | 设置 | 无 |

- 设置页「悬浮工具栏」里的缩放会同时放大背景图、按钮位置和尺寸。
- 用户在设置页关掉某个按钮后：写了坐标的按钮那里就空着；按 `button_flow` 排的按钮后面的会往前补上。打开一个按钮时同理，皮肤没给它写表也会排进队里。
- 动图只在 `schema_version = 2` 的皮肤里播放，规则同[动图](#动图v2)；工具栏隐藏或自动隐藏淡出后停止播放。窗口形状按所有帧合起来的轮廓裁，任何一帧画到的地方都不会被裁掉，也都能点到、能拖动。
- 未知的按钮名、状态名，以及不在 `mode = "image"` 时写的 `image` / `buttons` / `button_defaults` / `button_flow`，不会让皮肤读不出来，只在设置页的诊断信息里提示一条。

## 颜色格式

所有颜色键——`[candidate.dark]` / `[candidate.light]` 里除 `show_selected_bar` 外的键、`[candidate.*.menu]` 和 `[toolbar.dark]` / `[toolbar.light]` 的全部键——都接受以下写法。数字、布尔、枚举和路径类的键不是颜色，不能写成颜色值。

| 写法 | 示例 | 说明 |
|---|---|---|
| `#rgb` / `#rrggbb` | `#e8a`、`#e08aa8` | 不透明 |
| `#rgba` / `#rrggbbaa` | `#e8a4`、`#e08aa847` | 末尾一位或两位是不透明度 |
| `rgb(r, g, b)` | `rgb(224, 138, 168)` | `r`、`g`、`b` 为 0–255 |
| `rgba(r, g, b, a)` | `rgba(224, 138, 168, 0.28)` | `a` 为 0–1 |
| `transparent` | `transparent` | 全透明，等同 `#0000` |

- 十六进制大小写均可，但**必须带 `#`**：这是清单格式的约定，与 CSS 写法一致；渲染器对不带 `#` 的写法有容错，但不要依赖它。
- 不透明度在**末尾**（`#RRGGBBAA`），不是 Windows 常见的 `#AARRGGBB`。例如 `#e9e8e89d` 约为 62% 不透明，`rgba(224, 138, 168, 0.28)` 与 `#e08aa847` 等价。
