# ESP32-S3-WROOM-1 基板天线净空与 USB 布线独立核查

> 状态：已完成（2026-08-08）

本文件将逐条记录基于乐鑫一手文档的核查结果；未将博客、论坛或 AI 生成内容作为依据。

## A. 「15 mm 净空」

**判定：❌错误（整体结论）。**

- **原文是否存在：是。** 出自 *ESP32-S3 Hardware Design Guidelines*，`PCB Layout Design > General Principles of PCB Layout for Modules (Positioning a Module on a Base Board)`。原文紧接模块基板摆放/净空图之后：`After the base board is placed in the end product ... consider the impact of the housing ... Ensure that the PCB antenna on the base board also has a sufficiently large clearance area inside the housing. A clearance of at least 15 mm is recommended in all directions.`
- **对该段的严格解读：**它明确属于 end-product/housing 语境，但没有逐字规定“15 mm 的两端只能是天线和外壳”；把它单独解读为外壳相关建议可以，进而断言“15 mm 不构成板内约束”则不成立。
- **官方另有板级明确数值：有。**同一节的 `Keepout Zone for ESP32-S3 Module’s Antenna` 图（单位 mm）标出 `Min15`、`6`、`Max 1`、`Max 2` 及 `Clearance Area`；图前正文要求天线不能出板时切除天线两侧及下方基板。更直接地，乐鑫 *ESP-FAQ > Hardware Design > For modules with PCB antennas...* 写道：`The antenna area of the module and the area 15 mm outside the antenna should be kept clean (namely no copper, routing, components on it).` 这是官方、明确的“无铜/无走线/无器件”要求（FAQ 是通用模块问答，非仅 S3 专页）。
- **结论：**不能以“裸板、尚无外壳”为由删去 15 mm 板内净空。对 S3-WROOM-1，应同时遵从 S3 模块摆放图的 `Min15 × 6` clearance geometry，以及官方 FAQ 的通用 15 mm clean-area 表述；最终产品仍须实测吞吐量和通信距离。

**一手来源：**
1. Espressif, *ESP32-S3 Hardware Design Guidelines*, §`PCB Layout Design > General Principles of PCB Layout for Modules (Positioning a Module on a Base Board)`：https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/pcb-layout-design.html
2. Espressif, *ESP-FAQ*, §`Hardware Design > For modules with PCB antennas, what should be noted when I design the PCB and the housing of the antenna?`：https://docs.espressif.com/projects/esp-faq/en/latest/hardware-related/hardware-design.html

## B. 板载天线净空的做法

**判定：❌错误（“仅三条几何要求而非数字”这一总括错误；四条原文转述本身大体准确）。**

1. **“天线伸出基板之外、馈点靠板边”——✅ 原文存在且转述准确。** 官方是 `It is suggested to place the module’s on-board PCB antenna outside the base board, and the feed point ... close to the edge of the base board.` 图中带 ✓ 的位置称为 strongly recommended。
2. **“若不能伸出，馈点靠边，切掉两侧和下方基板”——✅ 原文存在且转述准确。** 原文正是 `cut off the base board on both sides of the antenna and below it`，且明说目的为减少 base-board material 对 PCB antenna 的影响、提供 sufficiently large clearance area。
3. **“不应置板中央、四周镂空”——✅ 原文存在且转述准确。** 原文：`the module should not be placed in the center of the board with clearance created by hollowing out on all four sides.` 但**官方没有给出为何禁止这一构型的进一步物理理由**；只能报告它把“靠边/角”列为 strongly recommended、四周镂空不推荐，不能把某种自拟机理说成官方解释。
4. **“附近要有足够地铜及密集地过孔”——✅ 原文存在且转述准确。** 原文：`sufficient ground copper and dense ground vias should be placed on the base board near the antenna.`

**重要修正：**官方不仅给几何文字，还在同节 Fig. `Keepout Zone for ESP32-S3 Module’s Antenna` 给了带单位 mm 的 suggested clearance-area 图，含 `Min15` 和 `6`（另有 Max 1/Max 2）。因此“官方无板级数字”不正确；图例本身仅写 `Clearance Area`，并未逐项定义为“无铜/无器件/无走线”，但 ESP-FAQ 对 15 mm clean area 作了明确三项解释（见 A）。

**一手来源：**
1. Espressif, *ESP32-S3 Hardware Design Guidelines*, §`PCB Layout Design > General Principles of PCB Layout for Modules (Positioning a Module on a Base Board)`（正文及 Fig. `Keepout Zone for ESP32-S3 Module’s Antenna`）：https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/pcb-layout-design.html
2. Espressif, *ESP-FAQ*, §`Hardware Design > For modules with PCB antennas...`：https://docs.espressif.com/projects/esp-faq/en/latest/hardware-related/hardware-design.html

## C. 「天线悬出板框」的机械理解

**判定：✅正确（尺寸公差及“约 6 mm”须精确表述）。**

- **独立模块及尺寸：✅** 官方 datasheet 将 WROOM-1 定义为带 `on-board PCB antenna` 的模块；§`10.1 Module Dimensions` 标称 **18 ± 0.2 × 25.5 ± 0.2 × 3.1 ± 0.15 mm**。因此“18 × 25.5 × 3.1 mm”作为标称尺寸正确。
- **天线段：✅约 6 mm。** §`11.1 PCB Land Pattern` Fig. 11-1 标出 `Antenna Area`，并有 **6 mm** 尺寸；同图另标 7.49 mm 的相关纵向尺寸。适宜称“官方 land-pattern 图给出的天线区域 6 mm 尺寸”，不应把它说成已精确测得的唯一辐射金属长度。
- **天线端短边焊盘：✅没有。** §`3.1 Pin Layout` 及 Fig. 11-1 显示 40 个 castellated pads 分布在两长边及与天线相反的短边，中央 EPAD 为 pin 41；标作 Antenna Area 的短边没有焊盘。官方 pin-layout 图也将此端标为 `Keepout Zone`。
- **悬出 6 mm 是否丢焊盘：✅不会。** 根据上述官方 pad 分布，这是模块自身天线端（无焊盘）越出基板，不是让基板伸“舌头”；只要基板边界不退到含边缘焊盘的区域，6 mm 悬出本身不损失焊盘。此最后一句是由官方尺寸/焊盘图得出的几何推断，并非 datasheet 的逐字承诺。

**一手来源：**
1. Espressif, *ESP32-S3-WROOM-1 & ESP32-S3-WROOM-1U Datasheet v1.8*, §`1 Module Overview`、§`3.1 Pin Layout`、§`10.1 Module Dimensions`、§`11.1 PCB Land Pattern`（Fig. 3-1、10-1、11-1）：https://documentation.espressif.com/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf
2. 同 datasheet, §`11.2 Module Placement for PCB Design`（明确引向 S3 Hardware Design Guidelines 的 module-placement 节）：同上 URL。

## D. 两层板的可行性与底层轴座

**判定：⚠️无法确认（原文准确；但“可把 9 个轴座放到底层且仍符合官方两层设计建议”不能确认）。**

- **引文：✅准确。** *ESP32-S3 Hardware Design Guidelines* 的顶级小节 `General Principles of PCB Layout for the Chip` 写：`A two-layer PCB design can also be used: Layer 1 (TOP): Signal traces and components. Layer 2 (BOTTOM): Do not place any components on this layer and keep traces to a minimum. Please make sure there is a complete GND plane for the chip, RF, and crystal.` 随后的 `Two-Layer PCB Design` 又写：`For a two-layer design, ensure to provide a continuous reference ground for the chip, RF, and crystal oscillator`、`Vias to the bottom layer should only be used when absolutely necessary`，及 `Note that there are no official two-layer modules. The figure above uses the ESP32 module as an example.`
- **适用范围：**该段标题确为 **for the Chip**，而文档开头又说本章以 ESP32-S3 module 为 example；所以不能把它解释为“专门针对模块底面禁止器件”的独立模块认证条款。但官方并没有另给“模块基板可任意层数、可随意占用底层”的豁免。
- **两层模块是否官方认可：部分可确认。**“two-layer PCB design can also be used”以及“figure above uses the ESP32 module as an example”表明乐鑫展示了模块例子；但同时明言没有 official two-layer modules，且推荐四层。它不是对具体两层成品板 RF/EMC 的性能保证。
- **9 个热插拔轴座：**若这些是穿孔/底面实体部件，占用了本应尽量连续的 Bottom GND 面，则与文字 `Do not place any components ... keep traces to a minimum` **直接冲突**，至少不是按官方两层推荐做法。不能仅以“模块已有屏蔽罩”否定风险；官方未给出允许此种底面元件密度的例外。实际可行性需保留 antenna clearance、底层在模块/RF 下连续地参考，并做 RF 范围/吞吐测试；是否达标未找到可替代实测的官方放行标准。

**一手来源：**
1. Espressif, *ESP32-S3 Hardware Design Guidelines*, §`PCB Layout Design > General Principles of PCB Layout for the Chip` 及 §`Two-Layer PCB Design`：https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/pcb-layout-design.html
2. Espressif, *ESP-FAQ*, §`Do Espressif Wi-Fi modules support single-layer PCBs?`（官方称已测试 2/4 层、推荐 4 层以获所需 RF 性能；该 FAQ 为通用模块结论）：https://docs.espressif.com/projects/esp-faq/en/latest/hardware-related/hardware-design.html

## E. USB 布线

**判定：❌错误（五句引用正确；“第一版可不做 90 Ω 阻抗控制”不应作为有官方/权威背书的结论）。**

- **五条引文：✅逐字准确。** S3 Hardware Design Guidelines 的 `USB` 节依次要求：预留靠 chip side 的 R/C；差分对平行且等长；**90 Ω ±10%**；尽量少过孔、必须过孔则每转换点加一对 ground-return vias；下方连续 reference layer（推荐 GND）；周围铺地。
- **速率：✅ ESP32-S3 是 USB 2.0 Full-Speed，不是 High-Speed。** 官方 *ESP32-S3 Series Datasheet* §`4.2.1.7 USB 2.0 OTG Full-Speed Interface`：`full-speed USB OTG interface`，支持 FS/LS；§`4.2.1.8` 还列出 USB Serial/JTAG 是 `USB Full-speed device`。乐鑫 USB 概览明确给 S2/S3 USB-OTG Full-Speed bus transfer rate **12 Mbps**。
- **“12 Mbps 所以不做 90 Ω 可接受”：❌不能据此成立。** 12 Mbps 不会自动取消传输线/反射约束；而本芯片制造商给出的唯一明确布局指标恰是 90 Ω ±10%。未找到乐鑫或 USB-IF 一手文件为 ESP32-S3 两层、未控制 90 Ω 的 PCB 提供豁免或保证。USB-IF 的 USB 2.0 规范对应链路/线缆阻抗并非“任意即可”；即使工程上短线常可能工作，也只能是风险取舍和实测结果，不能说官方认可。**设计建议：**向板厂索取两层叠层（介质厚度/铜厚），按其阻抗计算器/阻抗控制能力设线宽线距；至少满足连续底层 GND、短且等长、少 via。

**一手来源：**
1. Espressif, *ESP32-S3 Hardware Design Guidelines*, §`PCB Layout Design > USB`：https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/pcb-layout-design.html
2. Espressif, *ESP32-S3 Series Datasheet v2.2*, §`4.2.1.7 USB 2.0 OTG Full-Speed Interface`、§`4.2.1.8 USB Serial/JTAG Controller`：https://documentation.espressif.com/esp32-s3_datasheet_en.pdf
3. Espressif, *ESP-FAQ > USB > What are the USB features of ESP32-S2 and ESP32-S3?*（OTG supports Full-Speed）：https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/usb.html
4. Espressif, *ESP-IoT-Solution > USB-OTG Peripheral Introduction*（S2/S3 FS bus transfer rate 12 Mbps）：https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_overview/usb_otg.html
5. USB Implementers Forum, *USB 2.0 Specification*（规范原始发布页）：https://www.usb.org/document-library/usb-20-specification

## F. USB / UART 远离天线

**判定：✅正确（引文准确；但其对已封装模块外部 UART 的同等强制性，官方没有单独界定）。**

- **引文：✅准确。** 原文为：`The RF antenna should be placed away from high-frequency components... In addition, the USB port, USB-to-serial chip, UART signal lines (including traces, vias, test points, header pins, etc.) must be as far away from the antenna as possible. The UART signal line should be surrounded by ground copper and ground vias.`
- **章节性质：**它在 *ESP32-S3 Hardware Design Guidelines* 的 `PCB Layout Design > General Principles of PCB Layout for the Chip > RF` 下；该 `RF` 小节前半还讨论 chip 的 50 Ω RF trace、CLC matching 和 IPEX connector，故并非模块专属的摆放条款。模块专属要求在同页后面的 `General Principles of PCB Layout for Modules (Positioning a Module on a Base Board)`。
- **可得与不可得的结论：**不能引用该“chip/RF”段来声称乐鑫已专门规定“WROOM-1 屏蔽罩外的任意 UART 必须遵从同样级别/距离的规则”；未找到该模块豁免，也未找到数值距离。与此同时，官方模块摆放节要求最小化基板对模块天线的干扰，FAQ 也要求 15 mm clean area。因此在此两层键盘板上，USB 口、USB-UART 芯片、UART 线、测试点和排针仍应布局在天线 15 mm clean/clearance area 之外，并尽可能远离天线；UART 加地铜/地过孔是符合官方保守方向的做法。后一句是基于两段官方要求的设计推断，不是官方给 WROOM-1 单列的数值强制条款。

**一手来源：**
1. Espressif, *ESP32-S3 Hardware Design Guidelines*, §`PCB Layout Design > General Principles of PCB Layout for the Chip > RF`：https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/pcb-layout-design.html
2. 同文档，§`General Principles of PCB Layout for Modules (Positioning a Module on a Base Board)`：https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/pcb-layout-design.html
3. Espressif, *ESP-FAQ*, §`For modules with PCB antennas...`：https://docs.espressif.com/projects/esp-faq/en/latest/hardware-related/hardware-design.html

## 汇总判定表

| 编号 | 我的结论 | 判定 | 一手来源（文档名 + 章节 + URL） | 修正/补充 |
|---|---|---|---|---|
| A | 15 mm 仅指 housing，裸板不构成板内约束 | ❌错误 | *ESP32-S3 Hardware Design Guidelines*，`General Principles of PCB Layout for Modules`；*ESP-FAQ*，`For modules with PCB antennas...`。见 A 的 URL。 | housing 段确实存在；但同一 S3 节有 `Min15` clearance 图，FAQ 明确 15 mm 外须 clean（无铜/走线/器件）。不可因无外壳删除。 |
| B | 官方仅给几何做法、没有数值；外伸/切两侧下方/反对中央四周镂空/附近地铜地过孔 | ❌错误 | *ESP32-S3 Hardware Design Guidelines*，同 A。 | 四项文字均存在且转述基本正确；但并非“无数字”，Fig. 含 `Min15`、`6` 等。官方未解释为何四周镂空不推荐。 |
| C | WROOM-1 的模块天线端可悬出，约 6 mm，且不损焊盘 | ✅正确 | *ESP32-S3-WROOM-1 & WROOM-1U Datasheet v1.8*，§3.1、§10.1、§11.1。见 C URL。 | 标称 18×25.5×3.1 mm（含公差）；6 mm 为官方 land-pattern 图的 antenna-area 尺寸。天线短边无 castellated pad，故 6 mm 悬出是模块本体而非基板舌片。 |
| D | 两层基板 + 模块可行，底面可放 9 个轴座 | ⚠️无法确认 | *ESP32-S3 Hardware Design Guidelines*，`General Principles of PCB Layout for the Chip`、`Two-Layer PCB Design`。见 D URL。 | 官方允许两层但推荐四层，且要求底层不放元件、少走线、chip/RF/crystal 下完整地；又称无 official two-layer modules。9 个轴座不符合该推荐，未找到豁免/性能保证。 |
| E | USB 引文正确，FS 12 Mbps 所以首版不做 90 Ω 可接受 | ❌错误 | *ESP32-S3 Hardware Design Guidelines*，`USB`；*ESP32-S3 Series Datasheet*，§4.2.1.7。见 E URL。 | 五句引文准确，ESP32-S3 是 USB 2.0 Full-Speed（12 Mbps），不是 HS；但官方明确仍要求 90 Ω ±10%，未找到未控阻抗的官方豁免。 |
| F | USB/UART 必须尽量远离天线，UART 周围地铜/地过孔 | ✅正确 | *ESP32-S3 Hardware Design Guidelines*，`General Principles of PCB Layout for the Chip > RF`。见 F URL。 | 引文准确，但所在为 chip/RF 章节、不是模块专属章节；未找到针对已封装 WROOM-1 外部 UART 的数值距离或豁免。保守地仍应放在模块 15 mm clean/clearance 区外。 |

## 对两层板 + 模块方案额外重要的官方要求

1. **完整参考地不可被底面轴座/走线切碎。** S3 指南两层要求为 chip、RF、crystal 提供 `complete/continuous reference ground`，且 bottom 元件不放、走线最少；对模块没有另行豁免。务必把 WROOM-1 下方及 USB 对下方的 Bottom GND 连续性作为 layout DRC/人工检查项。
2. **模块天线的板级 clearance 不等于只留模块脚下无铜。** 模块必须靠板边；优先天线端越出板边。不能越出时，官方要求切除天线两侧及下方的**基板**；`Min15`/`6` suggested-clearance 图与 FAQ 的 15 mm clean-area 都应纳入板框和铜皮/器件禁布规则。
3. **验收要做 RF 实测。** 模块摆放节明说最终产品应测试 throughput 和 communication range，确保 RF performance；两层板且底层有机械轴座时，这不是可省略的验证。
4. **N16R8 的可用引脚限制（与键盘矩阵直接相关）。** 目标型号嵌 ESP32-S3R8（Octal SPI PSRAM）；datasheet §3.2 明列 IO35/IO36/IO37 已连接到 PSRAM、不可用于其他用途。USB D−/D+ 对应 IO19/IO20（datasheet §3.2）。

## 核查完成

核查日期：2026-08-08。判定统计按汇总表：✅ 2 条、❌ 3 条、⚠️ 1 条。
