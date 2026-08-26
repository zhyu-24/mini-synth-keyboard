param(
    [Parameter(Mandatory = $true)]
    [string]$Sketch,
    [switch]$Clean
)

. (Join-Path $PSScriptRoot "common.ps1")
Assert-ArduinoCli
Ensure-OutputDirectories

$sketchPath = Get-SketchPath $Sketch
$buildPath = Join-Path $BuildRoot $Sketch
New-Item -ItemType Directory -Force -Path $buildPath | Out-Null

$args = @(
    "compile"
    "--fqbn", $MiniSynthFqbn
    "--library", $LibraryRoot
    "--build-path", $buildPath
    "--export-binaries"
    "--warnings", "all"
)
if ($Clean) { $args += "--clean" }
$args += $sketchPath

Write-Host "Building $Sketch"
Write-Host "FQBN: $MiniSynthFqbn"
& arduino-cli @args
if ($LASTEXITCODE -ne 0) { throw "Build failed for $Sketch." }

Write-Host "Build passed: $buildPath"
