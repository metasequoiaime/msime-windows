param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$ExpectedSha256)
$ErrorActionPreference = 'Stop'
$actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $ExpectedSha256.ToLowerInvariant()) { throw "Test certificate SHA256 mismatch: $actual" }
$result = & certutil.exe -user -addstore Root $Path 2>&1
if ($LASTEXITCODE -ne 0) { throw "Could not import the SignPath test certificate: $result" }
$cert = Get-ChildItem Cert:\CurrentUser\Root | Where-Object Thumbprint -eq ((Get-PfxCertificate -FilePath $Path).Thumbprint) | Select-Object -First 1
if (-not $cert) { throw 'Could not find the imported SignPath test certificate.' }
"thumbprint=$($cert.Thumbprint)" >> $env:GITHUB_OUTPUT
Write-Host "Trusted SignPath test certificate $($cert.Thumbprint) for this runner only."
