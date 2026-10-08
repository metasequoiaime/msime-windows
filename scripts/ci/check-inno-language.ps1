# Prove ISCC can resolve the Chinese language file, by compiling a script that needs it.
#
# install-inno-language.ps1 used to write the file next to the Chocolatey ISCC shim rather than into
# the Inno Setup installation, report success, and let packaging fail several steps later with an
# include error naming a path nothing had written to. Eight releases went out as unpublished drafts
# before anybody connected the two.
#
# Asserting the file exists where install-inno-language.ps1 put it would only restate that script's
# own belief about where ISCC looks, which is precisely the thing that was wrong. Compiling a script
# whose only content is a [Languages] entry pointing at compiler:Languages\ChineseSimplified.isl is
# what makes ISCC answer the question. Seconds on an image that already ships Inno Setup.
#
# The probe also carries msime_setup.iss's WizardStyle=modern dynamic, which ISCC older than 6.6
# rejects, so the same compile answers whether this ISCC can build the real installer.
$ErrorActionPreference = 'Stop'

& "$PSScriptRoot/install-inno-language.ps1"

$output = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
$probe = Join-Path $output 'language-probe.iss'

@(
    '[Setup]'
    'AppName=Language probe'
    'AppVersion=1.0'
    'DefaultDirName={autopf}\LanguageProbe'
    "OutputDir=$output"
    'OutputBaseFilename=language-probe'
    'WizardStyle=modern dynamic'
    '[Languages]'
    'Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"'
) | Set-Content -Path $probe -Encoding UTF8

ISCC.exe $probe
if ($LASTEXITCODE -ne 0) {
    throw "ISCC could not resolve compiler:Languages\ChineseSimplified.isl (exit code $LASTEXITCODE)"
}

Write-Host 'ISCC resolved compiler:Languages\ChineseSimplified.isl'
