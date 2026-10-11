<#
.SYNOPSIS
Build a small C program with MSVC after a simulated sccache setup failure.

.EXAMPLE
./scripts/ci/test-sccache-fallback.ps1
#>
$ErrorActionPreference = 'Stop'

$launcherArgs = @(& "$PSScriptRoot/cmake-sccache-args.ps1" -SetupOutcome failure)
if ($launcherArgs.Count -ne 0) {
    throw 'The failed sccache setup selected a compiler launcher.'
}

$tempRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
$workDir = Join-Path $tempRoot "sccache-fallback-$([guid]::NewGuid().ToString('N'))"
$sourceDir = Join-Path $workDir 'source'
$buildDir = Join-Path $workDir 'build'
New-Item -ItemType Directory -Path $sourceDir -Force | Out-Null

try {
    Set-Content -Path (Join-Path $sourceDir 'CMakeLists.txt') -Encoding ascii -Value @'
cmake_minimum_required(VERSION 3.20)
project(sccache_fallback C)
add_executable(fallback_probe main.c)
'@
    Set-Content -Path (Join-Path $sourceDir 'main.c') -Encoding ascii -Value 'int main(void) { return 0; }'

    $cmakeArgs = @('-S', $sourceDir, '-B', $buildDir, '-G', 'Ninja Multi-Config') + $launcherArgs
    cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    if (Select-String -Path (Join-Path $buildDir 'CMakeCache.txt') -Pattern '^CMAKE_C_COMPILER_LAUNCHER:' -Quiet) {
        throw 'CMake configured a compiler launcher after the failed setup.'
    }

    $compilerFile = Get-ChildItem -Path (Join-Path $buildDir 'CMakeFiles') -Filter CMakeCCompiler.cmake -Recurse |
        Select-Object -First 1
    if (-not $compilerFile -or
        -not (Select-String -Path $compilerFile.FullName -Pattern '^set\(CMAKE_C_COMPILER_ID "MSVC"\)' -Quiet)) {
        throw 'CMake did not select MSVC.'
    }

    cmake --build $buildDir --config Release
    if ($LASTEXITCODE -ne 0) { throw 'The direct MSVC build failed.' }

    & (Join-Path $buildDir 'Release/fallback_probe.exe')
    if ($LASTEXITCODE -ne 0) { throw 'The compiled program failed.' }
} finally {
    Remove-Item -LiteralPath $workDir -Recurse -Force
}
