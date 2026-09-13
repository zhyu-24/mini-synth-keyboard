# 变更记录

本文件记录对使用者或开发者有意义的变化。当前尚未建立项目级 Git tag；在第一次正式发布前，内容保留在 `Unreleased`。版本含义见 [`VERSIONING.zh-CN.md`](VERSIONING.zh-CN.md)。

## Unreleased

### Added

- Settings 新增全局 `PITCH EQ` 开关，默认关闭并持久化，同时作用于 Play 和 Song。
- 新增 PITCH EQ 的 C++ 主机回归，支持与修改前正式源码逐采样比较。
- 新增当前状态、版本管理、测试导航、Markdown 链接检查和 GitHub 主机侧自动检查。

### Changed

- 设置记录升级为 v2，并兼容读取 12 字节 v1 记录；保留原音量、八度模式和 Play 音色。
- 固定 U8g2 2.36.19，由 Arduino 环境脚本安装和检查。
- 固件测试文档改为区分首次上电、功能隔离、历史调音和主机回归。

### Fixed

- 音乐库 CLI 单元测试显式统一子进程 UTF-8，在中文 Windows 与 Python UTF-8 模式组合下不再发生输出解码错误。
- 合并两份重复的 MUSB 单元测试入口，保留 `tools/usb-import/tests/` 为唯一自动发现位置，并补回删除/初始化命令编解码覆盖。

### Hardware validation

- 新版较大全频喇叭配合 PITCH EQ OFF 已完成烧录和实板听感验证。
- PITCH EQ ON 适用于原先谐振约 950 Hz、在约 800 Hz–1.2 kHz 区间导致高音偏响的小喇叭；全频喇叭通常保持 OFF。

## 2026-08-26 — Initial public repository history (untagged)

- `e1017a1`：首次开源提交。
- `041579e`：加入简体中文文档；这是审计时 `origin/main` 的最新提交。

这两次提交没有项目级版本标签，本节不追溯赋予语义化版本号。
