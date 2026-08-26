# Windows 本地开发环境配置

[English](LOCAL_SETUP_WINDOWS.md)

本项目使用 Arduino IDE 供人工检查，使用 Arduino CLI 实现可重复的本地命令行构建和自动化。

## 1. 在 Arduino IDE 中安装乐鑫核心

在“开发板管理器”中安装：

```text
esp32 by Espressif Systems
版本 3.3.11
```

选择：

```text
工具 → 开发板 → esp32 → ESP32S3 Dev Module
```

目标选项如下：

```text
CPU Frequency: 240MHz (WiFi)
Flash Mode: QIO 80MHz
Flash Size: 16MB (128Mb)
PSRAM: OPI PSRAM
USB Mode: Hardware CDC and JTAG
USB CDC On Boot: Enabled
Upload Mode: UART0 / Hardware CDC
Partition Scheme: 16M Flash (3MB APP/9.9MB FATFS)
Core Debug Level: 调试阶段使用 Info
Erase All Flash Before Sketch Upload: Disabled
JTAG Adapter: 需要交互调试时使用 Integrated USB JTAG
```

## 2. 安装 Arduino CLI

从 Arduino 官方页面下载 Windows 64 位 MSI 或 ZIP：

<https://docs.arduino.cc/arduino-cli/installation>

安装后打开新的 PowerShell 窗口并检查：

```powershell
arduino-cli version
```

如果找不到命令，请把 `arduino-cli.exe` 所在目录加入 Windows `PATH`，然后重新打开 PowerShell 和本地开发工具。

## 3. 确认 IDE 与 CLI 使用同一核心

除非单独修改过配置，Windows 上的 Arduino IDE 和 Arduino CLI 默认使用同一个 Arduino 数据目录。执行：

```powershell
arduino-cli config dump --verbose
arduino-cli core list
```

预期核心：

```text
esp32:esp32  3.3.11
```

如果不存在，请在项目根目录运行：

```powershell
.\firmware\arduino\tools\setup.ps1
```

该脚本通过乐鑫官方包索引安装精确版本 `esp32:esp32@3.3.11`。

## 4. 把项目放到本机

在调试阶段，建议把项目解压到较短且不受云同步影响的路径，例如：

```text
C:\Projects\mini-synth-keyboard
```

将该目录作为工作区。连接硬件前，先阅读根目录 `README.md`、`07-hardware-test-guide.md` 和 `firmware/arduino/apps/README.zh-CN.md`，然后只运行环境检查，不要上传。

## 5. 运行环境检查

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\firmware\arduino\tools\check-env.ps1
```

此操作不会上传固件。它检查：

- `arduino-cli` 是否可用；
- ESP32 核心 3.3.11 是否安装；
- 定制板级库是否存在；
- 是否能读取通用 ESP32-S3 板卡及其选项。

## 6. 连接板卡并识别端口

只有在 `07-hardware-test-guide.md` 中的实物首次上电检查通过后，才运行：

```powershell
arduino-cli board list
```

Windows 通常会把原生 USB Serial/JTAG 设备显示为 COM 端口。记录实际端口，例如 `COM7`。

如果没有出现端口，可手动进入下载模式：

1. 按住 BOOT；
2. 按下并释放 RESET；
3. 松开 BOOT；
4. 再次运行 `arduino-cli board list`。

如果板卡发热、出现异味、电源进入限流或 TP2 不稳定，不要反复重试。

## 7. 不连接硬件进行编译

```powershell
.\firmware\arduino\tools\build.ps1 -Sketch 01_mcu_usb_test
```

构建产物位于：

```text
firmware\arduino\build\01_mcu_usb_test
```

编译成功只能证明源码和工具链可用，不能证明 PCB 正常。

## 8. 上传与串口监视

先关闭 Arduino IDE 串口监视器，避免它占用 COM 端口。

```powershell
.\firmware\arduino\tools\upload.ps1 -Sketch 01_mcu_usb_test -Port COM7
.\firmware\arduino\tools\monitor.ps1 -Port COM7
```

使用 Ctrl+C 停止监视。

按以下顺序运行测试：

```text
01_mcu_usb_test
02_keyboard_test
03_i2c_scan     仅在核对 OLED 引脚顺序并于断电状态连接后执行
```

首个测试包有意不包含音频测试。音频测试必须先检查 J5/扬声器，并取得明确的人工安全确认。

## 9. 本地自动化权限

若使用本地自动化工具，可允许其：

- 读写本项目目录；
- 运行 PowerShell 和 `arduino-cli`；
- 访问人工选定的 COM 端口；
- 读取构建日志和串口日志。

不要授予自动化工具不受限制的系统管理员权限。上传和硬件压力操作必须遵守项目脚本及 `07-hardware-test-guide.md` 中的明确安全门槛。

## 10. 常用直接命令

查看已安装板卡的完整选项：

```powershell
arduino-cli board details --fqbn esp32:esp32:esp32s3 --full
```

列出已连接板卡：

```powershell
arduino-cli board list
```

列出已安装核心和库：

```powershell
arduino-cli core list
arduino-cli lib list
```

打开带时间戳的串口监视器：

```powershell
arduino-cli monitor --port COM7 --config baudrate=115200 --timestamp
```

项目使用的精确 FQBN 和全部板卡选项 ID 统一维护在 `firmware/arduino/config/board.ps1` 中。
