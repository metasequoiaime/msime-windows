# 启动本目录 Output\ 里刚编译好的安装包。
# 安装包由 Compile-Installer.ps1 写下的 Output\last-build.json 确定，解析规则见 Resolve-LocalInstaller.ps1。
# 传 -Light 时要求上次编译的是轻量包；-InstallerPath 可显式指定安装包。

param(
    [switch]$Light,
    [string]$InstallerPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$resolvedInstaller = & (Join-Path $PSScriptRoot 'Resolve-LocalInstaller.ps1') @PSBoundParameters
& $resolvedInstaller
