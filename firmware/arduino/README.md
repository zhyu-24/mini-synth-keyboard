# Mini Synth Keyboard · Arduino board abstraction

## What this provides

`libraries/MiniSynthBoard/src/MiniSynthPins.h` is the board-level GPIO definition for this custom PCB. Application sketches should include it and use semantic names instead of raw GPIO numbers:

```cpp
#include <MiniSynthPins.h>

pinMode(MINI_SYNTH_PIN_KEY_DO, MINI_SYNTH_KEY_PIN_MODE);
Wire.begin(MINI_SYNTH_PIN_I2C_SDA, MINI_SYNTH_PIN_I2C_SCL);
digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);
```

The first test example is:

`libraries/MiniSynthBoard/examples/PinMapSmokeTest/PinMapSmokeTest.ino`

## Why selecting ESP32S3 Dev Module is not enough

Arduino-ESP32 separates two kinds of information:

1. **Board build settings** in `boards.txt`: MCU target, Flash size/mode, PSRAM type, USB mode, partitions and upload method.
2. **Board pin aliases** in `variants/<board>/pins_arduino.h`: names such as `SDA`, `SCL`, `TX`, `RX` and `LED_BUILTIN`.

Selecting the generic `ESP32S3 Dev Module` loads Espressif's generic `variants/esp32s3/pins_arduino.h`. Its defaults include `SDA=8`, `SCL=9` and a generic RGB LED on GPIO48. Those are not this PCB's connections: this board uses SDA=10, SCL=11 and has no programmable built-in LED.

Therefore the project currently uses two layers:

- Select `ESP32S3 Dev Module` and configure the N16R8 memory/USB options in Arduino IDE.
- Include `MiniSynthPins.h` so every sketch uses the custom PCB's real connections.

## Arduino IDE settings for ESP32-S3-WROOM-1-N16R8

- Board: `ESP32S3 Dev Module`
- Flash Size: `16MB (128Mb)`
- Flash Mode: `QIO 80MHz`
- PSRAM: `OPI PSRAM`
- USB Mode: `Hardware CDC and JTAG`
- USB CDC On Boot: `Enabled`
- Upload Mode: `UART0 / Hardware CDC`
- Partition Scheme: a scheme explicitly marked for `16M Flash`

## Installing the local library

Copy the folder:

`firmware/arduino/libraries/MiniSynthBoard`

into the Arduino sketchbook's `libraries` directory, then restart Arduino IDE. After that, examples appear under:

`File → Examples → MiniSynthBoard → PinMapSmokeTest`

## Command-line development package

The project is pinned to Arduino-ESP32 3.3.11. Board settings are centralized in:

`config/board.ps1`

PowerShell helpers are under `tools/`:

- `setup.ps1` — install the pinned core;
- `check-env.ps1` — check CLI, core, board options and local library without uploading;
- `build.ps1` — compile a named test sketch;
- `upload.ps1` — upload an existing build to a confirmed COM port;
- `monitor.ps1` — capture timestamped serial logs.

Initial board tests are under `tests/` and must be run in the order documented there.

## Future full custom-board package

A complete Arduino board entry can later be added so the Tools menu shows `Mini Synth Keyboard` directly. That package needs its own `boards.txt` entry plus a `variants/mini_synth_keyboard/pins_arduino.h` file. It would preselect N16R8 memory and USB settings as well as standard pin aliases.

The current library-plus-pinned-FQBN approach is intentionally used for first-board bring-up because it stays inside the official Espressif core, is easy to audit, and does not modify files under Arduino15 that package upgrades may overwrite.

## Sources

- Espressif Arduino-ESP32 `boards.txt`, generic `ESP32S3 Dev Module` entry.
- Espressif Arduino-ESP32 `variants/esp32s3/pins_arduino.h`.
- Arduino Platform Specification (`boards.txt`, platform and variant mechanism).
- ESP32-S3-WROOM-1/1U Datasheet, module variant N16R8 and GPIO restrictions.
- Project `00-design-decisions.md`, current PCB GPIO mapping.
