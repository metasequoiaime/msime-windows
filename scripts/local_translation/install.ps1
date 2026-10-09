param(
    [Parameter(Mandatory = $true)][string]$Python,
    [string]$InstallDirectory = (Join-Path $env:LOCALAPPDATA 'MetasequoiaLocalTranslation'),
    [string]$ImeConfig,
    [ValidateRange(0.01, 5)][double]$CpuPercent = 1,
    [ValidateRange(1024, 65535)][int]$Port = 1188,
    [switch]$AutoStart
)
$ErrorActionPreference = 'Stop'

# Run from an ordinary user PowerShell, outside a packaged app's virtualized profile.
$InstallDirectory = [IO.Path]::GetFullPath($InstallDirectory)
if (-not $ImeConfig) {
    $dataDirectory = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Metasequoia\MetasequoiaIME').DataDir
    $ImeConfig = Join-Path $dataDirectory 'config.toml'
}
if (-not (Test-Path -LiteralPath $ImeConfig -PathType Leaf)) { throw 'Install the IME before configuring translation.' }
& $Python -c 'import sys; assert sys.version_info[:2] == (3, 12), "Use 64-bit Python 3.12"; assert sys.maxsize > 2**32'
if ($LASTEXITCODE -ne 0) { throw 'A 64-bit Python 3.12 installation is required.' }

New-Item -ItemType Directory -Force -Path $InstallDirectory | Out-Null
$appDirectory = Join-Path $InstallDirectory 'app'
$configPath = Join-Path $InstallDirectory 'service.json'
if (Test-Path -LiteralPath $configPath) {
    throw 'This installation already exists. Use control.ps1 to start/stop it; keep its token and model generation.'
}
New-Item -ItemType Directory -Force -Path $appDirectory | Out-Null
Get-ChildItem -LiteralPath $PSScriptRoot -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $appDirectory
}
$venvDirectory = Join-Path $InstallDirectory 'venv'
& $Python -m venv $venvDirectory
if ($LASTEXITCODE -ne 0) { throw 'Could not create the isolated Python runtime.' }
$servicePython = Join-Path $venvDirectory 'Scripts\python.exe'
& $servicePython -m pip install --disable-pip-version-check -r (Join-Path $appDirectory 'requirements.txt')
if ($LASTEXITCODE -ne 0) { throw 'Runtime dependency installation failed.' }
& $servicePython -X utf8 (Join-Path $appDirectory 'fetch_models.py') --root (Join-Path $InstallDirectory 'models')
if ($LASTEXITCODE -ne 0) { throw 'Model verification failed.' }

$random = New-Object byte[] 32
$rng = [Security.Cryptography.RandomNumberGenerator]::Create()
try { $rng.GetBytes($random) } finally { $rng.Dispose() }
$token = [Convert]::ToBase64String($random)
$config = @{models = (Join-Path $InstallDirectory 'models'); cpu_percent = $CpuPercent; port = $Port; token = $token}
$utf8 = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText($configPath, ($config | ConvertTo-Json), $utf8)

& (Join-Path $appDirectory 'control.ps1') -InstallDirectory $InstallDirectory -Action Start
& $servicePython -X utf8 (Join-Path $appDirectory 'configure.py') --ime-config $ImeConfig --service-config $configPath --backup-directory (Join-Path $InstallDirectory 'backups')
if ($LASTEXITCODE -ne 0) { throw 'IME configuration failed; its original configuration has not been overwritten.' }

if ($AutoStart) {
    $pythonw = Join-Path $venvDirectory 'Scripts\pythonw.exe'
    $serviceScript = Join-Path $appDirectory 'service.py'
    $command = '"' + $pythonw + '" "' + $serviceScript + '" --config "' + $configPath + '"'
    $runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    if (-not (Test-Path -LiteralPath $runKey)) { New-Item -Path $runKey | Out-Null }
    New-ItemProperty -Path $runKey -Name 'MetasequoiaLocalTranslation' -PropertyType String -Value $command -Force | Out-Null
}

$pipe = New-Object IO.Pipes.NamedPipeClientStream('.', 'FanyImeAuxNamedPipe', [IO.Pipes.PipeDirection]::Out)
try {
    $pipe.Connect(2500)
    $bytes = [Text.Encoding]::Unicode.GetBytes('ConfigChanged')
    $pipe.Write($bytes, 0, $bytes.Length)
    $pipe.Flush()
} catch {
    Write-Warning 'Translation is configured. Restart the IME if its server was not running.'
} finally { $pipe.Dispose() }
Write-Output ('Ready: http://127.0.0.1:' + $Port + '/translate; CPU hard cap ' + $CpuPercent + '%; GPU unused.')
