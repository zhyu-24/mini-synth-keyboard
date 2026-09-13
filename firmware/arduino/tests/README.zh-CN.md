# 固件测试导航

[English](README.md)

本目录同时保存“首次板卡上电测试”“功能隔离测试”“历史调音工具”和“不接设备的主机回归”。文件名前的数字主要表示历史执行阶段，不是当前推荐的全量测试顺序；`06_`、`07_` 重号因此不表示版本冲突。

## 首次板卡上电顺序

必须结合根目录 [`07-hardware-test-guide.md`](../../../07-hardware-test-guide.md)，按安全门槛逐项进行。

## `01_mcu_usb_test`

前提：OLED 和扬声器均未连接，TP1、TP2、TP4、TP5 检查通过。该程序测试原生 USB、代码执行、16 MB Flash、8 MB PSRAM 和复位原因，并始终保持功放关闭。

## `02_keyboard_test`

前提：MCU/USB 测试通过。该程序为九个低电平有效按键输出稳定的按下/释放事件。

## `03_i2c_scan`

前提：已经对照 PCB 丝印检查 OLED 引脚顺序，在断电状态下连接 OLED，并在连接后重新检查 TP2。该程序在 SDA=GPIO10、SCL=GPIO11 上扫描 I2C 总线，不预设 OLED 控制器型号或地址。

## `04_oled_display_test`

断电连接并核对 OLED 后，验证 128×64、地址、方向、双色区域布局和刷新。

## `05_i2s_audio_test`

只有扬声器连接与供电检查通过后，才以低音量短时验证 I2S/NS4168。编译通过不代表实板通过；上传会改变设备，串口必须现场确认。

## 功能隔离与历史测试

- `06_ble_keyboard_test`：BLE 两字节 NKRO 键盘、Alt 抑制 R、释放门控、重连与 OLED。历史记录表明已完成实板验收，相关改动后仍应回归。
- `07_usb_mspkg_import_test`：旧的单文件 MUSB/FFat 隔离原型及协议说明。正式固件已经集成 MUSB v1；本目录保留用于协议追溯和故障隔离。
- `06_harmonic_ab_test`：未合入正式路线的谐波 A/B 实验。
- `07_pure_tone_level_calibrator`：21 音响度校准工具；校准值已进入正式固件，仅用于追溯或明确的重新标定。

这些目录保留原名以避免旧文档和外部链接失效。正常开发入口始终是 `../apps/mini_synth_v1/mini_synth_v1.ino`。

## 不连接设备的回归

- `host_pitch_eq`：执行从正式源码提取的设置与音频路径，运行方法见 [`host_pitch_eq/README.md`](host_pitch_eq/README.md)。
- MusicXML、音乐库和 USB 协议测试位于仓库 `tools/*/tests`，统一命令见 [`PROJECT_STATUS.zh-CN.md`](../../../PROJECT_STATUS.zh-CN.md)。

目前没有自动化的硬件在环测试；GitHub 自动检查只运行不会访问串口的主机测试。
