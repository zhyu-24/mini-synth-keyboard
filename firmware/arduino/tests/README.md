# Firmware test index

[简体中文](README.zh-CN.md)

This directory combines first-power board checks, isolated feature tests, historical tuning utilities, and device-free host regression. Numeric prefixes reflect historical stages rather than one current global sequence; duplicate `06_` and `07_` prefixes are not version identifiers.

Follow [`07-hardware-test-guide.md`](../../../07-hardware-test-guide.md) for the first-power sequence.

## `01_mcu_usb_test`

Prerequisites: OLED disconnected, speaker disconnected, TP1/TP2/TP4/TP5 checks passed. Tests native USB, execution, 16 MB Flash, 8 MB PSRAM, reset reason, and keeps the amplifier disabled.

## `02_keyboard_test`

Prerequisite: MCU/USB test passed. Prints stable pressed/released events for all nine active-low keys.

## `03_i2c_scan`

Prerequisites: OLED pin order has been checked against the PCB silkscreen, OLED was connected while power was off, and TP2 was rechecked after connection. Scans the bus at SDA=GPIO10 and SCL=GPIO11. It does not assume an OLED controller or address.

## `04_oled_display_test`

After powered-off connection and pin verification, check the 128×64 geometry, address, orientation, color-region layout, and refresh.

## `05_i2s_audio_test`

Only after speaker and power safety checks, verify I2S/NS4168 briefly at low volume. Compilation is not hardware acceptance; uploads require a freshly confirmed port.

## Isolated and historical tests

- `06_ble_keyboard_test`: BLE two-byte NKRO, Alt/R suppression, release gates, reconnect, and OLED. It has historical hardware acceptance but should be rerun after relevant changes.
- `07_usb_mspkg_import_test`: earlier isolated MUSB/FFat prototype and protocol reference. Production firmware now integrates MUSB v1.
- `06_harmonic_ab_test`: abandoned harmonic A/B experiment retained for traceability.
- `07_pure_tone_level_calibrator`: source of the production 21-pitch table, retained for traceability or explicit recalibration.

Names remain stable to preserve old links. The production entry point is always `../apps/mini_synth_v1/mini_synth_v1.ino`.

## Device-free regression

- `host_pitch_eq`: executes settings and audio paths extracted from production source; see [`host_pitch_eq/README.md`](host_pitch_eq/README.md).
- MusicXML, music-library, and USB protocol tests live under repository `tools/*/tests`; commands are in the Chinese [`PROJECT_STATUS.zh-CN.md`](../../../PROJECT_STATUS.zh-CN.md).

There is no automated hardware-in-the-loop rig yet. GitHub checks are host-only and never access a serial port.
