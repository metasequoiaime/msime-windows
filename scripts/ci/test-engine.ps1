param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$StagingRoot
)
# Runs the engine's own test suite against the product's exact dictionaries.
#
# The Server build pulls the engine in with BUILD_TESTING off (see server/CMakeLists.txt), so these
# tests never ran in CI. Some of them are the only coverage for behaviour the Server tests cannot
# see -- the user journal's connection handling and the data-directory cleanup contract among them.
# The data layout mirrors test-server.ps1.
$ErrorActionPreference = 'Stop'
$automationRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$helpcodes = Join-Path $automationRoot 'engine/helpcode/helpcodes'
if (-not (Test-Path -LiteralPath $helpcodes -PathType Container)) {
    throw "Helpcodes not found at $helpcodes; the engine is vendored in-tree, so this is an incomplete checkout"
}
$radical = Join-Path $automationRoot 'engine/radical/decomposition.txt'
if (-not (Test-Path -LiteralPath $radical -PathType Leaf)) {
    throw "Radical decomposition table not found at $radical; the engine is vendored in-tree, so this is an incomplete checkout"
}
python (Join-Path $automationRoot 'scripts/product_lock.py') fetch-dictionaries --staging-root $StagingRoot
if ($LASTEXITCODE -ne 0) { throw 'Could not provision locked dictionaries' }
$verified = (Resolve-Path (Join-Path $StagingRoot 'MetasequoiaImeDict/out')).Path
# Isolate LOCALAPPDATA as well, so no installed dictionary or user journal is ever touched.
$env:LOCALAPPDATA = Join-Path (Resolve-Path $StagingRoot).Path 'user-local'
$data = Join-Path $env:LOCALAPPDATA 'metasequoiaime'
New-Item -ItemType Directory -Force -Path $data | Out-Null
Copy-Item (Join-Path $verified '*') -Destination $data -Force
Copy-Item $helpcodes -Destination $data -Recurse -Force
# U 模式的拆字表按 assets 清单的 radical/decomposition.txt 相对路径部署。
New-Item -ItemType Directory -Force -Path (Join-Path $data 'radical') | Out-Null
Copy-Item $radical -Destination (Join-Path $data 'radical') -Force
Copy-Item (Join-Path $automationRoot 'server/assets/tables/*') -Destination $data -Force
Copy-Item (Join-Path $automationRoot 'server/assets/config/config.toml') -Destination $data -Force
$env:METASEQUOIA_IME_DATA_DIR = $data
ctest --no-tests=error --test-dir $BuildDir -C Release --output-on-failure --timeout 120
if ($LASTEXITCODE -ne 0) { throw 'Engine tests failed for the locked product' }
