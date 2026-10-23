[CmdletBinding()]
param(
    # installer\Prepare-PackageFiles.ps1 的产物根目录（installer 目录本身）。
    [Parameter(Mandatory)][string]$StagingRoot,
    # 这次构建的版本号，写进目录名与说明文件。
    [Parameter(Mandatory)][string]$TargetVersion,
    # zip 输出目录，默认放在 staging 的上一级。
    [string]$OutputDirectory
)

# 免安装包：把 Prepare-PackageFiles.ps1 已经收集好的 server_exe / app_data 摊平成一个可以直接
# 双击运行的目录，再压成 zip。解决的是「只想看一眼设置窗口，不想装个完整输入法」这件事：
# 安装包会写 HKLM、装 TSF、需要管理员权限与证书信任，预览界面没一样用得上。
#
# 目录布局刻意与安装后的相对关系一致，这样程序内部的相对路径不用改：
#
#   MetasequoiaIME_Portable_v<版本>\
#     MetasequoiaImeSettings.exe     设置窗口本体
#     *.dll                          它链接的运行时（libcurl、sqlite3 等）
#     assets\settings\...            侧栏图标、皮肤预览图
#     skins\...                      皮肤
#     run-settings.cmd               双击即可运行，帮你设好数据目录
#     README.txt                     说明它是什么、不能做什么
#
# 数据目录用 run-settings.cmd 里的 METASEQUOIA_IME_DATA_DIR 指向包内的 data\，不碰注册表，
# 也不碰 %LOCALAPPDATA% 里已安装版本的配置。删掉整个目录就是卸载。

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-PathExists {
    param([Parameter(Mandatory)][string]$LiteralPath, [Parameter(Mandatory)][string]$Description)
    if (-not (Test-Path -LiteralPath $LiteralPath)) {
        throw "$Description 不存在：$LiteralPath"
    }
}

$staging = (Resolve-Path -LiteralPath $StagingRoot).Path
$serverExeDir = Join-Path $staging 'server_exe'
$appDataDir = Join-Path $staging 'app_data'
Assert-PathExists -LiteralPath $serverExeDir -Description 'server_exe（先跑 Prepare-PackageFiles.ps1）'
Assert-PathExists -LiteralPath $appDataDir -Description 'app_data（先跑 Prepare-PackageFiles.ps1）'

$settingsExe = Join-Path $serverExeDir 'MetasequoiaImeSettings.exe'
Assert-PathExists -LiteralPath $settingsExe -Description '设置窗口 MetasequoiaImeSettings.exe'
$settingsAssets = Join-Path $appDataDir 'assets\settings'
Assert-PathExists -LiteralPath $settingsAssets -Description '设置窗口图片资源 assets\settings'

$folderName = "MetasequoiaIME_Portable_v$TargetVersion"
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Split-Path -Parent $staging
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stageOut = Join-Path $OutputDirectory $folderName
if (Test-Path -LiteralPath $stageOut) {
    Remove-Item -LiteralPath $stageOut -Recurse -Force
}
New-Item -ItemType Directory -Path $stageOut -Force | Out-Null

# 1. 设置程序与它同目录的依赖。这里只挑 exe 同目录的文件：server_exe 里的 Server 本体、
#    候选窗、面板都不需要，带着只会让包大一倍，还给人「这是个完整输入法」的错觉。
Get-ChildItem -LiteralPath $serverExeDir -File | Where-Object {
    # 只保留设置程序和运行时依赖；其它可执行文件不是这次要预览的东西。
    $_.Extension -in @('.exe', '.dll') -and $_.Name -ne 'MetasequoiaImeServer.exe'
} | Copy-Item -Destination $stageOut -Force

# 2. 设置窗口要读的图片与皮肤。资源路径按「数据目录\assets\settings」解析
#    （native_settings_controls.cpp 的 AssetPath），所以必须落在 data\ 下面，不是 exe 旁边。
$dataOut = Join-Path $stageOut 'data'
New-Item -ItemType Directory -Path $dataOut -Force | Out-Null

$assetsOut = Join-Path $dataOut 'assets'
New-Item -ItemType Directory -Path $assetsOut -Force | Out-Null
Copy-Item -LiteralPath $settingsAssets -Destination $assetsOut -Recurse -Force

$skinsIn = Join-Path $appDataDir 'skins'
if (Test-Path -LiteralPath $skinsIn) {
    Copy-Item -LiteralPath $skinsIn -Destination $dataOut -Recurse -Force
}

# 3. 出厂配置模板：设置页读它来显示各默认值，与安装版的文件名保持一致。
$factoryConfig = Join-Path $appDataDir 'config.default.toml'
if (Test-Path -LiteralPath $factoryConfig) {
    Copy-Item -LiteralPath $factoryConfig -Destination $stageOut -Force
    # 首次运行用的 config.toml：安装版是「模板 → config.toml」，这里照做，不污染用户已有配置。
    Copy-Item -LiteralPath $factoryConfig -Destination (Join-Path $dataOut 'config.toml') -Force
}

# 4. 启动脚本。环境变量走进程级，不落盘、不影响已安装的版本。
#    .cmd 一律只写 ASCII：中文注释在 GBK 控制台下会变成乱码。
#    两个模式：
#      默认    数据目录指到包内 data\，出厂空白，改动不碰本机
#      live    用本机正式版的数据目录（注册表 DataDir 或 %LOCALAPPDATA%），改动会写进去
#    两种模式都加实例后缀，保证能和已安装的正式版同时开着。
$launcher = @'
@echo off
rem Portable preview of the settings window.
rem   run-settings.cmd        isolated: data lives in .\data, nothing touches this machine
rem   run-settings.cmd live   live: uses the installed copy's data directory and config
setlocal
set "METASEQUOIA_IME_SETTINGS_INSTANCE=portable"
rem Default the data directory to the copy inside this folder. Assign it outside the if-block
rem on purpose: cmd expands the whole block when it parses it, so a set inside is easy to get
rem wrong later.
set "METASEQUOIA_IME_DATA_DIR=%~dp0data"
if /I "%~1"=="live" (
    rem Must clear the inherited value: a developer shell usually sets this to a test profile,
    rem and leaving it would send live mode to that profile instead of the installed one.
    set "METASEQUOIA_IME_DATA_DIR="
    echo Using the installed data directory. Changes are written to it.
) else (
    echo Using the isolated data directory inside this folder.
)
start "" "%~dp0MetasequoiaImeSettings.exe"
endlocal
'@
Set-Content -LiteralPath (Join-Path $stageOut 'run-settings.cmd') -Value $launcher -Encoding ascii

$readme = @"
水杉输入法 免安装预览包 v$TargetVersion

这个压缩包只包含设置窗口，用来查看界面，不是可以输入中文的输入法。
它不写注册表、不装 TSF、不需要管理员权限，也不需要导入任何证书。

怎么用
  1. 解压到普通目录，别放 Program Files：那里需要管理员权限才能写。
  2. 双击 run-settings.cmd。
  3. 默认模式下看完直接删掉整个目录，不留任何痕迹。

两种启动方式
  run-settings.cmd          用包内的空白配置，改动不影响本机已装的版本
  run-settings.cmd live     用本机已装版本的真实配置，能看出新界面在你自己的设置下是什么样

live 模式的注意事项
  这个模式改的是你正式版的配置文件，删掉本目录不会自动恢复。
  想留后路的话，先备份 %LOCALAPPDATA%\metasequoiaime\config.toml。
  本机没装过正式版时，它会落到 %LOCALAPPDATA%\metasequoiaime；那里没有现成配置，
  改动会保存失败，请改用默认模式。
  另外这个设置程序来自开发构建，没有正式版的签名证书。

两种方式都会给设置程序一个独立的实例名，可以和已安装的正式版同时开着。

哪些能看到
  全部设置页面的界面与交互。隔离模式下改动写进本目录下的 data\config.toml。

哪些看不到
  输入法本身的输入效果：候选窗、上屏、皮肤在真实输入时的样子。
  这些需要 TSF DLL 注册到系统里，免安装包做不到。
  要完整体验请用同一构建产出的安装包。

如果打开后弹「设置保存失败」
  说明目录不可写（例如解压到了 Program Files）。换到普通目录再试。
"@
Set-Content -LiteralPath (Join-Path $stageOut 'README.txt') -Value $readme -Encoding utf8

# 5. 压成 zip。用压缩等级 Optimal：包本来就只有几十 MB，省的是下载时间。
$zipPath = Join-Path $OutputDirectory "$folderName.zip"
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
Compress-Archive -Path (Join-Path $stageOut '*') -DestinationPath $zipPath -CompressionLevel Optimal

$size = [math]::Round((Get-Item -LiteralPath $zipPath).Length / 1MB, 1)
$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLower()
Write-Host "免安装包：$zipPath（$size MB，sha256 $hash）"

# GitHub Actions 用它填 step summary。
if ($env:GITHUB_OUTPUT) {
    "portable_path=$zipPath" >> $env:GITHUB_OUTPUT
    "portable_size=$size" >> $env:GITHUB_OUTPUT
    "portable_sha256=$hash" >> $env:GITHUB_OUTPUT
}
