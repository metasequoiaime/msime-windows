[CmdletBinding()]
param(
    [string]$IsccPath,
    [string]$IssPath = (Join-Path $PSScriptRoot 'msime_setup.iss'),
    [switch]$Light,
    # 安装包版本，通过 ISCC /D 传入；缺省时用 msime_setup.iss 里的默认值（0.0.1）。
    [string]$TargetVersion
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Resolve-IsccPath {
    param([string]$ExplicitPath)

    if ($ExplicitPath) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "指定的 Inno Setup 编译器不存在：$ExplicitPath"
        }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }

    $pathCommand = Get-Command 'ISCC.exe' -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($pathCommand) {
        return $pathCommand.Source
    }

    $uninstallPaths = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )
    $candidates = @(
        Get-ItemProperty -Path $uninstallPaths -ErrorAction SilentlyContinue |
            Where-Object {
                $displayName = $_.PSObject.Properties['DisplayName']
                $installLocation = $_.PSObject.Properties['InstallLocation']
                $displayName -and $displayName.Value -like 'Inno Setup*' -and
                    $installLocation -and $installLocation.Value
            } |
            Sort-Object {
                $displayVersion = $_.PSObject.Properties['DisplayVersion']
                try { [version]$displayVersion.Value } catch { [version]'0.0' }
            } -Descending |
            ForEach-Object {
                Join-Path $_.PSObject.Properties['InstallLocation'].Value 'ISCC.exe'
            }
    )

    $programRoots = @(
        ${env:ProgramFiles(x86)},
        $env:ProgramFiles,
        (Join-Path $env:LOCALAPPDATA 'Programs')
    ) | Where-Object { $_ }
    foreach ($version in @(7, 6)) {
        foreach ($root in $programRoots) {
            $candidates += Join-Path $root "Inno Setup $version\ISCC.exe"
        }
    }

    $candidate = $candidates |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if ($candidate) {
        return $candidate
    }

    throw @'
找不到 Inno Setup 命令行编译器 ISCC.exe。请安装 Inno Setup 6.6 或更高版本，或通过
-IsccPath 参数传入 ISCC.exe 的完整路径。
'@
}

if (-not (Test-Path -LiteralPath $IssPath -PathType Leaf)) {
    throw "Inno Setup 脚本不存在：$IssPath"
}

$resolvedIssPath = (Resolve-Path -LiteralPath $IssPath).Path
$resolvedIsccPath = Resolve-IsccPath -ExplicitPath $IsccPath

$outputDirectory = Join-Path $PSScriptRoot 'Output'
$lastBuildPath = Join-Path $outputDirectory 'last-build.json'
# 先删掉旧记录：这次编译失败时，Sign-Installer-Local.ps1 / Install.ps1 应该报错，而不是拿上一次的包。
Remove-Item -LiteralPath $lastBuildPath -Force -ErrorAction SilentlyContinue

# 版本只从参数来，不经任何中间文件：调用方（CI workflow、package-simplysign.ps1、
# Invoke-LocalTest.ps1）本来就知道版本。不传时用 .iss 里的默认值，与直接用 ISCC 编译本文件一致。
if ($TargetVersion) {
    $appVersion = $TargetVersion
    # VersionInfoVersion 只接受数字和点号；从目标版本剥掉 -后缀（dev 构建的
    # 0.7.1-dev.20261019.<hash> → 0.7.1），正式版本剥了也原样。
    $versionInfo = ($appVersion -replace '-.*$', '')
}
else {
    $issContent = Get-Content -LiteralPath $resolvedIssPath -Raw
    if ($issContent -notmatch '(?m)^#define\s+MyAppVersion\s+"(?<version>[^"]+)"') {
        throw '没有传 -TargetVersion，也未能在安装脚本中找到默认的 MyAppVersion。'
    }
    $appVersion = $Matches.version
    if ($issContent -notmatch '(?m)^#define\s+MyVersionInfo\s+"(?<version>[^"]+)"') {
        throw '没有传 -TargetVersion，也未能在安装脚本中找到默认的 MyVersionInfo。'
    }
    $versionInfo = $Matches.version
    Write-Warning "没有传 -TargetVersion，使用安装脚本里的默认版本 $appVersion。"
}
if ($versionInfo -notmatch '^[0-9]+(\.[0-9]+)*$') {
    throw "目标版本剥掉后缀后不是数字版本号，无法写进 VersionInfoVersion：$appVersion"
}

Write-Host "Inno Setup 编译器：$resolvedIsccPath"
Write-Host "安装脚本：$resolvedIssPath"
Write-Host "安装包版本：$appVersion（VersionInfo $versionInfo）"

$isccArgs = @()
if ($Light) {
    $isccArgs += '/DLightPackage=1'
    Write-Host '轻量模式：安装包不含词库，输出文件名带 _light。'
}
$isccArgs += "/DMyAppVersion=$appVersion"
$isccArgs += "/DMyVersionInfo=$versionInfo"
$isccArgs += $resolvedIssPath

Push-Location $PSScriptRoot
try {
    & $resolvedIsccPath @isccArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Inno Setup 编译失败，退出码：$LASTEXITCODE"
    }
}
finally {
    Pop-Location
}

$suffix = if ($Light) { '_light' } else { '' }
$installerPath = Join-Path $outputDirectory "MetasequoiaIME_Setup_v$appVersion$suffix.exe"
if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
    throw "编译成功但找不到预期产物：$installerPath"
}
@{
    appVersion    = $appVersion
    versionInfo   = $versionInfo
    light         = [bool]$Light
    installerPath = (Resolve-Path -LiteralPath $installerPath).Path
} | ConvertTo-Json | Set-Content -LiteralPath $lastBuildPath -Encoding utf8NoBOM

Write-Host "安装包编译完成：$installerPath"
