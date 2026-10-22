# Builds the Rust host-api library from metasequoiaime/msime and stages it for the Server
# build. The Windows product consumes the shared account/cloud-sync implementation through
# the C ABI (msime_client.h) instead of reimplementing session management and sync
# orchestration in C++; the library is built from the upstream develop tree at a pinned
# commit so the binary in the installer always matches a reviewed source state.
#
# Inputs:
#   -TargetCommit   msime commit to build (default: the commit recorded in
#                   scripts/ci/host-api-pin.txt, "latest" tracks develop's head)
#   -OutputDir      where to stage msime_host_api.dll(.lib) and msime_client.h
# The staging directory also receives HOST-API-VERSION.txt recording the source commit, so
# packaging can embed it in the installer's about data and the audit can verify freshness.
#
# Runner prerequisites (installed by the CI job before calling): Rust toolchain 1.97.1
# (rustup), pnpm not needed (we only build the host-api crate). The clone is shallow and
# cached via actions/cache keyed on the commit.
param(
    [string]$TargetCommit = (Get-Content (Join-Path $PSScriptRoot 'host-api-pin.txt') -ErrorAction SilentlyContinue),
    [Parameter(Mandatory)][string]$OutputDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoUrl = 'https://github.com/metasequoiaime/msime.git'
$Workspace = Join-Path ([IO.Path]::GetTempPath()) 'msime-host-api-build'

if (-not $TargetCommit -or $TargetCommit -eq 'latest') {
    # The CI job resolves develop's head before calling this script; keep a
    # local fallback for manual runs.
    $head = git ls-remote $RepoUrl refs/heads/develop 2>$null | ForEach-Object { ($_ -split '\s+')[0] } | Select-Object -First 1
    if (-not $head) { throw "Could not resolve msime develop's head commit" }
    $TargetCommit = $head
}
if ($TargetCommit -notmatch '^[0-9a-f]{40}$') { throw "Invalid commit: $TargetCommit" }

Write-Host "Building msime-host-api from metasequoiaime/msime@$($TargetCommit.Substring(0, 12))"

# Shallow fetch just this commit; the cache action keeps it across runs.
if (Test-Path $Workspace) { Remove-Item -Recurse -Force $Workspace }
New-Item -ItemType Directory -Force -Path $Workspace | Out-Null
git -C $Workspace init -q
git -C $Workspace remote add origin $RepoUrl
git -C $Workspace fetch --depth 1 origin $TargetCommit
git -C $Workspace checkout -q FETCH_HEAD

$toolchain = (Get-Content (Join-Path $Workspace 'rust-toolchain.toml') |
    Select-String 'channel\s*=\s*"(.+?)"').Matches[0].Groups[1].Value
Write-Host "msime pins Rust $toolchain"
if (-not (Get-Command rustup -ErrorAction SilentlyContinue)) {
    # GitHub-hosted Windows runners ship Rust, but do not rely on that here.
    Invoke-RestMethod https://win.rustup.rs/x86_64 -OutFile "$env:TEMP\rustup-init.exe"
    & "$env:TEMP\rustup-init.exe" -y --default-toolchain none --profile minimal | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "rustup-init failed" }
    $env:PATH = "$env:USERPROFILE\.cargo\bin;$env:PATH"
}
& rustup toolchain install $toolchain --profile minimal
if ($LASTEXITCODE -ne 0) { throw "rustup toolchain install failed" }

$env:CARGO_TARGET_DIR = Join-Path $Workspace 'target'
Push-Location $Workspace
try {
    & cargo "+$toolchain" build --locked --release --target x86_64-pc-windows-msvc -p msime-host-api
    if ($LASTEXITCODE -ne 0) { throw "cargo build failed" }
}
finally {
    Pop-Location
}

$release = Join-Path $env:CARGO_TARGET_DIR 'x86_64-pc-windows-msvc\release'
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
foreach ($name in @('msime_host_api.dll', 'msime_host_api.dll.lib')) {
    Copy-Item -LiteralPath (Join-Path $release $name) -Destination $OutputDir -Force
    if (-not (Test-Path (Join-Path $OutputDir $name))) { throw "Missing staged output: $name" }
}
# The C ABI header describes the boundary; taken from the same commit as the binary.
Copy-Item -LiteralPath (Join-Path $Workspace 'crates\host-api\include\msime_client.h') -Destination $OutputDir -Force
Set-Content -LiteralPath (Join-Path $OutputDir 'HOST-API-VERSION.txt') -Value $TargetCommit

Write-Host "Staged msime_host_api to $OutputDir (commit $TargetCommit)"
