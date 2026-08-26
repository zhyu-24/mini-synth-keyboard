Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:ArduinoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$script:ProjectRoot = (Resolve-Path (Join-Path $script:ArduinoRoot "..\..")).Path
$script:BoardConfig = Join-Path $script:ArduinoRoot "config\board.ps1"

if (-not (Test-Path $script:BoardConfig)) {
    throw "Board configuration not found: $script:BoardConfig"
}

. $script:BoardConfig

$script:LibraryRoot = Join-Path $script:ArduinoRoot "libraries\MiniSynthBoard"
$script:AppsRoot = Join-Path $script:ArduinoRoot "apps"
$script:TestsRoot = Join-Path $script:ArduinoRoot "tests"
$script:BuildRoot = Join-Path $script:ArduinoRoot "build"
$script:LogsRoot = Join-Path $script:ArduinoRoot "logs"

function Assert-ArduinoCli {
    if (-not (Get-Command arduino-cli -ErrorAction SilentlyContinue)) {
        throw "arduino-cli was not found in PATH. Install it using LOCAL_SETUP_WINDOWS.md, then reopen PowerShell."
    }
}

function Get-SketchPath([string]$Sketch) {
    $candidates = @(@(
            (Join-Path $script:AppsRoot $Sketch)
            (Join-Path $script:TestsRoot $Sketch)
        ) | Where-Object { Test-Path $_ })

    if ($candidates.Count -eq 0) {
        throw "Sketch not found in apps or tests: $Sketch"
    }
    if ($candidates.Count -gt 1) {
        throw "Sketch name is ambiguous between apps and tests: $Sketch"
    }
    return $candidates[0]
}

function Ensure-OutputDirectories {
    New-Item -ItemType Directory -Force -Path $script:BuildRoot | Out-Null
    New-Item -ItemType Directory -Force -Path $script:LogsRoot | Out-Null
}
