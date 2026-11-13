# 轻量本地测试：只编译 TSF / Server，收集它们的产物和设置窗口图片资源，打不含词库的安装包。
#
# 适用：只改了 TSF 或 Server，本机已有完整安装（词库已在
# %LOCALAPPDATA%\metasequoiaime）。不要用这个脚本做首次安装。
#
# 完整流程（含词库）仍用 .\test.ps1；需要包内带 PDB 时用 .\test-symbols.ps1。
# 只构建不安装时加 -NoInstall；版本默认从 version.txt 派生，可用 -TargetVersion 覆盖。
#
# 编译是增量的，实现见 Invoke-LocalTest.ps1。

[CmdletBinding()]
param(
    [switch]$Reconfigure,
    [switch]$NoInstall,
    [string]$TargetVersion
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'Invoke-LocalTest.ps1') -Light -Reconfigure:$Reconfigure -NoInstall:$NoInstall `
    -TargetVersion $TargetVersion
