# 启动本目录 Output\ 里刚编译好的安装包。
# 安装包文件名从 msime_setup.iss 的 MyAppVersion 读出。
# 传 -Light 时启动 *_light.exe。

param(
    [switch]$Light
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$outputDirectory = Join-Path $PSScriptRoot 'Output'
$lastBuildPath = Join-Path $outputDirectory 'last-build.json'
$installerPath = $null
$lightMode = [bool]$Light
if (Test-Path -LiteralPath $lastBuildPath -PathType Leaf) {
    $lastBuild = Get-Content -LiteralPath $lastBuildPath -Raw | ConvertFrom-Json
    if (-not $PSBoundParameters.ContainsKey('Light')) {
        $lightMode = [bool]$lastBuild.light
    }
    if ($lightMode -eq [bool]$lastBuild.light) {
        $installerPath = [string]$lastBuild.installerPath
    }
}
if (-not $installerPath) {
    $issPath = Join-Path $PSScriptRoot 'msime_setup.iss'
    if (-not (Test-Path -LiteralPath $issPath -PathType Leaf)) {
        throw "找不到安装脚本：$issPath"
    }
    $issContent = Get-Content -LiteralPath $issPath -Raw
    if ($issContent -notmatch '(?m)^#define\s+MyAppVersion\s+"(?<version>[^"]+)"') {
        throw '未能在 msime_setup.iss 中找到 MyAppVersion。'
    }
    $suffix = if ($lightMode) { '_light' } else { '' }
    $installerPath = Join-Path $outputDirectory "MetasequoiaIME_Setup_v$($Matches.version)$suffix.exe"
}
if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
    throw "安装文件不存在，请先运行 Compile-Installer.ps1：$installerPath"
}

& $installerPath
