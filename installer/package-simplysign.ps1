# Build a distributable installer and sign it with the real Certum Code Signing certificate
# exposed by SimplySign Desktop.
#
# Typical usage (Version is positional):
#   pwsh -File .\package-simplysign.ps1 1.2.3
#   pwsh -File .\package-simplysign.ps1 1.2.3 -IncludeSymbols
#   pwsh -File .\package-simplysign.ps1 1.2.4 -Light
#
# Start SimplySign Desktop and connect the virtual card before running this script. Signtool may
# display a SimplySign PIN prompt for each signing operation. The private key never leaves
# SimplySign; signtool selects the certificate from the CurrentUser\My store by SHA-1 thumbprint.

[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [Alias('TargetVersion')]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version,

    # Published CI installers contain matching PDBs. They make local packaging substantially slower,
    # so this local entry keeps them opt-in, like test-symbols.ps1.
    [switch]$IncludeSymbols,

    # Hotfix package: only TSF, Server and the settings window assets, like test-light.ps1. It carries no dictionaries,
    # neural models or factory config template, so it is only valid on top of a full install whose
    # data files still match this build.
    [switch]$Light,

    # Force CMake configure before the incremental build.
    [switch]$Reconfigure,

    # Normally the single valid Certum code-signing certificate is selected automatically. Pass the
    # thumbprint when more than one is connected through SimplySign Desktop.
    [string]$CertificateThumbprint,

    [ValidateNotNullOrEmpty()]
    [string]$TimestampUrl = 'http://time.certum.pl',

    [string]$IsccPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$CodeSigningEku = '1.3.6.1.5.5.7.3.3'
$LocalTestCertificateSubject = 'CN=Metasequoia IME Local Test Code Signing'

function Find-SignTool {
    $kitsBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $candidate = Get-ChildItem -LiteralPath $kitsBin -Directory -ErrorAction SilentlyContinue |
        Sort-Object { try { [version]$_.Name } catch { [version]'0.0' } } -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (-not $candidate) {
        throw '找不到 signtool.exe；请安装 Windows SDK。'
    }
    return $candidate
}

function Test-CodeSigningEku {
    param([Parameter(Mandatory)]$Certificate)

    foreach ($usage in @($Certificate.EnhancedKeyUsageList)) {
        # PowerShell 7 exposes System.Security.Cryptography.Oid objects here (Value), while Windows
        # PowerShell may expose EnhancedKeyUsageRepresentation objects (ObjectId). Support both so
        # certificate discovery does not fail merely because the script host changed.
        $valueProperty = $usage.PSObject.Properties['Value']
        if ($valueProperty -and $valueProperty.Value -eq $CodeSigningEku) {
            return $true
        }

        $objectIdProperty = $usage.PSObject.Properties['ObjectId']
        if ($objectIdProperty) {
            $objectId = $objectIdProperty.Value
            if (($objectId -is [System.Security.Cryptography.Oid] -and
                    $objectId.Value -eq $CodeSigningEku) -or
                $objectId -eq $CodeSigningEku) {
                return $true
            }
        }
    }
    return $false
}

function Get-SimplySignCertificate {
    param([string]$Thumbprint)

    $now = Get-Date
    $normalizedThumbprint = $Thumbprint -replace '\s', ''
    $candidates = @(
        Get-ChildItem -Path 'Cert:\CurrentUser\My' |
            Where-Object {
                $_.HasPrivateKey -and
                $_.NotBefore -le $now -and
                $_.NotAfter -gt $now -and
                (Test-CodeSigningEku -Certificate $_) -and
                $_.Subject -ne $LocalTestCertificateSubject -and
                $_.Issuer -match '(?i)Certum'
            }
    )

    if ($normalizedThumbprint) {
        $candidates = @(
            $candidates | Where-Object {
                ($_.Thumbprint -replace '\s', '') -ieq $normalizedThumbprint
            }
        )
        if ($candidates.Count -eq 0) {
            throw @"
在 Cert:\CurrentUser\My 中找不到可用的代码签名证书：$normalizedThumbprint
请先启动 SimplySign Desktop、连接虚拟卡并确认该证书带有私钥且仍在有效期内。
"@
        }
    }
    elseif ($candidates.Count -eq 0) {
        throw @'
没有发现 SimplySign 提供的有效 Certum 代码签名证书。
请先启动 SimplySign Desktop，在手机端生成 OTP 并连接虚拟卡，然后重新运行本脚本。
'@
    }
    elseif ($candidates.Count -gt 1) {
        $details = $candidates | ForEach-Object {
            "  $($_.Thumbprint)  $($_.Subject)  有效期至 $($_.NotAfter.ToString('yyyy-MM-dd'))"
        }
        throw "发现多个可用的 Certum 代码签名证书，请用 -CertificateThumbprint 指定其中一个：`n$($details -join "`n")"
    }

    return $candidates[0]
}

function Invoke-SimplySign {
    param(
        [Parameter(Mandatory)][string[]]$LiteralPath,
        [Parameter(Mandatory)]$Certificate
    )

    # 最外层的 @() 不能省：只有一个待签文件时管道结果是标量，.Count 在严格模式下会报错。
    $resolvedPaths = @(
        @(
            foreach ($item in $LiteralPath) {
                if (-not (Test-Path -LiteralPath $item -PathType Leaf)) {
                    throw "待签名文件不存在：$item"
                }
                (Resolve-Path -LiteralPath $item).Path
            }
        ) | Sort-Object -Unique
    )

    $thumbprint = $Certificate.Thumbprint -replace '\s', ''
    Write-Host "正在使用 SimplySign 签名 $($resolvedPaths.Count) 个文件："
    $resolvedPaths | ForEach-Object { Write-Host "  $_" }

    # 一次 signtool 调用签完所有文件。SimplySign 可能对每次调用弹一次 PIN，逐个文件调用会让
    # 整包签名变成十几次手机确认。签名配额按文件计，合并调用不会多花配额。
    & $script:signTool sign /sha1 $thumbprint /s My /fd sha256 /tr $TimestampUrl /td sha256 /v @($resolvedPaths)
    if ($LASTEXITCODE -ne 0) {
        throw "signtool.exe 签名失败，退出码：$LASTEXITCODE"
    }

    foreach ($resolvedPath in $resolvedPaths) {
        & $script:signTool verify /pa /all /v $resolvedPath
        if ($LASTEXITCODE -ne 0) {
            throw "signtool.exe 签名校验失败，退出码：$LASTEXITCODE；文件：$resolvedPath"
        }

        $signature = Get-AuthenticodeSignature -LiteralPath $resolvedPath
        if (-not $signature.SignerCertificate -or
            (($signature.SignerCertificate.Thumbprint -replace '\s', '') -ine $thumbprint)) {
            throw "签名证书指纹与选择的 SimplySign 证书不一致：$resolvedPath"
        }
        if (-not $signature.TimeStamperCertificate) {
            throw "签名中没有可信时间戳：$resolvedPath"
        }
        Write-Host "签名有效：$resolvedPath"
    }
}

# 包内所有随安装包落到用户磁盘上的 EXE/DLL。只签 MetasequoiaImeServer.exe 是不够的：
# 未签名的辅助进程（设置页、表情面板、看门狗等）和随包分发的 DLL 会被 Microsoft Defender
# 的 SmartScreen/信誉判定拦下，而它们是用户实际会启动的程序。
function Get-PayloadBinaryPaths {
    $packageDirectories = @(
        (Join-Path $PSScriptRoot 'server_exe'),
        (Join-Path $PSScriptRoot 'tsf_dll')
    )
    foreach ($directory in $packageDirectories) {
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
            throw "待签名目录不存在，请先运行 Prepare-PackageFiles.ps1：$directory"
        }
    }

    $targets = @(
        @(
            foreach ($directory in $packageDirectories) {
                Get-ChildItem -LiteralPath $directory -Recurse -File |
                    Where-Object { $_.Extension -in @('.exe', '.dll') }
            }
        ) | Sort-Object FullName -Unique
    )
    if ($targets.Count -eq 0) {
        throw '没有找到待签名的 EXE 或 DLL，请先运行 Prepare-PackageFiles.ps1。'
    }
    return @($targets.FullName)
}

# Fail before compiling anything if SimplySign is not connected. Merely having its desktop process
# installed or running is not enough: the cloud certificate appears in CurrentUser\My only while the
# virtual card is connected.
$certificate = Get-SimplySignCertificate -Thumbprint $CertificateThumbprint
$script:signTool = Find-SignTool
Write-Host "SimplySign 证书：$($certificate.Subject)"
Write-Host "证书指纹：$($certificate.Thumbprint)"
Write-Host "有效期至：$($certificate.NotAfter.ToString('yyyy-MM-dd HH:mm:ss'))"
Write-Host "时间戳服务：$TimestampUrl"

$repoRoot = Split-Path -Parent $PSScriptRoot
$tsfCompile = Join-Path $repoRoot 'windows\scripts\lcompile-release-both.ps1'
$serverCompile = Join-Path $repoRoot 'server\scripts\lcompile-release.ps1'
foreach ($required in @($tsfCompile, $serverCompile)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "缺少组件构建入口：$required"
    }
}

# Prepare-PackageFiles writes the requested version into msime_setup.iss. Keep that implementation
# as the single source of installer naming/version logic, but do not leave a local packaging command
# changing the tracked template after it finishes.
$issPath = Join-Path $PSScriptRoot 'msime_setup.iss'
$tsfResourcePath = Join-Path $repoRoot 'windows\src\IME\MetasequoiaIME.rc'
$originalIss = [IO.File]::ReadAllBytes($issPath)
$originalTsfResource = [IO.File]::ReadAllBytes($tsfResourcePath)
$lightSuffix = if ($Light) { '_light' } else { '' }
$compiledInstallerPath = Join-Path $PSScriptRoot "Output\MetasequoiaIME_Setup_v$Version$lightSuffix.exe"
$installerSuffix = $lightSuffix + $(if ($IncludeSymbols) { '_with_pdb' } else { '' })
$installerPath = Join-Path $PSScriptRoot "Output\MetasequoiaIME_Setup_v$Version$installerSuffix.exe"

Push-Location $PSScriptRoot
try {
    # The command-line version drives both Inno Setup and the TSF DLL's VERSIONINFO. Restore the
    # tracked resource template afterwards; the compiled DLL keeps the requested version.
    python (Join-Path $repoRoot 'scripts\apply_version.py') --version $Version
    if ($LASTEXITCODE -ne 0) { throw "TSF 版本资源写入失败 ($LASTEXITCODE)" }

    Push-Location (Join-Path $repoRoot 'windows')
    try { & $tsfCompile -Reconfigure:$Reconfigure } finally { Pop-Location }

    Push-Location (Join-Path $repoRoot 'server')
    try { & $serverCompile -Reconfigure:$Reconfigure -ProductVersion $Version } finally { Pop-Location }

    # 完整包的数据输入和 Invoke-LocalTest.ps1 一样准备：词库从本仓同级的 msime-dictionary 构建，
    # 不下载 Release；源数据提交和构建脚本都没变、工作区干净时直接跳过。不跑这一步，
    # Prepare-PackageFiles 只会原样拿 MetasequoiaImeDict\out\ 里上次留下的东西。词格语言模型 sc.lm
    # 不随包发布（设置页按需下载），这里不生成它。
    if (-not $Light) {
        python -m pip install -q pypinyin
        if ($LASTEXITCODE -ne 0) { throw "安装 pypinyin 依赖失败（$LASTEXITCODE）" }
        python (Join-Path $repoRoot 'scripts\build-dictionary.py') --if-stale
        if ($LASTEXITCODE -ne 0) { throw "从 msime-dictionary 构建词库失败（$LASTEXITCODE）" }
        & (Join-Path $repoRoot 'scripts\download-neural-model.ps1')
    }

    & (Join-Path $PSScriptRoot 'Prepare-PackageFiles.ps1') `
        -TargetVersion $Version `
        -IncludeSymbols:$IncludeSymbols `
        -Light:$Light `
        -RepoRoot $repoRoot `
        -TsfDirectory windows `
        -ServerDirectory server `
        -NoticesDirectory . `
        -HelpCodeDirectory engine/helpcode

    # 整包签名，而不只是 MetasequoiaImeServer.exe。它是唯一一个「必须」签的——uiAccess=true
    # 没有可信签名就会被 Windows 忽略——但其余辅助进程和 DLL 不签名会被 Defender 拦截，
    # 用户看到的就是输入法装完之后某个面板打不开。外层安装包在编译之后单独签。
    Invoke-SimplySign -LiteralPath (Get-PayloadBinaryPaths) -Certificate $certificate

    & (Join-Path $PSScriptRoot 'Compile-Installer.ps1') -IsccPath $IsccPath -Light:$Light
    if (-not (Test-Path -LiteralPath $compiledInstallerPath -PathType Leaf)) {
        throw "Inno Setup 没有生成预期的安装包：$compiledInstallerPath"
    }
    if ($compiledInstallerPath -ne $installerPath) {
        Move-Item -LiteralPath $compiledInstallerPath -Destination $installerPath -Force
    }

    Invoke-SimplySign -LiteralPath $installerPath -Certificate $certificate
    $hash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash

    Write-Host ''
    if ($Light) {
        Write-Host 'SimplySign 正式签名轻量安装包已生成（只能覆盖安装在已有完整安装之上）：'
    }
    else {
        Write-Host 'SimplySign 正式签名安装包已生成：'
    }
    Write-Host "  $installerPath"
    Write-Host "  SHA256: $hash"
}
finally {
    [IO.File]::WriteAllBytes($issPath, $originalIss)
    [IO.File]::WriteAllBytes($tsfResourcePath, $originalTsfResource)
    Pop-Location
}
