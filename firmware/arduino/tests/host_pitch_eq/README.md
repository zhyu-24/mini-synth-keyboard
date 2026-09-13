# 音高校正主机回归 / Pitch EQ host regression

本测试不打开串口、不连接设备。需要 Python 3 和支持 C++17 的 g++（本机已使用 WSL Ubuntu 的 g++ 11.4）。

在仓库根目录运行：

```sh
python3 firmware/arduino/tests/host_pitch_eq/run_tests.py
```

可选：提供修改前的正式源码备份，额外比较开启校正后与旧版的逐采样输出：

```sh
python3 firmware/arduino/tests/host_pitch_eq/run_tests.py --baseline /path/to/old/mini_synth_v1.ino
```

Windows 可通过 `wsl -d Ubuntu-22.04 -- python3 /mnt/d/Projects/mini-synth-keyboard/firmware/arduino/tests/host_pitch_eq/run_tests.py` 运行。备份路径也需转换为 WSL 的 `/mnt/...` 路径。

测试从正式 `.ino` 提取实际函数、类型和常量，编译为 C++ 主机程序；没有另写一份设置或音频算法。硬件替身仅模拟 Preferences 字节存储、屏幕绘制、I2S 帧输出和任务输入。完整 `audioRenderTask` 保持原样，输出达到指定块数后由 I2S 替身结束。编译启用 `-Wall -Wextra -Werror` 和未定义行为检查。

覆盖范围：

- 88 种合法 v1 设置组合迁移；176 种 v2 开关/原设置组合往返；加载不写入，后续保存写 v2。
- 校验和逐位损坏、非法开关值 2–255、版本/长度/音量/音色/八度非法、读取失败、写入失败后重试。
- 四项循环选择、三行显示、E/F/G、底部提示和 BLE 标志保留。
- 128 个 MIDI 音高、21 个 Play 音、四音色；关闭时音高校正与归一化权重为 1。
- Play 和 Song 完整渲染，重叠四音及同拍和弦输入；开关变化后新起音生效、旧音及尾音锁存；12 ms / 192 采样八度过渡、静音、输出限幅和 100 ms 启动静音。
- 提供旧源码时：两种应用 × 四音色逐采样比较；校准表、内嵌歌曲、输入任务、启动顺序、波形生成器不变量比较。

该测试不验证真实 NVS 断电时序、RTOS 调度、OLED 光学显示、扬声器响度或供电稳定性，不能替代实板验收。

## English

Run `python3 firmware/arduino/tests/host_pitch_eq/run_tests.py` with Python 3 and a C++17 g++ compiler. Optionally pass `--baseline` pointing to the pre-PITCH-EQ sketch for sample-identical audio regression against the previous implementation.

The runner extracts production functions rather than reimplementing their logic. Only hardware I/O is stubbed; it executes the complete audio task, settings migration/persistence, and menu rendering. Tests include all MIDI pitches/four timbres, Play's 21 pitches, gain latching, release tails, octave glide, NVS malformed records, and UI behavior. The runner uses temporary generated files, warnings-as-errors and undefined-behavior checks. It never accesses a device; hardware/power-cycle/listening acceptance remains separate.
