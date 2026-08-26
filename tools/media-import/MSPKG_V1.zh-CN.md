# MSPKG v1——Mini Synth 音乐包格式

[English](MSPKG_V1.md)

`*.mspkg` 是九键 ESP32-S3 设备使用的传输容器。版本 1 定义了 `SEQUENCE`；`AUDIO` 使用相同容器预留，但在 MP3 播放通过实物验证前不会启用。

所有整数字段均为小端序，字符串使用 UTF-8。CRC 使用 IEEE CRC-32，与 Python `zlib.crc32` 的实现一致。

## 容器头——32 字节

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | `char[4]` | 魔数 `MSPK` |
| 4 | `uint8` | 格式主版本 = 1 |
| 5 | `uint8` | 格式次版本 = 0 |
| 6 | `uint8` | 内容类型：1 = sequence，2 = audio |
| 7 | `uint8` | flags；v1 = 0 |
| 8 | `uint32` | 头部字节数 = 32 |
| 12 | `uint32` | metadata 字节数 |
| 16 | `uint32` | payload 字节数 |
| 20 | `uint32` | metadata CRC-32 |
| 24 | `uint32` | payload CRC-32 |
| 28 | `uint32` | 最低固件 ABI；v1 = 1 |

容器头之后依次是 metadata TLV 和 payload 字节。

## Metadata TLV

每条记录以 `<uint16 tag, uint8 type, uint8 flags, uint32 length>` 开头，随后是 `length` 字节的值。

类型：1 UTF-8，2 `uint32`，3 `int32`，4 原始字节，5 布尔字节。

Sequence v1 使用的标签：

| 标签 | 含义 |
|---:|---|
| 1 | 标题 |
| 2 | 作者/来源标签 |
| 3 | 原文件格式 |
| 4 | 设备配置 ID |
| 5 | 来源 SHA-256，原始 32 字节 |
| 16 | 每四分音符 tick 数 |
| 17 | 音符事件数量 |
| 18 | 总时长 tick |
| 19 | 建议播放移调半音数（`int32`） |
| 20 | 原始最低 MIDI 音高 |
| 21 | 原始最高 MIDI 音高 |
| 22 | 峰值同时发声音符数 |
| 23 | 七个自然音键教学兼容性（`bool`） |
| 24 | UTF-8 转换摘要 |
| 25 | 建议音色 UTF-8：`SINE`、`8BIT`、`ORGAN`、`PIANO_SYNTH` 或未来注册值 |

建议音色只是提示。设备可以允许用户在不重新转换歌曲的情况下覆盖它。`PIANO_SYNTH` 表示合成钢琴近似音色，不要求钢琴采样资源。

遇到未知标签时，必须根据其 length 跳过。

## Sequence payload

### Payload 头——28 字节

`<4s HH III HH I>`

| 字段 | 含义 |
|---|---|
| magic | `MSQ1` |
| ticks per quarter | v1 默认 480 |
| note event record bytes | 12 |
| note event count | 合并延音线后事件数量 |
| tempo record count | 速度变化数量 |
| duration ticks | 最后一个音符/休止时间线结束位置 |
| time-signature count | 拍号变化数量 |
| reserved | 0 |
| reserved | 0 |

### 速度记录——8 字节

`<uint32 tick, uint32 microseconds_per_quarter>`

### 拍号记录——8 字节

`<uint32 tick, uint8 numerator, uint8 denominator_power_of_two, uint8 midi_clocks, uint8 thirty_seconds_per_quarter>`

对于 4/4 拍，分母幂为 2，因为 `2^2 = 4`。

### 音符事件——12 字节

`<uint32 start_tick, uint32 duration_tick, uint8 midi_pitch, uint8 velocity, uint8 voice, uint8 flags>`

- MIDI 音高使用标准 0–127 十二平均律音符编号；
- 如果来源没有可靠力度，v1 的力度默认值为 100；
- Voice 是从 0 开始的紧凑来源声部索引；
- v1 的 Flags 为 0；未来版本可以定义奏法或教学角色；
- 事件依次按开始 tick、voice 和 MIDI 音高排序。

## 转换原则

- 事件记录保留来源音高。建议移调属于 metadata，可由播放器接受或覆盖；
- 打包前合并延音线连接的音符；
- 转换器必须展开反复或明确拒绝；v1 不保存乐谱反复标记；
- MusicXML 的版面、歌词、制谱信息、踏板、任意装饰音和不支持的记谱内容不会进入 sequence payload；
- 转换器必须报告任何时间量化、音高修改、声部删除或不支持特性，绝不能静默把变化音替换为自然音；
- `MASTER_PEAK=18000` 是固件输出上限，不是音乐包字段，导入媒体不能覆盖它。
