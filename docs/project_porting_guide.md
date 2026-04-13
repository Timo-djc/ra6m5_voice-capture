# A_i2s_capture 项目移植与集成说明

## 1. 适用场景

这份文档面向两类交接场景：

1. 把当前工程完整搬到另一台电脑，继续在相同或接近的硬件上编译运行。
2. 把本项目中的语音采集、W800 联网、云端识别/声纹能力，裁剪后接入对方现有工程。

当前仓库并不是单一功能 Demo，而是一个组合工程，包含：

- RA6M5 端音频采集、按键触发、结果上报
- W800 AT 控制、Wi-Fi 联网、TCP/HTTP 通信
- PC 端 FastAPI 服务
- 可选 UI / 触摸 / 声纹注册识别流程
- 一个更适合独立联调的 MVP 路径

因此移植时不要直接把整个 `src/` 生搬硬套，建议先明确“要移植的是哪一层能力”。

## 2. 工程结构与职责划分

### 2.1 MCU 侧核心模块

- `src/audio_capture.c` / `src/audio_capture.h`
  - I2S 采样、环形缓冲、按压录音、结果发布。
  - 依赖 `g_i2s0`、`g_timer0`、`g_timer1`、`g_uart7`、`g_ioport` 等 FSP 实例。
- `src/net/cloud_asr_client.c` / `src/net/cloud_asr_client.h`
  - 云端数字识别客户端。
- `src/net/cloud_speaker_client.c` / `src/net/cloud_speaker_client.h`
  - 云端声纹识别/注册客户端。
- `src/net/w800_at.*`
  - W800 基础 AT 控制、桥接、串口交互。
- `src/net/w800_socket.*`
  - W800 socket 层封装。
- `src/hal_entry.c`
  - 整个主流程入口。
  - 同时包含两种运行方式：
    - `W800_ONLY_DEBUG_MODE == 1`：进入 MVP 最小联调流
    - `W800_ONLY_DEBUG_MODE == 0`：进入完整 UI + 触摸 + 云端业务流

### 2.2 MVP 路径

- `src/mvp/app_main.c`
  - 最小可跑通路径，适合先验证 W800、网络、协议。
- `src/mvp/config.h`
  - MVP 的 Wi-Fi、服务端 IP/端口、超时配置。
- `src/mvp/cloud_http_client.c`
  - HTTP 上传与响应解析。
- `src/mvp/cloud_speaker_demo_client.c`
  - 声纹 demo TCP 协议收发。
- `src/mvp/test_audio_data.c`
  - 内嵌音频样本，用于脱离真实 MIC 先验证链路。

如果对方只是要“接入现有项目并跑通云端识别/声纹”，建议先从 MVP 路径迁过去，再决定是否引入 UI 和触摸。

### 2.3 PC 服务端

- `server/main.py`
  - FastAPI 入口。
  - HTTP ASR 接口默认 `8000`。
  - 声纹原始 TCP 服务默认 `18080`。
- `server/config.py`
  - 服务端环境变量入口。
- `server/speaker_model_adapter.py`
  - 声纹后端适配，支持 `modelscope` 和 `fallback`。
- `tools/start_speaker_web.py`
  - Windows 下一键启动 Web 服务的辅助脚本。

## 3. 当前工程的一个关键事实

当前 `src/hal_entry.c` 中：

- `W800_ONLY_DEBUG_MODE` 被定义为 `1`

这意味着当前默认构建并不会进入完整 UI 流程，而是直接走 `app_main_init()` / `app_main_poll()` 的 MVP 路径。

交付给别人时必须先说明这一点，否则对方会以为现在的主工程已经默认启用了 UI、触摸、声纹页面和完整业务流。

## 4. 建议的交付方式

不建议只发源码压缩包，建议一起交付下面这些内容：

1. 当前仓库源码快照
2. 本文档
3. 一份“接收方需修改的配置项清单”
4. 一份“已验证的硬件连接清单”
5. 一份“服务端启动命令”

如果要减少对方环境差异带来的问题，建议额外说明：

- 当前工程是在 Windows + Keil MDK 环境下维护
- Keil 工程里存在绝对路径依赖
- Python 服务端有可选模型依赖，未安装时会退化到 fallback 模式

## 5. 新电脑环境准备

### 5.1 MCU 开发环境

建议接收方至少准备：

- Windows
- Keil MDK-ARM
- Arm Compiler 6
- Renesas RA 相关 Pack

当前工程记录到的关键信息：

- `rasc_version.txt` 中记录的 RASC 版本为 `6.3.0`
- `A_i2s_capture.uvprojx` 中记录的编译器为 `ARMCLANG V6.24`

注意：如果对方只是编译现有工程，不一定要安装 e2 studio，但如果要重新生成 FSP 配置、改引脚、改外设实例，则最好准备和当前工程接近版本的 RASC / FSP 环境。

### 5.2 Python 服务端环境

建议：

- Python 3.10 及以上
- 安装 `server/requirements.txt`

基础命令：

```powershell
cd server
pip install -r requirements.txt
uvicorn main:app --host 0.0.0.0 --port 8000
```

也可以在仓库根目录运行：

```powershell
uvicorn server.main:app --host 0.0.0.0 --port 8000
```

### 5.3 ModelScope 依赖说明

服务端默认声纹后端是 `modelscope`，但 `server/requirements.txt` 里并没有把 `modelscope` 写成强制依赖。

实际效果是：

- 如果 `modelscope` 可用，则使用真实声纹模型
- 如果不可用，服务会回退到 `fallback` 后端

所以交付时要明确告诉对方：

- 只是演示流程：可以先用 `fallback`
- 要做真实声纹识别：必须额外安装 `modelscope` 及其运行依赖

## 6. 新电脑最容易出问题的地方

### 6.1 Keil 工程中的绝对路径

`A_i2s_capture.uvprojx` 和 `A_i2s_capture_prebuilt.uvprojx` 中都存在类似下面的绝对路径：

- `d:/keil_v5/keil/tensorflow/...`
- `d:/keil_v5/keil/ARM/CMSIS-DSP/...`
- `d:/keil_v5/keil/ARM/CMSIS-NN/...`

这意味着换电脑后，如果目录结构不同，Keil 直接打开工程很可能就会报找不到头文件或源文件。

处理方法二选一：

1. 在新电脑上恢复接近的目录结构
2. 手工修改 Keil target 的 Include Path / Source Path

建议在交付时优先说明这件事，因为这是“别人机器上第一时间就会编不过”的核心原因。

### 6.2 不要把生成物当源码交付

建议不要把下面这些目录作为正式交付内容的核心部分：

- `Objects/`
- `Listings/`
- `server/__pycache__/`
- `tmp/`
- `server_data/`（除非你明确要把演示数据一起交付）

这些目录更多是构建产物、缓存或运行态数据。

### 6.3 配置里有硬编码信息

当前 `src/mvp/config.h` 里已经写了实际 Wi-Fi、IP 和端口，交付前建议改成占位值或在文档里明确要求接收方修改：

- `WIFI_SSID`
- `WIFI_PASSWORD`
- `CLOUD_HOST_IP`
- `SPEAKER_SERVER_IP`
- 各类端口和超时参数

如果直接把当前值给别人，通常会出现两类问题：

1. 到新环境网络不通
2. 把本地测试环境信息一并泄露出去

## 7. 硬件移植要点

### 7.1 MIC / I2S 链路

当前工程的音频采集基于：

- `SSI0` 做 I2S
- `GPT` 产生时钟
- INMP441 麦克风

当前仓库已有硬件连接说明，可参考：

- `docs/inmp441_hardware_link.md`

已确认的核心连接关系是：

- `INMP441 SCK -> RA6M5 P112`
- `INMP441 WS  -> RA6M5 P113`
- `INMP441 SD  -> RA6M5 P114`

如果换板子或换引脚，不是改一两个宏就结束，通常还要一起调整：

- `configuration.xml`
- `ra_cfg/`
- `ra_gen/`
- `g_i2s0` 对应实例配置
- `g_timer0` / `g_timer1` 对应的 GPT 输出
- Pin PFS 映射

### 7.2 W800 链路

当前设计里，W800 至少涉及三类连接：

- `SCI6`：AT 控制口
- `SCI5`：下载/桥接口
- `SCI7`：日志口
- `BOOT/WAKE/RESET` 控制脚

这些默认宏集中在：

- `src/net/cloud_asr_cfg.h`

尤其要确认：

- `W800_BOOT_PIN`
- `W800_WAKE_PIN`
- `W800_RESET_PIN`
- `W800_AT_UART_INSTANCE`
- `W800_FLASH_UART_INSTANCE`

如果目标项目的 W800 接线方式不同，这些宏和对应 FSP 实例必须一起改。

### 7.3 LCD / 触摸不是强制项

如果目标项目只是要语音能力，不一定要把 LCD、触摸、UI 一起搬过去。

完整 UI 相关代码主要在：

- `src/ui/*`
- `src/drivers/drv_touch.*`
- `src/drivers/drv_lcd.*`
- `src/hal_entry.c` 的非 debug 路径

建议先做“无 UI 版本”移植，等语音链路稳定后再接界面。

## 8. 接入现有项目时的最小迁移边界

### 8.1 只移植“语音采集 + 云端识别”

最小建议迁移内容：

- `src/audio_capture.c`
- `src/audio_capture.h`
- `src/net/cloud_asr_client.c`
- `src/net/cloud_asr_client.h`
- `src/net/w800_at.*`
- `src/net/w800_socket.*`
- `src/net/cloud_asr_cfg.h`
- `ra_cfg/`
- `ra_gen/`
- `configuration.xml`（如果沿用当前外设布局）

主循环至少要保留下面这些调用关系：

```c
audio_capture_init();
audio_capture_start();
cloud_asr_client_init();

while (1)
{
    audio_capture_process();
    cloud_asr_client_poll();
    audio_capture_run_inference_if_ready();
}
```

### 8.2 再加“声纹识别/注册”

额外加入：

- `src/net/cloud_speaker_client.c`
- `src/net/cloud_speaker_client.h`
- `src/net/cloud_speaker_cfg.h`

同时保留：

- `audio_capture_consume_slot_pcm16()`
- `audio_capture_publish_speaker_result()`
- `audio_capture_publish_enroll_progress()`

### 8.3 最后再加 UI

最后一层再引入：

- `src/ui/*`
- `src/drivers/*`
- `src/hal_entry.c` 中完整状态机逻辑

这个顺序的好处是，问题定位清晰。否则一上来就把触摸、显示、联网、录音、声纹、云端全绑一起，出现问题会很难分层排查。

## 9. 推荐移植顺序

建议按下面顺序推进：

1. 先在新电脑上把 Keil 工程打开并解决路径问题
2. 先跑通串口日志
3. 再跑通 INMP441 采样
4. 再跑通 W800 AT
5. 再跑通 Wi-Fi 入网
6. 再跑通数字识别 HTTP
7. 再跑通声纹 TCP
8. 最后再接 UI / 触摸 / 目标业务逻辑

## 10. 服务端联调注意事项

### 10.1 端口不是一个

当前服务端分成两类端口：

- HTTP API：默认 `8000`
- 声纹原始 TCP：默认 `18080`

接收方最容易混淆这一点。

对应关系如下：

- 数字识别走 HTTP，访问 `8000`
- 声纹识别走原始 TCP，访问 `18080`

### 10.2 推荐启动方式

Windows 下可以直接使用：

```powershell
python tools/start_speaker_web.py
```

这个脚本会：

- 检查 `8000` 端口
- 启动 `server.main:app`
- 尝试打开浏览器

### 10.3 服务端可配项

服务端主要通过环境变量调整，重点关注：

- `SPEAKER_BACKEND`
- `SPEAKER_MODEL_ID`
- `SPEAKER_TCP_PORT`
- `SPEAKER_IDENTIFY_THRESHOLD`
- `SPEAKER_DATA_ROOT`

对应入口在：

- `server/config.py`

## 11. 验证清单

### 11.1 MCU 侧

至少确认下面几点：

- 上电后串口日志正常
- `audio_capture` 能持续输出状态信息
- 没有持续出现 `samples=0`
- W800 能回应 `AT`
- 能成功连上 Wi-Fi

### 11.2 服务端

至少确认下面几点：

- `http://<server_ip>:8000/` 可访问
- `http://<server_ip>:8000/api/system/status` 可访问
- 服务启动日志能看到当前 speaker backend 状态

### 11.3 联调链路

至少确认下面几点：

- 数字识别请求能得到 HTTP 响应
- 声纹识别能收到一行返回结果
- MCU 侧端口配置与服务端端口一致

## 12. 常见问题

### 12.1 新电脑上 Keil 一打开就报头文件缺失

优先检查：

- target include path 里的绝对路径
- 第三方库目录是否存在
- Pack 是否安装齐全

### 12.2 日志里一直没有有效采样

优先检查：

- INMP441 三根线是否接对
- GPT 时钟是否真的输出
- SSI / DTC 实例是否仍然对应原始工程配置

### 12.3 Wi-Fi 能配但就是不通云端

优先检查：

- `src/mvp/config.h` 的 IP 和端口
- 服务端电脑的防火墙
- HTTP 端口和 speaker TCP 端口是否混用

### 12.4 服务启动了，但声纹效果不对

优先检查：

- 当前是不是落到了 `fallback` 后端
- `modelscope` 是否真的安装成功
- 阈值是否过高或过低

## 13. 交接建议

如果这是要正式发给别人集成，建议你在交付前再做一次整理：

1. 把 `src/mvp/config.h` 里的真实 Wi-Fi 和 IP 改成占位值
2. 明确告诉对方当前默认走的是 MVP/debug 路径
3. 把“必须改的宏”和“必须改的引脚/实例”单独列一个表
4. 最好附一份“已验证的服务端启动命令”和“已验证的串口号/波特率”

如果后续要进一步降低对方移植成本，建议再补两样东西：

- 一份 `.env.example` 或服务端环境变量模板
- 一份“Keil 新电脑首次打开工程需要改哪些路径”的截图说明

