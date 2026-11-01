# AGENTS.md — windows

组织级约定见 [组织 AGENTS.md](https://github.com/metasequoiaime/.github/blob/main/AGENTS.md)，仓库地图、组件边界和发布流程见[仓库根 AGENTS.md](../AGENTS.md)。本文件只规定 TSF 前端本身：COM / 焦点 / 线程、注册和本目录的构建。路径都相对于 `windows/`。

## 本目录地图

| 位置 | 职责 |
|---|---|
| `src/IME/` | TSF 文本服务主体、激活/停用、消息窗口、后台 IPC 与主题监听生命周期 |
| `src/Key/` | `ITfKeyEventSink` 按键预判、异步按键队列与 edit session 分发 |
| `src/Composition/` | 预编辑缓冲、光标、按键表、preserved key、输入模式与标点/全半角状态 |
| `src/Edit/` | TSF edit session、文本范围和 composition 写入 |
| `src/UI/`、`src/Candidate/` | TSF 候选 UI element、候选状态、候选提交与布局通知；可见候选窗主要由 Server 绘制 |
| `src/IPC/` | Main / ToTsf / Worker / Aux Named Pipe 协议、握手、请求配对和重连状态 |
| `src/Thread/`、`src/Tf/` | 焦点、context stack、文本布局等 TSF sink |
| `src/Compartment/`、`src/LanguageBar/` | 中英文、标点、全半角 compartment 与语言栏按钮 |
| `src/Register/`、`src/DisplayAttribute/` | COM/TSF 注册及预编辑显示属性 |
| `src/Global/`、`src/Header/`、`src/Utils/` | 全局状态、资源/常量、COM 与 Win32 工具 |
| `src/DllMain.cpp`、`src/Server.cpp` | DLL 入口、class factory、引用计数和注册导出 |
| `image/`、`src/IME/MetasequoiaIME.rc` | DLL 内嵌图标与资源 |
| `scripts/` | 环境生成、Debug/Release 双架构构建、注册和签名脚本 |

## 核心输入链路

```text
宿主按键
  → OnTestKeyDown / _IsKeyEaten（同步决定是否吃键）
  → OnKeyDown / _DispatchKeyDown
  → Main Pipe 发给 Server
  → request_id 对应的回复或异步 Worker Pipe 消息
  → 消息窗口回到 TSF 所属线程
  → ITfEditSession 修改 composition / 提交文本
  → TSF UI element 与 Server 候选窗同步
```

查问题时先沿这条链路定位。按键判定主要看 `src/Key/KeyEventSink.cpp`（延迟按键队列与失败处理在
`KeyEventSink_DeferredKeys.cpp`，失败分类在 `DeferredKeyFailurePolicy.h`，输入模式热键在
`KeyEventSink_Hotkeys.cpp`），编辑行为看
`src/Key/KeyHandler.cpp` 与 `src/Key/KeyStateCategory.cpp`，协议与连接状态看 `src/IPC/Ipc.*`，
激活、焦点和工作线程生命周期看 `src/IME/MetasequoiaIME*`（Worker Pipe 读线程在
`MetasequoiaIME_IpcWorker.cpp`，消息窗口与主题监听在 `MetasequoiaIME_WindowProc.cpp`，
按键路径发起的 edit session 与异步按键请求表在 `MetasequoiaIME_AsyncRequests.cpp`）与 `src/Thread/`。

## TSF / COM 硬约定

- **Test 与实际处理必须一致**：`OnTestKeyDown` 决定宿主是否还能收到按键。一旦返回“吃掉”，后续
  IPC 超时或处理分支不能再把该键交还宿主。新增按键或模式时，同时检查 fresh composition、普通
  composition、候选态、离线 fallback 和 `_DispatchKeyDown` 的实际分支，避免丢键或重复上屏。
- **文档修改只在 edit session 内完成**：composition 的开始、更新、提交和删除必须通过
  `ITfContext::RequestEditSession` 获得有效 cookie；后台线程和窗口回调不得直接写 TSF range。
- **TSF 对象遵守 apartment 与线程归属**：不要把 `ITfContext`、`ITfComposition`、presenter 等 COM
  对象交给后台线程直接调用。跨线程结果通过现有隐藏消息窗口、`PostMessage` 和队列回到 owner
  线程；同步 fallback 只能沿用已有且已验证的路径。
- **异步回调必须验证所属会话**：延迟按键和 Server 回复可能晚于失焦、composition 结束或 DLL
  停用。应用前核对现有的 focus token / generation、composition epoch、local reset token、
  延迟按键的派发 token（代码里仍叫 `deferredReplayToken`）；过期结果应丢弃，不能作用到新会话。
- **引用与 teardown 成对**：新增 COM 接口、sink、HANDLE、线程、窗口或 presenter 时，在所有失败
  分支与停用路径补齐 `Release`、Unadvise、关闭和 join。不要在析构/停用后留下会访问 `this` 的
  posted message 或 watcher。
- **`DllMain` 保持最小化**：它处于 loader lock 下，禁止在其中连接管道、启动 Server、创建线程、
  等待锁或做复杂初始化。重活放到文本服务激活后的既有生命周期中。
- **DLL 会注入第三方宿主进程**：保持静态 CRT（`MultiThreaded[Debug]`）与
  `/Zc:threadSafeInit-` 约定，避免和 QQ、浏览器等宿主的 CRT/TLS 冲突；不要随意增加全局构造器、
  进程级 hook 或修改宿主状态。

## IPC 协议——单一契约

协议定义的唯一来源是 `../engine/contracts/`，Server 读的是同一份——引擎已经是本仓的目录，两侧由同一个提交钉住，不可能不一致。TSF 仅引用头文件，不链接引擎运行库。这里保留已发布的固定宽度 Win32 ABI：

- 修改管道名、opcode、字段、容量或对齐时，在 `../engine/contracts/` 中修改并保留 ABI 检查，两侧的改动落在同一个 PR 里。禁止恢复两份手写定义。
- 主连接的版本化 ClientHello 必须获得关联 request_id 的 ProtocolReady 后才可发键；协议确认包不能作为候选或上屏文本。新 Server 兼容旧 DLL 的未版本化 hello；新 DLL 对旧或不兼容 Server 使用现有原始输入回退。版本与能力规则见共享 contracts/README.md。
- `WCHAR` 按 16 位 UTF-16 码元传输；不要用 `wchar_t` 在非 Windows 平台上的大小推断协议布局。
- 已发布 opcode 只追加、不复用、不重排。未知的较新 Worker opcode 应忽略，不能因此拆除连接。
- `request_id == 0` 表示 unsolicited，`UINT64_MAX` 表示本地无请求；不得当成普通请求回复配对。
- `PipeReady` 是反向管道握手帧，只在注册阶段消费；Worker handle 在握手完成前不能发布，Main pipe
  也不能抢先进入可用状态。
- Server 是候选选择、翻页与配置化导航的权威。TSF 本地影子状态只用于预判，不能在回复
  到达后反向覆盖 Server 已更新的候选状态。
- `DefinitelyNotSent` 与 `DeliveryAmbiguous` 都不重发：后者可能已被 Server 接收，重发会双写；
  前者也没有回放可走，按传输失败丢键。`TransportUnavailable` 是本地控制结果，绝不能当候选文本提交。
- 改协议后至少构建 TSF 的 x64/x86，并运行 Server 侧的 IPC 协议测试
  `tests/src/test_ipc_protocol_constants.cpp` 所属测试目标。

## 按键失败：不回放

被吃掉的键要么被处理，要么被有意吞掉；绝不交还宿主，也绝不处理两次。会话在组合中途出错时
清掉组合、从空开始，失败那一刻已排队的键一并吞掉（它们是按即将丢弃的组合分类的）。不要
重新引入按键回放或恢复前缀：回放带来过闪烁、双上屏和重试循环（#259、#404、#427）。

失败经 `_FailDeferredKey`（有在飞的键）或 `_ResetSessionAfterFailure`（没有）处理，由
`DeferredKeyFailurePolicy.h` 决定代价。新增失败点时选对原因走这两个入口，不要单独调用
`MarkNamedpipeSessionDirty*`——那会漏掉清队列，也会把管道没坏的问题升级成完整重置：

| 类别 | 原因 | 动作 |
|---|---|---|
| Stale | focus token、组合纪元、reset token 或焦点代已被取代 | 只丢这颗键，不再发起重置 |
| Resync | 宿主拒绝 edit session（S_FALSE、E_NOTIMPL 等）、RequestEditSession 失败、异步投递失败，以及 OnEndEdit 组合属性检查、文档不匹配等非传输问题 | 丢键、吞队列、本地取消组合；不轮换 token、不关管道，闸门重开时在同一 token 上发 HideCandidateWnd 清 Server 组合 |
| Transport | 写失败、回复读失败/坏帧、`TransportUnavailable`、`DefinitelyNotSent`、`DeliveryAmbiguous`、提交回复 1500 ms 窗口耗尽、已在等回复的提交键在同一焦点下被组合纪元取代、worker 断开 | 丢键、吞队列、完整重置（新 focus token、重开回复管道） |
| Offline | 离线原始通道里的宿主失败 | 丢键、吞队列，保留本地组合（此时它是唯一权威） |

翻页与高亮移动键（PageUp/PageDown/Up/Down/Tab）的回复只是确认，缺失是软错过，不拆管道、不清
队列；其余候选键可能上屏，保留「歧义即丢键 + 传输重置」。提交回复的总窗口是
`FANY_IME_COMMIT_REPLY_TIMEOUT_MS`（1500 ms），但等待跑在宿主 UI 线程上，不能一次阻塞读完：
消息处理函数用 `_PollCommitReply` 做不阻塞的轮询，没到就把请求放回表里、由
`TIMER_COMMIT_REPLY_POLL` 唤醒再试，并按请求 id 顺序结算。**会进 edit session 的提交路径
（确定、数字选词、标点顶高亮候选、空格顶首选）一律先经消息处理函数取回复**、按请求 id 缓存，
session 直接消费；session 自己那次读（`TryReadCommitReplyFromServerPipe`）只剩「缓存被清掉」
的竞态，它不阻塞、也不再把超时当传输失败——读不到就返回 `FANY_S_COMMIT_REPLY_NOT_READY`，
并由 **session 自己在完成钩子里**把请求交回 `_PollCommitReply`（窗口仍是同一个 1500 ms，
`_HandOffUnreadyCommitReply`）。没有轮询可交回的 session（消息窗口不可用、按键直接走同步
`_InvokeKeyHandler` 的路径）不能让出，仍保留 `FANY_IME_COMMIT_REPLY_SESSION_WAIT_MS`（300 ms）的
单次有界等待，超时按传输失败处理；由 `_SessionCommitReplyWaitMs` 按 session 记录的轮询消息判定。
交回必须发生在 session 侧：`RequestEditSession` 用的是
`TF_ES_ASYNCDONTCARE`，TSF 可能把 session 排到之后执行，那时消息处理函数早已返回、根本看不到
这个结果，只有 session 自己的钩子在两种情形下都会跑。新增会上屏的异步路径必须接入同一套轮询，
不要在 session 里阻塞等回复，不要把「结果要靠调用方从 `_InvokeKeyHandler` 的返回值里读」
当作契约，也不要让 session 的超时触发会话重置。

## 焦点、候选与离线回退

- TSF 激活/停用与窗口瞬时失焦不是同一事件。切换到别的输入法使用 `ClientDeactivated`；宿主内部
  的临时焦点变化使用 suspended/reset 流程。Excel 单元格编辑、浏览器和 Windows Text Input Host
  对焦点的处理不同，修改时都要回归。
- 候选 presenter teardown 时要先隐藏并清除**当前 Server 会话**，同时防止旧 cleanup 消息清掉
  新 composition 的候选；保留现有 session-active 与 async-cleanup 防护。
- Server 不可用时只能提交 TSF 已确定拥有的本地原始输入。没有候选权威时不要猜测候选文本；服务
  恢复后先完成 focus session 激活/reset，再排空 deferred keys。
- CN→EN、标点和全半角切换同时涉及 preserved key、TSF compartment、语言栏和 Server 状态。
  composition 中切换模式时须保持“先正确提交，再应用 compartment”的既有顺序，避免宿主双提交。
- `raw` / `pinyin` / `empty` 预编辑样式定义在 `src/Global/FanyDefines.h`；新增样式时同步检查配置读取、
  caret 映射和 Server 返回的 `Preedit` 消息，不能只改显示字符串。

## 资源、注册与日志

- CLSID、profile GUID、compartment GUID、语言栏 GUID 与注册表路径属于持久身份。除非明确进行迁移，
  不要重新生成或改写；改动时同时核对 `Register/`、`Globals.*`、`.rc` 与卸载路径。
- 新增/替换图标时同步资源 ID、`.rc`、`CMakeLists.txt` 和 `LanguageBar` 的索引；浅色/深色图标应成对。
- 语言配置（profile）图标：安装器把 `image/msime.ico` 装成 DLL 上一级目录的 `MetasequoiaImeTip.ico`，
  `RegisterProfiles` 发现它就注册这个固定路径，否则退回 DLL 内嵌的 `IDIS_METASEQUOIAIME`。不要改回总是
  注册 DLL 路径：版本目录每次升级都变，任务栏会在换目录时取图标失败，改显示「简体」直到资源管理器重启。
- 安装器把 DLL 装到 `System32\IME\MetasequoiaIME\msime_v*\`（x86 在 `SysWOW64` 下同名目录），不要挪回
  Program Files：CS2 等开启 Trusted Mode 的游戏只放行「位于系统目录且有代码签名」的第三方 in-proc DLL，
  否则输入法在游戏里根本切不出来。用 `linstall.ps1` 从构建目录注册的开发版不满足这一条，游戏内不可用。
- 语言配置的登记名称不总是 `TEXTSERVICE_DESC`：设置页「游戏兼容」会把它换成 CS2 / Dota 2 白名单里的名字
  （游戏按登记名称决定要不要自己处理输入法消息，不在名单里就弹系统默认组合窗，独占全屏下会把游戏踢出全屏）。
  选择记在 `HKLM\Software\Metasequoia\MetasequoiaIME\GameCompatProfileName`，`RegisterProfiles` 注册时以它为准——
  安装/升级重新注册会覆盖名称，不读它用户的选择就静默丢了。开启时 `ActivateEx` 也不再对这些游戏忽略 UI-less 声明。
- `BeginUIElement` 的 `pbShow=FALSE` 只表示「应用**可能**自绘」，不是「应用一定会自绘」。CUAS 是让 TSF
  输入法服务只认 IMM32 的宿主时走的兼容层，它因为承载不了 UI element 才回 FALSE，自己从不画候选。
  判断宿主是否自绘只看激活标志 `TF_TMF_UIELEMENTENABLEDONLY`：没有这个标志的宿主一律由我们画候选窗，
  否则只走 CUAS 的游戏（战舰世界等）候选窗会整个消失。加新宿主时先看日志里的
  `activate_flags`、`host_pbShow`、`pb_show_ignored` 三项，不要凭 `pbShow` 推断。
- 生产日志不得记录用户原始按键、预编辑、候选词或提交文本。性能与 IPC 诊断记录耗时、请求 ID、
  token、状态和错误码即可；调试日志也应尽量避免持久化用户输入。

## 生成文件与配置真源

`python .\scripts\prepare_env.py` 生成本机 `.clangd`，并只更新 `CMakePresets.json` 中的 vcpkg 路径。`CMakeLists.txt` 和 preset 结构直接维护；脚本不会用旧模板覆盖它们。`CMakeUserPresets.json` 中的架构配置继续继承这份 preset。

- 不提交生成的新个人绝对路径或 `build*` 等产物。
- 修改 `.clangd` include 列表时，同时检查模板和生成脚本。
- CI 在临时合仓目录实际运行环境脚本，验证 CMake 源文件、Release preset 和用户字段不会丢失。

## 代码风格

- 使用 C++17、Windows Unicode API、SAL 标注和项目现有的 4 空格 Microsoft 风格。
- 格式化遵循**本目录自己的 `.clang-format`**——仓库没有根目录 `.clang-format`，每个组件子树各有一份、规则互不相同；本目录这份是 Microsoft 风格、明确 `SortIncludes: false`，不要自动重排 include。
- 修改 `.cpp` / `.h` 后对**本次涉及的文件**运行：

```powershell
clang-format -i .\src\Path\Changed.cpp .\src\Path\Changed.h
```

- COM 出参失败时置空；检查 `HRESULT` / Win32 返回值；所有权不明确时优先沿用项目现有
  `SafeRelease`、RAII 或 teardown 模式，不要混用不兼容的智能指针约定。
- 不做与任务无关的大范围格式化或命名重构。TSF 状态机对时序敏感，小而可验证的改动优先。

注意：新增的代码尽量保持这个风格就可以了，不要去尝试格式化原有的代码。不严格要求新增代码保持这个风格。

## 构建与验证

环境：Windows、Visual Studio 2026、CMake 3.25+、vcpkg、Python 3.10+；首次准备：

```powershell
python .\scripts\prepare_env.py
```

Debug：

```powershell
.\scripts\lcompile.ps1 64
.\scripts\lcompile.ps1 32
```

Release：

```powershell
.\scripts\lcompile-release.ps1 64
.\scripts\lcompile-release.ps1 32
```

本目录的 `windows_ipc_contract` 在 x64/x86 上验证协议布局、版本握手与语音分帧；CI 还构建 Server 和 GUI 框架，并在锁定词库上运行 Server 测试。这些检查不能替代真实 TSF 宿主行为回归。按改动范围做手工回归：

- 至少验证 x64 宿主；协议、注册、资源或发布改动同时验证 x86 DLL；
- 输入首键、连续输入、空格/数字键盘选词、翻页、退格、光标移动和标点提交；
- composition 中与空 composition 下切换中英文、标点、全半角；
- 失焦/回焦、切换输入法、Server 未启动/重启/断管恢复；
- 涉及兼容性时覆盖 Win32 EDIT、Excel、Chromium 系浏览器及 UWP/现代 TSF 宿主。

本地注册脚本会执行提权后的 `regsvr32`，属于系统状态修改，不应作为普通验证自动运行。只有用户明确
要求安装/卸载时才执行：

```powershell
.\scripts\linstall.ps1
.\scripts\luninstall.ps1
```

签名脚本包含本机 Windows SDK 路径和证书占位符；没有对应证书时不要运行或提交真实 thumbprint。

需要生成可安装包做本地测试时，从仓库根目录开始，严格按以下顺序执行。任一步失败都先
停止并修复，不要继续使用旧产物打包：

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

`Prepare-PackageFiles.ps1` 会收集 Server Release、x86/x64 TSF DLL、词库和前端资源；因此必须在两个
C++ 组件编译完成且相关前端/词库产物已准备好之后运行。`Sign-PackageBinaries-Local.ps1` 使用本机
测试证书，只用于内部安装验证；它可能创建证书并尝试写入本机受信任存储，不得把本机证书指纹或
私钥提交到仓库。`Compile-Installer.ps1` 会调用 Inno Setup 命令行编译器 `ISCC.exe` 编译同目录的
`msime_setup.iss`，安装包输出到该版本目录的 `Output\`。机器上须预先安装 Inno Setup 6.6+；若脚本
无法自动找到编译器，可显式传入：

```powershell
.\Compile-Installer.ps1 -IsccPath 'C:\Program Files\Inno Setup 7\ISCC.exe'
```

## 提交纪律

一次改动经常同时落在 `windows/`、`server/` 和 `engine/`，现在它们在同一次提交里。提交前执行
`git status --short`，只暂存本任务涉及的显式路径，禁止 `git add -A` / `git add .` 把构建产物或其他
会话改动卷入提交。

提交信息沿用项目现有工程风格，推荐 `type(scope): 摘要`。不要添加 `Co-Authored-By`、
`Generated with` 或其他 AI 生成标记。
