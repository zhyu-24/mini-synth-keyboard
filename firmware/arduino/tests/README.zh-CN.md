# 板卡上电调试草图

[English](README.md)

请按下列顺序执行，并遵循项目根目录的 `07-hardware-test-guide.md`。

## `01_mcu_usb_test`

前提：OLED 和扬声器均未连接，TP1、TP2、TP4、TP5 检查通过。该程序测试原生 USB、代码执行、16 MB Flash、8 MB PSRAM 和复位原因，并始终保持功放关闭。

## `02_keyboard_test`

前提：MCU/USB 测试通过。该程序为九个低电平有效按键输出稳定的按下/释放事件。

## `03_i2c_scan`

前提：已经对照 PCB 丝印检查 OLED 引脚顺序，在断电状态下连接 OLED，并在连接后重新检查 TP2。该程序在 SDA=GPIO10、SCL=GPIO11 上扫描 I2C 总线，不预设 OLED 控制器型号或地址。

## 尚未包含的项目

- OLED 显示测试：应等待实物确认 SSD1315/SSD1306 兼容性、分辨率、地址和方向；
- I2S/音频测试：应等待 J5 固定焊片隔离、扬声器兼容性和首次发声安全条件得到确认；
- 完整合成器：应等待所有独立测试通过。
