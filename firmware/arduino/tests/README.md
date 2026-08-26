# Board bring-up sketches

[简体中文](README.zh-CN.md)

Run in this order and follow `07-hardware-test-guide.md`.

## `01_mcu_usb_test`

Prerequisites: OLED disconnected, speaker disconnected, TP1/TP2/TP4/TP5 checks passed. Tests native USB, execution, 16 MB Flash, 8 MB PSRAM, reset reason, and keeps the amplifier disabled.

## `02_keyboard_test`

Prerequisite: MCU/USB test passed. Prints stable pressed/released events for all nine active-low keys.

## `03_i2c_scan`

Prerequisites: OLED pin order has been checked against the PCB silkscreen, OLED was connected while power was off, and TP2 was rechecked after connection. Scans the bus at SDA=GPIO10 and SCL=GPIO11. It does not assume an OLED controller or address.

## Not included yet

- OLED rendering test: wait until SSD1315/SSD1306 compatibility, resolution, address and orientation are physically confirmed.
- I2S/audio test: wait until J5 fixed-tab isolation, speaker compatibility and safe first-audio conditions are confirmed.
- Full synthesizer: wait until all independent tests pass.
