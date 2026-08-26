param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COM\d+$')]
    [string]$Port,
    [int]$BaudRate = 115200
)

. (Join-Path $PSScriptRoot "common.ps1")
Assert-ArduinoCli
Ensure-OutputDirectories

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logPath = Join-Path $LogsRoot "serial-$stamp.log"

Write-Host "Monitoring $Port at $BaudRate baud. Press Ctrl+C to stop."
Write-Host "Log: $logPath"

arduino-cli monitor --port $Port --fqbn $MiniSynthFqbn --config "baudrate=$BaudRate" --timestamp 2>&1 |
    Tee-Object -FilePath $logPath
