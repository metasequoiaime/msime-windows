# Metasequoia IME Installer

本目录的脚本从 Windows 合仓目录收集产物、签名、用 Inno Setup 打成安装包。它服务两条流程：

- **正式发布**：由 `MSIME-Windows` 的 release workflow 驱动，用真实证书签名包内全部 EXE/DLL 和最终安装包；产物是挂在 Release 上的 `MetasequoiaIME_Setup_v<版本>.exe`，并携带与 Release 二进制匹配的 PDB 符号文件。见下面「CI 契约」
- **本地测试**：手工跑，用本机自签名证书，用来在自己机器上验证安装流程。本文其余部分讲的是这条

版本号不是固定的，由 `Prepare-PackageFiles.ps1` 的 `-TargetVersion` 决定，默认 `0.0.1`；正式发布时 CI 传入真实版本。

本仓库 **不包含** 正式代码签名证书、私钥、指纹，也不包含编译好的 EXE/DLL、词库或安装包。

签名脚本会在你自己的机器上生成并复用一张自签名测试证书。

## CI 契约

根 `.github/workflows/release.yml` **不加修改地调用本目录的 `Prepare-PackageFiles.ps1` 和 `Compile-Installer.ps1`**。源目录名是 `Prepare-PackageFiles.ps1` 的参数，不是写死在脚本里的字面量：默认值为 `windows/`、`server/`、`ui-html/` 和仓根授权文件；旧目录或自定义布局通过参数覆盖。回归检查同时执行默认路径和显式参数的打包。辅助码默认读取 `engine/helpcode/`，词库缓存位于 `MetasequoiaImeDict/out/`。

合仓后 release workflow 传的是本仓的目录名，辅助码随固定 Engine gitlink 检出，词库按产品锁下载到仓根缓存：

```powershell
pwsh -File ./Prepare-PackageFiles.ps1 -TargetVersion 1.2.3 -RepoRoot .. `
    -TsfDirectory windows -ServerDirectory server `
    -UiHtmlDirectory ui-html -NoticesDirectory .
```

由此产生几条约束：

- **改动 `Prepare-PackageFiles.ps1` 里那些 `Assert-PathExists` 的源路径，会直接弄坏发布流水线。** 它断言的源路径由 workflow 逐一准备，改了要同步改 `.github/workflows/release.yml`。目录名本身可以由调用方指定，但每个目录内部的相对路径（如 `build-release\bin\Release`）仍是硬契约
- **`THIRD_PARTY_NOTICES.txt` 从 `-NoticesDirectory` 指向的目录取，随安装包装到程序目录。** 合仓后这份声明覆盖整个产品、放在仓根，所以它和 `-TsfDirectory` 分成了两个参数。词库主体含 rime-ice（GPL-3.0）内容，其许可要求保留署名，所以这份文件缺失会让打包直接失败，而不是静默跳过
- **`Sign-PackageBinaries-Local.ps1` 和 `Sign-Installer-Local.ps1` 只用于本地验证，CI 不会调用它们。** 它们创建本机自签名证书并尝试写入受信任存储，正式发布走 workflow 里用仓库 secret 中真实证书的签名步骤
- 词库不再从相邻的 `MetasequoiaImeDict` 工作目录取，CI 从产品锁指定的 `dict-*` release 下载并校验 SHA256，再放到脚本期望的位置
- 语言模型 `sc.lm` 不在那个 release 里。它由 `scripts/build-language-model.ps1` 在打包前从 `language-model/lock.json` 钉住的 libime 语料转换出来，下载摘要和产物摘要都要对上。release workflow 里对应的是「Build the word-lattice language model」这一步，缺了它 `Prepare-PackageFiles.ps1` 会直接失败
- `windows-2025` runner 自带 Inno Setup 6.7.1，`Compile-Installer.ps1` 能自己找到 `ISCC.exe`。但它不带 `ChineseSimplified.isl`，`msime_setup.iss` 的 `[Languages]` 段依赖那个文件，所以 workflow 会在编译前按固定 revision 和校验和把它装进去

## 数据目录

程序本体固定装在 `Program Files\metasequoiaime`（Server 带 `uiAccess=true`，只有装在受信任目录里这个标志才生效），**用户数据目录可以在安装向导里改**：词库、`config.toml`、用户词库、皮肤和前端资源都在那里，整包几百 MB，装 C 盘吃紧的用户可以放到别的盘。

- 默认值 `%LOCALAPPDATA%\metasequoiaime`；升级安装时默认沿用上次的位置
- 选择写进 `HKLM\Software\Metasequoia\MetasequoiaIME` 的 `DataDir`。这是运行期唯一的权威来源：Server（`server/src/utils/ime_paths.cpp`）、TSF DLL（`windows/src/Utils/FanyUtils.cpp`）和引擎（`engine/core/data_path.h`）各自按 `METASEQUOIA_IME_DATA_DIR` 环境变量 → 该注册表值 → `%LOCALAPPDATA%\metasequoiaime` 的顺序解析，三处必须保持一致。32 位 TSF DLL 用 `KEY_WOW64_64KEY` 读，所以这个值必须写在 64 位视图里
- 升级时改了位置，安装器会把用户词库（`msime_user.db` 及其 WAL/SHM）、`config.toml`、`config.base.toml` 和 `skins\` 搬到新目录，再删掉旧目录；词库和前端资源由本次安装重新写入，不搬
- **数据目录必须整个归输入法**（[#537](https://github.com/metasequoiaime/MSIME-Windows/issues/537)）。用户选的目录里已经有别的文件、或者选了驱动器根目录时，安装器改用其中的 `metasequoiaime` 子目录，并在向导里把实际位置告诉用户；连那个子目录也不空就拒绝。输入法用过的目录（有标记，或历史默认位置）原样沿用
- 安装器在数据目录里放一个 `.metasequoiaime-data` 标记文件。只有标记里带 `exclusive: adopted empty by the installer` 这一行（安装时目录是空的），或目录就是历史默认位置，覆盖安装才整目录清理、卸载才整目录删除
- 旧版安装器不管目录里原来有什么都写标记，所以只有旧标记（没有那一行）的目录不能再信：覆盖安装、换目录和卸载都只按 `IsShippedAppDataItem` / `IsRuntimeAppDataItem` / `IsPreservedAppDataItem` 名单删输入法自己的条目，`html`、`logs`、`skins` 这种通用目录名只删输入法写进去的部分，最后目录空了才删目录。包里新增顶层文件要同步 `IsShippedAppDataItem`，`tests/package-files.ps1` 会拿真实打出的 `app_data` 核对；Server 在数据目录根下新写文件要同步 `IsRuntimeAppDataItem`
- 仓库 `skins\` 下的外部皮肤（除 `default\` 和素材授权为 `UNVERIFIED*` 的）随包装到 `skins\<id>`，完整包和轻量包都带，卸载不删。`Prepare-PackageFiles.ps1` 同时写出 `bundled_skins.manifest`（每行 `id|相对路径|sha256`），安装器在 `PrepareToInstall` 里拿它核对用户机器上的同名目录：文件集合和内容完全一致就直接覆盖，否则先改名成 `<id>.bak`（已有就 `<id>.2.bak`……，不覆盖旧备份）再装。Server 扫描皮肤时跳过 `*.bak` 目录。`tests/bundled-skins.ps1` 把这段 Pascal 编进探针安装器真跑一遍
- 静默安装用 `/DATADIR="D:\MetasequoiaIME"` 指定；该值在 `PrepareToInstall` 里和向导页走同一套换子目录和校验

## 运行时依赖检查

安装包不携带 WebView2 Runtime 和 VC 运行库——前者有自己的 Evergreen 更新通道，后者是系统级共享组件。缺 VC 运行库 Server 起不来；缺 WebView2 设置页打不开，但默认的 D2D 候选窗（以及表情 / 键盘 / 手写面板，都是 `msimeui` 原生绘制）照常可用，选了 `appearance.ui_backend = "webview2"` 的 Server 启动时检测不到运行时会临时退回 D2D（`FallBackToD2dIfWebView2RuntimeMissing`，不改配置，装好后下次启动自动恢复）。这些故障都出现在安装结束之后，用户看不到任何解释。所以 `msime_setup.iss` 的 `InitializeSetup` 在安装开始前先查一遍，缺哪个就当场说清楚，并给出下载地址。

- **Microsoft Edge WebView2 Runtime**：读 EdgeUpdate 客户端键 `{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}` 的 `pv`。机器级安装由 32 位的 EdgeUpdate 写入，所以主查 `HKLM32`，再看 `HKLM64` 和 `HKCU`（用户级安装）。`pv` 缺失或残留成 `0.0.0.0` 都算没装
- **Visual C++ 2015–2022 可再发行组件包（x64）**：读 `Software\Microsoft\VisualStudio\14.0\VC\Runtimes\x64` 的 `Installed` / `Major` / `Minor`，`HKLM32` 和 `HKLM64` 两个视图都查。要求 **14.40 以上**（常量 `VCRuntimeMinMinor`）：`server_exe\` 里的 exe 用 MSVC 14.40+（VS 2022 17.10 起）编译并导入 `msvcp140.dll`，这一版起 `std::mutex` 的构造函数是 constexpr，配上更旧的 `msvcp140.dll` 会在第一次加锁时访问冲突、无声闪退——设置页「点了没反应」就是这个现象。装了 2019 / 早期 2022 运行库的机器同样有 `Installed=1`，所以必须比版本号。构建工具链升级到要求更高 redist 时同步调高这个常量。32 位 TSF DLL 用静态 CRT（`windows/CMakeLists.txt` 的 `CMAKE_MSVC_RUNTIME_LIBRARY`），所以不检查 x86 redist
- **不要改成查 `System32` 里的 DLL。** Setup.exe 是 32 位进程，Pascal Script 的 `FileExists` 会被 WOW64 重定向到 `SysWOW64`，查到的是 x86 运行库；只装了 x64 redist 的机器会被误判成缺少 x64 运行库。绕开重定向的办法不可移植：`EnableFsRedirection` 在 Inno Setup 7 里已被移除，而它的替代品在 CI 用的 6.x 里还不存在
- 缺组件或版本过旧时弹一次对话框，区分「没装」和「已装 x.y 但过旧」，列出直接下载链接（VC++：`https://aka.ms/vc14/vc_redist.x64.exe`；WebView2 Evergreen Bootstrapper：`https://go.microsoft.com/fwlink/p/?LinkId=2124703`）和对应的说明页：选「是」直接下载这些安装包并结束安装，选「否」继续装。**不阻断安装**——用户可能马上就要去补组件，也可能只是在准备环境。静默安装（CI、批量部署）用 `SuppressibleMsgBox` 默认「否」直接继续，缺什么已经写进安装日志（`/LOG=`）
- 安装时检查只是第一道关：用户可能选了「否」，也可能事后卸掉或损坏了 WebView2。所以设置进程在 WebView2 environment / controller 创建失败（含运行时缺失时的同步失败）时会弹框说明原因和错误码、给出同一条 WebView2 直链并退出，而不是留一个空白窗口

## 依赖

- Windows 10/11，PowerShell 7（`pwsh`）
- [Inno Setup](https://jrsoftware.org/isinfo.php) 6.6 或更高版本
- Windows SDK（提供 `signtool.exe`）
- 先初始化本仓 submodule，并完成 `windows/`、`server/` 的 Release 编译以及 `ui-html/` 设置页构建。Release 构建必须生成同目录 PDB；打包脚本会拒绝缺少匹配符号的产物（符号是否随包安装是另一回事，由 `-IncludeSymbols` 决定）。
- 本地测试包的词库从与本仓同级的 `msime-dictionary` 检出构建（见下文 `build-dictionary.py`），`Invoke-LocalTest.ps1` 打完整包前会自动调用，不再下载产品锁的 Release；需要复现发布用的词库时再手动运行 `python scripts/product_lock.py fetch-dictionaries --staging-root .`。
- 词格整句打分用的 `sc.lm` 由 `scripts\build-language-model.ps1` 从 `language-model\lock.json` 钉住的上游语料转换而来。`Invoke-LocalTest.ps1` 打完整包前会自动调它，一般不用手工执行；首次运行要下 ~75 MB 并转换一次，之后摘要对得上就直接跳过。
- 本地打完整包前，可以直接运行 `scripts\prepare-package-inputs.ps1`，一次准备好神经整句模型、`sc.lm` 和词库。模型按各自的锁校验 SHA256，已就绪的直接跳过；词库用 `build-dictionary.py --if-stale` 从本地 `msime-dictionary` 构建。
- 想在本地从 msime-dictionary 的文本源数据构建词库，运行 `python scripts\build-dictionary.py [--dictionary <msime-dictionary checkout>]`（默认读与本仓同级的 `msime-dictionary`，需要 `pip install pypinyin`）：它按 msime Rust 构建器的规则读 `sources/` 与 `custom/`，加上 `scripts\dictionary_build\inputs.lock.json` 固定的仓外文件，生成本仓现有的 `msime.db` / `english.db` / `others.db` / `dict_japanese.dat` 布局写到 `MetasequoiaImeDict\out\`（产品锁的旧词库先备份到 `out.bak\`）。加 `--compare-release dict-vX.Y.Z` 会与官方 Release 逐表对比。加 `--if-stale` 时，`out\` 已由当前 `msime-dictionary` 提交和当前构建脚本构建、且工作区干净就跳过，否则重建；`Invoke-LocalTest.ps1` 与 `prepare-package-inputs.ps1` 都这样调用。CI 与正式发布仍按产品锁取词库。
- `engine/helpcode/helpcodes/` 是本仓的目录，普通检出即有，无需旧 HelpCode 仓库，也无需初始化 submodule。

## 本地打包路径

从 `installer/` 运行上面的带参数打包命令即可。`-RepoRoot` 指向 MSIME-Windows 根目录；目录不同时通过参数指定，无需编辑脚本中的复制路径。

## 一键测试

先准备上述构建依赖与锁定词库。然后在管理员 PowerShell 中（把本机测试证书写入受信任存储需要提升权限）：

```powershell
cd path\to\MSIME-Windows\installer
pwsh -File .\test.ps1
```

`test.ps1` 会按顺序做这些事：

1. 编译本仓 TSF、Server，并构建设置页；缺少组件入口则失败
2. `Prepare-PackageFiles.ps1` — 收集 `server_exe\`、`tsf_dll\`、`app_data\`，并把 `msime_setup.iss` 的版本写成 `0.0.1`
3. `Sign-PackageBinaries-Local.ps1` — 本机自签名包内 EXE/DLL
4. `Compile-Installer.ps1` — 编译出 `Output\MetasequoiaIME_Setup_v0.0.1.exe`
5. `Sign-Installer-Local.ps1` — 本机自签名安装包
6. `Install.ps1` — 启动安装程序

也可以按上面的顺序逐步运行。

## SimplySign 真签名打包

需要在本机用 Certum SimplySign 中的真实 Code Signing 证书制作可分发安装包时，先启动
SimplySign Desktop、用手机 OTP 连接虚拟卡，然后执行：

```powershell
cd path\to\MSIME-Windows\installer
pwsh -File .\package-simplysign.ps1 1.2.3
```

命令行版本会同时写入本轮 TSF DLL 的版本资源和安装包元数据（脚本结束后恢复源码模板）。脚本会
增量编译 TSF、Server 和设置页，制作完整包，用 `http://time.certum.pl` 的 RFC 3161 时间戳签名
`server_exe\` 和 `tsf_dll\` 下的**全部 EXE/DLL**，编译 Inno Setup 安装包，再签名并校验最终
安装包。产物位于 `Output\MetasequoiaIME_Setup_v1.2.3.exe`；脚本只产出文件，不会自动启动
安装程序。

**为什么整包都要签。** `MetasequoiaImeServer.exe` 是唯一一个技术上必须签的——`uiAccess=true`
没有可信 Authenticode 签名就会被 Windows 忽略。但其余辅助进程（设置页、表情面板、手写面板、
键盘面板、看门狗、词库回放）和随包分发的 DLL 不签名会被 Microsoft Defender 按信誉拦下，
用户看到的现象是输入法装完了、某个面板却打不开。所有文件在一次 `signtool sign` 调用里签完：
SimplySign 可能对每次调用弹一次 PIN，逐个文件调用会变成十几次手机确认；签名配额按文件计，
合并调用不会多花配额。

默认不把 PDB 放入安装包；需要与正式 CI 一致的符号包时加 `-IncludeSymbols`。如果 SimplySign
同时暴露了多张有效的 Certum Code Signing 证书，脚本会列出它们并要求用
`-CertificateThumbprint <指纹>` 明确选择。可用 `-Reconfigure` 强制重新配置 CMake，也可用
`-IsccPath <路径>` 指定 Inno Setup 编译器。带 PDB 的安装包会使用 `_with_pdb` 文件名后缀，
例如 `Output\MetasequoiaIME_Setup_v1.2.3_with_pdb.exe`。

也可以直接使用固定包含 PDB 的一键入口，不需要再写 `-IncludeSymbols`：

```powershell
pwsh -File .\package-simplysign-symbols.ps1 1.2.3
```

它是 `package-simplysign.ps1` 的薄包装，其他参数（例如 `-Reconfigure`、
`-CertificateThumbprint` 和 `-IsccPath`）保持一致，输出文件名固定带 `_with_pdb` 后缀。

### Hotfix 轻量包

只修 TSF / Server / HTML 的 hotfix 可以发轻量包，签名流程与完整包相同（包内 EXE/DLL 和最终安装包都签）：

```powershell
pwsh -File .\package-simplysign-light.ps1 1.2.4
```

它等价于 `package-simplysign.ps1 1.2.4 -Light`，产物是 `Output\MetasequoiaIME_Setup_v1.2.4_light.exe`
（加 `-IncludeSymbols` 时是 `_light_with_pdb`）。轻量包和 `test-light.ps1` 用的是同一个
`/DLightPackage=1` 编译开关，因此有同样的限制：

- **只能覆盖安装在已有的完整安装之上。** 包里没有词库、辅助码、拼音表、神经模型和出厂配置模板，
  安装程序也不检查本机是否装过完整包；新用户装它会得到一个没有词库的输入法。对外发布时要写清楚，
  下载页和自动更新清单仍应指向完整包。
- **hotfix 不能新增配置项。** 升级时 Server 以用户数据目录里的 `config.default.toml` 为骨架合并配置，
  轻量包不替换这份模板，新键会被丢弃。需要新配置项、新词库或新模型时发完整包。
- 轻量包不显示联网功能页，也不回放用户词库。

### 三个入口的区别

三个入口都是 `Invoke-LocalTest.ps1` 的薄包装，只是开关不同：

| 脚本 | 词库 | PDB |
| --- | --- | --- |
| `test.ps1` | 含 | 不含 |
| `test-light.ps1` | 不含 | 不含 |
| `test-symbols.ps1`（加 `-Full` 则含词库） | 默认不含 | 含 |

**PDB 默认不进本地安装包。** 它们有 ~140 MB，而真正的 EXE/DLL 只有 ~20 MB，Inno Setup 还要对它们做固实 LZMA2 压缩，是本地打包耗时的大头。只有需要在装好的机器上直接做崩溃分析时才用 `test-symbols.ps1`。正式发布仍然带符号：`release.yml` 显式传 `-IncludeSymbols`。

### 增量编译

`Invoke-LocalTest.ps1` 调用的 `lcompile-release*.ps1` 是增量的：

- `build-release` / `build{32,64}-release` 已有 CMake 缓存时不再重跑 configure。Visual Studio 生成器把 ZERO_CHECK 接进了每个目标，`CMakeLists.txt` 变化时构建会自己重新生成。改了 preset 或换了工具链，用 `-Reconfigure` 强制重跑
- 只构建打包用的目标（`MetasequoiaImeServer` / `MetasequoiaImeTsf`），不构建那些打完包又被删掉的测试可执行文件。要连测试一起构建（比如为了跑 ctest）传 `-Target ALL_BUILD`
- `cmake --build` 带 `--parallel`

没有改动时整轮编译约 15 秒；如果比这慢很多，慢的多半是打包而不是编译。

## 本机测试证书

第一次运行签名脚本时，会在当前用户证书存储里创建：

`CN=Metasequoia IME Local Test Code Signing`

之后复用同一张证书。产物只适合本机或已导入该证书公钥的测试机，不要当作正式分发包。

想让另一台测试机信任这张证书，把公钥导出成 `.cer` 后在那台机器上执行：

```powershell
certutil -addstore Root MetasequoiaImeLocalTest.cer
certutil -addstore TrustedPublisher MetasequoiaImeLocalTest.cer
```

不要把 `.cer` / `.pfx` 提交进本仓库。
