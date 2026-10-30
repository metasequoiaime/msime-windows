# 旧版把效果档神经模型和词格语言模型 sc.lm 装在数据目录根下，新版只认 models\<目录>\，覆盖安装的
# 清理又会把根下那份当退役文件删掉。AdoptRetiredModel 在清理之前把它们挪过去；这里把那段 Pascal
# 原样抠出来，编进探针安装器真跑。期望的 SHA256 是参数，所以用小文件代替真模型，真模型的哈希单独
# 和两份 lock.json 对一遍。
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

$work = Join-Path ([IO.Path]::GetTempPath()) ('msime-retired-models-' + [Guid]::NewGuid())
$desktop = 'sentence-model-desktop.safetensors'
$lattice = 'sc.lm'
function Write-Text([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Force -Path (Split-Path $Path) | Out-Null
    [IO.File]::WriteAllText($Path, $Text)
}
function Get-TextHash([string]$Text) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
    -join ([Security.Cryptography.SHA256]::HashData($bytes) | ForEach-Object { $_.ToString('x2') })
}
function Invoke-Probe([string]$DataDir) {
    $env:MSIME_PROBE_DATA_DIR = $DataDir
    try {
        $process = Start-Process $probeExe -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES' -PassThru -Wait
    } finally {
        Remove-Item Env:MSIME_PROBE_DATA_DIR
    }
    if (-not (Test-Path (Join-Path $DataDir 'ran.txt'))) { throw "Probe did not run (exit code $($process.ExitCode))" }
}
try {
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $iss = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '../msime_setup.iss'))
    $procedure = [regex]::Match($iss, '(?s)procedure AdoptRetiredModel\(.*?\r?\nend;').Value
    if (-not $procedure) { throw 'msime_setup.iss is missing AdoptRetiredModel' }

    # 两处调用都必须在轻量/完整包分叉之前，哈希必须是 lock.json 里那份发布文件的。
    $calls = [regex]::Match($iss, '(?s)BackupChangedBundledSkins;(.*?)\r?\n#ifdef LightPackage')
    if (-not $calls.Success) { throw 'AdoptRetiredModel must run before the light/full cleanup split' }
    $repoRoot = Join-Path $PSScriptRoot '../..'
    $neuralLock = Get-Content (Join-Path $repoRoot 'neural-model/lock.json') -Raw | ConvertFrom-Json
    $latticeLock = Get-Content (Join-Path $repoRoot 'language-model/lock.json') -Raw | ConvertFrom-Json
    $expectedCalls = @(
        @{ File = $desktop; Dir = 'neural-desktop'; Notice = ''; Sha = ($neuralLock.models | Where-Object file -eq $desktop).sha256 },
        @{ File = $lattice; Dir = 'language-model'; Notice = 'libime-lm-NOTICE.md'; Sha = $latticeLock.output.sha256 }
    )
    foreach ($call in $expectedCalls) {
        $pattern = "AdoptRetiredModel\('$([regex]::Escape($call.File))',\s*'$($call.Dir)',\s*'$([regex]::Escape($call.Notice))',\s*'$($call.Sha)'\);"
        if ($calls.Groups[1].Value -notmatch $pattern) { throw "Missing or stale AdoptRetiredModel call for $($call.File) (expected $($call.Sha))" }
    }

    $desktopBytes = 'model-v1 bytes'
    $latticeBytes = 'libime sc.lm bytes'
    $probe = Join-Path $work 'probe.iss'
    @"
[Setup]
AppName=Retired model probe
AppVersion=1.0
CreateAppDir=no
PrivilegesRequired=lowest
Uninstallable=no
OutputDir=$work
OutputBaseFilename=retired-model-probe
[Code]
function GetDataDir(Param: String): String;
begin
  Result := GetEnv('MSIME_PROBE_DATA_DIR');
end;

$procedure

function InitializeSetup: Boolean;
begin
  AdoptRetiredModel('$desktop', 'neural-desktop', '', '$(Get-TextHash $desktopBytes)');
  AdoptRetiredModel('$lattice', 'language-model', 'libime-lm-NOTICE.md', '$(Get-TextHash $latticeBytes)');
  SaveStringToFile(AddBackslash(GetDataDir('')) + 'ran.txt', 'ok', False);
  Result := False;
end;
"@ | Set-Content -Path $probe -Encoding UTF8
    & (Resolve-Iscc) /Q $probe
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed to compile the probe (exit code $LASTEXITCODE)" }
    $probeExe = Join-Path $work 'retired-model-probe.exe'

    # 升级用户：根下有两个模型和 sc.lm 的授权声明 -> 都挪到 models\ 下，声明改名 NOTICE.md。
    $upgrade = Join-Path $work 'upgrade'
    Write-Text (Join-Path $upgrade $desktop) $desktopBytes
    Write-Text (Join-Path $upgrade $lattice) $latticeBytes
    Write-Text (Join-Path $upgrade 'libime-lm-NOTICE.md') 'LGPL notice'
    Invoke-Probe $upgrade
    foreach ($left in @($desktop, $lattice, 'libime-lm-NOTICE.md')) {
        if (Test-Path (Join-Path $upgrade $left)) { throw "$left was left in the data directory root" }
    }
    if (-not (Test-Path (Join-Path $upgrade "models\neural-desktop\$desktop"))) { throw 'Desktop model was not moved to models\neural-desktop' }
    if (-not (Test-Path (Join-Path $upgrade "models\language-model\$lattice"))) { throw 'sc.lm was not moved to models\language-model' }
    if ([IO.File]::ReadAllText((Join-Path $upgrade 'models\language-model\NOTICE.md')) -ne 'LGPL notice') { throw 'sc.lm notice was not moved next to the model' }
    if (Test-Path (Join-Path $upgrade 'models\neural-desktop\NOTICE.md')) { throw 'Desktop model got a notice it never shipped with' }

    # 已经下载过：不覆盖新位置那份，根下的留给清理删。
    $downloaded = Join-Path $work 'downloaded'
    Write-Text (Join-Path $downloaded $lattice) $latticeBytes
    Write-Text (Join-Path $downloaded "models\language-model\$lattice") 'downloaded copy'
    Invoke-Probe $downloaded
    if ([IO.File]::ReadAllText((Join-Path $downloaded "models\language-model\$lattice")) -ne 'downloaded copy') {
        throw 'An already downloaded sc.lm was overwritten'
    }

    # 校验和不对：不挪，不能把坏文件当成可用模型。
    $mismatch = Join-Path $work 'mismatch'
    Write-Text (Join-Path $mismatch $desktop) 'something else'
    Write-Text (Join-Path $mismatch $lattice) 'something else'
    Invoke-Probe $mismatch
    if (Test-Path (Join-Path $mismatch 'models')) { throw 'A mismatched file was adopted as a model' }

    Write-Host 'Retired models: desktop model and sc.lm moved on upgrade, existing downloads kept, mismatched files ignored'
} finally {
    if (Test-Path $work) { Remove-Item $work -Recurse -Force }
}
