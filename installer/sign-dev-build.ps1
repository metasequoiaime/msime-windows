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
#
# 复用的判断必须来自「本次运行写入的标记」，不能只靠「证书看起来像 dev 证书」：改了 subject
# 之后，任何一台没被重置的机器上都会留下上次构建的证书，有效期 30 天，subject 和 EKU 全都
# 对得上。只按属性筛就会把上一个任务生成的私钥拿来签名，产出的包属于别的流水线。标记文件
# 放在 RUNNER_TEMP 下（托管 runner 每次都是新的临时目录），内容就是本次要复用的指纹。
function Get-RunMarkerPath {
    $root = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
    return Join-Path $root 'msime-dev-build-certificate.thumbprint'
}

function Get-RunCertificate {
    $marker = Get-RunMarkerPath
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf)) {
        return $null
    }
    $thumbprint = (Get-Content -LiteralPath $marker -Raw -ErrorAction SilentlyContinue)
    if ($thumbprint) { $thumbprint = $thumbprint.Trim() }
    if (-not $thumbprint) {
        return $null
    }
    $certificate = Get-ChildItem -Path 'Cert:\CurrentUser\My' -ErrorAction SilentlyContinue |
        Where-Object { $_.Thumbprint -eq $thumbprint -and $_.HasPrivateKey } |
        Select-Object -First 1
    if (-not $certificate) {
        # 标记在但证书没了（比如被清理）：当作本次没有证书，重新生成并覆盖标记。
        Write-Host "The certificate recorded for this run is gone from the store: $thumbprint"
    }
    return $certificate
}

function New-RunCertificate {
    # -Type CodeSigningCert 负责代码签名 EKU。有效期权衡的是两件事：artifact 只留 7 天，
    # 但用户可能下载到本地后放很久才装，而安装器检查的是签名时刻证书是否有效；拉得太长又会
    # 让一张随构建轮换的证书变成长期可信身份。90 天覆盖到拖延的安装，又不至于成为长期凭据。
    # import-dev-trust.ps1 会打印实际的到期时间，安装器发现包内证书过期时也会明确报出来。
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
        -NotAfter (Get-Date).AddDays(90)
    return $certificate
}

$certificate = Get-RunCertificate
if ($certificate) {
    Write-Host "Reusing the certificate created earlier in this run: $($certificate.Thumbprint)"
}
else {
    $certificate = New-RunCertificate
    # 先落标记再往下走：后面任何一步失败重跑，都应该复用这一张而不是再生成一张。
    Set-Content -LiteralPath (Get-RunMarkerPath) -Value $certificate.Thumbprint -NoNewline -Encoding utf8NoBOM
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
