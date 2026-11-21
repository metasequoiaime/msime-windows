# Confirm the build produced the DLL and PDB the installer collects.
#
# Reading a version back off the DLL would not work: the VERSIONINFO block in
# windows/src/IME/MetasequoiaIME.rc is named IDR_VERSION2, which is not defined anywhere, rather than the
# VS_VERSION_INFO the Windows version API looks for, so GetFileVersionInfo finds nothing and the
# DLL reports an empty FileVersion. apply_version.py --check covers the version instead.
#
# -Machine asserts the PE machine type, which is what keeps an Arm64 leg from quietly shipping an
# x64 or x86 DLL. It cannot prove an Arm64X file really carries both halves: the container reports
# the native Arm64 machine in its COFF header on purpose, so that a loader that has never heard of
# Arm64X still maps the Arm64 half. -NativeArm64Dll adds that proof by requirement instead: an
# Arm64X file embeds the whole Arm64 image, so it must be strictly larger than the Arm64-only build
# of the same sources.
param(
    [Parameter(Mandatory)][string]$BuildDir,
    [ValidateSet('x86', 'x64', 'arm64')][string]$Machine,
    [string]$NativeArm64Dll
)

$ErrorActionPreference = 'Stop'

$dll = Join-Path $BuildDir 'Release/MetasequoiaImeTsf.dll'
$pdb = Join-Path $BuildDir 'Release/MetasequoiaImeTsf.pdb'
if (-not (Test-Path $dll)) { throw "The build did not produce $dll" }
if (-not (Test-Path $pdb)) { throw "The build did not produce $pdb" }

$expectedMachines = @{ x86 = 0x014C; x64 = 0x8664; arm64 = 0xAA64 }
$names = @{ 0x014C = 'x86'; 0x8664 = 'x64'; 0xAA64 = 'arm64' }

if ($Machine) {
    $bytes = [IO.File]::ReadAllBytes($dll)
    if ($bytes.Length -lt 0x40) { throw "$dll is too small to hold a PE header" }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -le 0 -or $peOffset + 6 -gt $bytes.Length) { throw "$dll has a bogus e_lfanew" }
    if ([Text.Encoding]::ASCII.GetString($bytes, $peOffset, 4) -ne "PE`0`0") {
        throw "$dll does not start a PE image at offset $peOffset"
    }
    $actual = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    if ($actual -ne $expectedMachines[$Machine]) {
        $label = if ($names.ContainsKey($actual)) { $names[$actual] } else { '0x{0:X4}' -f $actual }
        throw "$dll is a $label binary; this build was expected to produce $Machine"
    }
    Write-Host "$dll machine type: $Machine"
}

if ($NativeArm64Dll) {
    if (-not (Test-Path -LiteralPath $NativeArm64Dll)) {
        throw "The Arm64 linker-input build did not produce $NativeArm64Dll"
    }
    $native = (Get-Item -LiteralPath $NativeArm64Dll).Length
    $merged = (Get-Item -LiteralPath $dll).Length
    if ($merged -le $native) {
        throw "$dll is $merged bytes, not larger than the Arm64-only build at $native bytes: the Arm64EC half was not merged in"
    }
    Write-Host "Arm64X $merged bytes against Arm64-only $native bytes; the second half is present"
}

Write-Host "$dll is $((Get-Item $dll).Length) bytes"
Write-Host "$pdb is $((Get-Item $pdb).Length) bytes"
