# 信任 nightly 通道的签名证书。
#
# 和 import-dev-trust.ps1 的区别：那张证书每次构建都换，所以每次装新包都要重跑一次；nightly
# 用的是一张长期固定的证书（installer/nightly-signing.cer，随仓库分发），导入一次就能一直
# 验证后续所有 nightly 构建。subject 是 CN=Metasequoia IME Windows Nightly Build Code
# Signing，与 dev 的 subject 不同，两个通道的证书互不影响。
#
# 用管理员 PowerShell 运行（写 LocalMachine 存储需要提升）：
#   pwsh -File .\import-nightly-trust.ps1 [-CertificatePath .\nightly-signing.cer]
#
# 导入完成就可以安装同目录下的 MetasequoiaIME_Setup_v*-nightly.*.exe。
#
# 证书到期前需要换发：本脚本会在证书进入最后 30 天时给出提醒。
[CmdletBinding()]
param(
    # 默认取脚本同目录的 nightly-signing.cer。仓库里那份是权威来源，本机副本只在这个位置找不到时才用。
    [string]$CertificatePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Subject = 'CN=Metasequoia IME Windows Nightly Build Code Signing'
$TrustStores = @('Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher')
# 证书离到期不到这么多天时提醒维护者换发。
$RenewalWarningDays = 30

function Test-Administrator {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [System.Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not $CertificatePath) {
    $candidate = Join-Path $PSScriptRoot 'nightly-signing.cer'
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "找不到 nightly-signing.cer。请从 https://github.com/metasequoiaime/msime-windows/releases/tag/nightly 的附件里取，并用 -CertificatePath 指定。"
    }
    $CertificatePath = $candidate
}
if (-not (Test-Path -LiteralPath $CertificatePath -PathType Leaf)) {
    throw "证书文件不存在：$CertificatePath"
}

$certificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2((Resolve-Path -LiteralPath $CertificatePath).Path)
if ($certificate.Subject -ne $Subject) {
    throw "这不是 nightly 签名证书（subject：$($certificate.Subject)）。预期：$Subject"
}
if ($certificate.NotAfter -le (Get-Date)) {
    throw "这张 nightly 证书已经过期（$($certificate.NotAfter.ToString('yyyy-MM-dd'))）。请从 Releases 的 nightly 页面取最新的一份。"
}

if (-not (Test-Administrator)) {
    throw '需要管理员 PowerShell：写入 LocalMachine 受信任存储必须提升权限。'
}

# 固定的证书理论上只需要导一次，重复运行时按指纹跳过导入，但仍然核对两个存储里都在。
foreach ($store in $TrustStores) {
    $present = Get-ChildItem -Path $store -ErrorAction SilentlyContinue |
        Where-Object { $_.Thumbprint -eq $certificate.Thumbprint }
    if ($present) {
        Write-Host "已存在，跳过：$store"
        continue
    }
    $temporaryFile = Join-Path ([IO.Path]::GetTempPath()) ('msime-nightly-trust-' + [Guid]::NewGuid() + '.cer')
    try {
        [IO.File]::Copy($CertificatePath, $temporaryFile, $true)
        Import-Certificate -FilePath $temporaryFile -CertStoreLocation $store | Out-Null
        Write-Host "已信任：$store"
    }
    finally {
        Remove-Item -LiteralPath $temporaryFile -Force -ErrorAction SilentlyContinue
    }
}

# 同一 subject 下不该存在第二张证书：那说明换发过而旧的那张没清，留着就是一条多余的信任。
$stale = @(Get-ChildItem -Path $TrustStores -ErrorAction SilentlyContinue |
    Where-Object { $_.Subject -eq $Subject -and $_.Thumbprint -ne $certificate.Thumbprint })
foreach ($old in $stale) {
    Write-Warning "受信任存储里还有另一张 nightly 证书（$($old.Thumbprint)，到期 $($old.NotAfter.ToString('yyyy-MM-dd'))）。换发后应把它删掉。"
}

Write-Host ''
Write-Host "Nightly 证书已就绪：$($certificate.Subject)"
Write-Host "指纹：$($certificate.Thumbprint)"
Write-Host "有效期至：$($certificate.NotAfter.ToString('yyyy-MM-dd'))"
$remaining = ($certificate.NotAfter - (Get-Date)).Days
if ($remaining -lt $RenewalWarningDays) {
    Write-Warning "证书只剩 $remaining 天，请联系维护者换发。"
}
Write-Host '现在可以安装同目录下的 nightly 安装包了。之后新的 nightly 构建不用再跑本脚本。'
Write-Host ''
Write-Host '如果不做这一步就直接装：程序能启动、能打字，但在以管理员身份运行的窗口'
Write-Host '（管理员 PowerShell、以管理员打开的记事本或终端）里候选窗不会出现。'
Write-Host 'Server 带 uiAccess 清单，Windows 只对可信签名的程序承认这个权限。'
