<#
.SYNOPSIS
Converts a PNG or SVG into a multi-resolution Windows icon with ImageMagick.

.DESCRIPTION
Non-square inputs are centered on a transparent square canvas. SVG inputs are rasterized at
-Density DPI first (the default renders a 1000 px logo at about 2000 px) so that every frame is a
downscale; never upscale a small PNG into the 256 px frame.

.EXAMPLE
.\scripts\convert-png-to-ico.ps1 .\input.png .\output.ico

.EXAMPLE
.\scripts\convert-png-to-ico.ps1 .\windows\image\msime-logo-v2.svg .\windows\image\msime-logo-v2.ico -Force

.EXAMPLE
.\scripts\convert-png-to-ico.ps1 .\input.png .\output.ico -PaddingPercent 0 -Force
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [string] $InputPath,

    [Parameter(Position = 1)]
    [string] $OutputPath,

    [ValidateNotNullOrEmpty()]
    [int[]] $Sizes = @(256, 128, 64, 48, 44, 40, 36, 32, 28, 24, 20, 16),

    [ValidateRange(0, 25)]
    [double] $PaddingPercent = 3,

    # Rasterization DPI for SVG inputs; ignored for raster inputs.
    [ValidateRange(72, 1200)]
    [int] $Density = 192,

    [switch] $Force
)

$ErrorActionPreference = 'Stop'

$magick = Get-Command magick -ErrorAction SilentlyContinue
if (-not $magick) {
    throw 'ImageMagick was not found. Install it and make sure magick.exe is available on PATH.'
}

$resolvedInput = (Resolve-Path -LiteralPath $InputPath).Path
if (-not $OutputPath) {
    $OutputPath = [System.IO.Path]::ChangeExtension($resolvedInput, '.ico')
}

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
if ([System.IO.Path]::GetExtension($resolvedOutput) -ne '.ico') {
    throw "The output path must have an .ico extension: $resolvedOutput"
}
if ((Test-Path -LiteralPath $resolvedOutput) -and -not $Force) {
    throw "The output file already exists. Pass -Force to replace it: $resolvedOutput"
}

$outputDirectory = Split-Path -Parent $resolvedOutput
if (-not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory | Out-Null
}

$normalizedSizes = @($Sizes | Where-Object { $_ -ge 16 -and $_ -le 256 } | Sort-Object -Descending -Unique)
if ($normalizedSizes.Count -eq 0) {
    throw 'Sizes must contain at least one value from 16 through 256.'
}
if ($normalizedSizes.Count -ne $Sizes.Count) {
    throw 'Sizes must be unique values from 16 through 256.'
}

$isSvg = [System.IO.Path]::GetExtension($resolvedInput) -eq '.svg'
# -background and -density must precede an SVG input: they set the transparent backdrop and the
# rasterization resolution, not a post-processing step.
$densityOptions = if ($isSvg) { @('-density', "$Density") } else { @() }
$readOptions = if ($isSvg) { @('-background', 'none') + $densityOptions } else { @() }

$dimensions = (& $magick.Source identify @densityOptions -format '%w %h' $resolvedInput).Trim() -split '\s+'
if ($LASTEXITCODE -ne 0 -or $dimensions.Count -ne 2) {
    throw "ImageMagick could not read the input image dimensions: $resolvedInput"
}

$width = [int] $dimensions[0]
$height = [int] $dimensions[1]
$longestSide = [Math]::Max($width, $height)
$contentRatio = 1 - (2 * $PaddingPercent / 100)
$canvasSize = [int] [Math]::Ceiling($longestSide / $contentRatio)
$sizeList = $normalizedSizes -join ','

Write-Host "Input:  $resolvedInput ($width x $height)"
Write-Host "Canvas: $canvasSize x $canvasSize ($PaddingPercent% padding per side)"
Write-Host "Frames: $sizeList"

& $magick.Source `
    @readOptions `
    $resolvedInput `
    -background none `
    -gravity center `
    -extent "${canvasSize}x${canvasSize}" `
    -filter Lanczos `
    -define "icon:auto-resize=$sizeList" `
    $resolvedOutput

if ($LASTEXITCODE -ne 0) {
    throw "ImageMagick failed to create the icon: $resolvedOutput"
}

$actualSizes = @(& $magick.Source identify -format '%wx%h ' $resolvedOutput).Trim() -split '\s+'
if ($LASTEXITCODE -ne 0 -or $actualSizes.Count -ne $normalizedSizes.Count) {
    throw "The icon was created, but its frames could not be verified: $resolvedOutput"
}

Write-Host "Created: $resolvedOutput"
Write-Host "Verified frames: $($actualSizes -join ', ')"
