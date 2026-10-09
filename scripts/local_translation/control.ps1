param(
    [string]$InstallDirectory = (Join-Path $env:LOCALAPPDATA 'MetasequoiaLocalTranslation'),
    [ValidateSet('Start', 'Stop', 'Status', 'DisableAutoStart')][string]$Action = 'Status'
)
$ErrorActionPreference = 'Stop'
$configPath = Join-Path $InstallDirectory 'service.json'
$config = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
$base = 'http://127.0.0.1:' + $config.port
$headers = @{Authorization = ('Bearer ' + $config.token)}

if ($Action -eq 'DisableAutoStart') {
    $runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    if (Get-ItemProperty -LiteralPath $runKey -Name 'MetasequoiaLocalTranslation' -ErrorAction SilentlyContinue) {
        Remove-ItemProperty -LiteralPath $runKey -Name 'MetasequoiaLocalTranslation'
    }
    Write-Output 'Login startup disabled; files and IME configuration retained.'
    return
}
if ($Action -eq 'Stop') {
    Invoke-RestMethod -Uri ($base + '/shutdown') -Method Post -Headers $headers -ContentType 'application/json' -Body '{}' -TimeoutSec 5 | Out-Null
    Write-Output 'Local translation service stopped.'
    return
}
if ($Action -eq 'Status') {
    Invoke-RestMethod -Uri ($base + '/health') -Headers $headers -TimeoutSec 5
    return
}

try {
    $health = Invoke-RestMethod -Uri ($base + '/health') -Headers $headers -TimeoutSec 2
    if ($health.status -eq 'ready') { Write-Output 'Local translation service already running.'; return }
} catch { }
$pythonw = Join-Path $InstallDirectory 'venv\Scripts\pythonw.exe'
$serviceScript = Join-Path $InstallDirectory 'app\service.py'
$arguments = '"' + $serviceScript + '" --config "' + $configPath + '"'
$process = Start-Process -FilePath $pythonw -ArgumentList $arguments -WindowStyle Hidden -PassThru
$deadline = [DateTime]::UtcNow.AddSeconds(90)
do {
    if ($process.HasExited) { throw 'Local translator exited during startup; check the CPU cap and model installation.' }
    try {
        $health = Invoke-RestMethod -Uri ($base + '/health') -Headers $headers -TimeoutSec 2
        if ($health.status -eq 'ready') {
            Write-Output ('Local translation service ready; CPU hard cap ' + $health.cpu_limit_percent + '%.')
            return
        }
    } catch { }
    Start-Sleep -Milliseconds 500
} while ([DateTime]::UtcNow -lt $deadline)
throw 'Local translation service did not become ready within 90 seconds.'
