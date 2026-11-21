# AGENTS.md — server

组织级约定见 [组织 AGENTS.md](https://github.com/metasequoiaime/.github/blob/main/AGENTS.md)，仓库地图和组件之间的边界见[仓库根 AGENTS.md](../AGENTS.md)。本文件补充本组件的实现、数据和验证规则，路径都相对于 `server/`。

本组件是常驻后端进程：输入引擎与候选状态、配置、词典、Named Pipe 服务，以及候选窗、悬浮工具栏、托盘菜单（`msimeui` Direct2D presenter）。设置窗口（`MetasequoiaImeSettings.exe`）由本组件的 `server/src/settings/` 单独构成一个进程，不在服务端进程内。

## vcpkg_installed 的解析

`CMakeLists.txt` 引用 `vcpkg_installed` 的地方（头文件目录、`sqlite3.dll` 拷贝）全部走 `${CMAKE_BINARY_DIR}`。

**不要改回 `${CMAKE_SOURCE_DIR}/build/...`。** 那样 binary dir 就只能叫字面的 `build`，本仓自带的 `vcpkg-release` preset（`binaryDir` 是 `build-release`）和 `scripts/lcompile-release.ps1` 都会找不到 vcpkg 装好的头文件与运行时 DLL。

CI 刻意构建到 `build-release` 而不是 `build`，为的就是让这条不会悄悄退回去——两者在目录名恰好是 `build` 时解析结果相同，所以只有构建到别处才测得出来。

## 测试跑在真实词库上，本地要先备好数据

`tests/CMakeLists.txt` 构建 `MetasequoiaImeServerTests`，`tests/src/` 下的测试源文件规模持续增长（引用具体数字前先数一遍）。本目录 `CMakeLists.txt:25` 的 `include(CTest)` 让 `BUILD_TESTING` 默认为 ON，`:556` 据此 `add_subdirectory(tests)`，而 `tests/CMakeLists.txt:169` 用 `add_test` 注册同名用例。

CI 现在会真的跑它：根 `.github/workflows/ci.yml` 的 Server job 在 Build 之后执行 `scripts/ci/test-server.ps1`，那个脚本按 `product-lock.json` 取词库、备好数据根目录，再跑 `ctest --test-dir server/build-release -C Release --timeout 120`。断言失败会让 CI 变红，不再是编译通过就算数。

**关键在于这些测试依赖真实词库，不是 fixture。**引擎从数据根目录读数据，解析顺序是 `METASEQUOIA_IME_DATA_DIR` 环境变量 → 注册表 `HKLM\Software\Metasequoia\MetasequoiaIME` 的 `DataDir`（安装时用户选的位置）→ `%LOCALAPPDATA%\metasequoiaime`。装了正式版的机器上第二项会生效，所以本地跑测试必须用环境变量覆盖，否则会读到安装目录。另有 `METASEQUOIA_IME_CONFIG_DIR` 只改 `config.toml` 的位置、不搬词库，给那些既要真实词库又要隔离配置的测试用。`scripts/ci/test-server.ps1` 往那里放了四样东西，本地跑测试要凑齐同样的：

- `msime.db`、`others.db`、`english.db`——从产品锁指定的 `dict-*` release 下载，CI 逐个核对 `SHA256SUMS.txt`。词库损坏要立刻失败，而不是拖到测试里表现成「候选为空」这种难查的样子
- `helpcodes/`——六套辅助码方案，本仓 `assets/tables` 只有其中一套，得从 `../engine/helpcode/` 取全
- `assets/tables/*` 和 `assets/config/config.toml`

数据不全时的典型症状是候选查询返回空集，断言信息看起来像逻辑错误，实际是缺数据。排查测试失败前先确认数据根目录是齐的。

IPC 线格式、opcode 和语音分帧的唯一实现位于 `../engine/contracts/`——引擎已内嵌进本仓，和 Server 在同一个提交里；本仓的 `src/ipc/ipc.h` 只保留连接状态和 Server API。不要重新复制协议定义。共享头的 ABI 断言随主工程编译，主连接必须通过 `NegotiateMainPipeClient` 后才能激活；旧 DLL 的未版本化 hello 仍兼容，版本化客户端会收到关联 request_id 的协议确认。

改了协议或候选逻辑，正常构建并备好数据之后自己跑一次：

```powershell
# scripts/lcompile-release.ps1 默认只构建 MetasequoiaImeServer 目标（打包用的那一套），
# 不含测试可执行文件。要跑 ctest 就得显式构建全部：
.\scripts\lcompile-release.ps1 -Target ALL_BUILD
ctest --test-dir build-release -C Release --output-on-failure
```

## uiAccess 与 Topmost 时序

`MetasequoiaImeServer.manifest` 用 `requestedExecutionLevel level="asInvoker" uiAccess="true"`，浮层才能盖在高完整性宿主之上。`uiAccess` 真正生效还依赖正确签名和安装在 Windows 认可的位置，清单、签名、安装路径是同一套发布约定。

进程在 `src/main.cpp:90` 设 `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`。

三个小窗口的置顶由 `src/window/small_windows.cpp` 统一管理：建窗时不带 `WS_EX_TOPMOST`，以 DWM cloak 状态显示在真实显示器上；第一次真正显示时 `EnsureSmallWindowsTopmost` 打开置顶门，候选窗、悬浮工具栏、托盘菜单按固定间隔依次提升到 `HWND_TOPMOST`，托盘菜单最后（同一置顶带里后置顶的在上面）。这个分阶段顺序沿用自已移除的 WebView2 时期，D2D 窗口一直按它发布；要改成一次性置顶，先在签名并安装到受信位置的构建上验证 `uiAccess` 下的表现。不要在布局函数里随手传 `HWND_TOPMOST` 绕过这道门；托盘菜单需要回到最上面时用 `RaiseTrayMenuAboveSmallWindows`。

隐藏候选窗用 cloak 而不是 `SW_HIDE`，presenter 在下一次显示时解除 cloak；悬浮工具栏在 presenter 绑定失败时只 cloak 不 hide。

## Boost

`CMakeLists.txt:13` 的 `Boost_ROOT` 是有守卫的：设了 `BOOST_ROOT` 环境变量就用环境变量，否则回退到作者本机路径。**这是有意的，不要改成只认环境变量**，那会破坏作者的本地环境。CI 就是靠设这个环境变量工作的。

Boost 是静态链接但没有写进 `vcpkg.json`，所以只能用 classic 模式装，且 triplet 必须是 `x64-windows-static-md` 而不是 `x64-windows-static`——`Boost_USE_STATIC_LIBS ON` 要静态 Boost 库，而项目其余部分用动态 CRT，纯 static triplet 会让 Boost 也切到静态 CRT，链接时报 LNK2038。

## 生成文件

`scripts/prepare_env.py` 只生成本目录的 `.clangd`（从 `scripts/config_files/.clangd` 模板展开）。测试使用 Server 主工程及同一套 preset，不再生成独立的 `tests/CMakePresets.json`。`CMakePresets.json` 是**原地**改写 `VCPKG_ROOT` 与 `CMAKE_TOOLCHAIN_FILE`，不再从模板覆盖。

`CMakeLists.txt` 和 `tests/CMakeLists.txt` 不再由脚本生成，直接改就行。这两个文件曾经也是从 `scripts/config_files/` 覆盖过来的，但模板早已落后到 147 行对 400 行、以及一个无关的通用 `WinCppTemplate`，跑一次脚本就会毁掉构建，因此模板已删除。

`.clangd` 模板用 `@NAME@` 占位符，不再按固定行号替换——旧的行号索引已经漂移过一次，把 Boost 路径写到了别的 include 上。新增 include 时在模板里加占位符、并在脚本的 `clangd_substitutions` 里补上对应项即可；有占位符没被替换脚本会直接报错。

## 提交

提交信息用 `type(scope): 摘要`。不要添加 `Co-Authored-By`、`Generated with` 或其他 AI 生成标记。

## 窗口与设置窗口的维护边界

- **界面只有 Direct2D 一套**：候选窗、悬浮工具栏和托盘菜单的 HWND、尺寸、位置、显示、DPI、Z-order 在
  `src/window/`（`ime_windows*.cpp` 是宿主窗口过程，`*_presenter.cpp` 用 `msimeui` 绘制）。WebView2
  已整体移除，`appearance.ui_backend` / `appearance.settings_ui_backend` 两个键也随之删除，不要恢复。
- 候选内容和输入状态的权威仍在 Server/引擎，presenter 只负责展示与发出用户动作；不要在界面侧复制
  候选选择、翻页、输入模式或配置持久化的核心状态机。
- 设置窗口是独立进程 `MetasequoiaImeSettings.exe`（`src/settings/`）：原生创建、激活、驻留和配置
  读写映射在 `settings_app.cpp`，界面在 `src/settings/native/`，图片资源在 `assets/settings/`（安装到
  数据目录的 `assets\settings\`）。界面与宿主之间走 `../engine/contracts/webview/` 的消息契约：宿主推送
  `configSnapshot` 与各类响应，界面发 `configUpdate` 与请求，统一进 `HandleClientMessage` 校验和分发，
  配置读写映射只在 `settings_app.cpp`。每个 `native_page_*.cpp` 顶部注明了它当初移植自旧网页设置页的
  哪个 partial / module，那些文件只在 git 历史里（`ui-html/webview2/settings/ime-settings/src/`）。
  本地调试时可设 `METASEQUOIA_IME_SETTINGS_INSTANCE=<任意名>` 让开发版与已安装的设置窗口并存，
  并用 `METASEQUOIA_IME_CONFIG_DIR` 隔离配置。旧快捷方式传来的 `--d2d` / `--webview2` 参数会被忽略。
- **页面 builder 只搭控件**：窗口打开后索引器会把还没访问过的页面逐个建出来做搜索索引
  （`native_settings_window.cpp` 的 `BuildPageAndIndex` / `IndexNextPage`），所以 builder 里不解码图片、
  不发请求——那类动作放到页面真正显示时（页面的 `Attach`）。词库面板就是这样把首次查询挂在
  `DictTableScroll::Attach` 上的。
- 设置页的控件与页面用 `tests/includes/settings_window_harness.h` 测，它持有一个从不显示的真实窗口：
  文本框的 TSF 需要 HWND，把空指针传给 `Attach` 会在 `TextBox::Attach` 里崩。这些用例进
  `MetasequoiaImeSettingsUiTests`，`tests/CMakeLists.txt` 会把设置程序的源文件全量链进去。

### 尺寸、坐标与 DPI

- **所有涉及尺寸或坐标的代码都必须明确考虑 DPI**。新增或修改窗口大小、位置、边距、间距、字体、
  图标、命中区域、候选布局及屏幕边界计算时，不得默认固定像素在所有显示器上等价。
- 明确每个数值使用逻辑单位还是物理像素，并在正确的窗口/显示器 DPI 下统一换算；不要混用来自不同
  DPI 空间的坐标和尺寸，也不要长期缓存可能在窗口跨屏或 DPI 变化后失效的换算结果。
- 原生窗口需处理运行时 DPI 变化（包括跨显示器移动），同步更新窗口、子窗口和相关布局；优先沿用项目已有的 DPI 获取与缩放辅助函数，避免各处自行实现不一致的换算规则。
- 涉及尺寸或布局的改动，至少检查 100%、125%、150%、200% 缩放，以及不同 DPI 显示器之间移动时
  是否出现裁切、错位、模糊、点击区域偏移或窗口越出工作区。
