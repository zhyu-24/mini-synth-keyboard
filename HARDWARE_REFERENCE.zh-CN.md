# Mini Synth Keyboard——固件开发硬件速查

[English](HARDWARE_REFERENCE.md)

本文是精简的板卡参考。详细设计依据见 `00-design-decisions.md`，BOM 细节见 `02-bom-footprints.md`，实物测试流程见 `07-hardware-test-guide.md`。

## 主要器件

- MCU/模组：ESP32-S3-WROOM-1-N16R8
- 存储器：16 MB Quad SPI Flash、8 MB Octal SPI PSRAM
- 显示：0.96 英寸四针 I2C OLED，销售标称 SSD1315；控制器兼容性必须以实物模块确认
- 音频功放：NS4168，I2S 数字输入、BTL 扬声器输出
- 扬声器：标称 8 Ω / 1 W，通过 J5 连接
- 电源：USB 5 V 输入，AMS1117-3.3 产生公共 3.3 V 电源轨

## GPIO 对照表

| 宏 | GPIO | 连接硬件 |
|---|---:|---|
| `MINI_SYNTH_PIN_KEY_DO` | 15 | SW1 do |
| `MINI_SYNTH_PIN_KEY_RE` | 16 | SW2 re |
| `MINI_SYNTH_PIN_KEY_MI` | 17 | SW3 mi |
| `MINI_SYNTH_PIN_KEY_FA` | 7 | SW4 fa |
| `MINI_SYNTH_PIN_KEY_SOL` | 6 | SW5 sol |
| `MINI_SYNTH_PIN_KEY_LA` | 5 | SW6 la |
| `MINI_SYNTH_PIN_KEY_TI` | 4 | SW7 ti |
| `MINI_SYNTH_PIN_KEY_PLAY_STOP` | 18 | SW8 播放/停止 |
| `MINI_SYNTH_PIN_KEY_FN` | 8 | SW9 Fn |
| `MINI_SYNTH_PIN_I2C_SDA` | 10 | OLED SDA |
| `MINI_SYNTH_PIN_I2C_SCL` | 11 | OLED SCL |
| `MINI_SYNTH_PIN_I2S_DOUT` | 12 | NS4168 SDATA |
| `MINI_SYNTH_PIN_I2S_BCLK` | 13 | NS4168 BCLK |
| `MINI_SYNTH_PIN_I2S_LRCLK` | 14 | NS4168 LRCLK |
| `MINI_SYNTH_PIN_AMP_CTRL` | 21 | NS4168 CTRL |
| `MINI_SYNTH_PIN_SPARE_ADC` | 9 | J4 备用/ADC1_CH8 |
| `MINI_SYNTH_PIN_UART_TX` | 43 | J3 TX0 |
| `MINI_SYNTH_PIN_UART_RX` | 44 | J3 RX0 |
| `MINI_SYNTH_PIN_BOOT` | 0 | SW10 BOOT |
| `MINI_SYNTH_PIN_USB_DM` | 19 | Type-C USB D- |
| `MINI_SYNTH_PIN_USB_DP` | 20 | Type-C USB D+ |

模组内部的 Octal PSRAM 占用 GPIO35/36/37，因此不能使用这些引脚。GPIO3/45/46 与启动配置有关，本设计有意避开。EN 由 RESET 硬件控制，不是普通应用 GPIO。

## 测试点

| 测试点 | 网络 | 用途 |
|---|---|---|
| TP1 | +5V | 检查 USB/输入电源 |
| TP2 | +3V3 | 测量 LDO 输出和动态电源轨 |
| TP3 | GND | 测量参考地 |
| TP4 | EN | 诊断复位状态 |
| TP5 | BOOT/GPIO0 | 诊断下载模式 |

## 固件电气假设

- 九个按键按下时都会把对应 GPIO 短接到 GND。应配置为 `INPUT_PULLUP`；按下 = LOW。
- I2C 引脚不是通用 ESP32-S3 Arduino 默认值，必须始终显式传入 SDA 和 SCL。
- NS4168 必须默认为关闭，并在固件最早阶段将 CTRL 拉低。
- CTRL 拉高会启用功放并选择右声道。
- 音频引脚顺序是 SDATA=12、BCLK=13、LRCLK=14。旧文档中的 BCLK=12/LRCLK=13/SDATA=14 已作废。
- 本 PCB 只有电源指示灯，没有可编程 `LED_BUILTIN`。
- 原生 USB Serial/JTAG 直接连接 GPIO19/20；板卡正常时，它应支持上传、串口监视和集成 JTAG。

## 首次上电边界

首次上电和 MCU/USB 测试期间，OLED 与扬声器均保持断开。电气验收及示波器操作由 `07-hardware-test-guide.md` 规定，固件测试不能代替这些测量。
