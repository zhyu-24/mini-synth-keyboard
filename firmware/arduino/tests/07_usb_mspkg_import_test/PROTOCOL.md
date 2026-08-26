# Mini Synth USB MSPKG Import Protocol v1

本协议运行在 ESP32-S3 原生 **Hardware CDC/JTAG** 串口上。设备使用 Arduino `Serial`，不使用 TinyUSB。多字节整数均为 little-endian。

## 1. 帧格式

每帧由固定 20 字节头和 payload 组成：

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | `char[4]` | magic `MUSB` |
| 4 | `uint8` | protocol version = 1 |
| 5 | `uint8` | command/response |
| 6 | `uint16` | flags，v1 必须为 0 |
| 8 | `uint32` | sequence，由主机分配；重试同一请求时必须复用 |
| 12 | `uint32` | payload bytes，最大 1152 |
| 16 | `uint32` | payload IEEE CRC-32；空 payload 为 0 |

设备逐字节寻找 `MUSB`，丢弃此前噪声。版本错误、超长帧或 CRC 错误会产生机器可读错误并重置解析器；截断帧在 1 秒无新字节后丢弃，下一次主机重试可重新同步。设备不会输出协议外文本。

## 2. 命令

| 值 | 名称 | 请求 payload | 成功响应 |
|---:|---|---|---|
| `0x01` | LIST | 空 | 0..N 个 LIST_ENTRY，随后 LIST_DONE |
| `0x02` | STATUS | 空 | STATUS_REPLY |
| `0x03` | BEGIN | 文件长度、整文件 CRC、文件名 | ACK |
| `0x04` | CHUNK | offset、chunk length、数据 | ACK |
| `0x05` | FINISH | 空 | ACK；只在验证并提交成功后返回 |
| `0x06` | ABORT | 空 | ACK；删除本次临时文件，不动最终文件 |
| `0x80` | ACK | — | — |
| `0x81` | ERROR | — | — |
| `0x82` | LIST_ENTRY | — | — |
| `0x83` | LIST_DONE | — | — |
| `0x84` | STATUS_REPLY | — | — |

### BEGIN payload

```text
uint32 file_length
uint32 whole_file_crc32
uint8  filename_length
uint8  filename[filename_length]
```

文件名必须为 1..48 字节 ASCII，只允许字母、数字、点、下划线、连字符；不得以点开头、不得含连续 `..`、斜杠或反斜杠；扩展名只能是 `.mspkg` 或 `.msp`。设备只在 FFat 根目录创建最终文件。

文件上限为 `32 + 4096 + 512 KiB = 528416` 字节，对应正式 loader 的 512 KiB payload 边界、32 字节容器头和最多 4096 字节 metadata。

### CHUNK payload

```text
uint32 offset
uint16 chunk_length       # 1..1024
uint16 reserved           # 必须为 0
uint8  data[chunk_length]
```

正常 offset 必须等于 ACK 中的 `next_offset`。如果 ACK 丢失，主机可以用相同 sequence 重发整个帧；设备缓存最近上传请求的响应，不重复执行。即使使用新 sequence 重发旧 chunk，设备也会逐字节核对已写入区域：完全相同则返回当前 offset，不同则报 `OFFSET_MISMATCH`。超前、重叠到未写区域或长度越界均拒绝。

### ACK / ERROR payload（16 字节）

```text
uint8  request_command
uint8  device_state
uint16 error_code          # ACK 通常为 0
uint32 next_offset
uint32 total_length
uint32 expected_file_crc32
```

每个 CHUNK ACK 的 `next_offset/total_length` 是设备端权威进度。

### STATUS_REPLY payload（20 字节）

```text
uint8  device_state
uint8  ffat_mounted
uint16 last_error_code
uint32 next_offset
uint32 total_length
uint32 expected_file_crc32
uint32 running_file_crc32
```

状态：`0 IDLE`、`1 RECEIVING`、`2 VALIDATING`、`3 COMMITTING`、`4 ERROR`。

### LIST_ENTRY payload

```text
uint32 file_size
uint8  filename_length
uint8  filename[filename_length]
```

LIST 只返回根目录中非隐藏的 `.mspkg` / `.msp` 普通文件。LIST_DONE payload 为 `uint16 entry_count`。

## 3. 错误码

| 值 | 名称 | 含义 |
|---:|---|---|
| 0 | OK | 成功 |
| 1 | BAD_FRAME_CRC | 帧 payload CRC 错误 |
| 2 | PROTOCOL_VERSION | 协议版本错误 |
| 3 | UNKNOWN_COMMAND | 未知命令 |
| 4 | BAD_PAYLOAD | payload 结构错误 |
| 5 | UNSAFE_FILENAME | 文件名不安全 |
| 6 | FILE_TOO_LARGE | 超出上传边界 |
| 7 | FS_NOT_MOUNTED | FFat 未挂载；设备不会自动格式化 |
| 8 | BUSY | 已有不同上传会话 |
| 9 | NO_UPLOAD | 当前没有上传会话 |
| 10 | OFFSET_MISMATCH | chunk 偏移或重复数据不匹配 |
| 11 | CHUNK_LENGTH | chunk 长度/范围非法 |
| 12 | FILE_IO | 文件读写失败 |
| 13 | FILE_LENGTH | 实收长度不等于声明长度 |
| 14 | FILE_CRC | 整文件 CRC 不等于 BEGIN 声明 |
| 15 | MSPKG_MAGIC | 容器 magic 错误 |
| 16 | MSPKG_VERSION | MSPKG 版本/flags/ABI 不支持 |
| 17 | MSPKG_TYPE | 不是 SEQUENCE |
| 18 | MSPKG_DECLARED_SIZE | 头、metadata、payload 声明尺寸不一致 |
| 19 | METADATA_CRC | metadata CRC 错误 |
| 20 | PAYLOAD_CRC | payload CRC 错误 |
| 21 | PAYLOAD_FORMAT | `MSQ1` 或序列基本结构非法 |
| 22 | COMMIT_FAILED | 提交 rename/remove 失败，但旧 final 已保留或恢复 |
| 23 | ROLLBACK_FAILED | final→backup 后无法恢复，需人工检查 `.bak` |
| 24 | NO_SPACE | FFat 空间不足/短写 |
| 25 | FRAME_TOO_LARGE | 帧 payload 超过 1152 字节 |

## 4. 验证与提交语义

1. BEGIN 只创建固定隐藏临时文件 `/.~usb-upload.tmp`，不会改动目标 final。
2. CHUNK 顺序写入，使用常量大小缓冲；设备同时计算整文件 CRC。
3. FINISH 检查实收长度与整文件 CRC，再从临时文件验证：`MSPK`、v1.0、SEQUENCE、flags/ABI、总声明尺寸、metadata/payload CRC、完整 TLV 边界、`MSQ1` 头及记录总尺寸。
4. 若 final 不存在：把 temp rename 为 final。
5. 若 final 已存在：先把 final rename 为 `/.~<filename>.bak`，再把 temp rename 为 final；第二步失败时立即将 backup rename 回 final。成功后删除 backup。
6. 启动时只做安全恢复：若同时有 final 和 backup，保留 final、删 backup；若只有 backup，恢复 final；删除未提交的固定 temp。绝不格式化 FFat。
7. 30 秒无上传流量会关闭并删除 temp，旧 final 始终不受影响。

FFat/FATFS 不提供跨掉电事务，因此“两次 rename 之间掉电”依靠同一独立测试程序下次启动的 recovery 完成。若之后先刷入别的固件，应该先运行本 importer 一次或人工检查隐藏 backup。
