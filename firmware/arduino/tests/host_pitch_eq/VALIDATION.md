# 音高校正开关离线验收记录 / Offline validation

日期：2026-09-03。范围：正式应用、文档和离线测试；没有打开设备串口、上传固件或修改歌曲。

## 结果

- 主机测试全部通过：v1 迁移、v2 往返和损坏记录、四项滚动设置、四音色与全部音高增益、起音和尾音锁存、192 采样八度过渡、静音与限幅。
- 使用修改前正式源码备份，Play/Song × 四音色的测试序列在 ON 状态下逐采样一致。仅替换主机硬件 I/O，执行实际音频任务；不改变既有声部分配算法。
- 编译成功，退出码 0。Arduino-ESP32 3.3.11，`--warnings all`：**无 warnings**。
- Flash：819351 / 3145728 字节（CLI 报告 26%）。
- 全局 RAM：38940 / 327680 字节（CLI 报告 11%）；剩余 288740 字节用于局部变量等。此数字不代表运行期堆、任务栈及歌曲加载后的峰值用量。
- 默认 OFF；有效 v1 设置的其他三个值不变。编译和主机桩不能替代断电保存、听感、OLED 与供电的实板验收。

## 完整 FQBN

来自未修改的 `firmware/arduino/config/board.ps1`：

```text
esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=info,PSRAM=opi,EraseFlash=none,JTAGAdapter=builtin
```

编译使用 `arduino-cli compile --fqbn <上述完整值> --library firmware/arduino/libraries/MiniSynthBoard --build-path firmware/arduino/build/mini_synth_v1_pitch_eq --warnings all firmware/arduino/apps/mini_synth_v1`，不含上传参数。

本地完整编译输出：`firmware/arduino/logs/mini_synth_v1_pitch_eq_compile.log`。本地产物：`firmware/arduino/build/mini_synth_v1_pitch_eq/`。源码备份：`firmware/arduino/backups/pitch-eq-20260903-221649/mini_synth_v1.ino`。这些构建/日志/备份目录按仓库原有规则不提交 Git。

## 后续实板结果

用户随后已自行烧录新版固件，并用较大的全频喇叭完成听感验证，确认 PITCH EQ OFF 更适合该扬声器。原小喇叭谐振约 950 Hz，在约 800 Hz–1.2 kHz 的明显峰值会使高音偏强，适合使用 ON 逐音衰减；全频喇叭通常使用 OFF。未知喇叭仍应从低总音量比较两种状态。无需写入 FFat、重新初始化曲库或改变板卡/分区。

## English summary

Host regression and the real ESP32-S3 production build passed. EQ ON is sample-identical to the pre-change implementation for both apps and all four timbres in the test sequences. Flash: 819351 bytes; static RAM: 38940 bytes; warnings: none; exit code: 0. The user later flashed the firmware and confirmed that EQ OFF suits the larger full-range replacement speaker. The implementation turn itself did not upload firmware or change the device library.
