$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Resolve-Iscc {
    $command = Get-Command 'ISCC.exe' -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Source }
    foreach ($version in @(7, 6)) {
        foreach ($root in @(${env:ProgramFiles(x86)}, $env:ProgramFiles, (Join-Path $env:LOCALAPPDATA 'Programs'))) {
            if (-not $root) { continue }
            $candidate = Join-Path $root "Inno Setup $version\ISCC.exe"
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
        }
    }
    throw '找不到 ISCC.exe'
}

function Get-FileDigest([string]$Path) {
    [Convert]::ToBase64String([IO.File]::ReadAllBytes($Path))
}

$root = Join-Path ([IO.Path]::GetTempPath()) ('msime-cloud-choice-' + [Guid]::NewGuid().ToString('N'))
$script = Join-Path $PSScriptRoot 'cloud-choice-probe.iss'
$binary = Join-Path $root 'cloud-choice-probe.exe'
New-Item -ItemType Directory -Force $root | Out-Null

try {
    & (Resolve-Iscc) "/O$root" $script | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $binary)) {
        throw 'Cannot compile cloud-choice probe'
    }

    # SameDir：目标就是原目录（默认位置首次安装或原地升级）。
    # Fail：安装应当停止，两份配置保持原样，日志里有 Message。
    # Lock：安装期间独占打开原目录的 config.toml，让迁移失败。
    $cases = @(
        @{ Name = 'fresh-default'; SameDir = $true; Previous = ''; Target = ''; Choice = '0'; Expected = 'false' },
        @{ Name = 'fresh-moved'; SameDir = $false; Previous = ''; Target = ''; Choice = '0'; Expected = 'false' },
        @{ Name = 'target-existing'; SameDir = $false; Previous = ''; Target = 'true'; Choice = '0'; Expected = 'true' },
        @{ Name = 'upgrade-moved'; SameDir = $false; Previous = 'true'; Target = ''; Choice = '0'; Expected = 'true' },
        @{ Name = 'upgrade-in-place'; SameDir = $true; Previous = 'false'; Target = ''; Choice = '1'; Expected = 'false' },
        @{ Name = 'migration-retry'; SameDir = $false; Previous = 'false'; Target = 'false'; Choice = '1'; Expected = 'false' },
        @{ Name = 'migration-conflict'; SameDir = $false; Previous = 'false'; Target = 'true'; Choice = '0'; Fail = '目标数据目录已有 config.toml' },
        @{ Name = 'migration-locked'; SameDir = $false; Previous = 'false'; Target = ''; Choice = '1'; Fail = '无法把用户配置'; Lock = $true }
    )

    foreach ($case in $cases) {
        $caseRoot = Join-Path $root $case.Name
        $previous = Join-Path $caseRoot 'previous'
        $target = if ($case.SameDir) { $previous } else { Join-Path $caseRoot 'target' }
        New-Item -ItemType Directory -Force $previous, $target | Out-Null
        $oldConfig = Join-Path $previous 'config.toml'
        $targetConfig = Join-Path $target 'config.toml'
        if ($case.Previous) {
            [IO.File]::WriteAllText($oldConfig, "[general]`ncloud_candidates = $($case.Previous)`n")
        }
        if ($case.Target) {
            [IO.File]::WriteAllText($targetConfig, "[general]`ncloud_candidates = $($case.Target)`n")
        }
        $app = Join-Path $caseRoot 'app'
        $installLog = Join-Path $caseRoot 'install.log'
        $before = @{}
        foreach ($path in @($oldConfig, $targetConfig)) {
            if (Test-Path -LiteralPath $path) { $before[$path] = Get-FileDigest $path }
        }

        $lock = $null
        if ($case.ContainsKey('Lock')) {
            $lock = [IO.File]::Open($oldConfig, 'Open', 'Read', 'None')
        }
        try {
            & $binary '/VERYSILENT' '/SUPPRESSMSGBOXES' '/NORESTART' "/DIR=$app" "/TARGET=$target" "/PREVIOUS=$previous" "/CLOUD=$($case.Choice)" "/LOG=$installLog" | Out-Null
            $exitCode = $LASTEXITCODE
        }
        finally {
            if ($lock) { $lock.Dispose() }
        }

        if ($case.ContainsKey('Fail')) {
            if ($exitCode -eq 0) { throw "$($case.Name): setup did not stop" }
            foreach ($path in @($oldConfig, $targetConfig)) {
                $now = if (Test-Path -LiteralPath $path) { Get-FileDigest $path } else { $null }
                if ($now -ne $before[$path]) { throw "$($case.Name): $path changed" }
            }
            if (-not ([IO.File]::ReadAllText($installLog).Contains($case.Fail))) {
                throw "$($case.Name): log has no '$($case.Fail)'"
            }
            $global:LASTEXITCODE = 0
            continue
        }
        if ($exitCode -ne 0) { throw "$($case.Name): setup failed ($exitCode)" }
        if (-not $case.SameDir -and (Test-Path -LiteralPath $oldConfig)) {
            throw "$($case.Name): previous config was not moved"
        }
        $config = [IO.File]::ReadAllText($targetConfig)
        if ($config -notmatch "(?m)^cloud_candidates = $($case.Expected)\r?$") {
            $found = [regex]::Match($config, '(?m)^cloud_candidates[^\r\n]*').Value
            throw "$($case.Name): expected $($case.Expected), found [$found]"
        }
        if ($case.Name -like 'fresh-*' -and $config -notmatch '云候选') {
            throw "$($case.Name): fresh config lost UTF-8 text"
        }
    }
    Write-Host "Cloud choice probe passed $($cases.Count) install scenarios"
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
