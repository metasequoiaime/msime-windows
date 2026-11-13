# 用本机自签名测试证书给 Inno Setup 编译出来的安装包签名。
# 与 Sign-PackageBinaries-Local.ps1 共用同一张本机测试证书。
# 每台机器各自生成证书，本仓库不包含任何预置证书或指纹。
#
# 直接运行，不需要参数：
#   pwsh -File .\Sign-Installer-Local.ps1
#
# 用的是和 Sign-PackageBinaries-Local.ps1 同一张测试证书：第一次运行时自动创建，
# 并装进本机受信任存储（这一步需要管理员 PowerShell；没有管理员权限只会警告，
# 签名照做，只是 signtool verify 的链校验过不了）。之后每次运行都会复用它。
#
# 安装包由 Compile-Installer.ps1 写下的 Output\last-build.json 确定，解析规则见
# Resolve-LocalInstaller.ps1。传 -Light 时要求上次编译的是轻量包；-InstallerPath 可显式指定安装包。

param(
    [switch]$Light,
    [string]$InstallerPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$CertificateSubject = 'CN=Metasequoia IME Local Test Code Signing'
$CertificateValidityYears = 5
$CodeSigningEku = '1.3.6.1.5.5.7.3.3'
$TrustStores = @('Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher')

function Test-Administrator {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [System.Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole(
        [System.Security.Principal.WindowsBuiltInRole]::Administrator)
}

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

function Get-LocalTestCertificate {
    $now = Get-Date
    $candidates = @(
        Get-ChildItem -Path 'Cert:\CurrentUser\My' |
            Where-Object {
                $_.Subject -eq $CertificateSubject -and
                $_.HasPrivateKey -and
                $_.NotBefore -le $now -and
                $_.NotAfter -gt $now -and
                ($_.EnhancedKeyUsageList.ObjectId -contains $CodeSigningEku)
            } |
            Sort-Object NotAfter -Descending
    )
    if ($candidates.Count -eq 0) {
        return $null
    }
    return $candidates[0]
}

function Add-CertificateTrust {
    param([Parameter(Mandatory)]$Certificate)

    $missing = @(
        $TrustStores | Where-Object {
            -not (Test-Path -LiteralPath (Join-Path $_ $Certificate.Thumbprint))
        }
    )
    if ($missing.Count -eq 0) {
        Write-Host '证书已在本机受信任存储中，跳过信任步骤。'
        return
    }
    if (-not (Test-Administrator)) {
        Write-Warning @"
证书还没进本机受信任存储，而写入它需要管理员权限：$($missing -join '、')
请用管理员 PowerShell 重新运行一次本脚本。在那之前签名依然有效，只是链校验会失败。
"@
        return
    }

    $temporaryFile = Join-Path ([System.IO.Path]::GetTempPath()) `
        ('msime-local-test-{0}.cer' -f $Certificate.Thumbprint)
    try {
        Export-Certificate -Cert $Certificate -FilePath $temporaryFile -Type CERT -Force | Out-Null
        foreach ($store in $missing) {
            Import-Certificate -FilePath $temporaryFile -CertStoreLocation $store | Out-Null
            Write-Host "已信任：$store"
        }
    }
    finally {
        if (Test-Path -LiteralPath $temporaryFile) {
            Remove-Item -LiteralPath $temporaryFile -Force
        }
    }
}

function Initialize-LocalTestCertificate {
    $certificate = Get-LocalTestCertificate
    if ($certificate) {
        Write-Host '已有本机测试证书，跳过创建。'
    }
    else {
        # -Type CodeSigningCert 负责代码签名 EKU；Exportable 是为了以后能导出公钥给测试机。
        $certificate = New-SelfSignedCertificate `
            -Type CodeSigningCert `
            -Subject $CertificateSubject `
            -FriendlyName 'Metasequoia IME Local Test Code Signing' `
            -KeyAlgorithm RSA `
            -KeyLength 3072 `
            -HashAlgorithm SHA256 `
            -KeyExportPolicy Exportable `
            -KeyUsage DigitalSignature `
            -CertStoreLocation 'Cert:\CurrentUser\My' `
            -NotAfter (Get-Date).AddYears($CertificateValidityYears)
        Write-Host '已创建本机测试证书。'
    }
    Write-Host "指纹：$($certificate.Thumbprint)"
    Write-Host "有效期至：$($certificate.NotAfter.ToString('yyyy-MM-dd HH:mm:ss'))"
    Add-CertificateTrust -Certificate $certificate
    return $certificate
}

function Test-SignedByLocalTestCertificate {
    param([Parameter(Mandatory)][string]$LiteralPath)

    $signature = Get-AuthenticodeSignature -LiteralPath $LiteralPath
    if (-not $signature.SignerCertificate) {
        return $false
    }
    # 不看 Status：测试证书没装进受信任根时链校验必然失败，但文件确实已由它签过名。
    return $signature.SignerCertificate.Thumbprint.Replace(' ', '') -ieq $script:thumbprint
}

function Invoke-Sign {
    param([Parameter(Mandatory)][string]$LiteralPath)

    # 自签名证书的时间戳没有意义，测试机还可能离线，所以不加 /tr。
    & $script:signTool sign /sha1 $script:thumbprint /s My /fd sha256 /v $LiteralPath
    if ($LASTEXITCODE -ne 0) {
        throw "signtool.exe 签名失败，退出码：$LASTEXITCODE"
    }

    & $script:signTool verify /pa /v $LiteralPath
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "已签名但链校验失败（测试证书尚未被本机信任）：$LiteralPath"
    }
}

$resolvedInstaller = & (Join-Path $PSScriptRoot 'Resolve-LocalInstaller.ps1') @PSBoundParameters

Write-Warning '使用本机测试证书签名，安装包只能内部测试，不要对外分发。'
$certificate = Initialize-LocalTestCertificate
$script:thumbprint = $certificate.Thumbprint
$script:signTool = Find-SignTool

if (Test-SignedByLocalTestCertificate -LiteralPath $resolvedInstaller) {
    Write-Host "安装文件已由本机测试证书签名，跳过：$resolvedInstaller"
}
else {
    Write-Host "正在签名安装文件：$resolvedInstaller"
    Invoke-Sign -LiteralPath $resolvedInstaller
}

Write-Host '安装文件本地签名检查完成。'
# 链校验警告会让最后一个 signtool 留下非零退出码；对本脚本来说整体是成功的。
$global:LASTEXITCODE = 0
