# 生成 nightly 通道的长期签名证书，供仓库维护者一次性执行。
#
# nightly 与 dev 是两条互不相干的信任链：dev 每次运行现场生成，用户每换一次构建就要重导；
# nightly 用这张固定证书，用户导入一次即可长期验证。因此 subject 必须与 dev 的
# CN=Metasequoia IME Dev Build Code Signing 区分开，否则 import-dev-trust.ps1 按 subject
# 清旧导新时会把 nightly 证书一并删掉。
#
# 本机只出现在你的机器上，产物不要去 shared 目录：
#   - .pfx  含私钥，转成 base64 存进 NIGHTLY_CERTIFICATE_BASE64 后立即删除
#   - .cer  公钥，随仓库分发或挂在 nightly 说明里给用户下载
#
#     pwsh -File ./scripts/ci/new-nightly-certificate.ps1
#     pwsh -File ./scripts/ci/new-nightly-certificate.ps1 -OutputDirectory D:\certs -Years 3
#
# 生成后脚本会打印 secret 需要的两项值。密码随机生成，只出现在这一次输出里。
[CmdletBinding()]
param(
    [string]$OutputDirectory = (Join-Path $PWD 'nightly-certificate'),
    # 与 sign-binaries.ps1 读取的 secret 对应；改名时两边要一起动。
    [string]$Subject = 'CN=Metasequoia IME Windows Nightly Build Code Signing',
    [int]$Years = 3
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($Years -lt 1 -or $Years -gt 10) {
    throw "有效期 $Years 年不合理：nightly 证书是长期信任，但不是永久信任，取 1..10 之间的整数。"
}

# 随机密码：PFX 要保护私钥，但 CI 每次运行都要用，所以强度靠 32 字节随机而不是人能记住。
function New-SecretPassword {
    $bytes = New-Object byte[] 32
    [System.Security.Cryptography.RandomNumberGenerator]::Fill($bytes)
    return [Convert]::ToBase64String($bytes)
}

$password = New-SecretPassword
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$pfxPath = Join-Path $OutputDirectory 'nightly-signing.pfx'
$cerPath = Join-Path $OutputDirectory 'nightly-signing.cer'

# New-SelfSignedCertificate 建在用户存储里，导出后再清理，避免长期留一张带私钥的证书。
$certificate = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject $Subject `
    -FriendlyName 'Metasequoia IME Nightly Build Code Signing' `
    -KeyAlgorithm RSA `
    -KeyLength 4096 `
    -HashAlgorithm SHA256 `
    -KeyUsage DigitalSignature `
    -KeyExportPolicy Exportable `
    -CertStoreLocation 'Cert:\CurrentUser\My' `
    -NotBefore (Get-Date).AddDays(-1) `
    -NotAfter (Get-Date).AddYears($Years)

try {
    $secure = ConvertTo-SecureString -String $password -AsPlainText -Force
    Export-PfxCertificate -Cert $certificate -FilePath $pfxPath -Password $secure | Out-Null
    Export-Certificate -Cert $certificate -FilePath $cerPath -Type CERT -Force | Out-Null
}
finally {
    # 私钥已经进了 .pfx，存储里的那份立刻删掉。
    Remove-Item -LiteralPath ("Cert:\CurrentUser\My\{0}" -f $certificate.Thumbprint) -Force -ErrorAction SilentlyContinue
}

# 立刻读回来验一遍：签出来的 PFX 若打不开，等到 CI 里才发现就太晚了。
$reloaded = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2(
    $pfxPath, $password,
    [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
if (-not $reloaded.HasPrivateKey) {
    throw '导出的 PFX 读回来没有私钥，生成失败。'
}
if ($reloaded.Thumbprint -ne $certificate.Thumbprint) {
    throw "导出的 PFX 指纹与生成的不一致：$($reloaded.Thumbprint) != $($certificate.Thumbprint)"
}

$base64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($pfxPath))

# 6 KB 的 base64 从终端复制容易掉字符，落一份纯文本让维护者直接拷贝。
$secretPath = Join-Path $OutputDirectory 'secret-base64.txt'
Set-Content -LiteralPath $secretPath -Value $base64 -NoNewline -Encoding ascii

Write-Host ''
Write-Host '证书已生成，请按下面两步处理。'
Write-Host ''
Write-Host "输出目录：$OutputDirectory"
Write-Host "  $([IO.Path]::GetFileName($pfxPath))  含私钥，存完 secret 后必须删除"
Write-Host "  $([IO.Path]::GetFileName($cerPath))  公钥，随仓库分发"
Write-Host "  $([IO.Path]::GetFileName($secretPath))  base64 全文，用完删除"
Write-Host ''
Write-Host "Subject      : $($certificate.Subject)"
Write-Host "Thumbprint   : $($certificate.Thumbprint)"
Write-Host "有效期至     : $($certificate.NotAfter.ToString('yyyy-MM-dd'))"
Write-Host "密钥长度     : $($certificate.PublicKey.Key.KeySize) 位 RSA"
Write-Host ''
Write-Host '1. 在仓库 Settings -> Secrets and variables -> Actions 里加两个 secret：'
Write-Host ''
Write-Host '   NIGHTLY_CERTIFICATE_PASSWORD 的值：'
Write-Host "   $password"
Write-Host ''
Write-Host '   NIGHTLY_CERTIFICATE_BASE64 的值（下面这一整行，也写在 secret-base64.txt 里）：'
Write-Host ''
Write-Host $base64
Write-Host ''
Write-Host '2. 删除含私钥的文件，只保留 .cer：'
Write-Host "   Remove-Item -LiteralPath '$pfxPath','$secretPath' -Force"
Write-Host ''
Write-Warning '密码和 base64 只在这段输出里出现一次，关掉终端就再也拿不回来了。先存 secret，再删文件。'
