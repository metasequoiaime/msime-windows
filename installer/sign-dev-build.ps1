# 在 runner 上现场生成自签名代码签名证书，签 dev-build 的包内二进制与安装包。
#
# dev-build workflow 每次运行生成一张新证书，私钥只存在于 runner 内存中，运行结束即消失；
# 公钥导出成 .cer 进 artifact，由用户用 import-dev-trust.ps1 导入本机受信任存储。
# uiAccess=true 的 Server 没有可信签名会被 Windows 忽略，所以签名是 dev 包可用的前提。
#
# 环境变量：DEV_CERT_PATH（.cer 输出路径）。在 installer/ 目录下运行，与
# Sign-PackageBinaries-Local.ps1 相同的待签集合（server_exe/ 与 tsf_dll/ 下全部 EXE/DLL）。
#
#   pwsh -File .\sign-dev-build.ps1                       # 签包内二进制
#   pwsh -File .\sign-dev-build.ps1 -Installer            # 签编译完成的安装包
#   pwsh -File .\sign-dev-build.ps1 -FilePath a.exe,b.exe # 签显式列出的文件（server 单独构建用）
[CmdletBinding()]
param(
    [switch]$Installer,
    [string[]]$FilePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$CertificateSubject = 'CN=Metasequoia IME Dev Build Code Signing'
$CodeSigningEku = '1.3.6.1.5.5.7.3.3'

function Find-SignTool {
    $kitsBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $candidate = Get-ChildItem -LiteralPath $kitsBin -Directory -ErrorAction SilentlyContinue |
        Sort-Object { try { [version]$_.Name } catch { [version]'0.0' } } -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (-not $candidate) {
        throw 'signtool.exe not found in the Windows SDK.'
    }
    return $candidate
}

# 同一次运行里重复调用本脚本时复用签名步骤生成的证书，保持包内二进制与安装包同一签名。
function Get-RunCertificate {
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

function New-RunCertificate {
    # -Type CodeSigningCert 负责代码签名 EKU。有效期对齐 artifact 的 7 天保留期再放宽，
    # 避免 runner 时钟略慢导致证书尚未生效。
    $certificate = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $CertificateSubject `
        -FriendlyName 'Metasequoia IME Dev Build Code Signing' `
        -KeyAlgorithm RSA `
        -KeyLength 3072 `
        -HashAlgorithm SHA256 `
        -KeyUsage DigitalSignature `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -NotBefore (Get-Date).AddHours(-1) `
        -NotAfter (Get-Date).AddDays(30)
    return $certificate
}

$certificate = Get-RunCertificate
if ($certificate) {
    Write-Host "Reusing the certificate created earlier in this run: $($certificate.Thumbprint)"
}
else {
    $certificate = New-RunCertificate
    Write-Host "Created the dev-build signing certificate: $($certificate.Thumbprint)"
}

$certPath = $env:DEV_CERT_PATH
if (-not $certPath) {
    throw 'DEV_CERT_PATH is not set; the workflow must say where to export the public key.'
}
$certDirectory = Split-Path -Parent $certPath
if (-not (Test-Path -LiteralPath $certDirectory -PathType Container)) {
    New-Item -ItemType Directory -Path $certDirectory -Force | Out-Null
}
Export-Certificate -Cert $certificate -FilePath $certPath -Type CERT -Force | Out-Null
Write-Host "Public key exported to $certPath"

$signTool = Find-SignTool

if ($Installer) {
    $candidates = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'Output') -Filter '*.exe' -File -ErrorAction SilentlyContinue)
    if ($candidates.Count -eq 0) {
        throw 'No installer found under Output\. Compile it first.'
    }
    if ($candidates.Count -gt 1) {
        throw "Multiple installers under Output\, refusing to guess: $($candidates.Name -join ', ')"
    }
    $targets = @($candidates[0].FullName)
}
elseif ($FilePath) {
    $targets = @(
        foreach ($item in $FilePath) {
            if (-not (Test-Path -LiteralPath $item -PathType Leaf)) {
                throw "File to sign does not exist: $item"
            }
            (Resolve-Path -LiteralPath $item).Path
        }
    )
}
else {
    $packageDirectories = @(
        (Join-Path $PSScriptRoot 'server_exe'),
        (Join-Path $PSScriptRoot 'tsf_dll')
    )
    foreach ($directory in $packageDirectories) {
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
            throw "Missing staging directory, run Prepare-PackageFiles.ps1 first: $directory"
        }
    }
    # 最外层的 @() 不能省：只有一个待签文件时管道结果是标量，.Count 在严格模式下会报错。
    $targets = @(
        @(
            foreach ($directory in $packageDirectories) {
                Get-ChildItem -LiteralPath $directory -Recurse -File |
                    Where-Object { $_.Extension -in @('.exe', '.dll') }
            }
        ) | Sort-Object FullName -Unique
    ) | ForEach-Object { $_.FullName }
    if ($targets.Count -eq 0) {
        throw 'No EXE or DLL found to sign.'
    }
}

$thumbprint = $certificate.Thumbprint
Write-Host "Signing $($targets.Count) file(s) with $thumbprint"
# 自签名证书的时间戳没有意义（测试机可能离线，且证书随 build 轮换），所以不加 /tr。
& $signTool sign /sha1 $thumbprint /s My /fd sha256 /v @($targets)
if ($LASTEXITCODE -ne 0) {
    throw "signtool.exe signing failed with exit code $LASTEXITCODE"
}

foreach ($target in $targets) {
    $signature = Get-AuthenticodeSignature -LiteralPath $target
    if (-not $signature.SignerCertificate -or
        $signature.SignerCertificate.Thumbprint -ne $thumbprint) {
        throw "Signature missing or signed by the wrong certificate: $target"
    }
}
Write-Host "Signed $($targets.Count) file(s); every signature verified against the dev certificate."
