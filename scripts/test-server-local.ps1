param(
    # ctest -R pattern, e.g. MetasequoiaImeSettingsUiTests. Empty runs everything.
    [string]$Test = '',
    # Build every test executable first. CI does this; incremental local runs usually skip it.
    [switch]$Build,
    [string]$ServerRoot = '',
    [string]$BuildDir = 'build-release',
    # Where the throwaway data root lives. Reused between runs so the copy happens once.
    [string]$StagingRoot = ''
)
$ErrorActionPreference = 'Stop'

# Runs the server test suite against a staged data root instead of the installed one.
#
# scripts/ci/test-server.ps1 does the same for CI, but it downloads the dictionaries from the
# product lock first. Local iteration does not need that download: MetasequoiaImeDict/out holds
# the dictionaries a local build produced. Running the suite without staging is the trap this
# script exists to close -- engine paths fall back to %LOCALAPPDATA%\metasequoiaime, so tests
# that write (import, journal, ranking) land in the installed IME's live data.
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $ServerRoot) { $ServerRoot = Join-Path $repo 'server' }
$server = (Resolve-Path $ServerRoot).Path

$verified = Join-Path $repo 'MetasequoiaImeDict/out'
if (-not (Test-Path -LiteralPath $verified -PathType Container)) {
    throw "No local dictionaries at $verified. Build them first (scripts/build-dictionary.py) or run scripts/ci/test-server.ps1, which downloads the locked copies."
}
$helpcodes = Join-Path $repo 'engine/helpcode/helpcodes'
if (-not (Test-Path -LiteralPath $helpcodes -PathType Container)) {
    throw "Helpcodes not found at $helpcodes; the engine is vendored in-tree, so this is an incomplete checkout"
}
$radical = Join-Path $repo 'engine/radical/decomposition.txt'
if (-not (Test-Path -LiteralPath $radical -PathType Leaf)) {
    throw "Radical decomposition table not found at $radical"
}

if (-not $StagingRoot) { $StagingRoot = Join-Path ([System.IO.Path]::GetTempPath()) 'msime-server-tests' }
$stage = [System.IO.Path]::GetFullPath($StagingRoot)
$data = Join-Path $stage 'user-local/metasequoiaime'
if (Test-Path -LiteralPath $data) { Remove-Item -LiteralPath $data -Recurse -Force }
New-Item -ItemType Directory -Force -Path $data | Out-Null
# The dictionaries come from a local build, so a stale copy here would silently test old data.
Copy-Item (Join-Path $verified '*') -Destination $data -Force
Copy-Item $helpcodes -Destination $data -Recurse -Force
New-Item -ItemType Directory -Force -Path (Join-Path $data 'radical') | Out-Null
Copy-Item $radical -Destination (Join-Path $data 'radical') -Force
Copy-Item (Join-Path $server 'assets/tables/*') -Destination $data -Force
Copy-Item (Join-Path $server 'assets/config/config.toml') -Destination $data -Force

# Both names matter: the server reads Win32 environment variables, the engine reads the CRT copy.
$env:LOCALAPPDATA = Join-Path $stage 'user-local'
$env:METASEQUOIA_IME_DATA_DIR = $data

$arguments = @('--test-dir', (Join-Path $server $BuildDir), '-C', 'Release', '--output-on-failure',
               '--timeout', '120')
if ($Build) {
    cmake --build (Join-Path $server $BuildDir) --config Release --target ALL_BUILD --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Build failed; ctest would only report missing executables.' }
}
if ($Test) { $arguments += @('-R', $Test) }
Write-Host "data root: $data"
ctest @arguments
