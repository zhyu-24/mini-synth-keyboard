# Mini Synth Keyboard

一台围绕 ESP32-S3 设计的九键迷你电子琴：它既可以直接演奏，也可以播放文件曲谱、通过 USB 管理音乐库，并作为蓝牙游戏键盘使用。本仓库包含正式固件、主机端工具、测试程序，以及可编辑的立创 EDA 专业版原理图和 PCB 工程。

> 当前仓库面向开发者和硬件制作者。制造或刷写前，请先阅读安全说明并核对所用硬件版本。

## 主要功能

- 七个音符键和两个功能键，支持和弦、三个八度、音色与音量设置；
- 128×64 OLED 界面和多应用主页；
- NS4168 I2S 功放音频输出；
- Song 模式支持 FFat 中最多 30 首 MSPKG/MSP 文件曲目；
- Windows 图形化音乐库管理器，支持 MusicXML 转换、增量导入、同名覆盖、精确同步、重排和单曲/多曲删除；
- USB MUSB v1 管理协议，上传采用临时文件、校验和原子提交；
- 全局 BLE HID 连接和九键游戏键盘模式；
- 设置保存在 NVS 中，歌曲保存在独立 FFat 分区中。

正式应用的详细行为见 [`firmware/arduino/apps/README.zh-CN.md`](firmware/arduino/apps/README.zh-CN.md)。

## 硬件概览

- MCU：ESP32-S3-WROOM-1-N16R8；
- 显示：SSD1306 兼容 128×64 I2C OLED；
- 音频：ESP32-S3 I2S → NS4168 → 扬声器；
- 输入：九个低电平有效按键；
- 存储：16 MB Flash，正式分区方案为 3 MB App + 9 MB FFat；
- 接口：原生 USB CDC/JTAG 与 BLE。

这是一块定制 PCB。选择 Arduino IDE 的通用 `ESP32S3 Dev Module` **不会**自动得到本项目的 GPIO 映射；固件必须使用 `MiniSynthBoard` 库中的语义化引脚定义。

## 仓库结构

```text
firmware/arduino/
  apps/mini_synth_v1/      正式应用
  libraries/MiniSynthBoard 定制板级引脚库
  tests/                   分阶段硬件测试程序
  config/board.ps1         唯一完整 FQBN 配置
  tools/                   环境、编译、上传和监视脚本
tools/
  music-library/           图形界面和高级音乐库 CLI
  media-import/            MusicXML → MSPKG 转换与校验
  usb-import/              底层 MUSB v1 串口客户端
hardware/lceda/            最新 LCEDA Pro 工程和校验值
verify/                    原理图/PCB 检查辅助材料
LICENSES/                  软件、硬件和文档许可证说明
```

构建产物、日志、Python 缓存、第三方 U8g2 副本、生成的歌曲包以及内部交接材料不会提交到仓库。

## 中文文档导航

- [Windows 本地开发环境配置](LOCAL_SETUP_WINDOWS.zh-CN.md)
- [固件开发硬件速查](HARDWARE_REFERENCE.zh-CN.md)
- [Arduino 板级抽象与构建工具](firmware/arduino/README.zh-CN.md)
- [正式固件功能与操作说明](firmware/arduino/apps/README.zh-CN.md)
- [分阶段板卡测试顺序](firmware/arduino/tests/README.zh-CN.md)
- [音乐库管理器](tools/music-library/README.md)
- [USB MSPKG 主机工具](tools/usb-import/README.md)
- [MUSB v1 设备协议](firmware/arduino/tests/07_usb_mspkg_import_test/PROTOCOL.md)
- [MSPKG v1 音乐包格式](tools/media-import/MSPKG_V1.zh-CN.md)
- [立创 EDA 专业版工程说明](hardware/lceda/README.zh-CN.md)
- [许可证范围说明](LICENSES/README.zh-CN.md)

## 构建正式固件

### 依赖

- Windows PowerShell；
- [`arduino-cli`](https://arduino.github.io/arduino-cli/);
- Arduino-ESP32 `3.3.11`；
- U8g2 Arduino 库；
- 本仓库的 `firmware/arduino/libraries/MiniSynthBoard`。

在仓库根目录执行：

```powershell
.\firmware\arduino\tools\setup.ps1
.\firmware\arduino\tools\check-env.ps1
.\firmware\arduino\tools\build.ps1 -Sketch mini_synth_v1 -Clean
```

项目使用的完整 FQBN 为：

```text
esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=info,PSRAM=opi,EraseFlash=none,JTAGAdapter=builtin
```

此值由 [`firmware/arduino/config/board.ps1`](firmware/arduino/config/board.ps1) 统一维护。不要为“方便”改用不同分区、Flash/PSRAM 或 USB 参数。

上传会改变真实设备，必须先确认目标串口并完成相应硬件测试：

```powershell
.\firmware\arduino\tools\upload.ps1 -Sketch mini_synth_v1 -Port COM12
```

示例中的 `COM12` 只是占位符，不能据此猜测设备端口。

## 音乐库管理器

需要 Python 3.10 或更高版本：

```powershell
python -m pip install pyserial numpy
```

Windows 用户可双击：

```text
tools\music-library\启动音乐库管理器.bat
```

或从命令行查看所有功能：

```powershell
python .\tools\music-library\music_library.py --help
```

“导入并保留设备歌曲”不会删除未选择的曲目；“精确同步”会让设备曲库与本地列表完全一致；“重排”要求提供设备全部歌曲对应的本地源文件。涉及初始化存储或删除时，请先阅读 [`tools/music-library/README.md`](tools/music-library/README.md)。

## 打开硬件工程

使用立创 EDA 专业版打开 [`hardware/lceda/elec_piano.eprj2`](hardware/lceda/elec_piano.eprj2)。该文件是当前最新的可编辑工程。`hardware/lceda/archive/` 中的 `.epro2` 是 2026-08-08 的旧便携快照，仅用于恢复参考。

下载后可核对：

```powershell
Get-FileHash .\hardware\lceda\elec_piano.eprj2 -Algorithm SHA256
```

预期 SHA-256：

```text
844621f4a98eb44d95c989bc3cf9edac758f37babbf94eb05fe17a1525102eff
```

请从你准备制造的确切工程版本重新导出 Gerber、钻孔、BOM 和坐标文件，不要把旧导出物与当前工程混用。

## 安全说明

- NS4168 的扬声器输出是 BTL 差分输出，任一扬声器端都不能接系统地，也不能按普通单端音频方式测量或外接；
- 首次上电先限流并测量电源轨，使用小音量和短时测试，发现异味、快速发热、复位、USB 反复断连或明显失真立即断电；
- 不要在串口监视器占用端口时上传或管理音乐库；
- `init-storage` 会格式化并清空 FFat 文件歌曲区；它不会被普通导入隐式调用，但执行前仍应确认目标设备；
- 固件、分区表和硬件工程必须成套核对。错误的 Flash 大小、PSRAM、USB 或分区设置可能导致启动失败或数据丢失。

更多硬件测试步骤见 [`07-hardware-test-guide.md`](07-hardware-test-guide.md)。

## 许可证

- 软件：MIT License；
- 硬件设计：CERN-OHL-P-2.0；
- 文档和项目自有图片：CC BY 4.0。

完整范围说明见 [`LICENSES/README.zh-CN.md`](LICENSES/README.zh-CN.md)。第三方库、数据手册、工具和音乐内容保留各自权利，不因本仓库而被重新许可。

## 贡献

提交问题时请提供硬件版本、Arduino-ESP32 版本、完整 FQBN、复现步骤和完整错误输出，但不要公开串口之外的个人路径、账户信息或任何凭据。涉及硬件改动时，请同时说明原理图网络和 PCB 影响。
