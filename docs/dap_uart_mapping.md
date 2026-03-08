# A_i2s_capture 板载 DAP 串口映射说明

## 目的
确认 `A_i2s_capture` 的日志路径为 MCU `SCI7` 经板载 DAP 虚拟串口回传到 PC。

## 原理图依据（`DshanMCU_RA6M5_V4.0.pdf`）
- 第 1 页可提取到：`Debug(UART/DAP)`、`P613 TXD7`、`P614 RXD7`。
- 第 2 页可提取到：`P613`、`P614`、`TXD7`、`RXD7`。

## 工程配置对应关系
- `SCI7 TXD` -> `P613`（`configuration.xml` 中 `p613.sci7.txd`）
- `SCI7 RXD` -> `P614`（`configuration.xml` 中 `p614.sci7.rxd`）
- UART 实例：`g_uart7`
- 波特率：`115200`

## PC 端默认口径
- 默认日志串口：`COM16`
- 默认参数：`115200-8-N-1`
- 60 秒抓取脚本：`tools/capture_uart_60s.py`

## 运行示例
```powershell
python tools/capture_uart_60s.py --port COM16 --baud 115200 --seconds 60
```

## `STAT` 日志字段（当前版本）
`STAT,t_sec,samples,fs_est,cb_count,cb_gap_max_us,rms,peak,clip,ovf,rst,err,read_ok,read_err,no_cb_to,sample_ok,cb_ok,error_ok,audio_ok`

- `read_ok/read_err`：最近一次 `R_SSI_Read` 提交状态。
- `no_cb_to`：1 秒窗口内无回调超时累计次数（触发后会请求重启采集）。
