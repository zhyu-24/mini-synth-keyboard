# Mini Synth Keyboard — Hardware reference for firmware work

[简体中文](HARDWARE_REFERENCE.zh-CN.md)

This is the concise board reference. Detailed reasoning remains in `00-design-decisions.md`, BOM details in `02-bom-footprints.md`, and physical test procedures in `07-hardware-test-guide.md`.

## Main devices

- MCU/module: ESP32-S3-WROOM-1-N16R8
- Memory: 16 MB Quad SPI Flash, 8 MB Octal SPI PSRAM
- Display: 0.96-inch four-pin I2C OLED sold as SSD1315; controller compatibility must be confirmed on the physical module
- Audio amplifier: NS4168, I2S digital input, BTL speaker output
- Speaker: 8 ohm / 1 W nominal module connected through J5
- Power: USB 5 V input, AMS1117-3.3 generates the common 3.3 V rail

## GPIO table

| Macro | GPIO | Connected hardware |
|---|---:|---|
| `MINI_SYNTH_PIN_KEY_DO` | 15 | SW1 do |
| `MINI_SYNTH_PIN_KEY_RE` | 16 | SW2 re |
| `MINI_SYNTH_PIN_KEY_MI` | 17 | SW3 mi |
| `MINI_SYNTH_PIN_KEY_FA` | 7 | SW4 fa |
| `MINI_SYNTH_PIN_KEY_SOL` | 6 | SW5 sol |
| `MINI_SYNTH_PIN_KEY_LA` | 5 | SW6 la |
| `MINI_SYNTH_PIN_KEY_TI` | 4 | SW7 ti |
| `MINI_SYNTH_PIN_KEY_PLAY_STOP` | 18 | SW8 play/stop |
| `MINI_SYNTH_PIN_KEY_FN` | 8 | SW9 Fn |
| `MINI_SYNTH_PIN_I2C_SDA` | 10 | OLED SDA |
| `MINI_SYNTH_PIN_I2C_SCL` | 11 | OLED SCL |
| `MINI_SYNTH_PIN_I2S_DOUT` | 12 | NS4168 SDATA |
| `MINI_SYNTH_PIN_I2S_BCLK` | 13 | NS4168 BCLK |
| `MINI_SYNTH_PIN_I2S_LRCLK` | 14 | NS4168 LRCLK |
| `MINI_SYNTH_PIN_AMP_CTRL` | 21 | NS4168 CTRL |
| `MINI_SYNTH_PIN_SPARE_ADC` | 9 | J4 spare/ADC1_CH8 |
| `MINI_SYNTH_PIN_UART_TX` | 43 | J3 TX0 |
| `MINI_SYNTH_PIN_UART_RX` | 44 | J3 RX0 |
| `MINI_SYNTH_PIN_BOOT` | 0 | SW10 BOOT |
| `MINI_SYNTH_PIN_USB_DM` | 19 | Type-C USB D- |
| `MINI_SYNTH_PIN_USB_DP` | 20 | Type-C USB D+ |

GPIO35/36/37 are unavailable because the module's Octal PSRAM occupies them internally. GPIO3/45/46 are intentionally not used because they are strap-related pins. EN is controlled by RESET hardware and is not a normal application GPIO.

## Test points

| Test point | Net | Purpose |
|---|---|---|
| TP1 | +5V | USB/input power verification |
| TP2 | +3V3 | LDO output and dynamic rail measurement |
| TP3 | GND | Measurement reference |
| TP4 | EN | Reset-state diagnosis |
| TP5 | BOOT/GPIO0 | Download-mode diagnosis |

## Firmware electrical assumptions

- All nine keys short their GPIO to GND when pressed. Configure them as `INPUT_PULLUP`; pressed = LOW.
- I2C pins are not the generic ESP32-S3 Arduino defaults. Always pass SDA and SCL explicitly.
- NS4168 must default to shutdown. Drive CTRL LOW at the earliest firmware point.
- CTRL HIGH enables the amplifier and selects the right audio channel.
- Audio pin order is SDATA=12, BCLK=13, LRCLK=14. Older project text using BCLK=12/LRCLK=13/SDATA=14 is obsolete.
- This PCB has only a power indicator LED; there is no programmable `LED_BUILTIN`.
- Native USB Serial/JTAG is wired directly to GPIO19/20 and should provide upload, serial monitoring, and integrated JTAG when the board is healthy.

## First-power boundaries

OLED and speaker remain disconnected during the first power and MCU/USB test. Electrical acceptance and oscilloscope instructions are defined in `07-hardware-test-guide.md`; firmware must not replace those measurements.
