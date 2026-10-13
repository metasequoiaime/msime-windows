param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$ExpectedSha256)
$ErrorActionPreference = 'Stop'
$actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $ExpectedSha256.ToLowerInvariant()) { throw "Test certificate SHA256 mismatch: $actual" }
$cert = Import-Certificate -FilePath $Path -CertStoreLocation Cert:\CurrentUser\Root
if (-not $cert) { throw 'Could not import the SignPath test certificate.' }
"thumbprint=$($cert.Thumbprint)" >> $env:GITHUB_OUTPUT
Write-Host "Trusted SignPath test certificate $($cert.Thumbprint) for this runner only."
