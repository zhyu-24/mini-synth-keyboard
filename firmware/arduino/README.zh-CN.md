# Mini Synth Keyboard · Arduino 板级抽象

[English](README.md)

## 本目录提供什么

`libraries/MiniSynthBoard/src/MiniSynthPins.h` 定义了这块定制 PCB 的板级 GPIO。应用草图应包含该文件，并使用语义化名称，不要直接写裸 GPIO 编号：

```cpp
#include <MiniSynthPins.h>

pinMode(MINI_SYNTH_PIN_KEY_DO, MINI_SYNTH_KEY_PIN_MODE);
Wire.begin(MINI_SYNTH_PIN_I2C_SDA, MINI_SYNTH_PIN_I2C_SCL);
digitalWrite(MINI_SYNTH_PIN_AMP_CTRL, MINI_SYNTH_AMP_DISABLE_LEVEL);
```

第一个测试示例是：

`libraries/MiniSynthBoard/examples/PinMapSmokeTest/PinMapSmokeTest.ino`

## 为什么只选择 ESP32S3 Dev Module 还不够

Arduino-ESP32 将配置分成两类：

1. `boards.txt` 中的**板卡构建设置**：MCU 目标、Flash 大小和模式、PSRAM 类型、USB 模式、分区和上传方式；
2. `variants/<board>/pins_arduino.h` 中的**板卡引脚别名**：例如 `SDA`、`SCL`、`TX`、`RX` 和 `LED_BUILTIN`。

选择通用 `ESP32S3 Dev Module` 会加载乐鑫的通用 `variants/esp32s3/pins_arduino.h`。它默认使用 `SDA=8`、`SCL=9`，并把 GPIO48 当作通用 RGB LED；这些都不是本 PCB 的连接。本项目使用 SDA=10、SCL=11，而且没有可编程板载 LED。

因此，本项目采用两层配置：

- 在 Arduino IDE 中选择 `ESP32S3 Dev Module`，并配置 N16R8 对应的存储器和 USB 选项；
- 在每个草图中包含 `MiniSynthPins.h`，确保使用定制 PCB 的真实连接。

## ESP32-S3-WROOM-1-N16R8 的 Arduino IDE 设置

- 开发板：`ESP32S3 Dev Module`
- Flash Size：`16MB (128Mb)`
- Flash Mode：`QIO 80MHz`
- PSRAM：`OPI PSRAM`
- USB Mode：`Hardware CDC and JTAG`
- USB CDC On Boot：`Enabled`
- Upload Mode：`UART0 / Hardware CDC`
- Partition Scheme：明确标有 `16M Flash` 的分区方案

## 安装本地库

将下面的文件夹复制到 Arduino sketchbook 的 `libraries` 目录，然后重启 Arduino IDE：

`firmware/arduino/libraries/MiniSynthBoard`

完成后可在以下菜单找到示例：

`文件 → 示例 → MiniSynthBoard → PinMapSmokeTest`

## 命令行开发工具

项目固定使用 Arduino-ESP32 3.3.11 和 U8g2 2.36.19。板卡与依赖设置集中在：

`config/board.ps1`

`tools/` 中提供以下 PowerShell 脚本：

- `setup.ps1`：安装固定版本的核心和 U8g2；
- `check-env.ps1`：不上传固件，只检查 CLI、核心、板卡选项和本地库；
- `build.ps1`：编译指定正式应用或测试草图；
- `upload.ps1`：把已有构建上传到经过确认的 COM 端口；
- `monitor.ps1`：记录带时间戳的串口日志。

初始板卡测试位于 `tests/`，必须按其中说明的顺序执行。

## 未来的完整自定义板卡包

未来可以增加完整 Arduino 板卡定义，让“工具”菜单直接显示 `Mini Synth Keyboard`。该包需要自己的 `boards.txt` 条目和 `variants/mini_synth_keyboard/pins_arduino.h`，并预设 N16R8 存储、USB 选项和标准引脚别名。

当前优先采用“项目库 + 固定 FQBN”的方式进行首板调试，因为它完全基于乐鑫官方核心，便于审计，也不会修改 Arduino15 中可能被包升级覆盖的文件。

## 参考来源

- 乐鑫 Arduino-ESP32 `boards.txt` 中的通用 `ESP32S3 Dev Module` 条目；
- 乐鑫 Arduino-ESP32 `variants/esp32s3/pins_arduino.h`；
- Arduino Platform Specification 中的 `boards.txt`、platform 和 variant 机制；
- ESP32-S3-WROOM-1/1U 数据手册中 N16R8 模块及 GPIO 限制；
- 项目 `00-design-decisions.md` 中的当前 PCB GPIO 映射。
