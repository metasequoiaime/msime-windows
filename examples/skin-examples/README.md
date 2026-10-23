# Metasequoia IME skin example

水杉输入法（Metasequoia IME）外部皮肤的官方样例。一个皮肤就是一个文件夹，外观全部写在一份 `skin.toml` 里：候选框的装饰图、背景图、圆角和配色，以及悬浮工具栏的配色。不需要写 CSS，也不会加载任何脚本。

更多现成皮肤见仓库根目录的 [skins/](../../skins/)。

- 字段说明：[schema/README.md](schema/README.md)
- 本地预览：[preview.html](preview.html)

## 快速开始

1. 复制 `skins/niya-demo/`，把文件夹名和 `skin.toml` 里的 `id` 改成同一个新名字。
2. 替换 `assets/` 里的图片：
   - `character.png`：卡片上方的人物装饰，建议透明 PNG。
   - `background.png`：卡片背景纹理。深浅两套主题共用这张图，建议大部分透明。
3. 修改 `skin.toml` 中的名称、作者、尺寸和配色。
4. 用浏览器打开 `preview.html`，大致确认横向、纵向两种候选框的效果。
5. 把皮肤文件夹复制到 `%LOCALAPPDATA%\metasequoiaime\skins\`，打开设置里的“皮肤”页面（会自动扫描），或者点“刷新皮肤”。

也可以直接运行安装脚本装上样例皮肤：

```powershell
./install.ps1            # 复制到 %LOCALAPPDATA%\metasequoiaime\skins\niya-demo\ 并启用
./install.ps1 -NoActivate  # 只复制，不改 config.toml
```

启用即把 `config.toml` 中的 `appearance.candidate_skin` 设为 `niya-demo`。装好后重启水杉输入法服务即可生效。

设置页会读取 `skins\` 下每个一级子目录中的 `skin.toml`。目录名与 `id` 不一致或字段不合法时，该皮肤不会出现在可选列表里，原因会显示在页面底部的诊断信息中。

## 目录结构

```text
examples/skin-examples/
├─ skins/
│  ├─ niya-demo/              # 一个皮肤一个文件夹，文件夹名 = id
│  │  ├─ skin.toml            # 元信息、候选框、工具栏、授权
│  │  └─ assets/
│  │     ├─ character.png     # 卡片上方的装饰图
│  │     └─ background.png    # 卡片背景图
│  └─ sakura-night/           # v2 实机演示「夜樱」：面板后的月亮、APNG 花瓣、GIF 星星
│     ├─ skin.toml
│     └─ assets/
│        ├─ panel.png         # 九宫格面板（2 倍图，四角小樱花不随拉伸变形）
│        ├─ moon.png          # 面板下层挂件
│        ├─ petals.apng       # 面板上层的循环 APNG 挂件
│        └─ stars.gif         # 面板上层的循环 GIF 挂件
├─ schema/                    # manifest 字段说明
├─ tools/                     # 生成样例图片的脚本（需要 Pillow）
├─ tests/                     # CI 用的 manifest 与安装脚本测试
├─ preview.html               # 本地预览
└─ install.ps1                # 样例安装脚本
```

只有 `skins/<id>/` 会被安装，`preview.html`、`schema/`、`tools/`、`tests/` 和安装脚本都只是制作工具。

## 样例皮肤能演示什么

`niya-demo` 的 `skin.toml` 每一段都带注释，下面是最常改的几处：

```toml
[candidate_window]
min_width_dip = 176
corner_radius_dip = 12      # 0–32，不写则沿用 base
item_corner_radius_dip = 6  # 高亮圆角，0–16
shadow = "soft"             # none | soft | strong

[candidate_window.decoration]
image = "assets/character.png"
top_inset_dip = 88          # 装饰框高度
width_dip = 102             # 装饰框宽度
align = "right"             # left | center | right

[candidate_window.background]
image = "assets/background.png"
fit = "cover"               # cover | contain | stretch
opacity = 0.3               # 0–1

[candidate.dark]
accent = "#e08aa8"          # 光标与选中胶囊条
selected = "rgba(224, 138, 168, 0.28)"
hover = "rgba(224, 138, 168, 0.16)"
preedit_text = "#f0b8cb"     # 细分配色都可省略，见 schema/README.md
preedit_divider = "rgba(224, 138, 168, 0.30)"

[candidate.dark.menu]         # 右键菜单
background = "#221a1e"
hover = "rgba(224, 138, 168, 0.20)"

[toolbar]
corner_radius_dip = 8

[toolbar.dark]
background = "#221a1e"
handle = "#e08aa8"
icon = "#f6e4eb"
```

几个要点：

- 装饰图放在一个 `width_dip × top_inset_dip` 的框里等比缩放，框贴在卡片正上方，不会与卡片重叠。框的比例和原图一致时就不会留白，比如样例原图是 408×353，所以用了 102×88。
- 卡片实际最小宽度取 `min_width_dip` 和装饰图 `width_dip` 中较大的那个。
- 背景图画在底色之上、文字之下，并按圆角裁剪。
- 细分配色（普通候选文字、预编辑文字与光标、选中行的文字 / 序号 / 翻译 / 竖条、预编辑底色与分隔线）不写时都回落到 `text` 或 `accent`，完整的回落规则见 [schema/README.md](schema/README.md)。设置页里的「候选文字颜色」仍优先于皮肤的文字色。
- 所有颜色键都可以写十六进制 `#rgb` / `#rgba` / `#rrggbb` / `#rrggbbaa`，也可以写 `rgb()` / `rgba()` 或 `transparent`。十六进制必须带 `#`，不透明度在末尾（`#RRGGBBAA`），详见 [schema/README.md](schema/README.md#颜色格式)。
- `font_family` 排在用户设置的字体前面，缺字时回落到用户字体；字号始终由用户设置决定。
- `page_arrows = true` 可以打开候选框里的翻页箭头；不写时沿用 `base` 内置皮肤的设置（出厂为关闭）。
- `fit_content = true` 让卡片严格按候选文字的宽度收紧，不再垫出厂最小宽度；`min_width_dip` 和装饰图宽度仍然生效。不写时沿用 `base` 内置皮肤的设置（出厂为关闭）。
- `[toolbar]` 下的所有键都可以省略，没写的沿用 `base` 皮肤；只有拖动条 `handle` 例外，没写时跟同一主题的 `accent`。
- `[supports].themes` 里声明的每套主题都要有对应的 `[candidate.<theme>]`；写了 `[toolbar]` 的话，也要有对应的 `[toolbar.<theme>]`。

完整字段见 [schema/README.md](schema/README.md)。

## 全贴图皮肤（schema_version 2）

`sakura-night` 和随安装包分发的 [`绿叶`](../../skins/green_leaf/skin.toml)（原 `canvas-demo`）都是 v2 皮肤：`schema_version = 2`、`base = "canvas"`，候选框的外观全部来自贴图。下面的片段取自绿叶。

```toml
schema_version = 2
base = "canvas"                  # 九宫格与挂件只在 canvas 上生效；base 不是 canvas 时按 1 加载

[candidate_window.border_image]
source = "assets/panel.png"      # 64×64 的 2 倍图
light_source = "assets/panel-light.png" # 可选：浅色主题换这张，几何与 source 相同
slice_dip = [20]                 # 切片线，按源图像素
width_dip = [10]                 # 边框绘制宽度，按 DIP：2 倍图写切片的一半，200% 缩放下逐像素清晰

[[candidate_window.decorations]]
image = "assets/sparkle.apng"    # APNG / GIF 动图会播放
anchor = "top-right"
offset_x_dip = 10
offset_y_dip = -12
width_dip = 24
height_dip = 24
z_index = 2                      # z_index < 1 画在面板之下，其余画在面板之上
```

- `canvas` 本身透明、无描边、无圆角、无阴影，不能单独选用；面板图、挂件和配色都由皮肤给出。
- 面板四角画死的花纹要落在切片之内，否则会被拉伸。中心块可以用 `fill = false` 留空。
- 文字离面板外缘的距离用 `content_insets_dip`，挂件伸出卡片时用 `outer_insets_dip` 给窗口留出空白。
- 动图在系统关闭「动画效果」时只显示第一帧。动图解码后合计不超过 64 MB、最多 1024 帧，超出时只显示第一帧。
- 绿叶和夜樱的图都是程序生成的几何图形，以 MIT 授权，可以直接改用；绿叶的浅色面板由 `tools/gen_green_leaf_panel.py` 生成。

`sakura-night`（夜樱）是一个更完整的实机演示，把 v2 的能力都用上了：

- 紫夜玻璃九宫格面板，四角的小樱花落在 24 像素切片内，候选框怎么伸缩都不变形。
- 月亮挂在右上角、`z_index = -1` 画在面板之后，只露出伸出面板的那一截。
- 右侧飘落的花瓣是 24 帧 × 70 ms 的循环 APNG，左下闪烁的星星是 8 帧 × 150 ms 的循环 GIF。
- 用 `content_insets_dip` 让文字避开边框和四角花纹，用 `outer_insets_dip` 给伸出去的挂件留出窗口空间，用 `line_gap_dip` 拉开预编辑行与候选。
- 左上角刻意不放挂件：预编辑文字从那里开始，挂件压上去会挡字。

它的图片都由 `tools/gen_sakura_night.py` 生成（需要 Pillow），重新运行会得到同样的文件：

```powershell
python tools/gen_sakura_night.py skins/sakura-night
```

## 制作约束

- 只用 `skin.toml` 和本地图片，不支持自定义 JavaScript，也不再读取 `cand.css` 或工具栏 CSS。
- 图片路径相对于皮肤目录，不能指向目录之外。
- `id` 只能包含小写字母、数字、点、下划线和连字符，最长 64 个字符。
- 单张图片建议不超过 500 KB（CI 会检查 PNG、APNG、GIF 和 WebP）。

## 测试

```powershell
python -m unittest discover -s tests -v   # 检查 manifest 与引用的图片
./tests/install.ps1                        # 在临时目录里测试安装、启用和重复安装
```

## 授权提示

**本目录采用 MIT 授权**（见 [LICENSE](LICENSE)），不随仓库其余部分的 GPL-3.0。这个目录就是拿来复制的：把它整个拷走，或者把 `skins/niya-demo/` 整个拷走改成自己的皮肤，都不需要额外授权。之所以用 MIT 而不是产品代码的 GPL-3.0，就是为了让你做出来的皮肤可以按自己的意愿授权。

皮肤代码和图片资源可以用不同的许可，发布前请在 `skin.toml` 的 `[license]` 里分别写明 `code` 和 `assets`。样例中的 `assets` 标为 `UNVERIFIED-DEMO-ONLY`，意思是这些图片只用于演示、来源未经核实，**请不要直接拿去发布**，换成你有权使用的图片。
