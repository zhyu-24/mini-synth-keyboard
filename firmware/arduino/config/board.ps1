# Arduino-ESP32 configuration for the Mini Synth Keyboard.
# Source: official Arduino-ESP32 3.3.11 ESP32S3 Dev Module board options.

$MiniSynthCore = "esp32:esp32@3.3.11"
$MiniSynthU8g2Library = "U8g2@2.36.19"
$MiniSynthBaseFqbn = "esp32:esp32:esp32s3"

$MiniSynthBoardOptions = @(
    "UploadSpeed=921600"
    "USBMode=hwcdc"
    "CDCOnBoot=cdc"
    "UploadMode=default"
    "CPUFreq=240"
    "FlashMode=qio"
    "FlashSize=16M"
    "PartitionScheme=app3M_fat9M_16MB"
    "DebugLevel=info"
    "PSRAM=opi"
    "EraseFlash=none"
    "JTAGAdapter=builtin"
)

$MiniSynthFqbn = "${MiniSynthBaseFqbn}:$($MiniSynthBoardOptions -join ',')"
