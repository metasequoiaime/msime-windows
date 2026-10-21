# 参与 MSIME-Windows 开发

组织级的通用约定（行为准则、PR 要求、安全上报）见[组织贡献指南](https://github.com/metasequoiaime/.github/blob/main/CONTRIBUTING.md)。这份文档只讲本仓特有的东西：环境、怎么把它编出来、改哪一层、以及提交前要跑什么。

想找活干的话先看[招募开源开发者](https://github.com/metasequoiaime/.github/blob/main/RECRUITING.md)和 [good first issue](https://github.com/metasequoiaime/MSIME-Windows/issues?q=is%3Aissue+is%3Aopen+label%3A%22good+first+issue%22)。不写代码也有事情可做：词库条目、文档、图标资源、兼容性测试都在 [no-code](https://github.com/metasequoiaime/MSIME-Windows/issues?q=is%3Aissue+is%3Aopen+label%3Ano-code) 标签下。

## 环境

- Windows 10 / 11
- Visual Studio 2026（需要 C++ 桌面开发工作负载）
- CMake 3.25+
- vcpkg，并把 `VCPKG_ROOT` 设进环境变量
- Python 3.10+
- Inno Setup 6.6+（只有要打安装包时才需要）

Boost 按 `-DBoost_ROOT=...` → 环境变量 `BOOST_ROOT` → `%USERPROFILE%\scoop\apps\boost\current` 的顺序解析。preset 里的 vcpkg 路径取自 `$env{VCPKG_ROOT}`，不需要改文件。

### 没有本地工具链时：dev-build workflow

只想验证一个小改动、不想为它装一整套工具链的话，让 CI 编。`Dev build` workflow 平时不跑主分支（develop 和 main 有自己的 CI 和发布流），三种触发方式：

```powershell
# 特性分支一推就自动编，不需要任何手动操作
git push -u origin fix/xxx

# 或者手动触发，可选单个组件快速迭代
gh workflow run dev-build.yml --ref <你的分支> -f component=all

# 等 run 结束拿产物；agent 也可以在这里等
gh run list --workflow=dev-build.yml --branch <你的分支> --limit 1 --json databaseId,status
gh run watch <runId> --exit-status
gh run download <runId> -n dev-installer
```

开向 develop 的 PR 也会自动跑一次，评审者和 fork 贡献者都能从 PR 的检查页拿到安装包。解压后双击 EXE，确认 Windows 管理员授权后按向导安装即可，不需要手动运行 PowerShell 或寻找证书。开发包内嵌本次构建的测试证书，安装前校验摘要并导入系统信任存储，导入失败时会在替换旧文件之前停止；准备安装页会说明这项操作。旧开发证书会保留，避免影响仍在使用的旧版本。版本号带 `-dev.<日期>.<commit>`，一眼可辨，设置页的更新检查不会提示它升级；产物保留 7 天。

只想看设置窗口的界面、不想为一个页面改动装整套输入法时，同一次构建还会出一个 `dev-settings-portable` 压缩包：

```powershell
gh run download <runId> -n dev-settings-portable
```

解压到普通目录（不要放 `Program Files`），双击 `run-settings.cmd` 即可。它不写注册表、不装 TSF、不需要管理员权限，也不需要导入证书；数据目录指向包内的 `data\`，实例名加了后缀，可以和已安装的正式版同时开着。改动写进包内的 `data\config.toml`，删掉整个目录就是卸载。代价是看不到真实输入效果（候选窗、上屏、皮肤在实际输入时的样子），那些仍然要靠安装包。

几个边界：`workflow_dispatch` 需要仓库写权限；fork PR 的构建用 `pull_request` 触发（只读 token，无 secrets），首个 run 要维护者点一次批准；`component=tip` 或 `server` 只编译单个组件出裸二进制；这个 workflow 不出 release、不碰更新清单，合并门槛仍然由 PR 上的 CI 把守。

## 取代码

```powershell
git clone --recursive https://github.com/metasequoiaime/MSIME-Windows.git
cd MSIME-Windows
```

忘了 `--recursive` 的话补一句 `git submodule update --init --recursive`。`vendor/` 下是 opencc 和 cpp-pinyin，缺了它们 Server 编不过。输入引擎在 `engine/`，是本仓的目录而不是 submodule，普通 clone 就有。

词库不在版本库里，构建时从 `dict-*` release 按锁定的 tag 和摘要取用：

```powershell
python scripts/product_lock.py fetch-dictionaries --staging-root .
```

## 构建

没有顶层聚合的 `CMakeLists.txt`，这是有意的：TSF DLL 用静态 CRT、Server 用动态 CRT，两者的 vcpkg manifest 与编译选项互不兼容，合成一个 build tree 会互相污染。每个组件是独立的 CMake 工程，从仓库根目录指定 `-S`：

```powershell
cmake -S windows -B windows/build64 -A x64        # TSF 文本服务 DLL
cmake -S server  -B server/build    -A x64        # Server
cmake -S ui      -B ui/build        -A x64        # 自研 GUI 框架 msimeui
```

`server/` 通过 `add_subdirectory(../ui ...)` 把 `msimeui` 拉进自己的构建，这是唯一的跨组件构建引用。

每个组件也各自带了本地脚本，比手敲 cmake 省事：

```powershell
.\windows\scripts\prepare_env.py    # 生成 .clangd 并把 preset 指向本机 vcpkg/Boost
.\windows\scripts\lcompile.ps1 64   # Debug；换 32 编 32 位
.\server\scripts\lcompile.ps1
```

各组件更细的说明在各自的 README 与 AGENTS.md 里，那两份文件写给人和 AI 都一样有效：

- [`windows/AGENTS.md`](windows/AGENTS.md) — TSF 层的构建、注册与**手工回归清单**（这份清单是本仓最难替代的知识，改 TSF 前先读）
- [`server/README.md`](server/README.md) — Server 的开发构建与本地词库准备
- [`ui/README.md`](ui/README.md) — GUI 框架的构建与边界约束
- [`installer/README.md`](installer/README.md) — 打包
- [`AGENTS.md`](AGENTS.md) — 跨组件契约：协议只有一份来源，改哪边要同时改哪边

## 打一个可安装的包

从仓库根开始，严格按顺序。任一步失败先修，不要拿旧产物继续打包：

```powershell
Set-Location .\windows
.\scripts\lcompile-release-both.ps1

Set-Location ..\server
.\scripts\lcompile-release.ps1

Set-Location ..\installer
.\Prepare-PackageFiles.ps1 -RepoRoot .. -TsfDirectory windows -ServerDirectory server -NoticesDirectory .
.\Sign-PackageBinaries-Local.ps1
.\Compile-Installer.ps1
```

`Sign-PackageBinaries-Local.ps1` 用的是本机测试证书，只用于内部安装验证；不要把本机证书指纹或私钥提交进仓库。

## 改动前要知道的几条边界

- **协议的唯一来源是 `engine/contracts/`。** IPC 线格式、opcode、语音分帧和设置窗口的消息定义都在那里，两侧引用同一份头文件，不要在任何一侧另写一份。
- **`engine/` 是本仓的一等代码。** 要为 Windows 改引擎就直接改，和改 `server/` 一样评审和测试；引擎和调用它的代码可以、也应该在同一个 PR 里。例外是 `engine/` 下的 `googlepinyinime-rev/`、`ngram/kenlm/`、`utfcpp/`、`voice/third_party/`，那是上游副本。见 [engine/UPSTREAM.md](engine/UPSTREAM.md)。
- **`ui/` 不许反向依赖产品。** 它不读 Server 配置、IPC、引擎、词库或全局输入状态。`ui/scripts/check-boundary.py` 在 CI 里检查。
- **界面只有 Direct2D 一套，全部在 `server/`。** 候选窗、工具栏、托盘菜单在 `server/src/window/`，设置窗口在 `server/src/settings/native/`，都用 `ui/` 的 `msimeui` 绘制。改设置窗口的消息 `type` 或 JSON 字段时，`engine/contracts/webview/` 与 `settings_app.cpp` 一起改。
- **候选与输入状态的权威在 Server 和引擎**，界面只负责展示。

## 提交前

```powershell
ctest --test-dir server/build --output-on-failure       # Server 单元测试（引擎随 Server 一起构建）
python -m unittest discover -s tests                    # 产品级组合验证
bash scripts/format.sh --check                          # 格式，覆盖 server/windows/ui/log/engine
bash scripts/ci/check-developer-paths.sh                # 构建输入里不许出现个人目录路径
```

改了 TSF 层就照 [`windows/AGENTS.md`](windows/AGENTS.md#构建与验证) 的手工回归清单跑一遍。输入法很难靠读代码判断对错，**PR 里必须写清你怎么验证的**：系统版本、宿主程序、操作步骤、观察到的结果、跑过的测试命令。

`git status --short` 确认只暂存本次任务涉及的路径，不要 `git add -A`。提交信息用 `type(scope): 摘要`，不要加 `Co-Authored-By` 或任何 AI 生成标记。

分支从 `develop` 切，PR 也合回 `develop`——它是本仓默认分支，开 PR 时不用改 base。`main` 是发布分支，只在发版时由维护者从 `develop` 合入；特性分支直接提到 `main` 会被 `Branch guard` 检查拦下，把 base 改回 `develop` 即可。发布链条见 [docs/product-release.md](docs/product-release.md)。

不要在 PR、Issue、截图或日志里泄露 API Key。仓库开了 secret scanning 和 push protection，但那是最后一道防线不是第一道。
