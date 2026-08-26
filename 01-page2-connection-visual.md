# 原理图第二页 · 连接关系可视化（已定稿）

> 配合 `00-design-decisions.md` 第十节使用。本页解释已经完成的第二页，不再作为“待画步骤”。
> 图中 `+3V3` / `GND` 是**网络标签**，不需要画长线连回第一页；同名网络在 EDA 中自动相连。
> **若本文与 `README.md` / `00-design-decisions.md` 的现行表格冲突，以现行表格为准并立即修正文，不保留冲突。**

---

## 图 0 · 第二页在整块板中的位置

<div style="display:flex;flex-direction:column;gap:8px;font-family:var(--font-sans);">
  <div style="background:var(--bg-sidebar);border:1px solid var(--border-default);border-radius:var(--radius-sm);padding:10px 12px;">
    <div style="font-size:12px;color:var(--text-muted);">第一页（已定稿）</div>
    <div style="font-size:14px;color:var(--text-primary);font-weight:600;">Type-C → TVS → AMS1117 → +3V3</div>
    <div style="font-size:12px;color:var(--text-secondary);margin-top:2px;">交给第二页的只有三样：<code>+5V</code> · <code>+3V3</code> · <code>GND</code>，以及 USB 的 <code>IO19</code>/<code>IO20</code></div>
  </div>
  <div style="text-align:center;color:var(--text-muted);font-size:16px;">↓</div>
  <div style="background:var(--accent-light);border:1px solid var(--accent-primary);border-radius:var(--radius-sm);padding:10px 12px;">
    <div style="font-size:12px;color:var(--accent-primary);font-weight:600;">第二页（已定稿）</div>
    <div style="font-size:14px;color:var(--text-primary);font-weight:600;">U1 模块 + 启动/复位 + 9 键 + OLED + UART</div>
  </div>
  <div style="text-align:center;color:var(--text-muted);font-size:16px;">↓</div>
  <div style="background:var(--bg-sidebar);border:1px solid var(--border-default);border-radius:var(--radius-sm);padding:10px 12px;">
    <div style="font-size:12px;color:var(--text-muted);">第三页（待画）</div>
    <div style="font-size:14px;color:var(--text-primary);font-weight:600;">I2S 三线 + CTRL → NS4168 → 喇叭</div>
    <div style="font-size:12px;color:var(--text-secondary);margin-top:2px;">第二页已引出 <code>I2S_SDATA</code>(IO12) · <code>I2S_BCLK</code>(IO13) · <code>I2S_LRCLK</code>(IO14) · <code>AMP_CTRL</code>(IO21)</div>
  </div>
</div>

---

## 图 1 · EN 复位电路（本页最关键）

<svg viewBox="0 0 400 215" width="100%" style="max-width:520px;font-family:var(--font-mono);">
  <!-- +3V3 rail -->
  <line x1="20" y1="32" x2="300" y2="32" stroke="var(--accent-primary)" stroke-width="1.8"/>
  <text x="20" y="24" font-size="13" fill="var(--accent-primary)">+3V3</text>
  <!-- R6 -->
  <line x1="110" y1="32" x2="110" y2="54" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <rect x="101" y="54" width="18" height="38" fill="none" stroke="var(--text-primary)" stroke-width="1.5" rx="2"/>
  <text x="126" y="70" font-size="12" fill="var(--text-primary)">R6</text>
  <text x="126" y="85" font-size="12" fill="var(--text-secondary)">10k</text>
  <line x1="110" y1="92" x2="110" y2="122" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <!-- EN node wire -->
  <line x1="110" y1="122" x2="298" y2="122" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <circle cx="110" cy="122" r="3.2" fill="var(--text-primary)"/>
  <circle cx="212" cy="122" r="3.2" fill="var(--text-primary)"/>
  <text x="240" y="115" font-size="12" fill="var(--accent-primary)">EN</text>
  <!-- U1 -->
  <rect x="298" y="98" width="90" height="50" fill="var(--bg-sidebar)" stroke="var(--text-primary)" stroke-width="1.5" rx="3"/>
  <text x="343" y="120" font-size="12" fill="var(--text-primary)" text-anchor="middle">U1</text>
  <text x="343" y="136" font-size="10" fill="var(--text-secondary)" text-anchor="middle">ESP32-S3</text>
  <!-- C7 -->
  <line x1="110" y1="122" x2="110" y2="150" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <line x1="94" y1="150" x2="126" y2="150" stroke="var(--text-primary)" stroke-width="2"/>
  <line x1="94" y1="158" x2="126" y2="158" stroke="var(--text-primary)" stroke-width="2"/>
  <text x="34" y="150" font-size="12" fill="var(--text-primary)">C7</text>
  <text x="20" y="165" font-size="12" fill="var(--text-secondary)">0.1µF</text>
  <line x1="110" y1="158" x2="110" y2="180" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <line x1="96" y1="180" x2="124" y2="180" stroke="var(--text-secondary)" stroke-width="1.8"/>
  <line x1="101" y1="186" x2="119" y2="186" stroke="var(--text-secondary)" stroke-width="1.8"/>
  <line x1="106" y1="192" x2="114" y2="192" stroke="var(--text-secondary)" stroke-width="1.8"/>
  <text x="88" y="207" font-size="11" fill="var(--text-muted)">GND</text>
  <!-- SW11 -->
  <line x1="212" y1="122" x2="212" y2="140" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <circle cx="212" cy="143" r="2.8" fill="none" stroke="var(--text-primary)" stroke-width="1.5"/>
  <circle cx="212" cy="167" r="2.8" fill="none" stroke="var(--text-primary)" stroke-width="1.5"/>
  <line x1="210" y1="165" x2="228" y2="145" stroke="var(--text-primary)" stroke-width="1.8"/>
  <text x="234" y="152" font-size="12" fill="var(--text-primary)">SW11</text>
  <text x="234" y="166" font-size="11" fill="var(--text-secondary)">RESET</text>
  <line x1="212" y1="170" x2="212" y2="180" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <line x1="198" y1="180" x2="226" y2="180" stroke="var(--text-secondary)" stroke-width="1.8"/>
  <line x1="203" y1="186" x2="221" y2="186" stroke="var(--text-secondary)" stroke-width="1.8"/>
  <line x1="208" y1="192" x2="216" y2="192" stroke="var(--text-secondary)" stroke-width="1.8"/>
</svg>

| 元件 | 作用 | 不放会怎样 |
|---|---|---|
| **R6 10k** | 平时把 EN 拉到 3.3V；乐鑫明确要求 EN 不得悬空 | EN 电平不确定，芯片可能无法可靠启动 |
| **C7 1µF** | 与 R6 形成上电延时 | 若电源上升与 EN 释放次序不合适，可能影响可靠启动 |
| **SW11** | 按下把 EN 拉到 GND 复位；松开后由 R6/C7 重新释放 | 只能靠断电重启 |

当前 R6×C7 的名义时间常数为 10ms，采用乐鑫当前指南通常建议的 10kΩ + 1µF 组合。

---

## 图 2 · BOOT 键（IO0）

<svg viewBox="0 0 400 200" width="100%" style="max-width:520px;font-family:var(--font-mono);">
  <line x1="20" y1="32" x2="300" y2="32" stroke="var(--accent-primary)" stroke-width="1.8"/>
  <text x="20" y="24" font-size="13" fill="var(--accent-primary)">+3V3</text>
  <line x1="130" y1="32" x2="130" y2="54" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <rect x="121" y="54" width="18" height="38" fill="none" stroke="var(--text-primary)" stroke-width="1.5" rx="2"/>
  <text x="146" y="70" font-size="12" fill="var(--text-primary)">R7</text>
  <text x="146" y="85" font-size="12" fill="var(--text-secondary)">10k</text>
  <line x1="130" y1="92" x2="130" y2="115" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <line x1="130" y1="115" x2="298" y2="115" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <circle cx="130" cy="115" r="3.2" fill="var(--text-primary)"/>
  <text x="236" y="108" font-size="12" fill="var(--accent-primary)">IO0</text>
  <rect x="298" y="92" width="90" height="48" fill="var(--bg-sidebar)" stroke="var(--text-primary)" stroke-width="1.5" rx="3"/>
  <text x="343" y="113" font-size="12" fill="var(--text-primary)" text-anchor="middle">U1</text>
  <text x="343" y="128" font-size="10" fill="var(--text-secondary)" text-anchor="middle">ESP32-S3</text>
  <!-- SW10 -->
  <line x1="130" y1="115" x2="130" y2="133" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <circle cx="130" cy="136" r="2.8" fill="none" stroke="var(--text-primary)" stroke-width="1.5"/>
  <circle cx="130" cy="160" r="2.8" fill="none" stroke="var(--text-primary)" stroke-width="1.5"/>
  <line x1="128" y1="158" x2="146" y2="138" stroke="var(--text-primary)" stroke-width="1.8"/>
  <text x="152" y="145" font-size="12" fill="var(--text-primary)">SW10</text>
  <text x="152" y="159" font-size="11" fill="var(--text-secondary)">BOOT</text>
  <line x1="130" y1="163" x2="130" y2="172" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <line x1="116" y1="172" x2="144" y2="172" stroke="var(--text-secondary)" stroke-width="1.8"/>
  <line x1="121" y1="178" x2="139" y2="178" stroke="var(--text-secondary)" stroke-width="1.8"/>
  <line x1="126" y1="184" x2="134" y2="184" stroke="var(--text-secondary)" stroke-width="1.8"/>
  <text x="106" y="197" font-size="11" fill="var(--text-muted)">GND</text>
</svg>

拓扑与图 1 相似，但 **GPIO0 不接对地电容**。

GPIO0 是启动绑带脚：复位后先采样，高电平正常启动，按住 BOOT 将其拉低可进入下载模式。GPIO0 自带约 45kΩ 内部弱上拉，乐鑫仍建议外接上拉；本设计用 R7=10kΩ提高默认高电平的确定性。

> 不要随意给 GPIO0 加对地电容。电容会改变它相对 EN 的上升时序，必须经过绑带采样时序计算后才能决定。

---

## 图 3 · 9 个按键（active low）

<svg viewBox="0 0 400 275" width="100%" style="max-width:520px;font-family:var(--font-mono);">
  <rect x="18" y="30" width="98" height="180" fill="var(--bg-sidebar)" stroke="var(--text-primary)" stroke-width="1.5" rx="3"/>
  <text x="67" y="112" font-size="12" fill="var(--text-primary)" text-anchor="middle">U1</text>
  <text x="67" y="128" font-size="10" fill="var(--text-secondary)" text-anchor="middle">ESP32-S3</text>
  <text x="67" y="146" font-size="9" fill="var(--text-muted)" text-anchor="middle">内部上拉 ON</text>

  <!-- row template: y = 55, 100, 145, 190 -->
  <g stroke="var(--text-secondary)" stroke-width="1.5">
    <line x1="116" y1="55" x2="212" y2="55"/>
    <line x1="116" y1="100" x2="212" y2="100"/>
    <line x1="116" y1="190" x2="212" y2="190"/>
  </g>
  <g font-size="11" fill="var(--accent-primary)">
    <text x="124" y="49">IO4</text>
    <text x="124" y="94">IO5</text>
    <text x="124" y="184">IO18</text>
  </g>
  <text x="124" y="140" font-size="13" fill="var(--text-muted)">⋮  IO6/7/15/16/17, IO8</text>

  <!-- switches -->
  <g>
    <circle cx="212" cy="55" r="2.8" fill="none" stroke="var(--text-primary)" stroke-width="1.5"/>
    <circle cx="264" cy="55" r="2.8" fill="none" stroke="var(--text-primary)" stroke-width="1.5"/>
    <line x1="214" y1="53" x2="262" y2="38" stroke="var(--text-primary)" stroke-width="1.8"/>
    <text x="212" y="30" font-size="11" fill="var(--text-primary)">SW1 (do)</text>
  </g>
  <g>
    <circle cx="212" cy="100" r="2.8" fill="none" stroke="var(--text-primary)" stroke-width="1.5"/>
    <circle cx="264" cy="100" r="2.8" fill="none" stroke="var(--text-primary)" stroke-width="1.5"/>
    <line x1="214" y1="98" x2="262" y2="83" stroke="var(--text-primary)" stroke-width="1.8"/>
    <text x="212" y="75" font-size="11" fill="var(--text-primary)">SW2 (re)</text>
  </g>
  <g>
    <circle cx="212" cy="190" r="2.8" fill="none" stroke="var(--text-primary)" stroke-width="1.5"/>
    <circle cx="264" cy="190" r="2.8" fill="none" stroke="var(--text-primary)" stroke-width="1.5"/>
    <line x1="214" y1="188" x2="262" y2="173" stroke="var(--text-primary)" stroke-width="1.8"/>
    <text x="212" y="165" font-size="11" fill="var(--text-primary)">SW9 (Fn)</text>
  </g>

  <!-- to common GND -->
  <g stroke="var(--text-secondary)" stroke-width="1.5">
    <line x1="264" y1="55" x2="340" y2="55"/>
    <line x1="264" y1="100" x2="340" y2="100"/>
    <line x1="264" y1="190" x2="340" y2="190"/>
    <line x1="340" y1="55" x2="340" y2="225"/>
  </g>
  <circle cx="340" cy="100" r="3.2" fill="var(--text-primary)"/>
  <circle cx="340" cy="190" r="3.2" fill="var(--text-primary)"/>
  <line x1="326" y1="225" x2="354" y2="225" stroke="var(--text-secondary)" stroke-width="1.8"/>
  <line x1="331" y1="231" x2="349" y2="231" stroke="var(--text-secondary)" stroke-width="1.8"/>
  <line x1="336" y1="237" x2="344" y2="237" stroke="var(--text-secondary)" stroke-width="1.8"/>
  <text x="318" y="252" font-size="11" fill="var(--text-muted)">GND</text>
</svg>

**每个键就两根线：一端 GPIO、一端 GND。** 没有电阻、没有二极管、没有电容。

- **不用外部上拉** —— 固件把这 9 个脚配成 `INPUT_PULLUP`，用芯片内部上拉，省 9 颗电阻。平时读到高，按下读到低（这就叫 active low）
- **不用二极管** —— 一键一 GPIO，没有矩阵，鬼键问题不存在，二极管的唯一用途消失
- **不用消抖电容** —— 软件消抖 20ms，但**必须非阻塞**（每键存一个"上次变化时间戳"）。用阻塞 `delay()` 的话按住一键时扫不到别的键，和弦就没了

---

## 图 4 · I2C + OLED 接口

<svg viewBox="0 0 400 235" width="100%" style="max-width:520px;font-family:var(--font-mono);">
  <line x1="20" y1="30" x2="300" y2="30" stroke="var(--accent-primary)" stroke-width="1.8"/>
  <text x="20" y="22" font-size="13" fill="var(--accent-primary)">+3V3</text>

  <!-- U1 -->
  <rect x="18" y="95" width="86" height="70" fill="var(--bg-sidebar)" stroke="var(--text-primary)" stroke-width="1.5" rx="3"/>
  <text x="61" y="124" font-size="12" fill="var(--text-primary)" text-anchor="middle">U1</text>
  <text x="61" y="139" font-size="10" fill="var(--text-secondary)" text-anchor="middle">ESP32-S3</text>

  <!-- SDA wire (top, y=118) SCL wire (y=148) -->
  <line x1="104" y1="118" x2="298" y2="118" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <line x1="104" y1="148" x2="298" y2="148" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <text x="112" y="112" font-size="11" fill="var(--accent-primary)">IO10 SDA</text>
  <text x="112" y="163" font-size="11" fill="var(--accent-primary)">IO11 SCL</text>

  <!-- R4 pull-up on SDA at x=196 -->
  <line x1="196" y1="30" x2="196" y2="50" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <rect x="187" y="50" width="18" height="36" fill="none" stroke="var(--text-primary)" stroke-width="1.5" rx="2"/>
  <text x="210" y="65" font-size="11" fill="var(--text-primary)">R4</text>
  <text x="210" y="79" font-size="11" fill="var(--text-secondary)">4.7k</text>
  <line x1="196" y1="86" x2="196" y2="118" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <circle cx="196" cy="118" r="3.2" fill="var(--text-primary)"/>

  <!-- R5 pull-up on SCL at x=252, hop over SDA -->
  <line x1="252" y1="30" x2="252" y2="50" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <rect x="243" y="50" width="18" height="36" fill="none" stroke="var(--text-primary)" stroke-width="1.5" rx="2"/>
  <text x="266" y="65" font-size="11" fill="var(--text-primary)">R5</text>
  <text x="266" y="79" font-size="11" fill="var(--text-secondary)">4.7k</text>
  <path d="M252,86 L252,111 A 7,7 0 0,0 252,125 L252,148" fill="none" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <circle cx="252" cy="148" r="3.2" fill="var(--text-primary)"/>

  <!-- J2 -->
  <rect x="298" y="58" width="86" height="112" fill="var(--bg-sidebar)" stroke="var(--text-primary)" stroke-width="1.5" rx="3"/>
  <text x="341" y="52" font-size="11" fill="var(--text-primary)" text-anchor="middle">J2  OLED 4P</text>
  <text x="306" y="78" font-size="11" fill="var(--text-muted)">1 GND</text>
  <text x="306" y="102" font-size="11" fill="var(--text-muted)">2 VCC</text>
  <text x="306" y="126" font-size="11" fill="var(--text-primary)">3 SCL</text>
  <text x="306" y="150" font-size="11" fill="var(--text-primary)">4 SDA</text>
  <!-- net-label stubs for GND / VCC -->
  <line x1="284" y1="74" x2="298" y2="74" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <text x="280" y="78" font-size="10" fill="var(--text-muted)" text-anchor="end">GND</text>
  <line x1="284" y1="98" x2="298" y2="98" stroke="var(--text-secondary)" stroke-width="1.5"/>
  <text x="280" y="102" font-size="10" fill="var(--accent-primary)" text-anchor="end">+3V3</text>

  <text x="200" y="205" font-size="10" fill="var(--text-muted)">小半圆 = 跨线不连（R5 的线从 SDA 上方跨过）</text>
  <text x="200" y="222" font-size="10" fill="var(--text-muted)">GND / +3V3 用网络标签，不画长线</text>
</svg>

| 元件 | 作用 |
|---|---|
| **R4 / R5 4.7k** | I2C 是**开漏**结构，需要由上拉电阻把总线恢复为高电平；本板预留 R4/R5，是否焊接取决于 OLED 模块是否已自带上拉 |
| **J2 4P 排母** | 屏是成品模块，板上只留座子 |

**两条实操提醒：**
1. **留位，是否焊接由实测决定** —— 所选 OLED 商品据客服称自带上拉。到手后量 SDA↔VCC、SCL↔VCC；确认存在合适上拉后不焊 R4/R5，若无上拉再焊。
2. **丝印必须标网络名** —— 市面模块排针顺序至少有 `GND-VCC-SCL-SDA` 和 `VCC-GND-SCL-SDA` 两类，VCC/GND 误接有损坏风险。插之前逐脚核对；顺序不同就用独立跳线转接。

---

## 图 5 · J3 UART 排针（三根线，无图）

`IO43 (TX0)` · `IO44 (RX0)` · `GND` 三个焊盘，什么都不接。

正常烧录调试全走 Type-C（S3 自带 USB-Serial-JTAG）。留它的唯一理由：**如果 USB 那条路画错了（比如 D+/D− 忘记短接），这是唯一能看到芯片启动日志的通道**。成本为零。

---

## 画完自查

- [ ] 位号不重复（第一页已用：J1 D1 D2 U2 R1~R3 C1~C6 TP1~TP3；第二页另有 TP4/TP5）
- [ ] U1 的 3 个 GND 脚 + **底部散热焊盘**全部连 GND，一个不漏
- [ ] IO35 / IO36 / IO37 未占用（Octal PSRAM 内部占用）
- [ ] IO3 / IO45 / IO46 未接任何东西（strapping）
- [ ] IO43 / IO44 只接 J3
- [ ] 所有悬空脚打 NC flag
- [ ] 网络名不以数字开头（`+3V3` 不是 `3.3V`）
- [ ] **DRC = 0 error**；warning 逐条解释。第二页定稿时仅保留 4 条跨页单网络预期警告，第三页接好后应消失
