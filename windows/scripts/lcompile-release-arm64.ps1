# Build the Arm64X TSF DLL.
#
# Two passes: the first links for Arm64 and leaves its linker inputs in windows/repros/, the second
# links for Arm64EC and merges those inputs into one Arm64X file under buildarm64x-release. Running
# them out of order fails during configure with an explicit message; see windows/cmake/arm64x.cmake.
#
# The first pass's DLL is not installable on its own. Prepare-PackageFiles.ps1 reads
# buildarm64x-release\Release only.
[CmdletBinding()]
param(
    [string]$Target = 'MetasequoiaImeTsf',
    [switch]$Reconfigure
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

if (-not $env:VCPKG_ROOT) {
    throw 'VCPKG_ROOT is not set; the Arm64 passes need the vcpkg toolchain for the fmt headers.'
}
$toolchain = Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'

function Invoke-Pass {
    param([string]$Pass, [string]$Architecture, [string]$Triplet, [string]$BuildDirectory)
    $generateStamp = Join-Path $BuildDirectory 'CMakeFiles\generate.stamp'
    # Same reasoning as lcompile-release.ps1: a failed configure leaves CMakeCache.txt behind, so
    # key off generate.stamp, which only appears once generation succeeded.
    if ($Reconfigure -or -not (Test-Path -LiteralPath $generateStamp)) {
        cmake -S $projectRoot -B $BuildDirectory -A $Architecture `
            "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
            "-DVCPKG_TARGET_TRIPLET=$Triplet" `
            "-DBUILD_AS_ARM64X=$Pass"
        if ($LASTEXITCODE -ne 0) { throw "TSF $Pass configure failed ($LASTEXITCODE)" }
    }
    cmake --build $BuildDirectory --config Release --target $Target --parallel
    if ($LASTEXITCODE -ne 0) { throw "TSF $Pass build failed ($LASTEXITCODE)" }
}

Push-Location $projectRoot
try {
    # Order matters: the Arm64EC link reads the response file the Arm64 link wrote.
    Invoke-Pass -Pass 'ARM64' -Architecture 'ARM64' -Triplet 'arm64-windows-static-md' `
        -BuildDirectory (Join-Path $projectRoot 'buildarm64-release')
    Invoke-Pass -Pass 'ARM64EC' -Architecture 'ARM64EC' -Triplet 'arm64ec-windows' `
        -BuildDirectory (Join-Path $projectRoot 'buildarm64x-release')
}
finally { Pop-Location }
