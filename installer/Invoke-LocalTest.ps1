# 本地测试安装流程的共用实现：编译本仓组件 → 收集安装文件 → 本机自签名 →
# Inno Setup 打包 → 签安装包 → 启动安装程序。
#
# 三个入口都是这个脚本的薄包装：
#   test.ps1          完整包（含词库），不含 PDB
#   test-light.ps1    轻量包（不含词库），不含 PDB
#   test-symbols.ps1  带 PDB，默认轻量，加 -Full 走完整包
#
# 编译是增量的：build-release / build{32,64}-release 已有 CMake 缓存时不再重跑 configure
# （Visual Studio 生成器的 ZERO_CHECK 会在 CMakeLists.txt 变化时自己重新生成），并且只构建
# 打包用的目标，不构建那些打完包又被删掉的测试可执行文件。-Reconfigure 可强制重跑 configure。

[CmdletBinding()]
param(
    # 轻量包：跳过词库、辅助码、拼音表和出厂配置，只刷新 TSF、Server 和设置窗口图片资源。
    # 适用：只改了 TSF 或 Server，本机已有完整安装（词库已在
    # %LOCALAPPDATA%\metasequoiaime）。不要用轻量包做首次安装。
    [switch]$Light,
    # 把 PDB 打进安装包。PDB 有 ~140 MB，而真正的二进制只有 ~20 MB，Inno Setup 还要对它们做
    # 固实 LZMA2 压缩，所以默认不打包；只有需要在装好的机器上直接做崩溃分析时才加这个开关。
    [switch]$IncludeSymbols,
    # 强制重跑 CMake configure（改了 preset、换了工具链或怀疑缓存不对时用）。
    [switch]$Reconfigure
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Every build step below uses its own Push-Location and absolute paths ($repoRoot / $PSScriptRoot),
# so the script never relies on the process cwd. Push/pop it here too, inside try/finally, so the
# caller's shell is left where it started instead of stranded in installer\ when the script exits.
Push-Location $PSScriptRoot
try {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $tsfCompile = Join-Path $repoRoot 'windows\scripts\lcompile-release-both.ps1'
    $serverCompile = Join-Path $repoRoot 'server\scripts\lcompile-release.ps1'

    # Both are directories of this repository now, so a missing one is a broken checkout rather
    # than a neighbour nobody cloned. Skipping the build and packaging whatever binaries happen to be
    # lying around would produce an installer you then run on your own machine.
    foreach ($required in @($tsfCompile, $serverCompile)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "缺少组件构建入口：$required"
        }
    }

    Push-Location (Join-Path $repoRoot 'windows')
    try { & $tsfCompile -Reconfigure:$Reconfigure } finally { Pop-Location }

    Push-Location (Join-Path $repoRoot 'server')
    try { & $serverCompile -Reconfigure:$Reconfigure } finally { Pop-Location }


    # 轻量包不带任何数据文件，所以只有完整包需要准备数据。词格语言模型 sc.lm 不随包发布，
    # 这里不再生成它。
    if (-not $Light) {
        # 词库从本仓同级的 msime-dictionary 构建，不下载 Release。源数据提交和构建脚本
        # 都没变、工作区干净时直接跳过；有变化就整套重建（约 45 秒）。
        # 词库构建需要 pypinyin，首次运行时自动安装。
        python -m pip install -q pypinyin
        if ($LASTEXITCODE -ne 0) { throw "安装 pypinyin 依赖失败（$LASTEXITCODE）" }
        python (Join-Path $repoRoot 'scripts\build-dictionary.py') --if-stale
        if ($LASTEXITCODE -ne 0) { throw "从 msime-dictionary 构建词库失败（$LASTEXITCODE）" }
        # 神经整句模型：从 chinese-ime-lm Release 下载。幂等脚本，摘要对上时直接返回。
        & (Join-Path $repoRoot 'scripts\download-neural-model.ps1')
    }

    & (Join-Path $PSScriptRoot 'Prepare-PackageFiles.ps1') -Light:$Light -IncludeSymbols:$IncludeSymbols `
        -RepoRoot $repoRoot -TsfDirectory windows -ServerDirectory server -NoticesDirectory .
    & (Join-Path $PSScriptRoot 'Sign-PackageBinaries-Local.ps1')
    & (Join-Path $PSScriptRoot 'Compile-Installer.ps1') -Light:$Light
    & (Join-Path $PSScriptRoot 'Sign-Installer-Local.ps1') -Light:$Light
    & (Join-Path $PSScriptRoot 'Install.ps1') -Light:$Light
} finally {
    Pop-Location
}
