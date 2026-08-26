. (Join-Path $PSScriptRoot "common.ps1")

Assert-ArduinoCli

$EspressifIndex = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"

Write-Host "Updating Arduino package indexes..."
arduino-cli core update-index --additional-urls $EspressifIndex
if ($LASTEXITCODE -ne 0) { throw "Failed to update Arduino package indexes." }

Write-Host "Installing pinned core $MiniSynthCore ..."
arduino-cli core install $MiniSynthCore --additional-urls $EspressifIndex
if ($LASTEXITCODE -ne 0) { throw "Failed to install $MiniSynthCore." }

Write-Host "Installed cores:"
arduino-cli core list

Write-Host "Setup complete. Run .\firmware\arduino\tools\check-env.ps1 next."
