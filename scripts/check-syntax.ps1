# 语法检查脚本：用 zig 自带的 clang 前端，按 windows-gnu 目标编译 server 的
# remote_store 相关源文件。这不是产品构建（产品用 MSVC），但能抓住拼写、类型、
# 缺少 include、函数签名不一致这类错误，比完全不编译强。
#
# 用法：
#   pwsh -File scripts/check-syntax.ps1
#
# 依赖：zig 在 PATH 里（或设置 ZIG 环境变量）。

param(
    [string[]] $Files = @(
        'server/src/settings/remote_store_common.cpp',
        'server/src/settings/remote_store_s3.cpp',
        'server/src/settings/remote_store_webdav.cpp',
        'server/src/settings/sync_merge.cpp',
        'server/src/settings/sync_service.cpp'
    )
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$zig = if ($env:ZIG) { $env:ZIG } else { 'zig' }
$cache = Join-Path $env:TEMP 'msime-syntax-cache'
New-Item -ItemType Directory -Force -Path (Join-Path $cache 'o') | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = $cache
$env:ZIG_LOCAL_CACHE_DIR = $cache

# 头文件搜索路径。zlib / curl / sqlite / boost 这些第三方依赖在纯语法检查里不需要
# 真实实现，但需要它们的头文件存在，否则包含不到就报错。这里只挂仓库内的路径；
# 引用了外部库的源文件不在默认列表里。
$includes = @(
    "-I$root/server/src",
    "-I$root/server",
    "-I$root/engine",
    "-I$root/ui/include",
    "-I$root"
)

# 第三方头文件目录。纯语法检查只需要头文件，不需要库；conda 的 Library/include 里
# 有 curl、sqlite3、zlib。缺失时跳过对应文件而不是误报。
$thirdParty = $env:MSIME_THIRD_PARTY_INCLUDE
if (-not $thirdParty) {
    $candidate = 'Z:\ProgramData\miniconda3\Library\include'
    if (Test-Path $candidate) { $thirdParty = $candidate }
}
if ($thirdParty -and (Test-Path $thirdParty)) { $includes += "-I$thirdParty" }
# toml++ 是单头库，产品构建从 vcpkg 取；本机没有 vcpkg 时允许用 MSIME_TOMLPP_INCLUDE
# 指向一份检出。缺它时 sync_merge.cpp 会因为 include 不到而失败，这是预期提示。
if ($env:MSIME_TOMLPP_INCLUDE) { $includes += "-I$env:MSIME_TOMLPP_INCLUDE" }

$failed = @()
foreach ($file in $Files) {
    $path = Join-Path $root $file
    if (-not (Test-Path $path)) {
        Write-Host "skip (missing): $file"
        continue
    }
    $out = Join-Path $cache ((Split-Path -Leaf $file) + '.obj')
    $arguments = @('c++', '-target', 'x86_64-windows-gnu', '-std=c++17', '-c') + $includes + @($path, '-o', $out)
    $output = & $zig @arguments 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        $failed += $file
        Write-Host "FAIL: $file"
        Write-Host $output
    }
    else {
        Write-Host "ok: $file"
    }
}

if ($failed.Count -gt 0) {
    Write-Host ("syntax check failed: " + ($failed -join ', '))
    exit 1
}
Write-Host 'syntax check passed'
