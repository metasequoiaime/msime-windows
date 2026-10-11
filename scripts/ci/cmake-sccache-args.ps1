<#
.SYNOPSIS
Return CMake launcher arguments when sccache is ready.

.EXAMPLE
./scripts/ci/cmake-sccache-args.ps1 -SetupOutcome success
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$SetupOutcome
)

if ($SetupOutcome -eq 'success' -and (Get-Command sccache -CommandType Application -ErrorAction SilentlyContinue)) {
    '-DCMAKE_C_COMPILER_LAUNCHER=sccache'
    '-DCMAKE_CXX_COMPILER_LAUNCHER=sccache'
} else {
    Write-Warning 'sccache is unavailable. Build with MSVC directly.'
}
