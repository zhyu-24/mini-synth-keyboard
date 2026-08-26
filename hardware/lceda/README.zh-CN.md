# 立创 EDA 专业版工程

[English](README.md)

## 文件说明

- `elec_piano.eprj2` 是最新的可编辑立创 EDA 专业版工程数据库。它于 2026-08-26 从本地项目复制，原项目最后修改时间为 2026-08-16。
- `archive/elec_piano_2026-08-08-21-36.epro2` 是较早的便携工程快照，仅作为恢复参考，不是当前设计的权威版本。

请使用立创 EDA 专业版打开 `elec_piano.eprj2`。允许较新版本的软件迁移数据库格式前，应先做好本地备份。

下载后可使用 `SHA256SUMS.txt` 核对文件完整性。在 PowerShell 中运行：

```powershell
Get-FileHash .\elec_piano.eprj2 -Algorithm SHA256
Get-FileHash .\archive\elec_piano_2026-08-08-21-36.epro2 -Algorithm SHA256
```

## 制造警告

本项目发布的是设计源文件，并不保证其自动满足所有板厂规则，也不保证任意元器件替代方案安全。请复核工程，并从准备制造的确切版本重新生成 Gerber、钻孔、BOM 和贴片坐标文件。
