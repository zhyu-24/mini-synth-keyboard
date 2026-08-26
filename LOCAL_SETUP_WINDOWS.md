# Windows local development setup

[简体中文](LOCAL_SETUP_WINDOWS.zh-CN.md)

This project uses Arduino IDE for human inspection and Arduino CLI for repeatable local-AI automation.

## 1. Install the Espressif core in Arduino IDE

In Boards Manager install:

```text
esp32 by Espressif Systems
version 3.3.11
```

Select:

```text
Tools → Board → esp32 → ESP32S3 Dev Module
```

The intended options are:

```text
CPU Frequency: 240MHz (WiFi)
Flash Mode: QIO 80MHz
Flash Size: 16MB (128Mb)
PSRAM: OPI PSRAM
USB Mode: Hardware CDC and JTAG
USB CDC On Boot: Enabled
Upload Mode: UART0 / Hardware CDC
Partition Scheme: 16M Flash (3MB APP/9.9MB FATFS)
Core Debug Level: Info during bring-up
Erase All Flash Before Sketch Upload: Disabled
JTAG Adapter: Integrated USB JTAG when interactive debugging is needed
```

## 2. Install Arduino CLI

Use Arduino's official Windows 64-bit MSI or ZIP from:

https://docs.arduino.cc/arduino-cli/installation

After installation, open a new PowerShell window and verify:

```powershell
arduino-cli version
```

If the command is not found, add the directory containing `arduino-cli.exe` to the Windows `PATH`, then reopen PowerShell and the local AI application.

## 3. Confirm that IDE and CLI see the same core

Arduino IDE and Arduino CLI use the same default Arduino data directory on Windows unless separately reconfigured. Check:

```powershell
arduino-cli config dump --verbose
arduino-cli core list
```

Expected core:

```text
esp32:esp32  3.3.11
```

If it is absent, run from the project root:

```powershell
.\firmware\arduino\tools\setup.ps1
```

The script installs exactly `esp32:esp32@3.3.11` using Espressif's official package index.

## 4. Put the project on the local machine

Unzip the project to a short path without cloud synchronization during bring-up, for example:

```text
C:\Projects\mini-synth-keyboard
```

Open that directory as your workspace. Before connecting hardware, read the root `README.md`, `07-hardware-test-guide.md`, and `firmware/arduino/apps/README.md`, then run the environment check without uploading anything.

## 5. Run the environment check

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\firmware\arduino\tools\check-env.ps1
```

This does not upload firmware. It checks:

- `arduino-cli` is available;
- ESP32 core 3.3.11 is installed;
- the custom board library exists;
- the generic ESP32-S3 board and its options can be read.

## 6. Connect the board and detect the port

Only after the physical first-power checks in `07-hardware-test-guide.md` have passed:

```powershell
arduino-cli board list
```

Windows normally shows the native USB Serial/JTAG device as a COM port. Record the port, for example `COM7`.

If no port appears, manually enter download mode:

1. Hold BOOT.
2. Press and release RESET.
3. Release BOOT.
4. Run `arduino-cli board list` again.

Do not repeatedly retry if the board is hot, smells abnormal, enters power-supply current limit, or TP2 is unstable.

## 7. Compile without hardware

```powershell
.\firmware\arduino\tools\build.ps1 -Sketch 01_mcu_usb_test
```

Build artifacts go to:

```text
firmware\arduino\build\01_mcu_usb_test
```

A successful compile verifies the source and toolchain only; it does not verify the PCB.

## 8. Upload and monitor

Close the Arduino IDE Serial Monitor first so it does not occupy the COM port.

```powershell
.\firmware\arduino\tools\upload.ps1 -Sketch 01_mcu_usb_test -Port COM7
.\firmware\arduino\tools\monitor.ps1 -Port COM7
```

Use Ctrl+C to stop monitoring.

Run the tests in order:

```text
01_mcu_usb_test
02_keyboard_test
03_i2c_scan     only after the OLED pin order is checked and it is connected while power is off
```

Audio tests are intentionally not included in the first package. They require J5/speaker checks and an explicit human safety confirmation.

## 9. Local AI permissions

For a useful closed loop, allow the local AI to:

- read/write this project directory;
- run PowerShell and `arduino-cli`;
- access the selected COM port;
- read build and serial logs.

Do not grant automation unrestricted system-administrator access. Keep uploads and hardware stress actions inside the project scripts and the explicit safety gates in `07-hardware-test-guide.md`.

## 10. Useful direct commands

Inspect the installed board options:

```powershell
arduino-cli board details --fqbn esp32:esp32:esp32s3 --full
```

List attached boards:

```powershell
arduino-cli board list
```

List installed cores and libraries:

```powershell
arduino-cli core list
arduino-cli lib list
```

Open a timestamped serial monitor:

```powershell
arduino-cli monitor --port COM7 --config baudrate=115200 --timestamp
```

The exact FQBN and all board option IDs used by this project are centralized in `firmware/arduino/config/board.ps1`.
