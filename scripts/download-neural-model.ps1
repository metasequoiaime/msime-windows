# 从 chinese-ime-lm Release 下载神经整句模型文件。
#
# 幂等：文件已在且摘要正确时直接返回。因此每次完整包构建前无条件调用，首次运行
# 要下 ~30 MB，之后都是秒回。

[CmdletBinding()]
param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$neuralModelDir = Join-Path $RepoRoot 'neural-model'
$lockPath = Join-Path $neuralModelDir 'lock.json'

if (-not (Test-Path -LiteralPath $lockPath)) {
    throw "神经模型的来源锁不存在：$lockPath"
}

$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json

New-Item -ItemType Directory -Path $neuralModelDir -Force | Out-Null

function Test-Digest {
    param([Parameter(Mandatory)][string]$LiteralPath, [Parameter(Mandatory)][string]$Expected)
    if (-not (Test-Path -LiteralPath $LiteralPath)) { return $false }
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $LiteralPath).Hash.ToLower() -eq $Expected.ToLower()
}

function Assert-Digest {
    param([Parameter(Mandatory)][string]$LiteralPath, [Parameter(Mandatory)][string]$Expected,
          [Parameter(Mandatory)][string]$Description)
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $LiteralPath).Hash.ToLower()
    if ($actual -ne $Expected.ToLower()) {
        throw "$Description 的 SHA256 与 lock.json 不符：期望 $($Expected.ToLower())，实得 $actual（$LiteralPath）"
    }
}

# 检查所有模型文件是否已就绪
$allReady = $true
foreach ($model in $lock.models) {
    $modelPath = Join-Path $neuralModelDir $model.file
    if (-not (Test-Digest -LiteralPath $modelPath -Expected $model.sha256)) {
        $allReady = $false
        break
    }
}

if ($allReady) {
    Write-Host "神经整句模型已就绪"
    return
}

Write-Host "下载神经整句模型（$($lock.release_tag)）..."
foreach ($model in $lock.models) {
    $modelPath = Join-Path $neuralModelDir $model.file

    if (Test-Digest -LiteralPath $modelPath -Expected $model.sha256) {
        Write-Host "  $($model.file) 已就绪"
        continue
    }

    Write-Host "  下载 $($model.file) ($([math]::Round($model.size / 1MB, 1)) MB)..."
    # 摘要没对上的残留必须先删
    if (Test-Path -LiteralPath $modelPath) { Remove-Item -LiteralPath $modelPath -Force }

    curl.exe -L --fail --retry 3 -o $modelPath $model.url
    if ($LASTEXITCODE -ne 0) { throw "下载失败（$LASTEXITCODE）：$($model.url)" }

    Assert-Digest -LiteralPath $modelPath -Expected $model.sha256 -Description $model.file
    Write-Host "  $($model.file) 验证成功"
}

Write-Host "神经整句模型下载完成"
