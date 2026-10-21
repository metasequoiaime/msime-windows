# 信任一张 dev-build 签名证书并清理旧证书。
#
# CI 的 dev-build workflow 每次运行现场生成一张自签名代码签名证书，签完整包后把公钥
# （.cer）放进 artifact。指纹每次都变，但 subject 固定，所以本脚本先把本地受信任存储里
# 旧 subject 的证书清掉，再导入新证书，测试机的受信任根不会随构建次数膨胀。
#
# 用管理员 PowerShell 运行（写 LocalMachine 存储需要提升）：
#   pwsh -File .\import-dev-trust.ps1 [-CertificatePath .\dev-build.cer] [-Keep]
#
# 导入完成就可以安装同目录下的 MetasequoiaIME_Setup_v*-dev.*.exe。
# -Keep 保留旧证书不清理（排查签名问题时用）。
[CmdletBinding()]
param(
    # 默认取脚本同目录下唯一的 .cer；有多个时报错让用户指定，避免导错文件。
    [string]$CertificatePath,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Subject = 'CN=Metasequoia IME Dev Build Code Signing'
$TrustStores = @('Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher')

function Test-Administrator {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [System.Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not $CertificatePath) {
    $candidates = @(
        Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.cer' -File -ErrorAction SilentlyContinue
    )
    if ($candidates.Count -eq 0) {
        throw "当前目录没有 .cer 文件。请把 dev-build artifact 解压后在本目录运行：$PSScriptRoot"
    }
    if ($candidates.Count -gt 1) {
        throw "发现多个 .cer 文件，请用 -CertificatePath 指定一个：$($candidates.Name -join ', ')"
    }
    $CertificatePath = $candidates[0].FullName
}
if (-not (Test-Path -LiteralPath $CertificatePath -PathType Leaf)) {
    throw "证书文件不存在：$CertificatePath"
}

$certificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($CertificatePath)
if ($certificate.Subject -ne $Subject) {
    throw "这不是 dev-build 签名证书（subject：$($certificate.Subject)）。预期：$Subject"
}

# 到期时间读的是这张 .cer 自带的字段，不靠「构建过了几天」去推。签名在证书有效期内完成才能
# 通过校验，所以证书过期后这份包就装不上了，导入得再干净也没用，必须换新构建。
# 这一步必须排在写受信任存储之前：过期证书导入进去只会让本机多一条无用的信任记录。
if ($certificate.NotAfter -le (Get-Date)) {
    Write-Warning "这张证书已经过期（$($certificate.NotAfter.ToString('yyyy-MM-dd HH:mm'))），包内签名无法通过校验。"
    Write-Warning '请重新下载最新的 dev build，用它的证书和安装包；不要继续安装当前这份。'
    exit 1
}

# 权限检查放在体检之后：先让没提升权限的用户知道包本身已经过期，再告诉他去开管理员。
if (-not (Test-Administrator)) {
    throw '需要管理员 PowerShell：写入 LocalMachine 受信任存储必须提升权限。'
}

# 按固定 subject 清理所有旧证书，再导入新的一张。跳过与本次导入指纹相同的那份。
if (-not $Keep) {
    foreach ($store in $TrustStores) {
        $stale = @(Get-ChildItem -Path $store -ErrorAction SilentlyContinue |
            Where-Object { $_.Subject -eq $Subject -and $_.Thumbprint -ne $certificate.Thumbprint })
        foreach ($old in $stale) {
            Remove-Item -LiteralPath ("{0}\{1}" -f $store, $old.Thumbprint) -Force
            Write-Host "已移除旧 dev 证书：$($old.Thumbprint)（$store）"
        }
    }
}

$temporaryFile = Join-Path ([IO.Path]::GetTempPath()) ('msime-dev-trust-' + [Guid]::NewGuid() + '.cer')
try {
    [IO.File]::Copy($CertificatePath, $temporaryFile, $true)
    foreach ($store in $TrustStores) {
        Import-Certificate -FilePath $temporaryFile -CertStoreLocation $store | Out-Null
        Write-Host "已信任：$store"
    }
}
finally {
    Remove-Item -LiteralPath $temporaryFile -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host "Dev 证书已就绪：$($certificate.Subject)"
Write-Host "指纹：$($certificate.Thumbprint)（本次构建专有，换新 build 需重跑本脚本）"
Write-Host "有效期至：$($certificate.NotAfter.ToString('yyyy-MM-dd HH:mm'))（本机时间）"
Write-Host '现在可以安装同目录下的 dev 安装包了。'
