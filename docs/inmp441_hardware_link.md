# INMP441 与 RA6M5 硬件链路确认（来自 `D:\Renesas\ide`）

## 已确认引脚映射
- `INMP441 SCK (Pin1)` -> `RA6M5 P112` (`SSIBCK0`)
- `INMP441 WS (Pin3)` -> `RA6M5 P113` (`SSILRCK0`)
- `INMP441 SD (Pin2)` -> `RA6M5 P114` (`SSIRXD0`)

## 必需电平与供电
- `INMP441 L/R (Pin4)`：单麦场景固定 `GND`（左声道）
- `INMP441 CHIPEN (Pin8)`：固定 `VDD`（高电平使能）
- `INMP441 VDD (Pin7)`：`3.3V`
- `INMP441 GND (Pin5/6/9)`：共地

## 时钟链路（MCU 主机模式）
- `GPT1A(P109)` 作为内部 AUDIO_CLK 来源
- `SSI0 Master` 输出 `BCLK/WS`
- 目标：`Fs=16k`，`BCLK=1.024MHz(64*Fs)`，`24-bit数据 + 32-bit slot`

## 当前工程的排查重点
- 若串口持续 `samples=0` 且 `ws_edge=0,bck_edge=0`：
  1. 优先检查 `P112/P113/P114` 三线连线是否按上表
  2. 检查 `L/R` 是否确实下拉到 GND
  3. 检查 `CHIPEN` 是否确实上拉到 VDD
  4. 检查麦克风供电与地是否稳定（VDD/GND）
