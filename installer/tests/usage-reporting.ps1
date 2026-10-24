$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# 匿名使用统计的安装选项（installer/usage_reporting_choice.iss）编进探针安装器真跑一遍。
# 只有显式记下 true 才发送：首装和没选过的升级都要问；已经选过的升级跳过询问页并保留选择；静默安装只认 /USAGEREPORTING=，没给就什么都不记。

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

$root = Join-Path ([IO.Path]::GetTempPath()) ('msime-usage-reporting-' + [Guid]::NewGuid().ToString('N'))
$script = Join-Path $PSScriptRoot 'usage-reporting-probe.iss'
$binary = Join-Path $root 'usage-reporting-probe.exe'
New-Item -ItemType Directory -Force $root | Out-Null

try {
    & (Resolve-Iscc) "/O$root" $script | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $binary)) {
        throw 'Cannot compile usage-reporting probe'
    }

    $undecided = "[general]`ncloud_candidates = true`n"
    $asked = "[general]`ncloud_candidates = true`nusage_reporting = `"ask`"`n"
    # Previous：安装前原目录的 config.toml，空表示首装。Moved：升级时换了数据目录。
    # Interactive / Page：模拟在询问页上看到并提交的勾选状态。Switch：/USAGEREPORTING= 的值。
    # Expected：安装后 usage_reporting 的值；Page 列是询问页该不该显示；Unchanged：配置一个字节都不能变。
    $cases = @(
        @{ Name = 'fresh-silent'; Previous = ''; Expected = '"ask"'; Shown = $true },
        @{ Name = 'fresh-accept'; Previous = ''; Interactive = $true; Page = '1'; Expected = 'true'; Shown = $true },
        @{ Name = 'fresh-decline'; Previous = ''; Interactive = $true; Page = '0'; Expected = 'false'; Shown = $true },
        @{ Name = 'fresh-switch-on'; Previous = ''; Switch = '1'; Expected = 'true'; Shown = $false },
        @{ Name = 'fresh-switch-off'; Previous = ''; Switch = '0'; Expected = 'false'; Shown = $false },
        @{ Name = 'fresh-bad-switch'; Previous = ''; Switch = 'maybe'; Expected = '"ask"'; Shown = $true },
        @{ Name = 'upgrade-no-key-accept'; Previous = $undecided; Interactive = $true; Page = '1'; Expected = 'true'; Shown = $true },
        @{ Name = 'upgrade-ask-decline'; Previous = $asked; Interactive = $true; Page = '0'; Expected = 'false'; Shown = $true },
        @{ Name = 'upgrade-no-key-silent'; Previous = $undecided; Expected = $null; Shown = $true; Unchanged = $true },
        @{ Name = 'upgrade-declined-kept'; Previous = "[general]`nusage_reporting = false`n"; Interactive = $true; Page = '1'; Expected = 'false'; Shown = $false; Unchanged = $true },
        @{ Name = 'upgrade-accepted-kept'; Previous = "[general]`nusage_reporting = true # chosen`n"; Expected = 'true # chosen'; Shown = $false; Unchanged = $true },
        @{ Name = 'upgrade-switch-overrides'; Previous = "[general]`nusage_reporting = true`n"; Switch = '0'; Expected = 'false'; Shown = $false },
        @{ Name = 'upgrade-moved-declined-kept'; Previous = "[general]`nusage_reporting = false`n"; Moved = $true; Interactive = $true; Page = '1'; Expected = 'false'; Shown = $false; Unchanged = $true },
        @{ Name = 'upgrade-moved-no-key-accept'; Previous = $undecided; Moved = $true; Interactive = $true; Page = '1'; Expected = 'true'; Shown = $true },
        @{ Name = 'upgrade-no-general-section'; Previous = "[appearance]`ntheme_mode = `"dark`"`n"; Interactive = $true; Page = '1'; Expected = 'true'; Shown = $true }
    )

    foreach ($case in $cases) {
        $caseRoot = Join-Path $root $case.Name
        $previous = Join-Path $caseRoot 'previous'
        $moved = $case.ContainsKey('Moved') -and $case.Moved
        $target = if ($moved) { Join-Path $caseRoot 'target' } else { $previous }
        New-Item -ItemType Directory -Force $previous, $target | Out-Null
        $oldConfig = Join-Path $previous 'config.toml'
        $targetConfig = Join-Path $target 'config.toml'
        if ($case.Previous) { [IO.File]::WriteAllText($oldConfig, $case.Previous) }

        $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/DIR=$(Join-Path $caseRoot 'app')",
            "/TARGET=$target", "/PREVIOUS=$(if ($case.Previous) { $previous } else { '' })", "/LOG=$(Join-Path $caseRoot 'install.log')")
        if ($case.ContainsKey('Interactive') -and $case.Interactive) { $arguments += '/INTERACTIVE=1'; $arguments += "/PAGE=$($case.Page)" }
        if ($case.ContainsKey('Switch')) { $arguments += "/USAGEREPORTING=$($case.Switch)" }
        & $binary @arguments | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "$($case.Name): setup failed ($LASTEXITCODE)" }

        $log = [IO.File]::ReadAllText((Join-Path $caseRoot 'install.log'))
        $expectedPage = if ($case.Shown) { 'Usage reporting page: shown' } else { 'Usage reporting page: skipped' }
        if (-not $log.Contains($expectedPage)) { throw "$($case.Name): expected '$expectedPage'" }

        if ($moved -and (Test-Path -LiteralPath $oldConfig)) { throw "$($case.Name): previous config was not moved" }
        $config = [IO.File]::ReadAllText($targetConfig)
        if ($case.ContainsKey('Unchanged') -and $case.Unchanged -and $config -ne $case.Previous) {
            throw "$($case.Name): config changed although a decision existed or none was given"
        }
        $lines = [regex]::Matches($config, '(?m)^usage_reporting\s*=\s*([^\r\n]*)\r?$')
        if ($null -eq $case.Expected) {
            if ($lines.Count -ne 0) { throw "$($case.Name): expected no usage_reporting, found [$($lines[0].Value)]" }
            continue
        }
        if ($lines.Count -ne 1) { throw "$($case.Name): expected one usage_reporting line, found $($lines.Count)" }
        $value = $lines[0].Groups[1].Value.Trim()
        if ($value -ne $case.Expected) { throw "$($case.Name): expected $($case.Expected), found [$value]" }
        if ($case.Name -like 'fresh-*' -and $config -notmatch '匿名使用统计') {
            throw "$($case.Name): fresh config lost UTF-8 text"
        }
        if ($case.Name -eq 'upgrade-no-general-section' -and $config -notmatch '(?m)^\[general\]\r?\nusage_reporting = true\r?$') {
            throw "$($case.Name): [general] section was not appended"
        }
        if ($case.Name -eq 'upgrade-no-key-accept' -and $config -notmatch '(?m)^\[general\]\r?\nusage_reporting = true\r?$') {
            throw "$($case.Name): decision was not inserted under [general]"
        }
    }
    Write-Host "Usage reporting probe passed $($cases.Count) install scenarios"
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
