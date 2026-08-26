param(
    [Parameter(Mandatory = $true)]
    [string]$Sketch,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COM\d+$')]
    [string]$Port,
    [switch]$Rebuild
)

. (Join-Path $PSScriptRoot "common.ps1")
Assert-ArduinoCli
Ensure-OutputDirectories

$sketchPath = Get-SketchPath $Sketch
$buildPath = Join-Path $BuildRoot $Sketch

if ($Rebuild -or -not (Test-Path $buildPath)) {
    & (Join-Path $PSScriptRoot "build.ps1") -Sketch $Sketch
}

Write-Warning "Upload is allowed only after the relevant hardware-test stage in 07-hardware-test-guide.md has passed."
Write-Host "Uploading $Sketch to $Port"
arduino-cli upload --fqbn $MiniSynthFqbn --port $Port --input-dir $buildPath
if ($LASTEXITCODE -ne 0) {
    throw "Upload failed. Close Serial Monitor, verify the COM port, and enter BOOT download mode if necessary."
}

Write-Host "Upload completed."
