# Mini Synth Keyboard 当前项目状态

> 状态快照：2026-09-13。本文是“现在做到哪里”的公开权威入口；历史变化看 [`CHANGELOG.md`](CHANGELOG.md)，长期版本规则看 [`VERSIONING.zh-CN.md`](VERSIONING.zh-CN.md)。具体实现仍以源码和对应格式/协议文档为准。

## 一句话结论

硬件源文件、正式固件、歌曲格式、USB 协议和音乐库管理器已形成一套可继续开发的基线；Play、Song、BLE Keyboard、MUSB、音乐库流程和音高校正开关已有实板使用记录。新版较大全频喇叭配合音高校正 OFF 已完成烧录和听感验证。Loop、Beat、Pet、Game 尚未实现。

## 权威文件地图

| 主题 | 权威入口 | 说明 |
|---|---|---|
| 项目现状 | 本文 | 只写当前完成度、风险和下一步 |
| 版本与发布 | [`VERSIONING.zh-CN.md`](VERSIONING.zh-CN.md) | 解释项目、固件、协议、设置和硬件版本 |
| 正式固件 | [`mini_synth_v1.ino`](firmware/arduino/apps/mini_synth_v1/mini_synth_v1.ino) | 唯一正式 Arduino 应用入口 |
| 正式功能/操作 | [`apps/README.zh-CN.md`](firmware/arduino/apps/README.zh-CN.md) | Play、Song、Settings、BLE、MUSB 行为 |
| 引脚 | [`MiniSynthPins.h`](firmware/arduino/libraries/MiniSynthBoard/src/MiniSynthPins.h) | 固件唯一 GPIO 定义；不要复制裸 GPIO |
| 完整 FQBN/依赖版本 | [`board.ps1`](firmware/arduino/config/board.ps1) | Arduino-ESP32 3.3.11、U8g2 2.36.19、N16R8 选项 |
| 硬件速查 | [`HARDWARE_REFERENCE.zh-CN.md`](HARDWARE_REFERENCE.zh-CN.md) | 电气假设、测试点和安全约束 |
| 可编辑原理图/PCB | [`hardware/lceda/`](hardware/lceda/) | `elec_piano.eprj2` 是当前工程；哈希见同目录清单 |
| 歌曲容器 | [`MSPKG_V1.zh-CN.md`](tools/media-import/MSPKG_V1.zh-CN.md) | 包格式与转换边界 |
| USB 设备协议 | [`PROTOCOL.md`](firmware/arduino/tests/07_usb_mspkg_import_test/PROTOCOL.md) | MUSB v1 帧、命令和错误码 |
| 音乐库操作 | [`tools/music-library/README.md`](tools/music-library/README.md) | 增量导入、覆盖、同步、重排、删除、初始化 |
| 测试导航 | [`firmware/arduino/tests/README.zh-CN.md`](firmware/arduino/tests/README.zh-CN.md) | 首次上电、隔离测试、历史工具和主机回归 |

根目录 `AGENT.md`、`FIRMWARE_HANDOFF.md` 是本地 Agent 交接材料，按 `.gitignore` 不公开，也不应替代本文成为项目事实源。构建目录、日志、备份和生成歌曲同样不进入版本库。

## 当前功能矩阵

| 模块 | 当前状态 | 证据与边界 |
|---|---|---|
| 硬件设计源 | 可编辑、完整性已核对 | 当前 `.eprj2` SHA-256 为 `844621f4a98eb44d95c989bc3cf9edac758f37babbf94eb05fe17a1525102eff`，与清单一致；仓库不提供可直接下单的冻结 Gerber/BOM/CPL 制造发布包 |
| 基础 Play | 已有实板基线 | 七键和弦、三八度、四音色、音量、OLED、12 ms 八度过渡；`MASTER_PEAK=18000` 与原 21 音表是不可随意修改的基线 |
| PITCH EQ 设置 | 代码、主机、编译和实板听感通过 | 默认 OFF、NVS v2、兼容 v1；ON 与修改前测试序列逐采样一致。用户已在新版全频喇叭设备上烧录验证 OFF 的效果 |
| Song | 已有实板基线 | 四首内嵌兜底歌；存在有效 FFat 文件曲时显示最多 30 首文件歌；E/F 只选择，C 播放/暂停，D 重启 |
| MusicXML/MSPKG | 主机测试通过且有实板导入记录 | 单 part、最多四个同时音；常见顺序反复可展开，复杂跳转明确拒绝；超过四音按当前转换策略简化 |
| 音乐库管理器 | 主机测试通过且有实板操作记录 | 增量保留、同名覆盖、精确同步、重排、单/多曲删除和显式存储初始化 |
| MUSB v1 | 主机协议测试通过且已用于实板 | 不等于烧录固件；普通操作只改 FFat 文件歌曲。`init-storage` 是破坏性格式化，必须明确确认 |
| BLE Keyboard | 已有独立及正式功能实板记录 | QWER/ASDF + Left Alt 的两字节 NKRO，Alt 抑制 R，退出/断线释放与重连门控 |
| Loop / Beat / Pet / Game | 未实现 | 仍为 `COMING SOON` 安全静音页面；这是后续功能开发的主要范围 |
| 自动化测试 | 主机侧已建立 | Python 单元测试、C++ 音频桩和文档链接检查；没有自动化硬件在环测试 |

## 已知未闭环事项

1. 新版较大全频喇叭已经实测适合 PITCH EQ OFF，但其确切料号、额定功率、安装方式和硬件 revision 尚未写入硬件工程/BOM；形成可复制硬件版本前仍需补齐这些信息。
2. 仓库没有冻结制造输出。制造前必须从当前 `.eprj2` 重新导出并复核 Gerber、钻孔、BOM、CPL、板框、封装和连接器方向。
3. 没有 Git tag 或 GitHub Release；`mini_synth_v1`、MUSB v1、MSPKG v1、Settings v2 含义不同，不能据其推断整个项目的发布版本。

## 维护性观察

- 正式 `.ino` 目前约 6234 行、204 KiB，内含内嵌歌曲数组、应用状态、OLED、音频、BLE、FFat 与 MUSB。单文件仍能编译和追溯，但继续同时增加多个应用会提高回归风险。
- 不建议在功能提交中顺手大拆文件。先冻结当前实板行为；之后可单独做一次“零行为变化”的模块化提交，优先把生成的内嵌歌曲数据、协议结构和纯函数移到独立文件，并以现有主机输出比较和真编译保护。
- 音乐库后端与 GUI 已分文件，但后端约 1227 行。后续新增能力应继续以计划/执行接口和单元测试扩展，不要把设备 I/O 写回 GUI 事件代码。
- GitHub 主机检查不编译 ESP32 工具链，也不接实板；正式 Arduino 编译和硬件验收仍是发布清单中的独立门槛。

## 2026-09-13 离线验证

不会访问串口的测试结果：

| 测试 | 结果 |
|---|---:|
| MusicXML 反复展开 | 4 / 4 通过 |
| 音乐库计划、CLI、容量与 BUSY 重试 | 40 / 40 通过 |
| MUSB 编解码与原子提交模拟 | 11 / 11 通过 |
| PITCH EQ C++ 主机回归 | 通过 |
| Markdown 本地链接 | 通过（由仓库检查脚本持续验证） |

正式 ESP32-S3 应用最近一次真实编译记录（2026-09-03）：Arduino-ESP32 3.3.11、完整 `board.ps1` FQBN、`--warnings all`；Flash 819351 / 3145728 字节（26%），静态 RAM 38940 / 327680 字节（11%），0 warnings。见 [`VALIDATION.md`](firmware/arduino/tests/host_pitch_eq/VALIDATION.md)。本轮文档与测试整理没有再改正式 `.ino`。

统一主机命令：

```powershell
python -m pip install -r .\requirements-dev.txt
python -m unittest discover -s .\tools\media-import\tests -v
python -m unittest discover -s .\tools\music-library\tests -v
python -m unittest discover -s .\tools\usb-import\tests -v
python .\tools\ci\check_markdown_links.py
wsl -d Ubuntu-22.04 -- python3 /mnt/d/Projects/mini-synth-keyboard/firmware/arduino/tests/host_pitch_eq/run_tests.py
```

最后一条需要 WSL 中的 g++；其他命令使用 Windows Python。所有命令均不连接设备。

## 2026-09-13 Git 收口

审计开始时，分支 `main` 与 `origin/main` 同在 `041579e`（2026-08-26），历史只有 2 个提交且没有 tag。本次按内容拆成两组，避免把固件功能和仓库维护混在一个不可读提交里：

1. `c2e5791 feat(firmware): add persistent pitch EQ setting`：PITCH EQ 固件、双语应用说明和主机测试；
2. `chore(repo): document status and add host checks`：本文所在的仓库整理提交，包含状态/版本文档、测试导航、依赖固定、CI/链接检查和 Windows 编码测试修复。

本次两组改动通过测试并推入 `main` 后，仓库即可作为下一阶段开发基线；是否建立第一个项目版本标签仍按 [`VERSIONING.zh-CN.md`](VERSIONING.zh-CN.md) 的发布清单另行决定。

## 建议的后续开发顺序

1. 推送后确认 GitHub 主线和本地 commit 一致、自动检查通过。
2. 先设计并评审音乐写入时的自定义设备显示名称，再单独开发和实板验收。
3. 在 Loop、Beat、Pet、Game 中一次只选一个功能，先写需求与资源预算，再建功能分支。
4. 若要复制新版扬声器方案，把确切料号、功率和安装/腔体条件作为独立硬件修订补入 BOM 与工程说明。
