. (Join-Path $PSScriptRoot "common.ps1")

Assert-ArduinoCli

Write-Host "=== Arduino CLI ==="
arduino-cli version
if ($LASTEXITCODE -ne 0) { throw "arduino-cli version check failed." }

Write-Host "`n=== Configuration ==="
arduino-cli config dump --verbose
if ($LASTEXITCODE -ne 0) { throw "Arduino CLI configuration check failed." }

Write-Host "`n=== Installed cores ==="
$coreList = arduino-cli core list 2>&1
$coreList | Write-Host
if ($LASTEXITCODE -ne 0) { throw "Could not list Arduino cores." }
if (($coreList -join "`n") -notmatch 'esp32:esp32\s+3\.3\.11') {
    throw "Required core esp32:esp32 3.3.11 is not installed. Run setup.ps1."
}

Write-Host "`n=== Required Arduino libraries ==="
$libraryList = arduino-cli lib list 2>&1
$libraryList | Select-String -Pattern '^U8g2\s' | Write-Host
if ($LASTEXITCODE -ne 0) { throw "Could not list Arduino libraries." }
if (($libraryList -join "`n") -notmatch 'U8g2\s+2\.36\.19') {
    throw "Required library U8g2 2.36.19 is not installed. Run setup.ps1."
}

if (-not (Test-Path (Join-Path $LibraryRoot "src\MiniSynthPins.h"))) {
    throw "MiniSynthBoard library is incomplete: MiniSynthPins.h is missing."
}

Write-Host "`n=== ESP32-S3 board definition ==="
arduino-cli board details --fqbn $MiniSynthBaseFqbn --full
if ($LASTEXITCODE -ne 0) { throw "ESP32-S3 board definition check failed." }

Write-Host "`n=== Project FQBN ==="
Write-Host $MiniSynthFqbn

Write-Host "`nEnvironment check passed. No firmware was uploaded."
