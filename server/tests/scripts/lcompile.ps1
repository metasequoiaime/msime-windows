param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug'
)
$ErrorActionPreference = 'Stop'
$serverRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildDirectory = Join-Path $serverRoot $(if ($Configuration -eq 'Release') { 'build-release' } else { 'build' })
$preset = if ($Configuration -eq 'Release') { 'default-release' } else { 'default' }
Push-Location $serverRoot
try {
    cmake --preset=$preset -DBUILD_TESTING=ON
    if ($LASTEXITCODE -ne 0) { throw "Server test configure failed ($LASTEXITCODE)" }
    cmake --build $buildDirectory --config $Configuration --target MetasequoiaImeServerTests MetasequoiaImeSettingsUiTests test_webview_contract
    if ($LASTEXITCODE -ne 0) { throw "Server test build failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
}
