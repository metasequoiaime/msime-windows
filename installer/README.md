# Metasequoia IME Installer

本目录的脚本从 Windows 合仓目录收集产物、签名、用 Inno Setup 打成安装包。它服务四条流程：

- **正式发布**：由 `MSIME-Windows` 的 release workflow 驱动，用真实证书签名包内全部 EXE/DLL 和最终安装包；产物是挂在 Release 上的 `MetasequoiaIME_Setup_v<版本>.exe`，并携带与 Release 二进制匹配的 PDB 符号文件。见下面「CI 契约」
- **Nightly**：由 `nightly.yml` workflow 驱动，每周一、三、六从 `develop` 的 HEAD 自动构建，用一张**固定**的自签名证书签名，发布到预发布 `nightly` tag。见下面「Nightly build」
- **开发构建**：由 `dev-build.yml` workflow 驱动，runner 上现场生成自签名证书并签名，产物随 artifact 分发。见下面「Dev build」
- **本地测试**：手工跑，用本机自签名证书，用来在自己机器上验证安装流程。本文其余部分讲的是这条

## 三个通道怎么选

产品对外有三条下载通道，签名方式、可信度和更新方式各不相同。

| | 正式发布 | Nightly | Dev build |
|---|---|---|---|
| 谁用 | 所有用户 | 想尝鲜的测试者 | 改代码的贡献者 |
| 从哪拿 | [Releases 的 Latest](https://github.com/metasequoiaime/msime-windows/releases/latest) | [Releases 的 `nightly` tag](https://github.com/metasequoiaime/msime-windows/releases/tag/nightly) | Actions 运行的 Artifacts |
| 什么时候出 | 合进 `main` 时 | 每周一、三、六，或手动触发 | 每次向特性分支 push、每个开向 develop 的 PR |
| 构建来源 | `main`，经发布流程筛选 | `develop` 的 HEAD，无人筛选 | 你的分支 |
| 签名证书 | 真实代码签名证书或 SignPath | 固定自签名证书，私钥在仓库 secret | 每次运行现场生成 |
| 用户要导入几次信任 | 0 | **1 次，之后不用再导** | **每换一次构建导一次** |
| 版本号 | `0.7.1` | `0.7.1-nightly.<日期>.<commit>` | `0.7.1-dev.<日期>.<commit>` |
| 更新检查 | 提示升级 | 不提示 | 不提示 |
| 稳定性承诺 | 有，出问题会修 | **无**，可能损坏配置或数据 | **无**，仅供验证 |

三条通道的证书 subject 互不相同，所以互不覆盖：

| 通道 | Subject | 有效期 |
|---|---|---|
| Nightly | `CN=Metasequoia IME Windows Nightly Build Code Signing` | 3 年，到期换发 |
| Dev build | `CN=Metasequoia IME Dev Build Code Signing` | 90 天，每次构建换新 |

**Nightly 永远进不了 Latest。** GitHub 的 Latest 只指向非预发布 Release，nightly 每次都带 `--prerelease`，所以首页那个 Latest 链接始终是正式版。

版本号不是固定的，由 `Prepare-PackageFiles.ps1` 的 `-TargetVersion` 决定，默认 `0.0.1`；正式发布时 CI 传入真实版本。

本仓库 **不包含** 正式代码签名证书、私钥、指纹，也不包含编译好的 EXE/DLL、词库或安装包。唯一的公钥是 `nightly-signing.cer`，它随仓库分发，用于给用户导入 nightly 通道的信任。

签名脚本会在你自己的机器上生成并复用一张自签名测试证书。

## Dev build

`dev-build.yml` workflow 给没有本地工具链的贡献者出可安装的包。触发见 [CONTRIBUTING.md](../CONTRIBUTING.md#没有本地工具链时dev-build-workflow)。签名证书由 `sign-dev-build.ps1` 在 runner 上现场生成，subject 固定为 `CN=Metasequoia IME Dev Build Code Signing`，私钥只在 runner 内存里存在一次构建；公钥 `.cer` 随 artifact 分发，用户用 `import-dev-trust.ps1` 导入本机受信任存储（按 subject 清旧导新，重复构建不会膨胀存储）。

- `sign-dev-build.ps1` 复用同一次运行早先创建的证书，所以包内二进制和安装包总在同一签名下；复用的依据是本次运行写在 `RUNNER_TEMP` 里的指纹标记，不按 subject 猜，机器上残留的旧证书不会被误用。`DEV_CERT_PATH` 指出 `.cer` 的导出位置
- 证书有效期 90 天：artifact 只留 7 天，但用户可能下载到本地后放很久才装，而安装器校验的是签名时刻证书是否有效；`import-dev-trust.ps1` 会打印这张 `.cer` 自带的到期时间，发现已过期就直接退出，不往受信任存储里写
- `import-dev-trust.ps1` 也随 artifact 分发，用户解压后在管理员 PowerShell 里直接跑
- 版本号带 `-dev.<日期>.<commit>`，由 `scripts/dev-build-version.py` 从 `version.txt` 派生；设置页的更新检查会剥掉 `-dev` 后缀做数字比较，dev 包不会提示升级
- 不要把这两个脚本接进 release 路径：正式发布的签名在 `scripts/ci/sign-binaries.ps1` 或 SignPath，两套互不相干

## Nightly build

`nightly.yml` 每周一、周三、周六 UTC 20:00（北京时间次日 04:00）从 `develop` 的 HEAD 出一份安装包，发布到固定的预发布 `nightly` tag。也可以手动触发：`workflow_dispatch` 带一个 `publish` 开关，取消勾选就只编译和上传 artifact、不发 Release，适合先验证一趟改动。它和 dev build 的差别只在签名与发布方式：

- **证书固定**：不是现场生成，而是仓库 secrets 里的 `NIGHTLY_CERTIFICATE_BASE64` / `NIGHTLY_CERTIFICATE_PASSWORD`，subject 为 `CN=Metasequoia IME Windows Nightly Build Code Signing`。公钥 `nightly-signing.cer` 提交在仓库里，同时随 Release 和 artifact 分发。用户跑一次 `import-nightly-trust.ps1` 即可长期验证后续所有 nightly 构建
- **签名走 `scripts/ci/sign-binaries.ps1`**，与正式发布同一套，只是证书不同；缺少 secret 时直接失败，不会降级成未签名包
- **发布前逐个核对签名**：用仓库里的 `.cer` 作为信任锚验证包内每个 EXE/DLL 和安装包，签错证书会被拦下
- **tag 固定为字面量 `nightly` 且标为 prerelease**，所以永远进不了 GitHub 的 Latest，设置页的更新检查也不会指向它；每次运行用 `--clobber` 覆盖同名附件，Release 上始终只有一份
- **版本号带 `-nightly.<日期>.<commit>`**，与 `-dev.` 区分
- **正文列出相较上一次构建的提交**：用 `git log --no-merges <上次 sha>..HEAD` 生成，最多展示 25 条，超出部分给完整对比链接。上次的 sha 记在正文末尾 `<!-- nightly-sha: ... -->` 这个隐藏注释里，而不是从 tag 读——tag 永远指向第一次构建的 commit，用它比较会一直拿今天的 HEAD 对第一次 nightly
- 失败时 `notify` job 会开一个 `bug` + `nightly-failure` 标签的 issue；已有未关闭的就追加评论，不重复开。成功不自动关 issue，由人确认后手动关

### 用户怎么装 nightly

打开 [Releases 的 nightly 页面](https://github.com/metasequoiaime/msime-windows/releases/tag/nightly)，下载三个附件：

- `MetasequoiaIME_Setup_v<版本>-nightly.<日期>.<commit>.exe`
- `nightly-signing.cer`
- `import-nightly-trust.ps1`

解压到同一个目录，然后在**管理员** PowerShell 里跑一次：

```powershell
cd <解压目录>
pwsh -File .\import-nightly-trust.ps1
```

看到 `有效期至：<日期>` 就说明导入成功，之后双击安装包。**这个脚本只需要跑一次**，后续新的 nightly 构建直接装就行，不会因为换了构建而失效——这正是 nightly 相对 dev build 的价值所在。脚本重复运行时按指纹跳过，不会重复导入。

**不导入就直接装会怎样：** 程序能启动、能打字，但在以管理员身份运行的窗口（管理员 PowerShell、以管理员打开的记事本或终端）里候选窗不会出现，看起来像输入法坏了。原因是 Server 带 `uiAccess="true"` 清单，好让候选窗浮在被 UIPI 保护的高权限窗口之上；Windows 只对「有可信签名、且装在受信任位置」的程序承认这个权限，没签名的 uiAccess 程序会被静默降权。

`nightly-signing.cer` 也提交在仓库里（`installer/nightly-signing.cer`），脚本默认从同目录找它；想直接用仓库那份可以加 `-CertificatePath`。

### 维护者换发证书

`nightly-signing.cer` 有效期 3 年，到期前：

1. 跑 `pwsh -File ./scripts/ci/new-nightly-certificate.ps1`，按输出更新仓库的 `NIGHTLY_CERTIFICATE_BASE64` 与 `NIGHTLY_CERTIFICATE_PASSWORD` 两个 secret
2. 用新生成的 `nightly-signing.cer` 覆盖 `installer/nightly-signing.cer` 并提交
3. 删掉本机含私钥的 `.pfx` 和 `secret-base64.txt`
4. 手动触发一次 nightly 验证

旧证书残留在用户机器上不会造成问题：`import-nightly-trust.ps1` 导入新证书时会警告同 subject 下还留着旧证书。

## CI 契约

根 `.github/workflows/release.yml` **不加修改地调用本目录的 `Prepare-PackageFiles.ps1` 和 `Compile-Installer.ps1`**。源目录名是 `Prepare-PackageFiles.ps1` 的参数，不是写死在脚本里的字面量：默认值为 `windows/`、`server/` 和仓根授权文件；旧目录或自定义布局通过参数覆盖。回归检查同时执行默认路径和显式参数的打包。辅助码默认读取 `engine/helpcode/`，词库缓存位于 `MetasequoiaImeDict/out/`。

合仓后 release workflow 传的是本仓的目录名，辅助码随固定 Engine gitlink 检出，词库按产品锁下载到仓根缓存：

```powershell
pwsh -File ./Prepare-PackageFiles.ps1 -TargetVersion 1.2.3 -RepoRoot .. `
    -TsfDirectory windows -ServerDirectory server `
    -NoticesDirectory .
```

由此产生几条约束：

- **改动 `Prepare-PackageFiles.ps1` 里那些 `Assert-PathExists` 的源路径，会直接弄坏发布流水线。** 它断言的源路径由 workflow 逐一准备，改了要同步改 `.github/workflows/release.yml`。目录名本身可以由调用方指定，但每个目录内部的相对路径（如 `build-release\bin\Release`）仍是硬契约
- **`THIRD_PARTY_NOTICES.txt` 从 `-NoticesDirectory` 指向的目录取，随安装包装到程序目录。** 合仓后这份声明覆盖整个产品、放在仓根，所以它和 `-TsfDirectory` 分成了两个参数。词库主体含 rime-ice（GPL-3.0）内容，其许可要求保留署名，所以这份文件缺失会让打包直接失败，而不是静默跳过
- **`Sign-PackageBinaries-Local.ps1` 和 `Sign-Installer-Local.ps1` 只用于本地验证，CI 不会调用它们。** 它们创建本机自签名证书并尝试写入受信任存储，正式发布走 workflow 里用仓库 secret 中真实证书的签名步骤
- 词库不再从相邻的 `MetasequoiaImeDict` 工作目录取，CI 从产品锁指定的 `dict-*` release 下载并校验 SHA256，再放到脚本期望的位置
- 词格语言模型 `sc.lm` 不随安装包发布（约 24 MB，几乎压不动），用户在设置页「整句候选」按需下载到 `<数据目录>\models\language-model\`（来源是 `metasequoiaime/chinese-ime-lm` 的 `language-model-libime-20260629` release，字节数与 SHA256 钉在 `server/src/settings/lattice_model.cpp`，与 `language-model/lock.json` 一致），未下载时词格整句退回启发式打分。`Prepare-PackageFiles.ps1` 不再读取它，覆盖安装和卸载会删掉旧版装进数据目录根下的 `sc.lm` 与 `libime-lm-NOTICE.md`；`models\` 整目录升级保留
- `windows-2025` runner 自带 Inno Setup 6.7.1，`Compile-Installer.ps1` 能自己找到 `ISCC.exe`。但它不带 `ChineseSimplified.isl`，`msime_setup.iss` 的 `[Languages]` 段依赖那个文件，所以 workflow 会在编译前按固定 revision 和校验和把它装进去

## 数据目录

程序本体固定装在 `Program Files\metasequoiaime`（Server 带 `uiAccess=true`，只有装在受信任目录里这个标志才生效），**用户数据目录可以在安装向导里改**：词库、`config.toml`、用户词库、皮肤和设置窗口的图片资源都在那里，整包几百 MB，装 C 盘吃紧的用户可以放到别的盘。

- 默认值 `%LOCALAPPDATA%\metasequoiaime`；升级安装时默认沿用上次的位置
- 选择写进 `HKLM\Software\Metasequoia\MetasequoiaIME` 的 `DataDir`。这是运行期唯一的权威来源：Server（`server/src/utils/ime_paths.cpp`）、TSF DLL（`windows/src/Utils/FanyUtils.cpp`）和引擎（`engine/core/data_path.h`）各自按 `METASEQUOIA_IME_DATA_DIR` 环境变量 → 该注册表值 → `%LOCALAPPDATA%\metasequoiaime` 的顺序解析，三处必须保持一致。32 位 TSF DLL 用 `KEY_WOW64_64KEY` 读，所以这个值必须写在 64 位视图里
- 升级时改了位置，安装器会把用户词库（`msime_user.db` 及其 WAL/SHM）、输入统计库（`stats.db` 及其 WAL/SHM）、`config.toml`、`config.base.toml`、`custom_translations.txt`，以及 `skins\`、`helpcodes\custom\`、`shuangpin\`、`models\` 搬到新目录，再删掉旧目录。`config.toml` 或 `custom_translations.txt` 在新旧目录里各有一份且内容不同时，安装器拒绝这个目录，不覆盖任何一份；`custom_translations.txt` 没搬成会中止安装；词库和设置窗口的图片资源由本次安装重新写入，不搬
- **数据目录必须整个归输入法**（[#537](https://github.com/metasequoiaime/MSIME-Windows/issues/537)）。用户选的目录里已经有别的文件、或者选了驱动器根目录时，安装器改用其中的 `metasequoiaime` 子目录，并在向导里把实际位置告诉用户；连那个子目录也不空就拒绝。输入法用过的目录（有标记，或历史默认位置）原样沿用
- 安装器在数据目录里放一个 `.metasequoiaime-data` 标记文件。只有标记里带 `exclusive: adopted empty by the installer` 这一行（安装时目录是空的），或目录就是历史默认位置，覆盖安装才整目录清理、卸载才整目录删除
- 旧版安装器不管目录里原来有什么都写标记，所以只有旧标记（没有那一行）的目录不能再信：覆盖安装、换目录和卸载都只按 `IsShippedAppDataItem` / `IsRuntimeAppDataItem` / `IsPreservedAppDataItem` 名单删输入法自己的条目，`assets`、`logs`、`skins` 这种通用目录名（以及已退役的 `html`）只删输入法写进去的部分，最后目录空了才删目录。包里新增顶层文件要同步 `IsShippedAppDataItem`，`tests/package-files.ps1` 会拿真实打出的 `app_data` 核对；Server 在数据目录根下新写文件要同步 `IsRuntimeAppDataItem`
- 仓库 `skins\` 下的外部皮肤（除 `default\` 和素材授权为 `UNVERIFIED*` 的）随包装到 `skins\<id>`，完整包和轻量包都带，卸载不删。`Prepare-PackageFiles.ps1` 同时写出 `bundled_skins.manifest`（每行 `id|相对路径|sha256`），安装器在 `PrepareToInstall` 里拿它核对用户机器上的同名目录：文件集合和内容完全一致就直接覆盖，否则先改名成 `<id>.bak`（已有就 `<id>.2.bak`……，不覆盖旧备份）再装。Server 扫描皮肤时跳过 `*.bak` 目录。`tests/bundled-skins.ps1` 把这段 Pascal 编进探针安装器真跑一遍
- 静默安装用 `/DATADIR="D:\MetasequoiaIME"` 指定；该值在 `PrepareToInstall` 里和向导页走同一套换子目录和校验

### 云候选安装矩阵

在一次性 Windows 测试机上运行完整安装包。每个场景使用独立的数据目录。
安装后读取注册表 `DataDir`，再检查该目录的 `config.toml`。

| 场景 | 安装前配置 | 向导选择 | 安装后检查 |
|---|---|---|---|
| 首次安装 | 默认目录无配置 | 取消云候选 | 最终目录为 `false` |
| 首次安装并改目录 | 默认目录无配置，新目录为空 | 改目录并取消云候选 | 新目录为 `false` |
| 目标目录已有配置 | 新目录有安装标记，`cloud_candidates = true` | 改到该目录 | 网络页跳过，配置保持 `true` |
| 原目录已有配置 | 原目录的 `cloud_candidates = true`，新目录为空 | 升级时改目录 | 网络页跳过，新目录保持 `true` |
| 原目录已有配置 | 原目录的 `cloud_candidates = false` | 原目录升级 | 网络页跳过，配置保持 `false` |
| 两处都有配置 | 原目录和新目录都有内容不同的 `config.toml` | 升级时改目录 | 数据目录页直接拦下；静默安装在停进程前停止，两份配置保持原样 |
| 上次迁移只复制未删除 | 两处 `config.toml` 内容相同 | 重试升级 | 视为同一份，删掉原目录副本后继续 |
| 配置搬不动 | 原目录 `config.toml` 被占用 | 升级时改目录 | 安装停止，原配置保持原样，不会装上出厂配置 |

写回「不启用云候选」失败时不中止安装（此时文件已落盘，中止只会留下半成品），而是在其他安装后步骤完成后提示用户到设置页手动关闭。

运行 `python installer/tests/test_cloud_choice.py` 检查生产安装器的时序：先查冲突再停进程，数据目录页也查冲突，写回选择排在必需的安装后步骤之后。
Windows CI 运行 `installer/tests/cloud-choice.ps1`，用 Inno Setup 跑八个隔离场景。探针复用 `cloud_choice.iss` 里的判断、冲突、去重和失败函数，并按生产顺序用同样的 `robocopy /MOVE` 搬 `config.toml`；`MigrateUserDataDir` 里搬词库、皮肤和清理旧目录的部分不在探针里，完整安装包仍按上表在测试机检查。

## 运行时依赖检查

安装包不携带 VC 运行库，它是系统级共享组件。缺了它 Server 起不来，而这个故障出现在安装结束之后，用户看不到任何解释。所以 `msime_setup.iss` 的 `InitializeSetup` 在安装开始前先查一遍，缺了就当场说清楚，并给出下载地址。界面全部由 `msimeui` 以 Direct2D 绘制，不再依赖 WebView2 Runtime；旧版本留下的 WebView2 用户数据（`ProgramData\metasequoiaime\webview2`、`webview2-settings`，以及 `%LOCALAPPDATA%\metasequoiaime` 下的同名回退目录）由安装器在升级和卸载时清掉。

- **Visual C++ 2015–2022 可再发行组件包（x64）**：读 `Software\Microsoft\VisualStudio\14.0\VC\Runtimes\x64` 的 `Installed` / `Major` / `Minor`，`HKLM32` 和 `HKLM64` 两个视图都查。要求 **14.40 以上**（常量 `VCRuntimeMinMinor`）：`server_exe\` 里的 exe 用 MSVC 14.40+（VS 2022 17.10 起）编译并导入 `msvcp140.dll`，这一版起 `std::mutex` 的构造函数是 constexpr，配上更旧的 `msvcp140.dll` 会在第一次加锁时访问冲突、无声闪退——设置页「点了没反应」就是这个现象。装了 2019 / 早期 2022 运行库的机器同样有 `Installed=1`，所以必须比版本号。构建工具链升级到要求更高 redist 时同步调高这个常量。32 位 TSF DLL 用静态 CRT（`windows/CMakeLists.txt` 的 `CMAKE_MSVC_RUNTIME_LIBRARY`），所以不检查 x86 redist
- **不要改成查 `System32` 里的 DLL，也不要在 Pascal Script 里用 `ApplyPathRedirRulesForCurrentProcess`。** 那是 Inno Setup 7 的 support function，CI 的 6.7.1 没有它，ISCC 会报 `Unknown identifier`（#925 引入后第一次在 hosted runner 上编安装包就炸了，#55e1127a 改回直接展开 `{sys}`）。Setup 在 x64/ARM64 上是 64 位进程，`{sys}` 本来就不经重定向；需要 32 位视图时用 `{syswow64}`
- 缺组件或版本过旧时弹一次对话框，区分「没装」和「已装 x.y 但过旧」，列出直接下载链接（`https://aka.ms/vc14/vc_redist.x64.exe`）和说明页：选「是」直接下载安装包并结束安装，选「否」继续装。**不阻断安装**——用户可能马上就要去补组件，也可能只是在准备环境。静默安装（CI、批量部署）用 `SuppressibleMsgBox` 默认「否」直接继续，缺什么已经写进安装日志（`/LOG=`）

## 依赖

- Windows 10/11，PowerShell 7（`pwsh`）
- [Inno Setup](https://jrsoftware.org/isinfo.php) 6.6 或更高版本
- Windows SDK（提供 `signtool.exe`）
- 先初始化本仓 submodule，并完成 `windows/`、`server/` 的 Release 编译。Release 构建必须生成同目录 PDB；打包脚本会拒绝缺少匹配符号的产物（符号是否随包安装是另一回事，由 `-IncludeSymbols` 决定）。
- 本地测试包的词库从与本仓同级的 `msime-dictionary` 检出构建（见下文 `build-dictionary.py`），`Invoke-LocalTest.ps1` 打完整包前会自动调用，不再下载产品锁的 Release；需要复现发布用的词库时再手动运行 `python scripts/product_lock.py fetch-dictionaries --staging-root .`。
- 词格整句打分用的 `sc.lm` 不随包发布，`Invoke-LocalTest.ps1` 不再生成它。本地跑引擎测试或评测需要它时，仍可用 `scripts\build-language-model.ps1` 从 `language-model\lock.json` 钉住的上游语料转换出来。
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

只修 TSF / Server / 设置窗口资源的 hotfix 可以发轻量包，签名流程与完整包相同（包内 EXE/DLL 和最终安装包都签）：

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
