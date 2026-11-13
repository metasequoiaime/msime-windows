param(
    # Rewrite the files in place. Without it the script only reports what would change.
    [switch]$Write,
    # Files or directories to format.
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Paths
)
$ErrorActionPreference = 'Stop'

# scripts/format.sh is the CI entry point, but it builds a venv with the Unix layout (bin/pip),
# so it cannot run on Windows. This is the same pinned formatter without the venv.
#
# The version matters: whichever clang-format happens to be on PATH reports different wrapping
# on lines nobody touched, so "fixing" those files turns an unrelated diff into the PR and CI
# still disagrees. 18.1.8 is what format.sh pins.
$version = '18.1.8'
if (-not (Get-Command uvx -ErrorAction SilentlyContinue)) {
    throw 'uvx (from uv) is required; install it with scoop/winget and rerun.'
}
if (-not $Paths -or $Paths.Count -eq 0) {
    throw 'usage: scripts/format.ps1 [-Write] <file-or-directory>...'
}

$formatterArguments = @()
if ($Write) {
    $formatterArguments += '-i'
} else {
    $formatterArguments += '--dry-run'
    $formatterArguments += '--Werror'
}
# --style=file picks the nearest .clang-format, so each subtree keeps its own rules.
$formatterArguments += '--style=file'
$formatterArguments += $Paths
& uvx --from "clang-format==$version" clang-format @formatterArguments
exit $LASTEXITCODE
