# Sign-Installer-Local.ps1 和 Install.ps1 共用：找出本地要签名 / 启动的那个安装包，输出它的绝对路径。
#
# 依据是 Compile-Installer.ps1 编译成功后写的 Output\last-build.json。对不上时直接报错，
# 不去猜一个文件名：猜出来的往往是 Output\ 里残留的旧包，签上名、装上去都不会有任何提示。
#
#   -InstallerPath  显式指定安装包，跳过 last-build.json（例如在 Inno Setup GUI 里编译的包）
#   -Light          要求上次编译的是轻量包；不传时沿用上次编译的模式

[CmdletBinding()]
param(
    [switch]$Light,
    [string]$InstallerPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($InstallerPath) {
    if (-not (Test-Path -LiteralPath $InstallerPath -PathType Leaf)) {
        throw "指定的安装包不存在：$InstallerPath"
    }
    return (Resolve-Path -LiteralPath $InstallerPath).Path
}

$outputDirectory = Join-Path $PSScriptRoot 'Output'
$lastBuildPath = Join-Path $outputDirectory 'last-build.json'
if (-not (Test-Path -LiteralPath $lastBuildPath -PathType Leaf)) {
    throw "找不到 $lastBuildPath。请先运行 Compile-Installer.ps1，或用 -InstallerPath 指定安装包。"
}

$lastBuild = Get-Content -LiteralPath $lastBuildPath -Raw | ConvertFrom-Json
$builtLight = [bool]$lastBuild.light
if ($PSBoundParameters.ContainsKey('Light') -and [bool]$Light -ne $builtLight) {
    $built = if ($builtLight) { '轻量包' } else { '完整包' }
    $wanted = if ($Light) { '轻量包' } else { '完整包' }
    throw "上次编译的是$built，而这次要求$wanted。请先编译$wanted，或用 -InstallerPath 指定安装包。"
}

$recorded = [string]$lastBuild.installerPath
if (-not (Test-Path -LiteralPath $recorded -PathType Leaf)) {
    throw "last-build.json 记录的安装包已不存在，请重新运行 Compile-Installer.ps1：$recorded"
}
$recorded = (Resolve-Path -LiteralPath $recorded).Path

# 绕过 Compile-Installer.ps1 编出来的包（例如在 Inno Setup GUI 里按 Ctrl+F9）不会更新
# last-build.json。Output\ 里有比记录更新的安装包时，说明记录多半已经过时，让人来决定用哪个。
$recordedTime = (Get-Item -LiteralPath $recorded).LastWriteTimeUtc
$newer = @(
    Get-ChildItem -LiteralPath $outputDirectory -Filter 'MetasequoiaIME_Setup_v*.exe' -File |
        Where-Object { $_.FullName -ne $recorded -and $_.LastWriteTimeUtc -gt $recordedTime }
)
if ($newer.Count -gt 0) {
    throw @"
Output\ 里有比 last-build.json 记录更新的安装包：
  $(($newer.FullName) -join "`n  ")
last-build.json 记录的是：
  $recorded
它可能不是经 Compile-Installer.ps1 编译的。请重新运行 Compile-Installer.ps1，或用 -InstallerPath 指定要用的安装包。
"@
}

return $recorded
