# Exercise the installer bootstrap with a stub importer; never change certificate stores.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$iscc = Get-Command ISCC.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $iscc) {
    $iscc = @(foreach ($version in @(7, 6)) {
        foreach ($base in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
            if ($base) {
                $candidate = Join-Path $base "Inno Setup $version/ISCC.exe"
                if (Test-Path -LiteralPath $candidate) { Get-Item -LiteralPath $candidate }
            }
        }
    }) | Select-Object -First 1
}
if (-not $iscc) { throw 'ISCC.exe not found' }
$compiler = if ($iscc -is [System.Management.Automation.ApplicationInfo]) { $iscc.Source } else { $iscc.FullName }
$root = Join-Path ([IO.Path]::GetTempPath()) ('msime dev trust ' + [Guid]::NewGuid())
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $certificate = Join-Path $root 'certificate.cer'
    $importer = Join-Path $root 'import-stub.ps1'
    $record = Join-Path $root 'called.txt'
    [IO.File]::WriteAllText($certificate, 'isolated probe certificate')
    $digest = (Get-FileHash -LiteralPath $certificate -Algorithm SHA256).Hash
    foreach ($case in @('success', 'mismatch', 'import-failure')) {
        $exitCode = if ($case -eq 'import-failure') { 17 } else { 0 }
        $hash = if ($case -eq 'mismatch') { '0' * 64 } else { $digest }
        $stub = @'
param([string]$CertificatePath, [switch]$Keep)
if (-not $Keep -or -not (Test-Path -LiteralPath $CertificatePath)) { exit 23 }
[IO.File]::WriteAllText('__RECORD__', 'called')
exit __EXIT__
'@
        [IO.File]::WriteAllText($importer, $stub.Replace('__RECORD__', $record.Replace("'", "''")).Replace('__EXIT__', [string]$exitCode))
        Remove-Item -LiteralPath $record -ErrorAction SilentlyContinue
        & $compiler "/O$root" "/DDevCertificate=$certificate" "/DDevCertificateSha256=$hash" "/DProbeScript=$importer" (Join-Path $PSScriptRoot 'dev-trust-probe.iss') | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Cannot compile development certificate probe' }
        $log = Join-Path $root "$case.log"
        & (Join-Path $root 'dev-trust-probe.exe') '/VERYSILENT' '/SUPPRESSMSGBOXES' "/LOG=$log" | Out-Null
        $text = [IO.File]::ReadAllText($log)
        if ($case -eq 'success' -and (-not (Test-Path -LiteralPath $record) -or $text -notmatch 'certificate is trusted')) {
            throw 'Embedded importer did not run successfully'
        }
        if ($case -eq 'mismatch' -and ((Test-Path -LiteralPath $record) -or $text -notmatch 'SHA256 mismatch')) {
            throw 'Certificate mismatch did not stop before invoking the importer'
        }
        if ($case -eq 'import-failure' -and $text -notmatch 'import failed: 17') {
            throw 'Importer failure did not abort bootstrap'
        }
        $global:LASTEXITCODE = 0
    }
    Write-Host 'Development installer imports the pinned certificate and rejects mismatches and importer failures.'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
