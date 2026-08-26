#pragma once

#include <Arduino.h>

/*
 * Mini Synth Keyboard — board-level pin map
 *
 * Target module: ESP32-S3-WROOM-1-N16R8
 * Hardware source of truth:
 *   - 00-design-decisions.md (current GPIO assignment)
 *   - ESP32-S3-WROOM-1/1U Datasheet (module pin capabilities)
 *
 * This file maps circuit functions to GPIO numbers. Application code should
 * use these names instead of raw numbers such as digitalRead(15).
 */

#if !defined(ARDUINO_ARCH_ESP32) || !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "MiniSynthPins.h requires an ESP32-S3 target. Select an ESP32S3 board in Arduino IDE."
#endif

#define MINI_SYNTH_BOARD_NAME "Mini Synth Keyboard"

/* Seven note keys: active-low, using ESP32-S3 internal pull-ups. */
#define MINI_SYNTH_PIN_KEY_DO         15
#define MINI_SYNTH_PIN_KEY_RE         16
#define MINI_SYNTH_PIN_KEY_MI         17
#define MINI_SYNTH_PIN_KEY_FA          7
#define MINI_SYNTH_PIN_KEY_SOL         6
#define MINI_SYNTH_PIN_KEY_LA          5
#define MINI_SYNTH_PIN_KEY_TI          4

/* Function keys: active-low, using ESP32-S3 internal pull-ups. */
#define MINI_SYNTH_PIN_KEY_PLAY_STOP  18
#define MINI_SYNTH_PIN_KEY_FN          8

#define MINI_SYNTH_NOTE_KEY_COUNT      7
#define MINI_SYNTH_FUNCTION_KEY_COUNT  2
#define MINI_SYNTH_KEY_ACTIVE_LEVEL    LOW
#define MINI_SYNTH_KEY_IDLE_LEVEL      HIGH
#define MINI_SYNTH_KEY_PIN_MODE        INPUT_PULLUP

/* OLED I2C bus. Always call Wire.begin(SDA, SCL) explicitly. */
#define MINI_SYNTH_PIN_I2C_SDA        10
#define MINI_SYNTH_PIN_I2C_SCL        11

/* NS4168 I2S input. Current mapping after the 2026-08-08 PCB remap. */
#define MINI_SYNTH_PIN_I2S_DOUT       12
#define MINI_SYNTH_PIN_I2S_BCLK       13
#define MINI_SYNTH_PIN_I2S_LRCLK      14

/* NS4168 CTRL: LOW = shutdown; HIGH = enabled and right-channel selected. */
#define MINI_SYNTH_PIN_AMP_CTRL       21
#define MINI_SYNTH_AMP_ENABLE_LEVEL   HIGH
#define MINI_SYNTH_AMP_DISABLE_LEVEL  LOW

/* Reserved spare input, intended for a future ADC1 volume control. */
#define MINI_SYNTH_PIN_SPARE_ADC       9

/* UART0 rescue/debug header J3. */
#define MINI_SYNTH_PIN_UART_TX        43
#define MINI_SYNTH_PIN_UART_RX        44

/* Boot-mode input. Use only when code explicitly needs to inspect GPIO0. */
#define MINI_SYNTH_PIN_BOOT            0

/* Native USB Serial/JTAG wiring. Reserved while USB is in use. */
#define MINI_SYNTH_PIN_USB_DM         19
#define MINI_SYNTH_PIN_USB_DP         20

/* Module memory configuration, useful for startup self-tests. */
#define MINI_SYNTH_EXPECTED_FLASH_BYTES (16UL * 1024UL * 1024UL)
#define MINI_SYNTH_EXPECTED_PSRAM_BYTES  (8UL * 1024UL * 1024UL)

/*
 * Board-specific restrictions:
 *   - There is no software-controlled built-in LED on this PCB. Do not use
 *     LED_BUILTIN from the generic ESP32S3 Dev Module variant.
 *   - GPIO35/36/37 are occupied internally by the module's Octal PSRAM.
 *   - GPIO19/20 are the native USB D-/D+ pair.
 *   - EN/RESET is not a normal GPIO and intentionally has no Arduino pin macro.
 */
