; Metasequoia IME — Inno Setup script
; 源文件根目录：本脚本所在目录
;
; 编译方法：
;   1. 安装 Inno Setup 6.6 或更高版本：https://jrsoftware.org/isinfo.php
;   2. 运行 .\Compile-Installer.ps1（或用 Inno Setup Compiler 打开本文件并按 Ctrl+F9）
;   输出安装包默认在：Output\
;
; 本地测试打包顺序：
;   1. Prepare-PackageFiles.ps1         收集本版本的安装文件
;   2. Sign-PackageBinaries-Local.ps1   用本机自签名测试证书给包内 EXE/DLL 签名
;   3. 本文件用 Inno Setup 编译
;   4. Sign-Installer-Local.ps1         用同一张本机测试证书给安装包签名
;
; 也可以直接运行 .\test.ps1 走完整测试流程。
; 只改 TSF / Server 时用 .\test-light.ps1：ISCC /DLightPackage=1，
; 打出不含词库的轻量包，安装时也不会删本机已有词库。
; 上面两条都不把 PDB 打进包；要带符号用 .\test-symbols.ps1。
; 本仓库不包含任何预置代码签名证书。

#define MyAppName      "Metasequoia IME 水杉输入法"
#define MyAppVersion   "0.0.1"
; VersionInfoVersion 是 PE 的 VERSIONINFO，只接受数字和点号；dev 构建的 MyAppVersion 带
; -dev.<日期>.<hash> 后缀（Prepare-PackageFiles.ps1 会把数字部分同步写进这个 define），
; 文件名与 AppVersion 用完整串，这里只用数字段。
#define MyVersionInfo  "0.0.1"
#define MyAppPublisher "Metasequoia"
#define MyAppExeName   "MetasequoiaImeServer.exe"
#define MySettingsExeName "MetasequoiaImeSettings.exe"
; 与 settings_app.cpp / settings_launcher.cpp 的 kQuitSettings 保持一致：WM_APP + 5。
#define MySettingsWindowClass "MetasequoiaImeSettingsWindow"
#define MySettingsQuitMessage 32773
#define MyEmojiPanelExeName "MetasequoiaImeEmojiPanel.exe"
#define MyKeyboardPanelExeName "MetasequoiaImeKeyboardPanel.exe"
#define MyHandwritingPanelExeName "MetasequoiaImeHandwritingPanel.exe"
#define MyWatchdogName "MetasequoiaImeWatchdog.exe"
#define MyWatchdogTaskName "Metasequoia IME Watchdog"
#define MyReplayName   "MetasequoiaImeDictionaryReplay.exe"
#define MyDictionaryBuildName "MetasequoiaImeDictionaryBuild.exe"
; 与 windows\src\IME\MetasequoiaIME.cpp 的 SERVER_LAUNCH_MUTEX_NAME 是同一个名字（installer/tests/package-files.ps1 核对）。
#define MyServerLaunchMutex "Local\MetasequoiaImeServer.Launch"
#define MyVersionDirBase "msime_v" + MyAppVersion
#define MySourceRoot   "."
; TSF DLL 装在系统目录下：CS2 等开启 Trusted Mode 的游戏只放行「位于系统目录且有代码签名」的
; 第三方 in-proc DLL，装在 Program Files 的 DLL 连加载都会被拒，输入法在游戏里根本切不出来。
; 本安装器在 x64/ARM64 上以 64 位模式运行（ArchitecturesInstallIn64BitMode），
; Inno Setup 6 的 Pascal Script 里访问 {sys} 就是真正的 System32；Inno Setup 7 不再如此，见 TsfRoot64。
; ApplyPathRedirRulesForCurrentProcess 是 Inno Setup 7 的 support function，
; windows-2025 runner 的 6.7.1 没有它，不加 #if Ver 判断 ISCC 直接报 Unknown identifier
; （#925 引入后第一次在 hosted runner 上编安装包就炸了，见 installer/README.md）。
#define MyTsfRoot64    "{sys}\IME\MetasequoiaIME"
#define MyTsfRoot32    "{syswow64}\IME\MetasequoiaIME"
#ifdef LightPackage
#define MyOutputSuffix "_light"
#else
#define MyOutputSuffix ""
#endif

[Setup]
AppId={{A7C3E91F-4B2D-4E8A-9F1C-6D5E8B0A2C4D}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\metasequoiaime
DefaultGroupName={#MyAppName}
DisableDirPage=yes
; DisableDirPage=yes 时就绪页默认不显示目标目录，显式打开以便用户确认装到哪。
AlwaysShowDirOnReadyPage=yes
DisableProgramGroupPage=yes
OutputDir=Output
OutputBaseFilename=MetasequoiaIME_Setup_v{#MyAppVersion}{#MyOutputSuffix}
SetupIconFile={#MySourceRoot}\MetasequoiaIME.ico
Compression=lzma2
SolidCompression=yes
; 安装和卸载界面自动跟随 Windows 的浅色/深色模式。
WizardStyle=modern dynamic
PrivilegesRequired=admin
UsedUserAreasWarning=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={commonpf64}\metasequoiaime\MetasequoiaIME.ico
VersionInfoVersion={#MyVersionInfo}

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Dirs]
Name: "{#MyTsfRoot32}\{code:GetVersionDir}"
Name: "{#MyTsfRoot64}\{code:GetVersionDir}"
Name: "{commonpf64}\metasequoiaime\server"
; 用户数据（词库、配置、皮肤、界面资源）。默认在 LocalAppData，安装时可以改到别的盘，
; 选择写进 HKLM 的 DataDir，Server / TSF DLL / 引擎三方都从那里读。
Name: "{code:GetDataDir}"; Permissions: users-modify
; 用户自己的辅助码方案放这里，Server 扫描后列进设置页，升级时保留。
Name: "{code:GetDataDir}\helpcodes\custom"
; 用户自己的双拼方案（.toml）放这里，同样由 Server 扫描、升级时保留。
Name: "{code:GetDataDir}\shuangpin\custom"

[Files]
; 独立安装应用图标，供 Windows“已安装的应用”列表稳定显示。
Source: "{#MySourceRoot}\MetasequoiaIME.ico"; \
    DestDir: "{commonpf64}\metasequoiaime"; Flags: ignoreversion

; 第三方声明随包安装。词库主体含 rime-ice（GPL-3.0）内容，其许可要求保留署名，
; 因此这份文件必须落到用户磁盘上，而不能只存在于源码仓库里。
Source: "{#MySourceRoot}\THIRD_PARTY_NOTICES.txt"; \
    DestDir: "{commonpf64}\metasequoiaime"; Flags: ignoreversion

; GPLv3 第 4、6 条要求分发时向接收者提供许可证副本，而 THIRD_PARTY_NOTICES.txt 只是指向
; "the LICENSE file"、本身不含 GPL 正文。macOS 与 Linux 的 CMake 安装规则早已随包装入许可证，
; Windows 是唯一大规模分发却漏掉这一步的平台。
Source: "{#MySourceRoot}\LICENSE.txt"; \
    DestDir: "{commonpf64}\metasequoiaime"; Flags: ignoreversion

; 输入法语言配置（profile）的图标放在固定路径。TSF DLL 每次升级都换版本目录，图标若指向 DLL，
; 注册表里的图标路径就跟着变；任务栏恰好在换目录时取不到图标，就改显示「简体」文字并一直沿用到
; 资源管理器重启。DLL 注册时发现自己上一级目录有这个文件就改用它（windows/src/Register/Register.cpp），
; 所以这两条必须排在下面两个 regserver 条目之前；32 位和 64 位 DLL 各按自己的上一级目录找，各放一份。
Source: "{#MySourceRoot}\MetasequoiaImeTip.ico"; \
    DestDir: "{#MyTsfRoot32}"; Flags: ignoreversion 32bit

Source: "{#MySourceRoot}\MetasequoiaImeTip.ico"; \
    DestDir: "{#MyTsfRoot64}"; Flags: ignoreversion

; TSF DLL 使用版本独立目录，避免升级时覆盖仍被进程加载的 DLL。系统目录的原因见 MyTsfRoot64。
; PDB 与对应 DLL 放在同一目录，调试器可按二进制的内嵌路径自动找到符号。
; 只有 Prepare-PackageFiles.ps1 -IncludeSymbols 才会把 PDB 放进 tsf_dll\；默认本地打包不含符号，
; 所以这两条必须带 skipifsourcedoesntexist，否则通配符匹配不到文件时 ISCC 会直接报错。
Source: "{#MySourceRoot}\tsf_dll\32\*.dll"; \
    DestDir: "{#MyTsfRoot32}\{code:GetVersionDir}"; \
    Flags: ignoreversion regserver 32bit

Source: "{#MySourceRoot}\tsf_dll\64\*.dll"; \
    DestDir: "{#MyTsfRoot64}\{code:GetVersionDir}"; \
    Flags: ignoreversion regserver

Source: "{#MySourceRoot}\tsf_dll\32\*.pdb"; \
    DestDir: "{#MyTsfRoot32}\{code:GetVersionDir}"; \
    Flags: ignoreversion skipifsourcedoesntexist 32bit

Source: "{#MySourceRoot}\tsf_dll\64\*.pdb"; \
    DestDir: "{#MyTsfRoot64}\{code:GetVersionDir}"; \
    Flags: ignoreversion skipifsourcedoesntexist

Source: "{#MySourceRoot}\server_exe\*"; \
    DestDir: "{commonpf64}\metasequoiaime\server"; \
    Flags: ignoreversion recursesubdirs createallsubdirs

; 内置皮肤的出厂清单（开关与完整外观）每次安装都整份覆盖：用户技术上能改，但不建议，升级会冲掉改动。
; 单独暂存在 default_skins，不经 app_data：skins 整个目录是用户数据，卸载时不删。
Source: "{#MySourceRoot}\default_skins\*"; \
    DestDir: "{code:GetDataDir}\skins\default"; \
    Flags: ignoreversion recursesubdirs createallsubdirs uninsneveruninstall

; 仓库 skins\ 下的外部皮肤，同名目录整目录覆盖。用户机器上的同名皮肤和随包的不一样时，
; PrepareToInstall 里的 BackupChangedBundledSkins 先把它改名成 <id>.bak 再复制。
Source: "{#MySourceRoot}\bundled_skins\*"; \
    DestDir: "{code:GetDataDir}\skins"; \
    Flags: ignoreversion recursesubdirs createallsubdirs uninsneveruninstall
Source: "{#MySourceRoot}\bundled_skins.manifest"; Flags: dontcopy

#ifdef LightPackage
; 轻量包只覆盖设置窗口的图片资源，不带词库/辅助码/出厂配置。
Source: "{#MySourceRoot}\app_data\assets\settings\*"; \
    DestDir: "{code:GetDataDir}\assets\settings"; \
    Flags: ignoreversion recursesubdirs createallsubdirs uninsneveruninstall
#else
; 包内故意不带 config.toml。通配复制再排除一次，防止以后又把用户配置打进包内。
Source: "{#MySourceRoot}\app_data\*"; DestDir: "{code:GetDataDir}"; \
    Excludes: "\config.toml,\config.base.toml,\config.default.toml"; \
    Flags: ignoreversion recursesubdirs createallsubdirs uninsneveruninstall

; msime.db、english.db、others.db、dict_japanese.dat 以文本源进包（SQLite 页和索引压不动，这样小约 40 MB），
; 只在安装过程中用：解到 {tmp}，ssPostInstall 的 BuildDictionaries 还原进数据目录，安装程序退出时随 {tmp} 删掉。
Source: "{#MySourceRoot}\dictionary_source\*"; DestDir: "{tmp}\dictionary_source"; \
    Flags: ignoreversion deleteafterinstall

; 用户配置只在首次安装时从出厂模板生成。升级时绝不覆盖已有 config.toml；
; Server 启动时再以 config.default.toml 合并：保留用户改过的值，带入新版新增项。
Source: "{#MySourceRoot}\app_data\config.default.toml"; \
    DestDir: "{code:GetDataDir}"; DestName: "config.toml"; \
    Flags: onlyifdoesntexist uninsneveruninstall
Source: "{#MySourceRoot}\app_data\config.default.toml"; \
    DestDir: "{code:GetDataDir}"; DestName: "config.default.toml"; \
    Flags: ignoreversion uninsneveruninstall
#endif

[Icons]
Name: "{group}\{#MyAppName}"; \
    Filename: "{commonpf64}\metasequoiaime\server\{#MySettingsExeName}"; \
    WorkingDir: "{commonpf64}\metasequoiaime\server"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"

[Registry]
Root: HKLM; Subkey: "Software\Metasequoia\MetasequoiaIME"; \
    ValueType: string; ValueName: "VersionDir"; ValueData: "{code:GetVersionDir}"; \
    Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Metasequoia\MetasequoiaIME"; \
    ValueType: string; ValueName: "ServerPath"; \
    ValueData: "{commonpf64}\metasequoiaime\server\{#MyAppExeName}"; \
    Flags: uninsdeletevalue
; 用户数据目录的唯一权威来源。Server、TSF DLL 和引擎都按
; METASEQUOIA_IME_DATA_DIR → 这个键 → %LOCALAPPDATA%\metasequoiaime 的顺序解析；
; 32 位 TSF DLL 用 KEY_WOW64_64KEY 读，所以这里必须写在 64 位视图里
; （ArchitecturesInstallIn64BitMode 已经保证了这一点）。
Root: HKLM; Subkey: "Software\Metasequoia\MetasequoiaIME"; \
    ValueType: string; ValueName: "DataDir"; ValueData: "{code:GetDataDir}"; \
    Flags: uninsdeletevalue

[Code]
const
  { 放在数据目录里，标记「输入法用过这个目录」。}
  DataDirMarkerName = '.metasequoiaime-data';
  { 标记文件里带上这一行，才表示目录是安装器在它为空时接手的，整个目录都归输入法，
    覆盖安装和卸载可以整目录清理。旧版安装器不管目录里原来有什么都写标记（#537），
    所以只有标记、没有这一行的目录只能按名单删输入法自己的文件。}
  DataDirExclusiveTag = 'exclusive: adopted empty by the installer';
  { 用户选的目录里已有别的文件时，数据改放到这个子目录里。}
  DataDirSubfolderName = 'metasequoiaime';

  VCRuntimeKey = 'Software\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';
  { 安装器界面本身就是中文（InitializeSetup 在选语言对话框之前跑，拿不到用户选的语言），
    说明页也统一给简体中文版，省得用户落到英文页上再自己切。}
  VCRuntimeDownloadUrl =
    'https://learn.microsoft.com/zh-cn/cpp/windows/latest-supported-vc-redist?view=msvc-170';
  { 微软给的「最新受支持 v14 x64 运行库」固定直链。}
  VCRuntimeDirectUrl = 'https://aka.ms/vc14/vc_redist.x64.exe';
  { 见 VCRuntimeInstalledInView 上方的说明。}
  VCRuntimeMinMinor = 40;

var
  VersionDirName: String;
  DataDirValue: String;
  PreviousDataDir: String;
  DataDirPage: TInputDirWizardPage;
  NetworkPage: TInputOptionWizardPage;
  CloudCandidatesIndex: Integer;
  UserConfigExistedBeforeInstall: Boolean;
  DataDirAdoptedEmpty: Boolean;

// VC 运行库不随包分发：它是系统级共享组件，安装器不该替用户装。缺了它 Server 直接
// 起不来，而故障现场在安装结束之后，用户看不到任何解释，所以在安装开始前查一遍，
// 把原因当场说清楚。
//
// 用注册表判定，而不是去 System32 里找 vcruntime140.dll：Setup.exe 是 32 位进程，
// Pascal Script 的 FileExists 会被 WOW64 重定向到 SysWOW64（[Files] 和 Exec 里的
// {sys} 由 Inno 自己处理，脚本函数不管），结果查的是 x86 运行库——只装了 x64 redist
// 的机器会被误判成缺少 x64 运行库。绕开重定向的两个办法都不可移植：
// EnableFsRedirection 在 Inno Setup 7 里已被移除，而它的替代品在 6.x 里还不存在；
// {sysnative} 对 64 位安装器又不可用。注册表访问没有这个问题，HKLM32 / HKLM64
// 两个视图都是显式指定的。
//
// x64 redist 的安装状态就记在这个键里。要求 14.40 以上：server_exe 里的 exe 都用
// MSVC 14.40+（VS 2022 17.10 起）编译并导入 msvcp140.dll，这一版起 std::mutex 的
// 构造函数改成了 constexpr，配上 14.40 之前的 msvcp140.dll 会在第一次加锁时访问冲突，
// 进程无声闪退（设置页打不开就是这个样子）。装了 2019 / 早期 2022 运行库的机器
// 同样有 Installed=1，所以必须比版本号，不能只看装没装。
// 32 位 TSF DLL 用的是静态 CRT（windows/CMakeLists.txt 里的
// CMAKE_MSVC_RUNTIME_LIBRARY），所以不必检查 x86 redist。
// 注意：Inno Setup 的花括号注释不能嵌套，上面提到的常量会提前闭合 { } 注释，
// 所以这一段只能用行注释。
function VCRuntimeInstalledInView(RootKey: Integer): Boolean;
var
  Installed: Cardinal;
  Major: Cardinal;
  Minor: Cardinal;
begin
  Result :=
    RegQueryDWordValue(RootKey, VCRuntimeKey, 'Installed', Installed) and
    (Installed = 1) and
    RegQueryDWordValue(RootKey, VCRuntimeKey, 'Major', Major) and
    RegQueryDWordValue(RootKey, VCRuntimeKey, 'Minor', Minor) and
    ((Major > 14) or ((Major = 14) and (Minor >= VCRuntimeMinMinor)));
end;

{ redist 的安装包是 32 位的，键通常落在 WOW6432Node 下，但较新的版本两个视图都写，
  所以两边都查。}
function VCRuntimeIsInstalled: Boolean;
begin
  Result :=
    VCRuntimeInstalledInView(HKLM32) or VCRuntimeInstalledInView(HKLM64);
end;

{ 只用于日志和提示文字，不参与判定。}
function ReadVCRuntimeVersion: String;
begin
  Result := '';
  if not RegQueryStringValue(HKLM32, VCRuntimeKey, 'Version', Result) then
    if not RegQueryStringValue(HKLM64, VCRuntimeKey, 'Version', Result) then
      Result := '';
  Result := Trim(Result);
end;

procedure OpenDownloadPage(const Url: String);
var
  ErrorCode: Integer;
begin
  { 安装器是提权进程，用它拉起的浏览器会以管理员身份运行；和 LaunchInstalledComponents
    一样交还给原用户。}
  ShellExecAsOriginalUser(
    'open', Url, '', '', SW_SHOWNORMAL, ewNoWait, ErrorCode);
end;

function InitializeSetup: Boolean;
var
  NeedsVCRuntime: Boolean;
  VCRuntimeVersion: String;
  Prompt: String;
begin
  Result := True;

  NeedsVCRuntime := not VCRuntimeIsInstalled;
  { 有版本号却没通过判定，就是装了但太旧，提示里要和「没装」分开说。}
  VCRuntimeVersion := ReadVCRuntimeVersion;
  if NeedsVCRuntime then
    Log('Prerequisite missing or outdated: Visual C++ 2015-2022 Redistributable (x64), found "' +
      VCRuntimeVersion + '", need 14.' + IntToStr(VCRuntimeMinMinor) + ' or later.')
  else
    Log('Visual C++ runtime found: ' + VCRuntimeVersion);

  if not NeedsVCRuntime then
    exit;

  Prompt := '这台电脑缺少输入法用到的系统组件，或版本过旧：' + #13#10 + #13#10 +
    '● Visual C++ 2015-2022 可再发行组件包（x64），需要 14.' +
    IntToStr(VCRuntimeMinMinor) + ' 或更高版本' + #13#10;
  if VCRuntimeVersion <> '' then
    Prompt := Prompt +
      '   当前已安装 ' + VCRuntimeVersion + '，版本过旧，设置页等程序会启动即闪退。' + #13#10
  else
    Prompt := Prompt +
      '   缺少它输入法主程序根本无法启动（提示找不到 VCRUNTIME140.dll 之类）。' + #13#10;
  Prompt := Prompt +
    '   直接下载：' + VCRuntimeDirectUrl + #13#10 +
    '   说明页面：' + VCRuntimeDownloadUrl + #13#10 + #13#10 +
    '点击「是」直接下载该组件并结束本次安装，装好后重新运行本安装程序；' + #13#10 +
    '点击「否」继续安装（装完仍需自行补齐该组件，输入法才能正常使用）。';

  { 静默安装（CI、企业批量部署）不该被一个对话框卡住：默认继续，缺什么已经写进日志了。}
  if
    SuppressibleMsgBox(Prompt, mbConfirmation, MB_YESNO, IDNO) = IDYES
  then
  begin
    OpenDownloadPage(VCRuntimeDirectUrl);
    Result := False;
  end;
end;

{ 上一次安装（或历史版本）的数据目录。没有注册表值就是历史默认位置。}
function ResolvePreviousDataDir: String;
var
  Recorded: String;
begin
  if PreviousDataDir = '' then
  begin
    Recorded := '';
    if
      RegQueryStringValue(
        HKLM,
        'Software\Metasequoia\MetasequoiaIME',
        'DataDir',
        Recorded
      ) and (Trim(Recorded) <> '')
    then
      PreviousDataDir := RemoveBackslashUnlessRoot(Trim(Recorded))
    else
      PreviousDataDir := ExpandConstant('{localappdata}\metasequoiaime');
  end;
  Result := PreviousDataDir;
end;

{ 本次安装要用的数据目录。静默安装可以用 /DATADIR="D:\..." 指定；
  两者都没有时沿用上一次的位置。}
function GetDataDir(Param: String): String;
var
  FromCommandLine: String;
begin
  if DataDirValue = '' then
  begin
    FromCommandLine := Trim(ExpandConstant('{param:DATADIR|}'));
    if FromCommandLine <> '' then
      DataDirValue := RemoveBackslashUnlessRoot(FromCommandLine)
    else
      DataDirValue := ResolvePreviousDataDir;
  end;
  Result := DataDirValue;
end;

function DataDirMarkerPath(const Directory: String): String;
begin
  Result := AddBackslash(Directory) + DataDirMarkerName;
end;

{ 输入法用过这个目录（新旧标记都算）。只凭这一点，只能删名单里输入法自己的文件。}
function DataDirHasMarker(const Directory: String): Boolean;
begin
  Result := (Directory <> '') and FileExists(DataDirMarkerPath(Directory));
end;

function DataDirMarkerIsExclusive(const Directory: String): Boolean;
var
  Lines: TArrayOfString;
  Index: Integer;
begin
  Result := False;
  if not LoadStringsFromFile(DataDirMarkerPath(Directory), Lines) then
    exit;
  for Index := 0 to GetArrayLength(Lines) - 1 do
    if Trim(Lines[Index]) = DataDirExclusiveTag then
    begin
      Result := True;
      exit;
    end;
end;

{ 只有这两种目录允许整目录清理：标记里写明是安装器在空目录上接手的，
  以及历史默认位置（老版本装的，那时还没有标记文件，而且它的名字本身就是输入法专用的）。}
function OwnsDataDirExclusively(const Directory: String): Boolean;
begin
  Result :=
    (Directory <> '') and
    ((CompareText(
        Directory,
        ExpandConstant('{localappdata}\metasequoiaime')) = 0) or
     (DataDirHasMarker(Directory) and DataDirMarkerIsExclusive(Directory)));
end;

{ Exclusive 只能在确认目录接手时为空（或本来就归我们）时传 True。
  已有的旧标记不会因为一次覆盖安装就升级成独占：那时目录里已经有东西，说不清是谁的。}
procedure WriteDataDirMarker(const Directory: String; Exclusive: Boolean);
var
  Lines: TArrayOfString;
begin
  if Exclusive then
  begin
    if DataDirMarkerIsExclusive(Directory) then
      exit;
    SetArrayLength(Lines, 2);
    Lines[0] := 'Metasequoia IME user data directory.';
    Lines[1] := DataDirExclusiveTag;
  end
  else
  begin
    if DataDirHasMarker(Directory) then
      exit;
    SetArrayLength(Lines, 1);
    Lines[0] := 'Metasequoia IME user data directory.';
  end;
  SaveStringsToFile(DataDirMarkerPath(Directory), Lines, False);
end;

function UserConfigPath: String;
begin
  Result := AddBackslash(GetDataDir('')) + 'config.toml';
end;

function IsPathInside(const Child, Parent: String): Boolean;
begin
  Result :=
    (CompareText(Child, Parent) = 0) or
    (CompareText(
       Copy(AddBackslash(Child), 1, Length(AddBackslash(Parent))),
       AddBackslash(Parent)) = 0);
end;

function DirectoryIsEmpty(const Directory: String): Boolean;
var
  FindRec: TFindRec;
begin
  Result := True;
  if not DirExists(Directory) then
    exit;
  if FindFirst(AddBackslash(Directory) + '*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          Result := False;
          exit;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

{ 返回空串表示这个路径可以用；否则返回要展示给用户的原因。}
function DataDirRejectionReason(const Directory: String): String;
var
  Critical: array[0..6] of String;
  Index: Integer;
  ProbePath: String;
begin
  Result := '';

  if (Length(Directory) < 4) or (Directory[2] <> ':') or (Directory[3] <> '\') then
  begin
    Result := '请填写本机磁盘上的完整路径，例如 D:\MetasequoiaIME。';
    exit;
  end;
  if not DirExists(Copy(Directory, 1, 3)) then
  begin
    Result := '找不到驱动器 ' + Copy(Directory, 1, 2) + '，请换一个位置。';
    exit;
  end;
  if CompareText(RemoveBackslashUnlessRoot(Directory), Copy(Directory, 1, 2)) = 0 then
  begin
    Result := '不能直接使用驱动器根目录，请指定一个子目录。';
    exit;
  end;

  { 覆盖安装会清理数据目录里的旧资源，卸载会整个删掉它。
    因此它既不能落在程序目录里，也不能反过来包住系统或用户的关键目录。}
  if
    IsPathInside(Directory, ExpandConstant('{commonpf64}\metasequoiaime')) or
    IsPathInside(Directory, ExpandConstant('{commonpf32}\metasequoiaime'))
  then
  begin
    Result := '数据目录不能放在输入法的程序目录里面。';
    exit;
  end;

  Critical[0] := ExpandConstant('{win}');
  Critical[1] := ExpandConstant('{commonpf64}');
  Critical[2] := ExpandConstant('{commonpf32}');
  Critical[3] := ExpandConstant('{localappdata}');
  Critical[4] := ExpandConstant('{userappdata}');
  { Inno 没有对应用户主目录的常量，用环境变量展开。取不到时下面的空值检查会跳过这一项。}
  Critical[5] := ExpandConstant('{%USERPROFILE|}');
  Critical[6] := ExpandConstant('{commonappdata}');
  for Index := 0 to 6 do
  begin
    if (Critical[Index] <> '') and IsPathInside(Critical[Index], Directory) then
    begin
      Result :=
        '这个目录包含了系统或用户的重要目录（' + Critical[Index] + '），' +
        '卸载时会连它一起删除。请另选一个专用目录。';
      exit;
    end;
  end;

  { AdoptableDataDir 已经把有别的文件的目录换成了子目录；走到这里还不空，说明连那个子目录
    里都有不归输入法的东西。}
  if
    (not DirectoryIsEmpty(Directory)) and
    (not DataDirHasMarker(Directory)) and
    (not OwnsDataDirExclusively(Directory))
  then
  begin
    Result :=
      '目录 ' + Directory + ' 里已经有其他文件。' +
      '为避免卸载时误删，请选择一个空目录。';
    exit;
  end;

  if not ForceDirectories(Directory) then
  begin
    Result := '无法创建目录 ' + Directory + '，请检查权限或换一个位置。';
    exit;
  end;
  ProbePath := AddBackslash(Directory) + 'msime-write-probe.tmp';
  if not SaveStringToFile(ProbePath, 'probe', False) then
  begin
    Result := '目录 ' + Directory + ' 不可写，请换一个位置。';
    exit;
  end;
  DeleteFile(ProbePath);
end;

{ 数据目录必须整个归输入法：用户选了一个本来就有东西的文件夹（或者驱动器根目录），
  就在里面建一个专用子目录。输入法用过的目录（有标记，或历史默认位置）原样沿用。}
function AdoptableDataDir(const Chosen: String): String;
begin
  Result := Chosen;
  if (Length(Chosen) = 3) and (Chosen[2] = ':') and (Chosen[3] = '\') then
    Result := Chosen + DataDirSubfolderName
  else if
    (not DirectoryIsEmpty(Chosen)) and
    (not DataDirHasMarker(Chosen)) and
    (not OwnsDataDirExclusively(Chosen))
  then
    Result := AddBackslash(Chosen) + DataDirSubfolderName;
end;

{ CreateInputDirPage 自带的浏览按钮调用 BrowseForFolder 时不给新建文件夹按钮，用户没法在对话框里
  当场建一个目录。接管它的 OnClick，换成带新建按钮的那种对话框。}
procedure DataDirBrowseClick(Sender: TObject);
var
  Chosen: String;
begin
  Chosen := Trim(DataDirPage.Values[0]);
  if BrowseForFolder('请选择输入法数据的存放位置：', Chosen, True) then
    DataDirPage.Values[0] := Chosen;
end;

{ 云候选是唯一一个装完就会联网的功能：输入过程中把当前拼写发给 Google 的 input-tools 服务。
  出厂默认开启，而安装器此前没有任何一屏提到过它，用户要读文档才会知道。这一页把它摆到安装
  过程里，选择写进首次生成的 config.toml。

  升级时跳过：那时 config.toml 已经属于用户，安装器不该替他重新决定。}
procedure InitializeWizard;
begin
  { 词库、用户配置、皮肤和界面资源都在这个目录下，整包有几百 MB，
    所以要让用户能把它放到别的盘。程序本体仍然装在 Program Files：
    Server 带 uiAccess=true，只有装在受信任目录里这个标志才生效。}
  DataDirPage := CreateInputDirPage(
    wpLicense,
    '选择数据位置',
    '输入法数据存放在哪里',
    '请选择输入法数据（词库、配置、皮肤）的存放位置。',
    True,
    'metasequoiaime'
  );
  DataDirPage.Add('');
  DataDirPage.Values[0] := ResolvePreviousDataDir;
  DataDirPage.Buttons[0].OnClick := @DataDirBrowseClick;

#ifndef LightPackage
  NetworkPage := CreateInputOptionPage(
    DataDirPage.ID,
    '联网功能',
    '选择安装后哪些功能可以联网',
    '拼音切分、候选排序和词频学习全部在本机完成，不联网。' + #13#10 +
    '下面这一项是唯一一个装完就会生效的联网功能。AI 联想、候选翻译、语音输入都需要你自己填入 API token 之后才会发出任何请求。' + #13#10#13#10 +
    '安装后随时可以在「设置 → 输入」里改变这个选择。',
    False,
    False
  );
  CloudCandidatesIndex := NetworkPage.Add(
    '启用云候选：输入过程中把当前正在输入的拼写通过 HTTPS 发送给 Google 的 input-tools 服务' +
    '（inputtools.google.com），换回一条额外候选。已上屏的文本、词库内容和学习到的词频都不会发送。');
  NetworkPage.Values[CloudCandidatesIndex] := True;
#endif
end;

#include "cloud_choice.iss"

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if (NetworkPage <> nil) and (PageID = NetworkPage.ID) then
    Result := UserConfigExistsBeforeInstall;
end;

procedure LaunchInstalledComponents;
var
  ErrorCode: Integer;
begin
  { uiAccess=true 的程序不能用 CreateProcess 拉起（会报 740）。
    完成页点击 Finish 后，以原用户身份执行 ShellExecute（等同双击）。}
  ShellExecAsOriginalUser(
    '',
    ExpandConstant('{commonpf64}\metasequoiaime\server\{#MyAppExeName}'),
    '',
    '',
    SW_SHOWNORMAL,
    ewNoWait,
    ErrorCode
  );
  ShellExecAsOriginalUser(
    '',
    ExpandConstant('{commonpf64}\metasequoiaime\server\{#MyWatchdogName}'),
    '',
    '',
    SW_SHOWNORMAL,
    ewNoWait,
    ErrorCode
  );
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Chosen: String;
  Adopted: String;
  Reason: String;
begin
  Result := True;

  if (DataDirPage <> nil) and (CurPageID = DataDirPage.ID) then
  begin
    Chosen := RemoveBackslashUnlessRoot(Trim(DataDirPage.Values[0]));
    Adopted := AdoptableDataDir(Chosen);
    { 用户可能指到一个本来就有东西的文件夹（#537）。数据目录会被覆盖安装清理、被卸载整个删除，
      所以不能和别人的文件混在一起：改用里面的专用子目录，并先让用户看到实际位置。}
    if
      (CompareText(Adopted, Chosen) <> 0) and
      (MsgBox(
         '目录 ' + Chosen + ' 里已经有其他文件（或是驱动器根目录）。' + #13#10 +
         '为了卸载时不误删这些文件，输入法数据将放在它的子目录里：' + #13#10 + #13#10 +
         '    ' + Adopted + #13#10 + #13#10 +
         '确定吗？',
         mbConfirmation, MB_OKCANCEL) <> IDOK)
    then
    begin
      Result := False;
      exit;
    end;
    Reason := DataDirRejectionReason(Adopted);
    if Reason <> '' then
    begin
      MsgBox(Reason, mbError, MB_OK);
      Result := False;
      exit;
    end;
    { 冲突要在这一页拦下，等到「安装」才报错时用户已经回不到这里了。}
    Reason := ConfigMigrationProblem(ResolvePreviousDataDir, Adopted);
    if Reason <> '' then
    begin
      MsgBox(Reason, mbError, MB_OK);
      Result := False;
      exit;
    end;
    DataDirPage.Values[0] := Adopted;
    DataDirValue := Adopted;
    exit;
  end;

  if (CurPageID = wpFinished) and (not WizardSilent) then
    LaunchInstalledComponents;
end;

function UpdateReadyMemo(
  Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo + NewLine + NewLine +
    '数据目录（词库、用户配置、皮肤）：' + NewLine + Space + GetDataDir('');
  if CompareText(GetDataDir(''), ResolvePreviousDataDir) <> 0 then
    Result := Result + NewLine + NewLine +
      '现有数据将从这里迁移：' + NewLine + Space + ResolvePreviousDataDir;
end;

// 本进程能直接访问的 TSF 根目录。Inno Setup 6 的 64 位模式下脚本函数不经重定向，
// sys 常量直接就是 System32；Inno Setup 7 去掉了这层处理，32 位 Setup 里 DirExists、
// FindFirst、DelTree 访问 System32 都会被 WOW64 改到 SysWOW64。那样 GetVersionDir 看不到
// 仍被占用的 64 位版本目录，选中同名目录去覆盖正在加载的 DLL，Restart Manager 就会
// 列出一长串要关闭的程序；旧的 64 位版本目录也永远清不掉。所以 7 上要换成 Sysnative 路径。
function TsfRoot64: String;
begin
#if Ver >= EncodeVer(7, 0, 0)
  Result := ApplyPathRedirRulesForCurrentProcess(True, ExpandConstant('{#MyTsfRoot64}'));
#else
  Result := ExpandConstant('{#MyTsfRoot64}');
#endif
end;

function TsfRoot32: String;
begin
  Result := ExpandConstant('{#MyTsfRoot32}');
end;

function GetVersionDir(Param: String): String;
var
  Candidate: String;
  Suffix: Integer;
begin
  if VersionDirName = '' then
  begin
    Candidate := '{#MyVersionDirBase}';
    Suffix := 0;
    while
      DirExists(AddBackslash(TsfRoot32) + Candidate) or
      DirExists(AddBackslash(TsfRoot64) + Candidate)
    do
    begin
      Suffix := Suffix + 1;
      Candidate := '{#MyVersionDirBase}.' + IntToStr(Suffix);
    end;
    VersionDirName := Candidate;
  end;
  Result := VersionDirName;
end;

function IsUserDatabaseFile(const FileName: String): Boolean;
begin
  { WAL 中可能还有尚未 checkpoint 的用户操作，必须与主库一起保留。}
  Result :=
    (CompareText(FileName, 'msime_user.db') = 0) or
    (CompareText(FileName, 'msime_user.db-wal') = 0) or
    (CompareText(FileName, 'msime_user.db-shm') = 0) or
    (CompareText(FileName, 'msime_user.db-journal') = 0);
end;

function IsUserStatisticsDatabase(const FileName: String): Boolean;
begin
  { 输入统计库和用户词库同一条理由：Server 打开它时会留下 sidecar，只搬主库会丢掉
    最后一次写入。这个库默认不建（统计开关关闭），建了之后就只有用户自己能重建它的内容。}
  Result :=
    (CompareText(FileName, 'stats.db') = 0) or
    (CompareText(FileName, 'stats.db-wal') = 0) or
    (CompareText(FileName, 'stats.db-shm') = 0) or
    (CompareText(FileName, 'stats.db-journal') = 0);
end;

function IsUserConfigFile(const FileName: String): Boolean;
begin
  { config.toml 是用户配置，config.base.toml 是上次合并用的模板基线：
    没有它，Server 就无法判断某一项到底是用户改的还是旧版默认值。}
  Result :=
    (CompareText(FileName, 'config.toml') = 0) or
    (CompareText(FileName, 'config.base.toml') = 0);
end;

function IsUserTranslationFile(const FileName: String): Boolean;
begin
  { 候选窗中英翻译的人工覆盖表，由用户手工放在数据目录根下（README「自定义候选窗翻译」一节），
    安装包不携带、引擎只读不写。它没有 config.toml 那样的出厂副本可回退，删掉就是内容
    彻底丢失，所以必须与用户配置同等对待。}
  Result := CompareText(FileName, 'custom_translations.txt') = 0;
end;

function IsUserSkinDirectory(const FileName: String): Boolean;
begin
  { 外部皮肤在 %LOCALAPPDATA%\metasequoiaime\skins，升级安装不得清掉。}
  Result := CompareText(FileName, 'skins') = 0;
end;

function IsUserShuangpinDirectory(const FileName: String): Boolean;
begin
  { shuangpin\custom 放的是用户自定义双拼方案，安装包不往 shuangpin 里写任何东西，整个目录都归用户。}
  Result := CompareText(FileName, 'shuangpin') = 0;
end;

function IsCollocationModelDirectory(const FileName: String): Boolean;
begin
  { octagram 语法模型包（目录首项为推荐包万象）在 <数据目录>\models\<id>，是设置页按需下载的
    约 390MB，安装包不携带，
    也不是每次安装都会重写的东西。神经整句效果档模型同样按需下载在 models\neural-desktop。这份名单的语义恰好是「升级保留、卸载删除」——
    IsPreservedAppDataItem 只被 CleanAppDataExceptUserFiles（覆盖安装）与 RemoveDataDir
    （卸载/换目录）读——所以归这里，不能进 IsRuntimeAppDataItem：后者两条路径都删，
    覆盖安装会把模型一并清掉，用户得重下一次。}
  Result := CompareText(FileName, 'models') = 0;
end;

function IsPreservedAppDataItem(const FileName: String): Boolean;
begin
  { 标记文件也要留下。它虽然会在 ssPostInstall 重写一遍，但安装若在中途失败，
    没有它的数据目录就不再被认作我们建的，后续的清理和卸载都会跳过。}
  Result :=
    IsUserDatabaseFile(FileName) or
    IsUserStatisticsDatabase(FileName) or
    IsUserConfigFile(FileName) or
    IsUserTranslationFile(FileName) or
    IsUserSkinDirectory(FileName) or
    IsUserShuangpinDirectory(FileName) or
    IsCollocationModelDirectory(FileName) or
    (CompareText(FileName, DataDirMarkerName) = 0);
end;

{ app_data 里随包装进数据目录根下的条目。installer/tests/package-files.ps1 拿真正打出来的
  app_data 对这份名单，打包新增文件而漏改这里会红。msime.db、english.db、others.db、dict_japanese.dat
  不经 app_data，由 BuildDictionaries 在安装时生成，但同样是出厂文件，覆盖安装和卸载要能删掉。}
function IsShippedAppDataItem(const FileName: String): Boolean;
begin
  Result :=
    (CompareText(FileName, 'pinyin.txt') = 0) or
    (CompareText(FileName, 'dict_pinyin.dat') = 0) or
    (CompareText(FileName, 'msime.db') = 0) or
    (CompareText(FileName, 'dictionary-manifest.json') = 0) or
    (CompareText(FileName, 'dict_japanese.dat') = 0) or
    (CompareText(FileName, 'MOZC_DICTIONARY_LICENSE.txt') = 0) or
    (CompareText(FileName, 'english.db') = 0) or
    (CompareText(FileName, 'others.db') = 0) or
    (CompareText(FileName, 'sentence-model.safetensors') = 0) or
    (CompareText(FileName, 'chinese-ime-lm-NOTICE.md') = 0) or
    (CompareText(FileName, 'config.default.toml') = 0) or
    (CompareText(FileName, 'helpcodes') = 0) or
    (CompareText(FileName, 'radical') = 0) or
    (CompareText(FileName, 'assets') = 0);
end;

{ Server、引擎和安装器自己运行时在数据目录根下写出来的条目（不含 IsPreservedAppDataItem 已列的）。}
function IsRuntimeAppDataItem(const FileName: String): Boolean;
begin
  Result :=
    (CompareText(FileName, 'msime.db-wal') = 0) or
    (CompareText(FileName, 'msime.db-shm') = 0) or
    (CompareText(FileName, 'msime.db-journal') = 0) or
    (CompareText(FileName, 'english.db-wal') = 0) or
    (CompareText(FileName, 'english.db-shm') = 0) or
    (CompareText(FileName, 'english.db-journal') = 0) or
    (CompareText(FileName, 'others.db-wal') = 0) or
    (CompareText(FileName, 'others.db-shm') = 0) or
    (CompareText(FileName, 'others.db-journal') = 0) or
    { BuildDictionaries 的中间文件，正常结束时已改名；安装被强行中断才会留下。}
    (CompareText(FileName, 'msime.db.building') = 0) or
    (CompareText(FileName, 'english.db.building') = 0) or
    (CompareText(FileName, 'others.db.building') = 0) or
    (CompareText(FileName, 'dict_japanese.dat.building') = 0) or
    (CompareText(FileName, 'user_dict.dat') = 0) or
    (CompareText(FileName, 'clipboard_history.json') = 0) or
    (CompareText(FileName, 'voice_system_audio_mute_state.txt') = 0) or
    (CompareText(FileName, 'config.toml.tmp') = 0) or
    (CompareText(FileName, 'config.base.toml.tmp') = 0) or
    (CompareText(Copy(FileName, 1, 20), 'config.toml.corrupt-') = 0) or
    (CompareText(FileName, '.ime-write-probe') = 0) or
    (CompareText(FileName, 'msime-write-probe.tmp') = 0) or
    (CompareText(FileName, 'logs') = 0);
end;

{ 数据目录不整个归我们时，只有这些名字允许删（#537）。assets、html、logs 这种通用目录名
  由 DeleteKnownAppDataDir 只删输入法自己写进去的部分。log\、webview2\ 等只会出现在
  历史默认位置，那里整个目录归我们，不必列。}
{ 旧版随包装进数据目录根下、现已不再分发的条目。留在名单里，覆盖安装与卸载才会继续清掉它们。
  神经整句效果档模型改为设置页按需下载到 models\neural-desktop\（归 IsCollocationModelDirectory 保留）。
  html\webview2\ 是旧版 WebView2 界面的页面，界面改由 Direct2D 绘制后不再分发；设置窗口
  用到的图片改装在 assets\settings\。词格语言模型 sc.lm 及其授权声明不再随包发布（约 24 MB，
  几乎压不动），改由设置页按需下载到 models\language-model\（随 models 整目录升级保留）。}
function IsRetiredAppDataItem(const FileName: String): Boolean;
begin
  Result :=
    (CompareText(FileName, 'sentence-model-desktop.safetensors') = 0) or
    (CompareText(FileName, 'sc.lm') = 0) or
    (CompareText(FileName, 'libime-lm-NOTICE.md') = 0) or
    (CompareText(FileName, 'html') = 0);
end;

function IsKnownAppDataItem(const FileName: String): Boolean;
begin
  Result := IsShippedAppDataItem(FileName) or IsRuntimeAppDataItem(FileName) or IsRetiredAppDataItem(FileName);
end;

{ 旧版把模型随包装在数据目录根下，现在改由设置页按需下载，Server 只认 models\<ModelDir>\。
  根下那份和设置页下载的是同一个发布文件，在清理把它当退役文件删掉之前挪过去，升级用户就不用重下
  （下载走 GitHub，不少用户连不上）。校验和对不上、或新位置已有文件时什么都不做，留给清理删。
  NoticeName 是旧版随包的授权声明，挪成新位置的 NOTICE.md，和设置页下载落位的布局一致。}
procedure AdoptRetiredModel(const FileName, ModelDir, NoticeName, ExpectedSha256: String);
var
  OldPath, NewDir, NewPath, OldNotice, NewNotice: String;
begin
  OldPath := AddBackslash(GetDataDir('')) + FileName;
  NewDir := AddBackslash(GetDataDir('')) + 'models\' + ModelDir;
  NewPath := AddBackslash(NewDir) + FileName;
  if (not FileExists(OldPath)) or FileExists(NewPath) then
    exit;
  try
    if CompareText(GetSHA256OfFile(OldPath), ExpectedSha256) <> 0 then
    begin
      Log('Retired ' + FileName + ' does not match its release; leaving it to cleanup.');
      exit;
    end;
  except
    Log('Cannot hash retired ' + FileName + ': ' + GetExceptionMessage);
    exit;
  end;
  if not (ForceDirectories(NewDir) and RenameFile(OldPath, NewPath)) then
  begin
    Log('Could not move retired ' + FileName + ' to ' + NewPath);
    exit;
  end;
  Log('Moved retired ' + FileName + ' to ' + NewPath);
  if NoticeName = '' then
    exit;
  OldNotice := AddBackslash(GetDataDir('')) + NoticeName;
  NewNotice := AddBackslash(NewDir) + 'NOTICE.md';
  if FileExists(OldNotice) and (not FileExists(NewNotice)) then
    RenameFile(OldNotice, NewNotice);
end;

function InitializeUninstall(): Boolean;
begin
  RegQueryStringValue(
    HKLM,
    'Software\Metasequoia\MetasequoiaIME',
    'VersionDir',
    VersionDirName
  );
  { uninsdeletevalue 会在卸载过程中删掉 DataDir，所以要先读出来缓存住。}
  ResolvePreviousDataDir;
  Result := True;
end;

procedure StopProcess(const ImageName: String);
var
  ResultCode: Integer;
begin
  Exec(
    ExpandConstant('{sys}\taskkill.exe'),
    '/F /T /IM "' + ImageName + '"',
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode
  );
end;

procedure StopSettingsProcess;
var
  SettingsWindow: HWND;
  WaitedMs: Integer;
begin
  // 设置窗口关闭后可能只是隐藏，进程按 appearance.settings_window_linger 继续驻留。
  // 覆盖安装要删掉整个 server 目录，所以这里必须把它请走：先投 kQuitSettings
  // 让它自己收尾（Server 已被强杀，没人替我们发这条消息了），再强杀兜底。
  SettingsWindow := FindWindowByClassName('{#MySettingsWindowClass}');
  if SettingsWindow <> 0 then
  begin
    PostMessage(SettingsWindow, {#MySettingsQuitMessage}, 0, 0);
    WaitedMs := 0;
    while (WaitedMs < 3000) and
          (FindWindowByClassName('{#MySettingsWindowClass}') <> 0) do
    begin
      Sleep(100);
      WaitedMs := WaitedMs + 100;
    end;
  end;
  StopProcess('{#MySettingsExeName}');
end;

procedure StopImeProcesses;
begin
  { Watchdog 先停，否则它会在我们删文件期间把 Server 拉起来。}
  StopProcess('{#MyWatchdogName}');
  StopProcess('{#MyAppExeName}');
  StopSettingsProcess;
  { 面板通常是 Server 的子进程、随 /T 一起走；Server 若已崩溃它们会变成孤儿，
    同样占着 server 目录里的 exe，所以显式再收一遍。}
  StopProcess('{#MyEmojiPanelExeName}');
  StopProcess('{#MyKeyboardPanelExeName}');
  StopProcess('{#MyHandwritingPanelExeName}');
end;

procedure DeleteWatchdogLogonTask;
var
  ResultCode: Integer;
begin
  { /F makes this idempotent when upgrading from a build without the task. }
  Exec(
    ExpandConstant('{sys}\schtasks.exe'),
    '/Delete /F /TN "{#MyWatchdogTaskName}"',
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode
  );
end;

procedure EnsureImeUserDataDir;
var
  AppDataPath: String;
  ResultCode: Integer;
begin
  // Elevated setup writes {localappdata} as high integrity. Medium-IL Server
  // and Settings cannot replace those files. Users who never rewrote config.toml
  // at the real path (the non-ASCII path bug) keep that leftover and cannot save.
  // Note: brace comments do not nest in Inno Setup, so a constant like the one
  // above would close a { } comment early -- keep these as line comments.
  // Also required when the data directory sits on another volume: files the elevated setup
  // copied there inherit the parent's ACL, which may not let a Medium-IL Server write them.
  AppDataPath := GetDataDir('');
  ForceDirectories(AppDataPath);
  Exec(
    ExpandConstant('{sys}\icacls.exe'),
    '"' + AppDataPath + '" /grant *S-1-5-32-545:(OI)(CI)M /T /C /Q',
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode
  );
  Exec(
    ExpandConstant('{sys}\icacls.exe'),
    '"' + AppDataPath + '" /setintegritylevel (OI)(CI)M /T /C /Q',
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode
  );
end;

procedure CreateWatchdogLogonTask;
var
  WatchdogPath: String;
  Params: String;
  ResultCode: Integer;
begin
  ResultCode := -1;
  WatchdogPath := ExpandConstant(
    '{commonpf64}\metasequoiaime\server\{#MyWatchdogName}');
  { /F replaces the same fixed-name task during an upgrade. /IT keeps the
    task in the interactive user's session; LIMITED avoids an elevated token. }
  Params :=
    '/Create /F /TN "{#MyWatchdogTaskName}" /SC ONLOGON ' +
    '/RL LIMITED /IT /TR "\"' + WatchdogPath + '\""';
  if
    (not Exec(
      ExpandConstant('{sys}\schtasks.exe'),
      Params,
      '',
      SW_HIDE,
      ewWaitUntilTerminated,
      ResultCode
    )) or
    (ResultCode <> 0)
  then
    RaiseException(
      '无法创建输入法登录启动任务（退出码：' +
      IntToStr(ResultCode) + '）。');
end;

{ 登录任务的 Priority 默认是 7（Below normal），watchdog 和它拉起的 Server 会一起被降级；
  任务还会默认在电池上不启动/被停止，并在 72 小时后强制结束。schtasks /Create 没有这些参数，
  这里用 Set-ScheduledTask 补齐并读回校验。配置失败必须让安装明确失败，不能静默留下旧设置。}
procedure ConfigureWatchdogLogonTask;
var
  Script: String;
  Params: String;
  ResultCode: Integer;
begin
  ResultCode := -1;
  Script :=
    '$ErrorActionPreference=''Stop''; ' +
    '$n=''{#MyWatchdogTaskName}''; ' +
    '$s=(Get-ScheduledTask -TaskName $n).Settings; ' +
    '$s.Priority=4; ' +
    '$s.DisallowStartIfOnBatteries=$false; ' +
    '$s.StopIfGoingOnBatteries=$false; ' +
    '$s.ExecutionTimeLimit=''PT0S''; ' +
    'Set-ScheduledTask -TaskName $n -Settings $s | Out-Null; ' +
    '$c=(Get-ScheduledTask -TaskName $n).Settings; ' +
    'if ($c.Priority -ne 4 -or $c.DisallowStartIfOnBatteries -or ' +
    '$c.StopIfGoingOnBatteries -or $c.ExecutionTimeLimit -ne ''PT0S'') ' +
    '{ exit 1 }';
  Params := '-NoProfile -NonInteractive -Command "' + Script + '"';
  if
    (not Exec(
      ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
      Params,
      '',
      SW_HIDE,
      ewWaitUntilTerminated,
      ResultCode
    )) or
    (ResultCode <> 0)
  then
    RaiseException(
      '无法配置输入法登录任务（退出码：' +
      IntToStr(ResultCode) + '）。');
end;

procedure TryDeleteTree(const Path: String);
begin
  { 忽略返回值：被占用文件保留，其他能删除的文件仍继续清理。}
  DelTree(Path, True, True, True);
end;

{ 诊断日志每个会话一个文件，文件名前缀 水杉IME诊断日志- 是安装器与写入方唯一的契约，
  见 server/src/log/diagnostic_log_naming.h。这里只删带前缀的文件，logs 目录里的
  dictionary-build.log 和用户自己放进去的东西都留下。固定名是旧版留下的，升级时一并清掉。}
procedure DeleteDiagnosticLogs(const Directory: String);
var
  FindRec: TFindRec;
begin
  DeleteFile(AddBackslash(Directory) + '水杉IME诊断日志.log');
  DeleteFile(AddBackslash(Directory) + '水杉IME诊断日志.log.1');
  if FindFirst(AddBackslash(Directory) + '水杉IME诊断日志-*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0 then
          DeleteFile(AddBackslash(Directory) + FindRec.Name);
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

{ 不独占的数据目录里删一个名单上的子目录。名字通用的几个只删我们写进去的内容，
  目录空了才删掉它：用户自己恰好也有一个 assets、html、logs 或 skins 文件夹时不能跟着没了。
  skins 里分不清哪些是外部皮肤、哪些是别人的东西，宁可留下。}
procedure DeleteKnownAppDataDir(const ItemPath, FileName: String);
begin
  if CompareText(FileName, 'skins') = 0 then
  begin
    { 只删空目录。}
  end
  else if CompareText(FileName, 'assets') = 0 then
    TryDeleteTree(AddBackslash(ItemPath) + 'settings')
  else if CompareText(FileName, 'html') = 0 then
    { 旧版的 WebView2 页面，见 IsRetiredAppDataItem。}
    TryDeleteTree(AddBackslash(ItemPath) + 'webview2')
  else if CompareText(FileName, 'logs') = 0 then
    DeleteDiagnosticLogs(ItemPath)
  else
  begin
    TryDeleteTree(ItemPath);
    exit;
  end;
  RemoveDir(ItemPath);
end;

{ 卸载、换数据目录时删掉旧数据目录。整个目录归我们才整目录删；只有旧版标记的目录里
  可能混着用户自己的文件（#537），只删名单里输入法的东西，目录空了才顺手删掉。
  两样都不是就一个文件也不碰。}
procedure RemoveDataDir(const Directory: String);
var
  FindRec: TFindRec;
  ItemPath: String;
begin
  if (Directory = '') or (not DirExists(Directory)) then
    exit;
  if OwnsDataDirExclusively(Directory) then
  begin
    TryDeleteTree(Directory);
    exit;
  end;
  if not DataDirHasMarker(Directory) then
    exit;

  Log('Data directory ' + Directory + ' is not exclusively ours; removing known items only.');
  if FindFirst(AddBackslash(Directory) + '*', FindRec) then
  begin
    try
      repeat
        if
          (FindRec.Name <> '.') and
          (FindRec.Name <> '..') and
          (IsPreservedAppDataItem(FindRec.Name) or IsKnownAppDataItem(FindRec.Name))
        then
        begin
          ItemPath := AddBackslash(Directory) + FindRec.Name;
          if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
            DeleteKnownAppDataDir(ItemPath, FindRec.Name)
          else
            DeleteFile(ItemPath);
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
  { 只删得掉空目录；用户的文件还在，目录就留着。}
  RemoveDir(Directory);
end;

{ 旧版 WebView2 的用户数据（浏览器缓存、Cookie 等）。界面改由 Direct2D 绘制后不再有人
  读写它们，覆盖安装和卸载都清掉。正常情况下在 ProgramData 下；那里不可写时旧版 Server
  退到历史默认数据目录下的同名子目录，那个目录整个归我们，按名字删是安全的。}
procedure RemoveLegacyWebView2UserData;
begin
  TryDeleteTree(ExpandConstant('{commonappdata}\metasequoiaime\webview2'));
  TryDeleteTree(ExpandConstant('{commonappdata}\metasequoiaime\webview2-settings'));
  RemoveDir(ExpandConstant('{commonappdata}\metasequoiaime'));
  TryDeleteTree(ExpandConstant('{localappdata}\metasequoiaime\webview2'));
  TryDeleteTree(ExpandConstant('{localappdata}\metasequoiaime\webview2-settings'));
end;

#ifdef LightPackage
{ 轻量包只替换数据目录里的个别子目录。整个目录归我们时整个删；只有旧版标记时（#537）
  按 DeleteKnownAppDataDir 的规则只删输入法自己写进去的部分。}
procedure CleanLightPackageItem(const FileName: String);
var
  AppDataPath: String;
begin
  AppDataPath := GetDataDir('');
  if OwnsDataDirExclusively(AppDataPath) then
    TryDeleteTree(AddBackslash(AppDataPath) + FileName)
  else if DataDirHasMarker(AppDataPath) then
    DeleteKnownAppDataDir(AddBackslash(AppDataPath) + FileName, FileName);
end;
#endif

{ helpcodes 里的内置表由本次安装重写，custom 子目录是用户自己放的辅助码，升级不得清掉。}
procedure CleanHelpcodesExceptCustom(const HelpcodesPath: String);
var
  FindRec: TFindRec;
  ItemPath: String;
begin
  if FindFirst(AddBackslash(HelpcodesPath) + '*', FindRec) then
  begin
    try
      repeat
        if
          (FindRec.Name <> '.') and
          (FindRec.Name <> '..') and
          (CompareText(FindRec.Name, 'custom') <> 0)
        then
        begin
          ItemPath := AddBackslash(HelpcodesPath) + FindRec.Name;
          if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
            TryDeleteTree(ItemPath)
          else
            DeleteFile(ItemPath);
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

procedure CleanAppDataExceptUserFiles;
var
  Exclusive: Boolean;
  AppDataPath: String;
  FindRec: TFindRec;
  ItemPath: String;
begin
  AppDataPath := GetDataDir('');
  if not DirExists(AppDataPath) then
    exit;
  { 整个目录归我们时清掉除用户数据以外的一切；旧版安装器可能把标记写进了一个本来就有
    别的文件的文件夹（#537），那里只动名单里输入法自己的东西。}
  Exclusive := OwnsDataDirExclusively(AppDataPath);
  if (not Exclusive) and (not DataDirHasMarker(AppDataPath)) then
    exit;

  if FindFirst(AddBackslash(AppDataPath) + '*', FindRec) then
  begin
    try
      repeat
        if
          (FindRec.Name <> '.') and
          (FindRec.Name <> '..') and
          (not IsPreservedAppDataItem(FindRec.Name)) and
          (Exclusive or IsKnownAppDataItem(FindRec.Name))
        then
        begin
          ItemPath := AddBackslash(AppDataPath) + FindRec.Name;
          if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0 then
            DeleteFile(ItemPath)
          else if CompareText(FindRec.Name, 'helpcodes') = 0 then
            CleanHelpcodesExceptCustom(ItemPath)
          else if Exclusive then
            TryDeleteTree(ItemPath)
          else
            DeleteKnownAppDataDir(ItemPath, FindRec.Name);
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

function RemoveFileWithRetry(const Path: String): Boolean;
var
  Attempt: Integer;
begin
  for Attempt := 1 to 5 do
  begin
    if not FileExists(Path) then
    begin
      Result := True;
      exit;
    end;
    DeleteFile(Path);
    if not FileExists(Path) then
    begin
      Result := True;
      exit;
    end;
    Sleep(200);
  end;
  Result := not FileExists(Path);
end;

function RemoveOldTargetDatabaseFiles(var FailedPath: String): Boolean;
var
  AppDataPath: String;
  FileNames: array[0..11] of String;
  Index: Integer;
  Path: String;
begin
  AppDataPath := GetDataDir('');
  { 先删 sidecar；若仍被占用，可在动主库和其他应用数据前安全中止。}
  FileNames[0] := 'msime.db-wal';
  FileNames[1] := 'msime.db-shm';
  FileNames[2] := 'msime.db-journal';
  FileNames[3] := 'english.db-wal';
  FileNames[4] := 'english.db-shm';
  FileNames[5] := 'english.db-journal';
  FileNames[6] := 'others.db-wal';
  FileNames[7] := 'others.db-shm';
  FileNames[8] := 'others.db-journal';
  FileNames[9] := 'msime.db';
  FileNames[10] := 'english.db';
  FileNames[11] := 'others.db';

  for Index := 0 to 11 do
  begin
    Path := AddBackslash(AppDataPath) + FileNames[Index];
    if not RemoveFileWithRetry(Path) then
    begin
      FailedPath := Path;
      Result := False;
      exit;
    end;
  end;
  Result := True;
end;

{ 把临时目录下 dictionary_source 的文本源还原成数据目录里的 msime.db、english.db、others.db、dict_japanese.dat。
  旧词库已在 PrepareToInstall 里删掉，这一步失败新版本就没有词库可用。
  还原程序先全部建成 .building，整套成功才替换，失败不留半套文件；明细追加在数据目录的 logs\dictionary-build.log。
  注意 ssPostInstall 里抛异常并不会中止安装：Inno 只弹「Runtime error」，安装照样算成功，CurStepChanged
  后面的步骤（回放、登录任务、目录权限）全被跳过。所以失败先让用户就地重试，放弃了才抛。}
procedure BuildDictionaries;
var
  BuildPath: String;
  DataPath: String;
  LogPath: String;
  ResultCode: Integer;
  Started: Boolean;
begin
  DataPath := GetDataDir('');
  LogPath := AddBackslash(DataPath) + 'logs\dictionary-build.log';
  BuildPath := ExpandConstant(
    '{commonpf64}\metasequoiaime\server\{#MyDictionaryBuildName}');
  repeat
    { PrepareToInstall 已经占住启动锁，正常不会有 Server；仍在跑的（比如锁生效前就被拉起的）会占着
      数据目录里的词库，替换必然失败，先请走。}
    StopProcess('{#MyWatchdogName}');
    StopProcess('{#MyAppExeName}');
    Log('Starting dictionary build.');
    WizardForm.StatusLabel.Caption := '正在生成词库，可能需要十几秒到一分钟……';
    WizardForm.FilenameLabel.Caption := '';
    WizardForm.ProgressGauge.Style := npbstMarquee;
    try
      Started := Exec(
        BuildPath,
        '--source "' + ExpandConstant('{tmp}\dictionary_source') + '" ' +
        '--data-dir "' + DataPath + '" --log "' + LogPath + '"',
        '',
        SW_HIDE,
        ewWaitUntilTerminated,
        ResultCode
      );
    finally
      WizardForm.ProgressGauge.Style := npbstNormal;
    end;
    if not Started then
      RaiseException(
        '无法启动词库生成程序。请确认安装文件完整后重试。');
    if ResultCode = 0 then
    begin
      Log('Dictionary build completed successfully.');
      exit;
    end;
    Log('Dictionary build failed with exit code ' + IntToStr(ResultCode) + '.');
  { 静默安装没人点按钮，默认取消，不能无限重试。}
  until SuppressibleMsgBox(
    '生成词库失败（退出码：' + IntToStr(ResultCode) + '）。' + #13#10 + #13#10 +
    '原因写在 ' + LogPath + ' 的最后一行。常见的是数据目录里的词库文件正被其他程序占用，' +
    '或磁盘空间不足（约需 350 MB）。' + #13#10 + #13#10 +
    '点「重试」重新生成；点「取消」放弃，输入法将没有词库可用。',
    mbError, MB_RETRYCANCEL, IDCANCEL) <> IDRETRY;
  RaiseException(
    '生成词库失败（退出码：' + IntToStr(ResultCode) + '），详情见 ' + LogPath + '。' + #13#10 +
    '请重新运行安装程序。');
end;

procedure ReplayUserDictionary;
var
  ReplayPath: String;
  DataPath: String;
  ResultCode: Integer;
begin
  DataPath := GetDataDir('');
  if not FileExists(AddBackslash(DataPath) + 'msime_user.db') then
  begin
    Log('User dictionary replay skipped: msime_user.db does not exist.');
    exit;
  end;

  ReplayPath := ExpandConstant(
    '{commonpf64}\metasequoiaime\server\{#MyReplayName}');
  Log('Starting user dictionary replay.');
  if not Exec(
    ReplayPath,
    '--data-dir "' + DataPath + '"',
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode
  ) then
    RaiseException(
      '无法启动用户词库回放程序。请确认安装文件完整后重试。');

  if ResultCode <> 0 then
    RaiseException(
      '用户词库回放失败（退出码：' + IntToStr(ResultCode) +
      '）。安装已停止，以避免启动未恢复用户词库的新版本。');
  Log('User dictionary replay completed successfully.');
end;

procedure TryDeleteOldVersionDirs(const RootPath: String);
var
  FindRec: TFindRec;
begin
  if FindFirst(
    AddBackslash(RootPath) + 'msime_v*',
    FindRec
  ) then
  begin
    try
      repeat
        if
          ((FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0) and
          (FindRec.Name <> '.') and
          (FindRec.Name <> '..') and
          (CompareText(FindRec.Name, VersionDirName) <> 0)
        then
          TryDeleteTree(AddBackslash(RootPath) + FindRec.Name);
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

{ 用户改了数据目录：把上一处的用户数据搬过来。只搬真正属于用户、装不回来的东西——
  词库主体、界面资源和辅助码都会由本次安装重新写入新目录。
  用 robocopy 而不是 RenameFile：跨盘移动目录时 MoveFile 会直接失败。}
function MigrateUserDataDir(const OldDir, NewDir: String): String;
var
  ResultCode: Integer;
  Moved: Boolean;
begin
  Result := '';
  if (OldDir = '') or (CompareText(OldDir, NewDir) = 0) or (not DirExists(OldDir)) then
    exit;

  Result := ConfigMigrationProblem(OldDir, NewDir);
  if Result <> '' then
    exit;

  Log('Migrating user data from ' + OldDir + ' to ' + NewDir);
  ForceDirectories(NewDir);
  DropMigratedConfigCopy(OldDir, NewDir);

  Moved := Exec(
    ExpandConstant('{sys}\robocopy.exe'),
    '"' + RemoveBackslashUnlessRoot(OldDir) + '" "' + RemoveBackslashUnlessRoot(NewDir) + '" ' +
    'msime_user.db msime_user.db-wal msime_user.db-shm msime_user.db-journal ' +
    'stats.db stats.db-wal stats.db-shm stats.db-journal ' +
    'config.toml config.base.toml custom_translations.txt /MOVE /R:2 /W:1 /NJH /NJS /NP /NFL /NDL',
    '',
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode
  ) and (ResultCode < 8);

  { 外部皮肤是用户自己放进来的，同样搬走。}
  if DirExists(AddBackslash(OldDir) + 'skins') then
    Moved :=
      Exec(
        ExpandConstant('{sys}\robocopy.exe'),
        '"' + AddBackslash(OldDir) + 'skins" "' + AddBackslash(NewDir) + 'skins" ' +
        '/E /MOVE /R:2 /W:1 /NJH /NJS /NP /NFL /NDL',
        '',
        SW_HIDE,
        ewWaitUntilTerminated,
        ResultCode
      ) and (ResultCode < 8) and Moved;

  { 自定义辅助码也是用户自己放进来的。}
  if DirExists(AddBackslash(OldDir) + 'helpcodes\custom') then
    Moved :=
      Exec(
        ExpandConstant('{sys}\robocopy.exe'),
        '"' + AddBackslash(OldDir) + 'helpcodes\custom" "' + AddBackslash(NewDir) + 'helpcodes\custom" ' +
        '/E /MOVE /R:2 /W:1 /NJH /NJS /NP /NFL /NDL',
        '',
        SW_HIDE,
        ewWaitUntilTerminated,
        ResultCode
      ) and (ResultCode < 8) and Moved;

  { 自定义双拼方案同理。}
  if DirExists(AddBackslash(OldDir) + 'shuangpin') then
    Moved :=
      Exec(
        ExpandConstant('{sys}\robocopy.exe'),
        '"' + AddBackslash(OldDir) + 'shuangpin" "' + AddBackslash(NewDir) + 'shuangpin" ' +
        '/E /MOVE /R:2 /W:1 /NJH /NJS /NP /NFL /NDL',
        '',
        SW_HIDE,
        ewWaitUntilTerminated,
        ResultCode
      ) and (ResultCode < 8) and Moved;

  { 下载的 octagram 语法模型包也搬走，否则留在旧目录里随 RemoveDataDir 一起删掉，用户得重下。}
  if DirExists(AddBackslash(OldDir) + 'models') then
    Moved :=
      Exec(
        ExpandConstant('{sys}\robocopy.exe'),
        '"' + AddBackslash(OldDir) + 'models" "' + AddBackslash(NewDir) + 'models" ' +
        '/E /MOVE /R:2 /W:1 /NJH /NJS /NP /NFL /NDL',
        '',
        SW_HIDE,
        ewWaitUntilTerminated,
        ResultCode
      ) and (ResultCode < 8) and Moved;

  if not Moved then
  begin
    Log('User data migration reported failures; leaving ' + OldDir + ' in place.');
    { 用户词库没搬成必须让安装失败。否则新版本会对着一个空的 msime_user.db 启动，
      ssPostInstall 的回放只会记一句 skipped，用户的自造词和词频就这么静悄悄没了——
      这正是 ReplayUserDictionary 失败时也要中止安装的那条理由。
      config.toml 同理，见 ConfigMigrationFailure；custom_translations.txt 没有出厂副本，
      见 TranslationMigrationFailure。皮肤等搬不动只记日志：它们丢了可以重建。}
    if FileExists(AddBackslash(OldDir) + 'msime_user.db') then
      Result :=
        '无法把用户词库从 ' + OldDir + ' 移动到 ' + NewDir + '。' + #13#10 +
        '请确认输入法相关进程已全部退出、目标磁盘可写且空间足够，然后重试安装。' + #13#10 +
        '你的用户词库仍留在 ' + OldDir + '，本次安装没有改动它。'
    else
    begin
      Result := ConfigMigrationFailure(OldDir, NewDir);
      if Result = '' then
        Result := TranslationMigrationFailure(OldDir, NewDir);
    end;
    exit;
  end;

  { 旧目录里剩下的是可重建的资源（词库、界面资源、cache 等）。}
  RemoveDataDir(OldDir);
end;

function CountFilesUnder(const Directory: String): Integer;
var
  FindRec: TFindRec;
begin
  Result := 0;
  if FindFirst(AddBackslash(Directory) + '*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
            Result := Result + CountFilesUnder(AddBackslash(Directory) + FindRec.Name)
          else
            Result := Result + 1;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

{ 数据目录里的同名皮肤与随包的文件集合、内容完全一致时返回 True。
  清单每行「id|相对路径|sha256」，由 Prepare-PackageFiles.ps1 按 id 分组写出。}
function SkinMatchesBundle(const SkinDir, Id: String; const Lines: TArrayOfString): Boolean;
var
  Index, P, Expected: Integer;
  Rest, FilePath: String;
begin
  Result := False;
  Expected := 0;
  for Index := 0 to GetArrayLength(Lines) - 1 do
  begin
    P := Pos('|', Lines[Index]);
    if (P = 0) or (Copy(Lines[Index], 1, P - 1) <> Id) then
      continue;
    Rest := Copy(Lines[Index], P + 1, Length(Lines[Index]));
    P := Pos('|', Rest);
    if P = 0 then
      exit;
    Expected := Expected + 1;
    FilePath := AddBackslash(SkinDir) + Copy(Rest, 1, P - 1);
    if not FileExists(FilePath) then
      exit;
    try
      if CompareText(GetSHA256OfFile(FilePath), Copy(Rest, P + 1, Length(Rest))) <> 0 then
        exit;
    except
      exit;
    end;
  end;
  { 多出来的文件也算改过：整目录覆盖不会删它们，留着就和随包的不是一个皮肤了。}
  Result := CountFilesUnder(SkinDir) = Expected;
end;

{ 覆盖随包皮肤之前，把用户机器上内容不同的同名目录改名成 <id>.bak；
  已经有 .bak 就用 <id>.2.bak、<id>.3.bak……，不覆盖上一次的备份——那可能正是用户改过的版本。
  内容一样的不备份，否则每次升级都会多一份一模一样的 .bak。Server 扫描皮肤时跳过 .bak 目录。}
function BackupChangedBundledSkins: String;
var
  Lines: TArrayOfString;
  Index, P, N: Integer;
  Id, LastId, SkinsDir, SkinDir, BackupDir: String;
begin
  Result := '';
  ExtractTemporaryFile('bundled_skins.manifest');
  if not LoadStringsFromFile(ExpandConstant('{tmp}\bundled_skins.manifest'), Lines) then
  begin
    Result := '无法读取随包皮肤清单，安装文件可能不完整。';
    exit;
  end;
  SkinsDir := AddBackslash(GetDataDir('')) + 'skins';
  LastId := '';
  for Index := 0 to GetArrayLength(Lines) - 1 do
  begin
    P := Pos('|', Lines[Index]);
    if P = 0 then
      continue;
    Id := Copy(Lines[Index], 1, P - 1);
    if Id = LastId then
      continue;
    LastId := Id;
    SkinDir := AddBackslash(SkinsDir) + Id;
    if not DirExists(SkinDir) then
      continue;
    if SkinMatchesBundle(SkinDir, Id, Lines) then
    begin
      Log('Bundled skin ' + Id + ' is unchanged; no backup needed.');
      continue;
    end;
    BackupDir := SkinDir + '.bak';
    N := 2;
    while DirExists(BackupDir) or FileExists(BackupDir) do
    begin
      BackupDir := AddBackslash(SkinsDir) + Id + '.' + IntToStr(N) + '.bak';
      N := N + 1;
    end;
    if not RenameFile(SkinDir, BackupDir) then
    begin
      Result :=
        '无法备份皮肤目录 ' + SkinDir + '。' + #13#10 +
        '请关闭正在打开该目录或其中图片的程序后重试安装。';
      exit;
    end;
    Log('Backed up skin ' + SkinDir + ' to ' + BackupDir);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  DataDirProblem: String;
#ifndef LightPackage
  FailedPath: String;
#endif
begin
  { 静默安装不会走向导页，/DATADIR= 传进来的值在这里才第一次被检查，
    有别的文件时同样改用子目录。向导里选的值已经换过一次，这里再换是空操作。}
  DataDirValue := AdoptableDataDir(GetDataDir(''));
  DataDirProblem := DataDirRejectionReason(GetDataDir(''));
  if DataDirProblem <> '' then
  begin
    Result := '数据目录 ' + GetDataDir('') + ' 不可用：' + DataDirProblem;
    exit;
  end;
#ifndef LightPackage
  { 静默安装也必须在最终目录确定后、配置迁移前检查。}
  UserConfigExistedBeforeInstall := UserConfigExistsBeforeInstall;
#endif
  { 迁移会往目录里搬文件，所以要在那之前记下它是不是空着被接手的。}
  DataDirAdoptedEmpty :=
    OwnsDataDirExclusively(GetDataDir('')) or DirectoryIsEmpty(GetDataDir(''));

  DataDirProblem := ConfigMigrationProblem(ResolvePreviousDataDir, GetDataDir(''));
  if DataDirProblem <> '' then
  begin
    Result := DataDirProblem;
    exit;
  end;

  { 先锁定本次目录名，再清理能够释放的旧版本 DLL。}
  VersionDirName := GetVersionDir('');
  { 占住 TSF DLL 拉起 Server 用的那把锁，直到安装程序退出：[Files] 一注册 DLL，输入法就可能在某个
    宿主里被激活并拉起 Server，而那时词库还没生成——引擎打开 msime.db 会先建一个空库并一直占着，
    BuildDictionaries 就替换不了它（拒绝访问，新机器首次安装必现）。DLL 拿不到这把锁时不启动 Server；
    完成页的 LaunchInstalledComponents 直接启动，不经过这把锁。}
  CreateMutex('{#MyServerLaunchMutex}');
  StopImeProcesses;
  { 进程停了才能动数据文件：msime_user.db 会被 Server 打开着。}
  DataDirProblem := MigrateUserDataDir(ResolvePreviousDataDir, GetDataDir(''));
  if DataDirProblem <> '' then
  begin
    Result := DataDirProblem;
    exit;
  end;
  { 迁移之后再备份：换了数据目录时，要比对的是已经搬到新目录的皮肤。}
  DataDirProblem := BackupChangedBundledSkins;
  if DataDirProblem <> '' then
  begin
    Result := DataDirProblem;
    exit;
  end;
  { 轻量包不清根目录，但新 Server 同样只认 models\ 下的位置，所以两种包都要挪。
    哈希分别对应 neural-model/lock.json 和 language-model/lock.json，installer/tests/retired-models.ps1 核对。}
  AdoptRetiredModel('sentence-model-desktop.safetensors', 'neural-desktop', '',
    '0a6ecba69bf1d39c7eb49549c716477dd6c03fdf05757773e1f50435262fb469');
  AdoptRetiredModel('sc.lm', 'language-model', 'libime-lm-NOTICE.md',
    '3588b3942c8fd62e1a6bd3bae8c7cadc0faf928c175b245224ed475862218387');
#ifdef LightPackage
  { 轻量包不替换词库：只清设置窗口图片资源和 Server/TSF，保留本机 msime.db 等。
    旧版留下的 html（WebView2 页面）也一并清掉。}
  CleanLightPackageItem('assets');
  CleanLightPackageItem('html');
#else
  { 不能让旧 WAL/SHM 与即将复制的新主数据库混用。}
  if not RemoveOldTargetDatabaseFiles(FailedPath) then
  begin
    Result :=
      '无法删除旧词库文件：' + FailedPath + #13#10 +
      '它可能仍被输入法相关进程占用。请关闭相关程序后重试安装。';
    exit;
  end;
  { 目标词库已安全移除，再清理旧的随包资源。用户配置和外部皮肤目录在这里保留。}
  CleanAppDataExceptUserFiles;
#endif
  RemoveLegacyWebView2UserData;
  TryDeleteTree(ExpandConstant(
    '{commonpf64}\metasequoiaime\server'));
  TryDeleteOldVersionDirs(TsfRoot32);
  TryDeleteOldVersionDirs(TsfRoot64);
  { 旧版把 TSF DLL 装在 Program Files 下，升级上来时一并清掉。}
  TryDeleteOldVersionDirs(ExpandConstant(
    '{commonpf32}\metasequoiaime'));
  TryDeleteOldVersionDirs(ExpandConstant(
    '{commonpf64}\metasequoiaime'));
  DeleteFile(ExpandConstant('{commonpf32}\metasequoiaime\MetasequoiaImeTip.ico'));
  DeleteFile(ExpandConstant('{commonpf64}\metasequoiaime\MetasequoiaImeTip.ico'));
  RemoveDir(ExpandConstant('{commonpf32}\metasequoiaime'));
  { 随后的 [Files] 与 ssPostInstall 会写入新 Server 和登录任务。}
  Result := '';
end;

{ TSF 的语言配置要以原用户身份装进键盘列表：提权进程的 HKCU 指向管理员账户，写进去当前用户看不到
  （见 server/src/settings/input_method_install.h）。放在最后，失败也不影响装好的文件。}
procedure RegisterProfileForUser;
var
  ErrorCode: Integer;
  Executable: String;
begin
  Executable := ExpandConstant(
    '{commonpf64}\metasequoiaime\server\MetasequoiaImeSettings.exe');
  if not FileExists(Executable) then
  begin
    Log('settings program missing, skipping keyboard list registration');
    Exit;
  end;
  if not ShellExecAsOriginalUser('', Executable, '--register-profile', '', SW_HIDE,
    ewWaitUntilTerminated, ErrorCode) then
    Log('could not start the settings program as the original user: ' +
      IntToStr(ErrorCode))
  else
    Log('keyboard list registration exit code: ' + IntToStr(ErrorCode));
end;

procedure CurStepChanged(CurStep: TSetupStep);begin
  if CurStep = ssPostInstall then
  begin
    { 先打标记，再调整权限：之后的覆盖安装和卸载靠它判断这个目录是不是我们建的。}
    WriteDataDirMarker(GetDataDir(''), DataDirAdoptedEmpty);
#ifndef LightPackage
    { 回放要往新词库里写，所以先还原词库。}
    BuildDictionaries;
    ReplayUserDictionary;
#endif
    CreateWatchdogLogonTask;
    ConfigureWatchdogLogonTask;
    EnsureImeUserDataDir;
    { Keep the old autostart intact until its scheduled-task replacement has
      been created successfully, then remove the Explorer-delayed Run entry. }
    RegDeleteValue(
      HKLM,
      'Software\Microsoft\Windows\CurrentVersion\Run',
      'MetasequoiaImeWatchdog'
    );
#ifndef LightPackage
    { 偏好写入放在最后：它失败只弹提示，不能挡住上面这些必需步骤。}
    ApplyNetworkChoiceToUserConfig;
#endif
    { TSF 的语言配置要以原用户身份装进键盘列表：提权进程的 HKCU 指向管理员账户，写进去当前用户
      看不到（见 server/src/settings/input_method_install.h）。放在最后，失败也不影响装好的文件。}
    RegisterProfileForUser;
  end;
end;

{ 旧版设置页的「设为默认」写的是「替代默认输入法」。卸载后它仍指向水杉，系统设置里
  就残留一个不存在的输入法；只清指向水杉的那一项，用户自己选的别的输入法不动。}
procedure ClearOurInputMethodOverride;
var
  OverrideId: String;
begin
  if RegQueryStringValue(HKCU, 'Control Panel\International\User Profile',
    'InputMethodOverride', OverrideId) and
    (CompareText(OverrideId,
      '0804:{E3062E9A-D834-4637-8958-ED8CFA427D01}{4D59B1B4-D503-44AE-9259-BAD9BB2778AB}') = 0) then
    RegDeleteValue(HKCU, 'Control Panel\International\User Profile',
      'InputMethodOverride');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    ClearOurInputMethodOverride;
    { 设置页「游戏兼容」记下的登记名称。升级时要留着（TSF DLL 注册时读它），卸载时不能留：
      否则日后重装会悄悄沿用别名。}
    RegDeleteValue(HKLM, 'Software\Metasequoia\MetasequoiaIME', 'GameCompatProfileName');
    DeleteWatchdogLogonTask;
    RegDeleteValue(
      HKLM,
      'Software\Microsoft\Windows\CurrentVersion\Run',
      'MetasequoiaImeWatchdog'
    );
    StopImeProcesses;
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    TryDeleteTree(ExpandConstant(
      '{commonpf64}\metasequoiaime\server'));
    { 还被宿主进程加载着的 DLL 删不掉，会留到重启后由用户或下次安装清理。}
    TryDeleteTree(TsfRoot32);
    TryDeleteTree(TsfRoot64);
    TryDeleteTree(ExpandConstant('{commonpf32}\metasequoiaime'));
    TryDeleteTree(ExpandConstant('{commonpf64}\metasequoiaime'));
    { 数据目录可能被用户指到了别的盘，甚至指到一个本来就有文件的文件夹（#537）。}
    RemoveDataDir(ResolvePreviousDataDir);
    { 旧版 WebView2 的用户数据，见 RemoveLegacyWebView2UserData。}
    RemoveLegacyWebView2UserData;
    { 数据目录在别处时，历史默认位置里可能只剩上面删掉的那两样；还有别的东西就删不掉，原样留着。}
    RemoveDir(ExpandConstant('{localappdata}\metasequoiaime'));
    TryDeleteTree(ExpandConstant('{commonappdata}\metasequoiaime'));
  end;
end;
