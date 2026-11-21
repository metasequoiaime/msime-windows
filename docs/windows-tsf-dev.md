# Windows TSF 本地开发与验证

本文件补充 `windows/AGENTS.md` 的「构建与验证」：改完 TSF DLL 或 `ui/` 的文本控件后，怎样在真实宿主里快速验证，以及验证时看哪份日志。

## 快速验证 DLL 改动

适用于改了 `windows/src/` 或 `ui/src/tsf/`、需要在真实宿主里看行为的场景。先构建 x64 Release：

```powershell
cmake --build windows/build64-release --config Release
```

Windows 允许重命名正在被加载的 DLL，所以不必关掉宿主，也不必出安装包：

```powershell
$dir = "C:\Windows\System32\IME\MetasequoiaIME\msime_v<版本>"
Rename-Item "$dir\MetasequoiaImeTsf.dll" "MetasequoiaImeTsf.dll.old"
Copy-Item windows\build64-release\Release\MetasequoiaImeTsf.dll $dir\
```

这两步需要管理员权限。之后重启宿主进程（QQ、浏览器、记事本……），只有新启动的进程会加载新 DLL。

回滚：删掉新的 `MetasequoiaImeTsf.dll`，把 `.old` 改回原名，再重启宿主。

这条回路只用于本地自测；安装包、签名和发布走 `installer/` 的流程。

## 图标资源（.ico）的制作

`windows/image/` 下的图标都是多尺寸 ICO。从 SVG 或大尺寸 PNG 生成，统一用仓库脚本（需要 ImageMagick 7，`magick` 在 PATH 上）：

```powershell
.\scripts\convert-png-to-ico.ps1 <源文件.svg|.png> <输出.ico> [-Force]
```

脚本等价于 `magick input -background none -gravity center -extent <正方形> -filter Lanczos -define icon:auto-resize=... output.ico`，但多做了几件手写命令容易漏的事：

- **帧尺寸**：默认 `256,128,64,48,44,40,36,32,28,24,20,16`。比常见的 `256,128,64,48,32,24,16` 多出 20/28/36/40/44，它们是任务栏和 Win+Space 在 125%、175%、225%、250%、275% 缩放下实际要的尺寸；缺了这些帧，系统会拿相邻尺寸缩放，图标发虚。
- **非正方形输入**：居中放到透明正方形画布上，每边留 `-PaddingPercent`（默认 3%）。不要直接 `-resize` 成正方形，那会把图拉变形。
- **SVG 输入**：先按 `-Density`（默认 192 DPI）栅格化，`-background none` 必须写在输入文件之前，否则背景是白的。保证每一帧都是从大图缩小，不要把小 PNG 放大成 256 帧。
- **校验**：生成后读回帧数，不对就报错。

生成后至少看一眼 16、24、32 三帧（`magick "out.ico[11]" -scale 800% preview.png`），细节在小尺寸下会糊成一团，必要时为小尺寸单独简化图形。

源文件（SVG）和产物（ICO）一起提交：`windows/image/profile-icons/<id>.svg` 旁边放 `<id>.ico`。只提交 ICO 而丢了源文件，日后就没法重新导出别的尺寸。

### 可选的输入法图标（Win+Space 里那个）

语言配置（profile）图标的机制见 [windows/AGENTS.md](../windows/AGENTS.md#资源注册与日志)。新增一款可选图标：

1. 把源文件和用上面脚本生成的 ICO 放进 `windows/image/profile-icons/`，文件名 `<id>.svg` / `<id>.ico`，`id` 只用小写字母、数字和连字符；
2. 在 `server/src/settings/profile_icon.cpp` 的 `Builtins()` 里加一行 `{id, 显示名, L"profile-icons/<id>.ico"}`；
3. 本地跑 `test_profile_icon`（`ctest -R profile_icon`），它会检查 `Builtins()` 里每个 id 在 `windows/image/profile-icons/` 下都有一个有效的 ICO。

`Prepare-PackageFiles.ps1` 把该目录下的 `*.ico` 装到数据目录的 `assets\settings\profile-icons\`，`default.ico` 由 `windows/image/msime.ico` 复制而来，只供设置页预览。

端到端验证要装好的输入法和管理员授权：设置 → 高级 →「输入法图标」选一款，点「应用」并在 UAC 里确认，然后检查

```powershell
reg query "HKLM\SOFTWARE\Microsoft\CTF\TIP\{E3062E9A-D834-4637-8958-ED8CFA427D01}\LanguageProfile\0x00000804\{4D59B1B4-D503-44AE-9259-BAD9BB2778AB}" /v IconFile
reg query "HKLM\SOFTWARE\Metasequoia\MetasequoiaIME" /v ProfileIconFile
```

`IconFile` 应指向 `System32\IME\MetasequoiaIME\MetasequoiaImeTip-<哈希>.ico`，`SysWOW64` 下同名目录里也应有同一个文件。然后在任务管理器里重新启动「Windows 资源管理器」（explorer.exe 缓存了 profile 图标，不重启不会换），再按 Win+Space 看图标。选回「水杉（默认）」应把 `IconFile` 改回 `MetasequoiaImeTip.ico` 并删掉 `ProfileIconFile`。

## 诊断日志

日志在 `%LOCALAPPDATA%\metasequoiaime\logs\`，文件名形如 `水杉IME诊断日志-tsf-<时间戳>.log`，Server 每次重启会新建一份。

排按键和提交问题时看这些字段：

| 字段 | 含义 |
|---|---|
| `stage=` | 按键走到了哪一步：`keydown-classified`、`edit-session-complete`、`deferred-replay-complete` 等 |
| `composing=` / `buffer_len=` | 当前是否在组合中、组合里的字符数 |
| `result=` | 该步骤的 HRESULT |
| `uiless=` | 这一拍 TSF 是否认为宿主自己画候选。首键与组合开始时的取值不同，能直接看出 UILess 标志是否慢了一拍 |
| `process=` | 宿主进程名，用来区分 QQ、浏览器、记事本 |
| `[host-window]` | 组合开始与结束、UI element 的创建与销毁 |

「提交 → 结束组合」的先后顺序能区分是 IME 提交成功，还是宿主中途插手。
