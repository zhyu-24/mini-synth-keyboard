# 07 USB 单文件 MSPKG 导入独立测试

本目录是与正式固件完全隔离的 USB 有线导入原型。它只验证：通过 ESP32-S3 原生 Hardware CDC/JTAG 接收一个 `.mspkg` / `.msp` 文件，写入 FFat 临时文件，完整校验，然后安全替换根目录目标文件。

**不会初始化 I2S，不会开启功放，不使用 TinyUSB，也不会修改正式固件。** 程序进入 `setup()` 后最先将 NS4168 CTRL 拉低，之后始终保持 shutdown。

## 1. 安全边界

- `FFat.begin(false)`：挂载失败时直接报告 `FS_NOT_MOUNTED`，绝不自动格式化。
- 上传只允许 FFat 根目录中的安全 ASCII 文件名，不接受任意路径。
- 文件最大 528416 字节：32 字节 MSPKG header + 4096 字节 metadata + 正式 loader 的 512 KiB payload 上限。
- 接收期间只写 `/.~usb-upload.tmp`。断线、超时、CRC/容器错误不会改动已有 final。
- 覆盖已有文件时使用 `final → backup → temp → final`；第二次 rename 失败立即 rollback。下次启动会恢复中断提交。
- 第一次上传测试或任何会改变 FFat 的测试前，必须先取得用户一次明确批准。批准后，本目录限定范围内的编译—上传—好包/坏包/中断测试循环可以连续运行，不需要每轮重复询问。
- 出现反复重启、USB 频繁消失、电源不稳、器件发热、气味或扬声器出声时立即停止。

协议细节和机器错误码见 [PROTOCOL.md](PROTOCOL.md)。

## 2. Arduino 编译设置

必须使用项目 `firmware/arduino/config/board.ps1` 中的精确 FQBN：

```text
esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=info,PSRAM=opi,EraseFlash=none,JTAGAdapter=builtin
```

关键设置：

- Arduino-ESP32 `3.3.11`
- Board `ESP32S3 Dev Module`
- USB Mode `Hardware CDC and JTAG`
- USB CDC On Boot `Enabled`
- Flash `16MB` / QIO
- PSRAM `OPI`
- Partition `16M Flash (3MB APP/9.9MB FATFS)`
- Erase Flash `none`

依赖只有：

- 项目库 `MiniSynthBoard`
- Arduino-ESP32 自带 `FFat` / `FS`

示例 PowerShell 编译：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
. .\firmware\arduino\config\board.ps1
arduino-cli compile `
  --fqbn $MiniSynthFqbn `
  --library .\firmware\arduino\libraries\MiniSynthBoard `
  .\firmware\arduino\tests\07_usb_mspkg_import_test
```

不要选择 TinyUSB；不要启用全片擦除。

## 3. Python 主机工具

要求 Python 3.10+ 与 pyserial：

```powershell
py -3 -m pip install -r .\tools\usb-import\requirements.txt
py -3 .\tools\usb-import\usb_mspkg_import.py ports
```

识别并人工核对正确 COM 口后：

```powershell
py -3 .\tools\usb-import\usb_mspkg_import.py --port COM13 status
py -3 .\tools\usb-import\usb_mspkg_import.py --port COM13 list
py -3 .\tools\usb-import\usb_mspkg_import.py --port COM13 upload `
  .\artifacts\media-import\四首歌-v1\远航星的告别\远航星的告别.mspkg `
  --name VOYAGE.MSP
```

目标名故意限定为 ASCII；中文标题仍保存在包内 metadata，由正式 loader 使用。

主机工具支持：

- `ports`：列出候选串口，不打开设备；
- `status`：显示 FFat、状态、offset、CRC；
- `list`：列出根目录 MSPKG 文件；
- `upload`：1024 字节分块、进度、超时重试、设备 offset 恢复；
- `abort`：删除本次临时文件，不改 final。

默认响应超时 2 秒、重试 3 次，可用 `--timeout` / `--retries` 调整。错误以非零退出码和明确英文错误名输出，便于 PowerShell/Agent 自动判断。

## 4. 无硬件自动测试

从仓库根目录执行：

```powershell
py -3 -m unittest discover -s .\tools\usb-import\tests -v
```

覆盖：帧编码/增量解码/CRC/噪声重同步、路径净化、正常上传模拟、整文件坏 CRC、错误 offset、重复 chunk、上传中断时旧 final 保持、提交 rename 失败 rollback，以及 MSPKG metadata/payload CRC。

## 5. 实板验收流程

### A. 只读基线

1. 备份仓库源码、当前正式 `.ino`、已有 FFat 文件列表和需要保留的歌曲包。
2. 编译本独立 sketch；确认 FQBN 包含 `USBMode=hwcdc`、`EraseFlash=none`。
3. 连接设备，用 `arduino-cli board list` 识别 COM 口；不可仅凭上一次端口号猜测。
4. 在取得第一次硬件写入批准后上传独立 sketch。上传后先运行 `status` 和 `list`。

### B. 正常上传

1. 上传一个已由 `tools/media-import/inspect_mspkg.py` 通过的包，目标用 ASCII 名。
2. 进度达到 100%，工具报告 `committed`。
3. 再运行 `list`，核对文件名和长度。
4. RESET 后再次 `list`，确认持久存在。

### C. 负向测试

每个测试前记录目标 final 的长度和内容 hash：

- 篡改整文件字节但仍发送旧的 whole-file CRC：期望 `FILE_CRC`，旧 final 不变。
- 修改 metadata 后重算 whole-file CRC、但不更新 metadata CRC：期望 `METADATA_CRC`。
- 修改 payload 后重算 whole-file CRC、但不更新 payload CRC：期望 `PAYLOAD_CRC`。
- 发送错误 offset：期望 `OFFSET_MISMATCH`，STATUS offset 不前进。
- 重发相同 chunk：应幂等 ACK，offset 只增加一次。
- 上传一部分后拔掉 USB/RESET：旧 final 保持；重启后 temp 被清理，STATUS 回到 IDLE。

### D. 覆盖与 rollback

1. 准备同名旧包 A 和新包 B，确认 A 当前存在。
2. 中断 B 的接收阶段，确认 A 完全不变。
3. 完整上传 B，确认 B 替换 A，且不残留 `.bak`。
4. 原子提交 rename 故障通常需要临时测试注入或受控 FFat 故障才能稳定触发；不要通过破坏分区来制造。若无法在实板安全触发，保留为主机模拟已覆盖项。

## 6. 回退

- 导入协议失败：执行 `abort` 或 RESET；接收临时文件不会替换歌曲。
- 覆盖提交失败且返回 `COMMIT_FAILED`：旧 final 应仍在或已 rollback；立即 `list` 并备份分区，不继续覆盖。
- 返回 `ROLLBACK_FAILED`：停止所有写入。重新运行同一 importer 会尝试从隐藏 `.bak` 恢复；仍失败则使用受控 FFat 镜像备份恢复，不能格式化。
- 测试结束后可以重新上传正式固件；本任务不修改正式源码。正式固件启动扫描只识别最终 `.mspkg/.msp`，不会把隐藏 temp/backup 当歌曲。

编译成功不等于实板通过；最终仍需完成正常、坏包、断线、覆盖和重启可见性测试。
