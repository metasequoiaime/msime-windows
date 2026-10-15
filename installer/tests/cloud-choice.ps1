$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Join-Path ([IO.Path]::GetTempPath()) ('msime-cloud-choice-' + [Guid]::NewGuid().ToString('N'))
$script = Join-Path $PSScriptRoot 'cloud-choice-probe.iss'
$binary = Join-Path $root 'cloud-choice-probe.exe'
New-Item -ItemType Directory -Force $root | Out-Null

try {
    & ISCC.exe "/O$root" $script | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $binary)) {
        throw 'Cannot compile cloud-choice probe'
    }

    $cases = @(
        @{ Name = 'fresh-default'; Previous = ''; Target = ''; Choice = '0'; Expected = 'false' },
        @{ Name = 'fresh-moved'; Previous = ''; Target = ''; Choice = '0'; Expected = 'false' },
        @{ Name = 'target-existing'; Previous = ''; Target = 'true'; Choice = '0'; Expected = 'true' },
        @{ Name = 'upgrade-moved'; Previous = 'true'; Target = ''; Choice = '0'; Expected = 'true' },
        @{ Name = 'upgrade-in-place'; Previous = 'false'; Target = 'false'; Choice = '1'; Expected = 'false' }
    )

    foreach ($case in $cases) {
        $caseRoot = Join-Path $root $case.Name
        $previous = Join-Path $caseRoot 'previous'
        $target = if ($case.Name -eq 'upgrade-in-place') { $previous } else { Join-Path $caseRoot 'target' }
        New-Item -ItemType Directory -Force $previous, $target | Out-Null
        if ($case.Previous) {
            [IO.File]::WriteAllText((Join-Path $previous 'config.toml'), "[general]`ncloud_candidates = $($case.Previous)`n")
        }
        if ($case.Target) {
            [IO.File]::WriteAllText((Join-Path $target 'config.toml'), "[general]`ncloud_candidates = $($case.Target)`n")
        }
        $app = Join-Path $caseRoot 'app'
        & $binary '/VERYSILENT' '/SUPPRESSMSGBOXES' '/NORESTART' "/DIR=$app" "/TARGET=$target" "/PREVIOUS=$previous" "/CLOUD=$($case.Choice)" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "$($case.Name): setup failed ($LASTEXITCODE)" }
        $config = [IO.File]::ReadAllText((Join-Path $target 'config.toml'))
        if ($config -notmatch "(?m)^cloud_candidates = $($case.Expected)\r?$") {
            $found = [regex]::Match($config, '(?m)^cloud_candidates[^\r\n]*').Value
            throw "$($case.Name): expected $($case.Expected), found [$found]"
        }
        if ($case.Name -eq 'fresh-default' -and $config -notmatch '云候选') {
            throw 'Fresh config lost UTF-8 text'
        }
    }
    Write-Host 'Cloud choice probe passed five install scenarios'
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
