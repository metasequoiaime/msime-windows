# 外部皮肤 manifest（TOML）

每个皮肤目录需要 `skin.toml`，`id` 必须与文件夹名一致。`schema_version` 支持 `1` 与 `2`（v2 为面向全贴图与复杂立绘皮肤的高级规范，仅在 WebView2 端全面启用，Direct2D 原生端保底降级）。

候选框和悬浮工具栏都由 `skin.toml` 声明，不要再提供 `cand.css` 或工具栏 CSS。所有图片路径都相对于皮肤目录，且不能指向目录之外。

## 顶层字段

- 必填：`schema_version`（`1` 或 `2`）、`id`、`name`、`version`、`base`（继承的内置皮肤：`fluent`、`wechat`、`graphite`、`willow_green`、`autumn_osmanthus`、`microsoft`；或与 msime 共享的基底：`system`、`shuishan`、`light`、`paper`、`night`、`ink`，按 `fluent` 的结构绘制并补上基底配色，固定明暗的基底只在对应的 `themes` 下生效）
- 可选：`author`、`description`
- `[supports]`：`layouts`（`horizontal` / `vertical`）、`themes`（`dark` / `light`）
- `[license]`：`code`、`assets`

## `[candidate_window]`

- `min_width_dip`：卡片最小宽度；实际最小宽度还会被装饰图的 `width_dip` 撑大
- `corner_radius_dip`：卡片圆角，`0`–`32`；不写则沿用 `base`
- `border_width_dip`：卡片外框线宽，`0`–`4`；颜色取 `border`，不写 `border` 时取 `base` 的边框色（杨柳青、秋桂没有外框，此时仍不可见）
- `item_corner_radius_dip`：选中 / 悬停高亮的圆角，`0`–`16`；贴着卡片四角的那几个角仍跟 `corner_radius_dip`
- `shadow`：卡片阴影，`none` | `soft` | `strong`；不写则沿用 `base`。只影响候选卡片，不影响右键菜单
- `font_family`：候选字体名，UTF-8 编码后最长 64 字节（约 21 个汉字），不能含引号、反斜杠、逗号、分号、尖括号、花括号和反引号。排在用户设置的字体前面，缺字时仍回落到用户字体；字号不由皮肤决定
- `page_arrows`：候选框里的翻页箭头，`true` | `false`。横排时是候选右侧贴着最后一行的一小列，竖排时是最后一行下方的一矮行、“‹”与序号左对齐；颜色取 `number`，悬停底色取 `hover`。不写则沿用 `base` 的默认皮肤设置（数据目录 `skins\default\<base>\skin.toml`，出厂为 `false`）
- `fit_content`：卡片是否严格按候选文字所需的宽度收紧，`true` | `false`。打开后不再垫出厂的最小宽度（D2D 160 DIP、WebView2 `7em`），同一次输入里后到的翻译等内容消失时宽度也立刻缩回；`min_width_dip` 和装饰图 `width_dip` 仍然生效。不写则沿用 `base` 的默认皮肤设置（出厂为 `false`）

### `[candidate_window.decoration]`（可选）

卡片上方的装饰图。写了这张表就必须同时给出 `image`、`top_inset_dip`、`width_dip`。

- `image`：图片路径
- `top_inset_dip` / `width_dip`：装饰框的高和宽，框贴在卡片上方，不与卡片重叠；图片在框内等比缩放
- `align`：`left` | `center` | `right`，相对卡片

### `[candidate_window.background]`（可选）

卡片背景图，画在底色之上、文字之下，按圆角裁剪，对深浅两套主题都生效。

- `image`：图片路径
- `fit`：`cover`（铺满裁切）| `contain`（完整显示）| `stretch`（拉伸）
- `opacity`：`0`–`1`

### `[candidate_window.border_image]`（v2 可选，仅 WebView2）

九宫格背景拉伸，面板整体贴图，用于全贴图与复杂纹理面板。

- `source`：图片路径（如 `assets/skin1.png`，支持 APNG/WebP/PNG）
- `slice_dip`：`[top, right, bottom, left]` 切片线位置，正数数组，支持 1~4 个值
- `stretch_dip`：可选，可拉伸区，语义同 `slice_dip` 但更直白，给出时覆盖它。**面板图里画死的装饰必须落在可拉伸区之外**，否则会被横向拉长；例如帆船画在右侧，就把右侧切片放到船身左侧
- `width_dip`：可选，边框绘制宽度，支持 1~4 个值
- `fill`：可选，布尔值，是否保留九宫格中心内容区域，默认为 `true`

### `[candidate_window.content_insets_dip]`（v2 可选，仅 WebView2）

候选框内容与文字的安全内边距，**从面板外缘量起**，用来避开贴图四周自带的花纹或立绘。渲染时会把该方向的边框宽度扣掉，落到内容盒上。

- `top` / `right` / `bottom` / `left`：四边内边距浮点数

### `[candidate_window]` 的 `line_gap_dip`（v2 可选）

预编辑行与候选行之间的行距，`0`–`100`。不写则沿用 `base` 的行距。

### `[candidate_window.outer_insets_dip]`（v2 可选）

挂件挂在卡片外沿时，窗口需要额外预留的透明空白，从卡片四边往外量。

- `top` / `right` / `bottom` / `left`：四边预留量浮点数

**不声明时窗口只按卡片大小开，挂到卡片外面的立绘会被窗口边界裁掉。**

### `[[candidate_window.decorations]]`（v2 可选，数组表，仅 WebView2）

自由锚定立绘挂件层，支持叠加角色立绘与 APNG 动图，层数不限。

- `image`：图片路径
- `anchor`：`top-left` | `top-right` | `bottom-left` | `bottom-right` | `left` | `right` | `top` | `bottom` | `center`
- `width_dip` / `height_dip`：挂件的尺寸
- `offset_x_dip` / `offset_y_dip`：相对锚点的平移量，正数向右、向下，两个后端同一套语义
- `z_index`：层级，默认为 `2`

## `[candidate.dark]` / `[candidate.light]`

`accent`、`selected`、`hover`、`surface`、`border`、`text`、`number`、`translation`、`show_selected_bar`

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

悬浮工具栏样式，D2D 与 WebView2 两个渲染器都生效。所有键都可省略，不写的沿用 `base`。

- `corner_radius_dip`：`0`–`32`
- `[toolbar.dark]` / `[toolbar.light]`：`background`、`border`、`handle`（左侧拖动条）、`divider`、`icon`、`hover`

## 颜色格式

所有颜色键——`[candidate.dark]` / `[candidate.light]` 里除 `show_selected_bar` 外的键、`[candidate.*.menu]` 和 `[toolbar.dark]` / `[toolbar.light]` 的全部键——都接受以下写法。数字、布尔、枚举和路径类的键不是颜色，不能写成颜色值。

| 写法 | 示例 | 说明 |
|---|---|---|
| `#rgb` / `#rrggbb` | `#e8a`、`#e08aa8` | 不透明 |
| `#rgba` / `#rrggbbaa` | `#e8a4`、`#e08aa847` | 末尾一位或两位是不透明度 |
| `rgb(r, g, b)` | `rgb(224, 138, 168)` | `r`、`g`、`b` 为 0–255 |
| `rgba(r, g, b, a)` | `rgba(224, 138, 168, 0.28)` | `a` 为 0–1 |
| `transparent` | `transparent` | 全透明，等同 `#0000` |

- 十六进制大小写均可，但**必须带 `#`**：原生（D2D）渲染器不带 `#` 也能识别，WebView2 渲染器按 CSS 解析，会把它当作无效值，两种渲染结果就不一致了。
- 不透明度在**末尾**（`#RRGGBBAA`），不是 Windows 常见的 `#AARRGGBB`。例如 `#e9e8e89d` 约为 62% 不透明，`rgba(224, 138, 168, 0.28)` 与 `#e08aa847` 等价。
