# USB MSPKG 主机上传工具

跨平台 Python/pyserial 客户端。完整构建、安全与实板流程见：

`firmware/arduino/tests/07_usb_mspkg_import_test/README.md`

Windows PowerShell 快速开始：

```powershell
py -3 -m pip install -r .\tools\usb-import\requirements.txt
py -3 .\tools\usb-import\usb_mspkg_import.py ports
py -3 .\tools\usb-import\usb_mspkg_import.py --port COM13 status
py -3 .\tools\usb-import\usb_mspkg_import.py --port COM13 list
py -3 .\tools\usb-import\usb_mspkg_import.py --port COM13 upload .\song.mspkg --name SONG01.MSP
```

自动测试：

```powershell
py -3 -m unittest discover -s .\tools\usb-import\tests -v
```


## 音乐库管理命令

```powershell
# 删除一首设备文件歌（不会删除内嵌兜底歌曲）
python .\tools\usb-import\usb_mspkg_import.py --port COM12 delete SONG01.MSP

# 仅当 status 显示 ffat=unavailable 时，初始化/修复音乐存储
# 会删除全部文件歌，不会擦除固件、设置或BLE配对
python .\tools\usb-import\usb_mspkg_import.py --port COM12 init-storage --confirm ERASE-MUSIC-LIBRARY
```

COM口始终通过 `--port` 在运行时指定，不写死在工具中。普通上传/删除不叫“烧录固件”，而是通过正式固件的MUSB协议同步音乐库。
