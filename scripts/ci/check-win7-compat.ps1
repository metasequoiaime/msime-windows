# Static Windows 7 compatibility audit for a -DMSIME_WIN7=ON Server build. There is no real
# Windows 7 device in the loop, so this script is the verification net: it reads the PE headers
# and the import tables of every binary the installer will ship and fails the job when a
# binary could not load on Windows 7 SP1. Three gates:
#
#   1. Subsystem/OS version: the loader on Windows 7 refuses PE images whose minimum OS version
#      is above 6.01 (the "_WIN32_WINNT=0x0601" build must also link with a matching subsystem
#      version; MSVC writes 6.01 from the macros, but a stray default would go unnoticed here).
#   2. Imports: DLLs that do not exist on Windows 7 (shcore, dcomp, runtimeobject, CoreMessaging,
#      windowsapp's api sets behind WinRT) must not appear in any import table.
#   3. WebSocket: WinHttpWebSocket* arrives with Windows 8.1; a static import means the Doubao
#      ASR client leaked back into the Win7 target.
#
# dumpbin ships with the MSVC toolchain the CI job has already put on PATH (msvc-dev-cmd).
param([Parameter(Mandatory)][string]$BuildDir)

$ErrorActionPreference = 'Stop'

$binaries = @(Get-ChildItem -LiteralPath $BuildDir -Recurse -File -Include '*.exe', '*.dll' |
    Where-Object { $_.BaseName -notlike '*Tests' -and $_.BaseName -notlike 'test_*' })
if ($binaries.Count -eq 0) {
    throw "No exe/dll found under $BuildDir"
}

# DLLs that do not exist on Windows 7 SP1. dwmapi (Vista+) is fine; shcore (8.1), dcomp (8),
# runtimeobject / CoreMessaging (WinRT, 10) are not.
$forbiddenDlls = @('SHCORE.dll', 'DCOMP.dll', 'RUNTIMEOBJECT.dll', 'CoreMessaging.dll', 'WINDOWSAPP.dll')
# WinHTTP WebSocket entry points (Windows 8.1+). A hit means the Doubao ASR client is back.
$forbiddenSymbols = @('WinHttpWebSocket')

$failures = @()
foreach ($binary in $binaries) {
    Write-Host "== $($binary.Name)"

    $headers = & dumpbin /nologo /headers $binary.FullName
    # dumpbin prints the value before the label ("6.01 operating system version").
    # The loader on Windows 7 consults both optional-header minimum versions and
    # refuses images above 6.01.
    $osVersion = [version]($headers | Select-String '(\d+\.\d+)\s+operating system version' |
        ForEach-Object { $_.Matches[0].Groups[1].Value })
    $subsystemVersion = [version]($headers | Select-String '(\d+\.\d+)\s+subsystem version' |
        ForEach-Object { $_.Matches[0].Groups[1].Value })
    if (-not $osVersion -or -not $subsystemVersion) {
        $failures += "$($binary.Name): could not parse the PE version fields"
        continue
    }
    Write-Host "   os $osVersion, subsystem $subsystemVersion"
    if ($osVersion.Major -gt 6 -or ($osVersion.Major -eq 6 -and $osVersion.Minor -gt 1)) {
        $failures += "$($binary.Name): minimum OS version $osVersion is above 6.01 (Windows 7 SP1)"
    }
    if ($subsystemVersion.Major -gt 6 -or ($subsystemVersion.Major -eq 6 -and $subsystemVersion.Minor -gt 1)) {
        $failures += "$($binary.Name): subsystem version $subsystemVersion is above 6.01 (Windows 7 SP1)"
    }

    $imports = & dumpbin /nologo /imports $binary.FullName
    foreach ($dll in $forbiddenDlls) {
        if ($imports -match "^\s+$([regex]::Escape($dll))\s*$" -or $imports -match [regex]::Escape($dll)) {
            $failures += "$($binary.Name): imports $dll, which does not exist on Windows 7"
        }
    }
    foreach ($symbol in $forbiddenSymbols) {
        if ($imports -match $symbol) {
            $failures += "$($binary.Name): imports $symbol* (Windows 8.1+ WebSocket API)"
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host "`nWindows 7 compatibility audit failed:"
    $failures | ForEach-Object { Write-Host "  - $_" }
    throw "$($failures.Count) Windows 7 compatibility failure(s)"
}
Write-Host "`nWindows 7 compatibility audit passed for $($binaries.Count) binaries."
