[CmdletBinding()]
param(
    [string]$IsccPath,
    [string]$IssPath = (Join-Path $PSScriptRoot 'msime_setup.iss'),
    [string]$DevCertificatePath,
    [switch]$Light
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

Write-Host "Inno Setup 编译器：$resolvedIsccPath"
Write-Host "安装脚本：$resolvedIssPath"

$isccArgs = @()
if ($DevCertificatePath) {
    $certificatePath = (Resolve-Path -LiteralPath $DevCertificatePath -ErrorAction Stop).Path
    $certificateHash = (Get-FileHash -LiteralPath $certificatePath -Algorithm SHA256).Hash
    $isccArgs += "/DDevCertificate=$certificatePath"
    $isccArgs += "/DDevCertificateSha256=$certificateHash"
}
if ($Light) {
    $isccArgs += '/DLightPackage=1'
    Write-Host '轻量模式：安装包不含词库，输出文件名带 _light。'
}
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

Write-Host "安装包编译完成，输出目录：$(Join-Path $PSScriptRoot 'Output')"
