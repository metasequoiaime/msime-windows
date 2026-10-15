param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$ExpectedSha256)
$ErrorActionPreference = 'Stop'
$actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $ExpectedSha256.ToLowerInvariant()) { throw "Test certificate SHA256 mismatch: $actual" }
$cert = Get-PfxCertificate -FilePath $Path
if (-not $cert) { throw 'Could not read the SignPath test certificate.' }
"thumbprint=$($cert.Thumbprint)" >> $env:GITHUB_OUTPUT
Write-Host "Pinned SignPath test certificate $($cert.Thumbprint) for signer verification."
