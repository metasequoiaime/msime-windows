<#
.SYNOPSIS
    下载 chinese-ime-lm 的神经整句模型（safetensors）到 neural-model/，并按 lock.json 校验 sha256。

.DESCRIPTION
    与 build-language-model.ps1 对 sc.lm 的处理同理：模型二进制不进仓库（见 .gitignore），
    只有 lock.json 和 NOTICE.md 进仓库。打包（installer/Prepare-PackageFiles.ps1）前跑一次
    本脚本把两个模型取到位。已存在且校验通过的文件跳过重复下载（除非 -Force）。

.PARAMETER Force
    忽略已存在文件，强制重新下载。
#>
[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$here = Split-Path -Parent $PSScriptRoot
$modelDir = Join-Path $here 'neural-model'
$lockPath = Join-Path $modelDir 'lock.json'
if (-not (Test-Path -LiteralPath $lockPath)) {
    throw "找不到 lock.json：$lockPath"
}
$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json

foreach ($model in $lock.models) {
    $dest = Join-Path $modelDir $model.file
    $needsDownload = $true
    if ((Test-Path -LiteralPath $dest) -and -not $Force) {
        $hash = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -eq $model.sha256.ToLowerInvariant()) {
            Write-Host "已存在且校验通过，跳过：$($model.file)"
            $needsDownload = $false
        }
        else {
            Write-Host "校验不符，重新下载：$($model.file)"
        }
    }
    if ($needsDownload) {
        Write-Host "下载 $($model.file) ..."
        # GitHub's release CDN drops connections regularly (the nightly pipeline has
        # seen it all week); the digest check below catches a truncated file either
        # way, so retry the download and let the checksum arbitrate.
        $downloaded = $false
        foreach ($attempt in 1..3) {
            try {
                Invoke-WebRequest -Uri $model.url -OutFile $dest -MaximumRetryCount 3 -RetryIntervalSec 5
                $downloaded = $true
                break
            }
            catch {
                if ($attempt -eq 3) { throw }
                Write-Warning "下载失败（第 $attempt 次）：$($_.Exception.Message)"
                Start-Sleep -Seconds (10 * $attempt)
            }
        }
        if (-not $downloaded) { throw "下载失败：$($model.url)" }
        $hash = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne $model.sha256.ToLowerInvariant()) {
            throw "$($model.file) sha256 校验失败：期望 $($model.sha256)，实际 $hash"
        }
        Write-Host "  校验通过：$($model.file)"
    }
}

Write-Host '神经整句模型准备完毕。'
