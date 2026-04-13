# W800 传输模块 + 服务端移植说明

## 1. 范围

这份文档只覆盖两部分：

1. MCU 侧 W800 传输栈
2. PC 侧服务端

不包含：

- `audio_capture.c` 这类音频采集实现细节
- LCD / 触摸 / UI
- 完整业务状态机

目标是把当前项目中的“W800 联网与传输能力”单独抽出来，给别人接入到自己的项目中。

## 2. 先说结论

当前仓库里和 W800 相关的代码分成两套：

- `src/net/w800_at.*`
  - 更偏桥接、烧录、运行模式切换
- `src/mvp/w800_*.{c,h}`
  - 更偏你现在要的传输栈
  - 包含 AT、Wi-Fi、Socket、HTTP 上传、声纹 TCP demo

如果你的目标是“保留 W800 网络通信能力并对接服务端”，建议直接以 `src/mvp/` 这套代码为基础移植。

## 3. MCU 侧建议迁移的文件

### 3.1 核心传输层

必须迁移：

- `src/mvp/config.h`
- `src/mvp/log.c`
- `src/mvp/log.h`
- `src/mvp/uart_ringbuf.c`
- `src/mvp/uart_ringbuf.h`
- `src/mvp/w800_at.c`
- `src/mvp/w800_at.h`
- `src/mvp/w800_wifi.c`
- `src/mvp/w800_wifi.h`
- `src/mvp/w800_socket.c`
- `src/mvp/w800_socket.h`

### 3.2 对接服务端的可选封装

如果要对接 HTTP 数字识别接口，再加：

- `src/mvp/cloud_http_client.c`
- `src/mvp/cloud_http_client.h`
- `src/mvp/http_parser_lite.c`
- `src/mvp/http_parser_lite.h`

如果要对接声纹 TCP 服务，再看情况使用：

- `src/mvp/cloud_speaker_demo_client.c`
- `src/mvp/cloud_speaker_demo_client.h`

注意：

- `cloud_speaker_demo_client.*` 目前是 demo 包装层，直接依赖 `test_audio_data.h` 里的 `test_audio_clip_t`
- 如果你在目标项目里已经有自己的 PCM/WAV 数据，不一定要直接搬这个 demo 文件
- 更常见的做法是保留 `w800_socket.*`，按服务端协议自己组包

## 4. MCU 侧外设依赖

这套 W800 传输栈最核心的硬件依赖并不多，主要是：

- `g_uart6`
  - W800 AT 通信串口
- `g_ioport`
  - 用于控制 W800 reset pin

另外还要保留中断回调绑定：

- `mvp_uart6_callback()`
- `mvp_uart5_callback()`

说明：

- `mvp_uart6_callback()` 是必须的，负责把 UART6 收到的字节推入 ring buffer
- `mvp_uart5_callback()` 在 MVP 传输路径里只是空实现，占位给 FSP 链接用

如果目标项目的串口号不是 `SCI6`，需要一起改：

- `g_uart6`
- 相关 FSP 实例
- 中断回调挂接

## 5. MCU 侧关键配置项

配置主要集中在：

- `src/mvp/config.h`

移植到新项目或新电脑时，至少要改下面这些值：

- `WIFI_SSID`
- `WIFI_PASSWORD`
- `CLOUD_HOST_IP`
- `CLOUD_PORT`
- `CLOUD_PATH`
- `SPEAKER_SERVER_IP`
- `SPEAKER_SERVER_PORT`
- `DEVICE_ID`
- `W800_UART_BAUD`

建议交付前把真实 Wi-Fi 和 IP 改成占位值，不要把本地测试环境直接发给别人。

## 6. W800 模块职责

### 6.1 `w800_at.*`

职责：

- 初始化 UART
- 设置波特率
- 发送 AT 命令
- 读取文本行和二进制数据
- 管理接收 ring buffer
- 执行硬复位

当前实现特点：

- 默认用 `g_uart6`
- 默认 reset pin 是 `P508`
- 使用 `DWT->CYCCNT` 做毫秒超时

如果目标板的 reset pin 不同，至少要调整：

- `W800_RESET_PIN`
- `W800_RESET_ACTIVE_LEVEL`

### 6.2 `w800_wifi.*`

职责：

- 探测 W800 是否在线
- 配置 STA 模式
- 配置 SSID / 密码 / DHCP
- 保存配置
- 执行入网
- 查询链路状态

当前推荐的最小联网顺序就是：

1. `w800_init()`
2. `w800_basic_check()`
3. `w800_set_echo(false)`
4. `w800_set_mode_sta()`
5. `w800_set_ssid()`
6. `w800_set_key_ascii()`
7. `w800_set_dhcp()`
8. `w800_save_config()`
9. `w800_reset()`
10. `w800_join_ap()`
11. `w800_get_link_status()`

### 6.3 `w800_socket.*`

职责：

- TCP 建连
- 发送数据
- 接收数据
- 查询 socket 状态
- 关闭 socket

这个文件是整个“W800 传输模块”的核心。

## 7. 这套 W800 传输栈的几个关键行为

### 7.1 `AT+SKCT` 返回 `+OK` 不代表已经连上

当前实现里，`w800_socket_open_tcp()` 在收到 `AT+SKCT` 的 `+OK=<socket>` 后，还会继续轮询 `AT+SKSTT`，直到 socket 进入 connected 状态。

这点很重要。

如果你在目标项目里自己重写 socket 封装，必须保留这个行为，否则经常会出现：

- `SKCT` 成功
- 但第一次 `SKSND` 就失败

### 7.2 `AT+SKSND` 不是等 `>` 提示符

当前实现按下面的方式发送：

1. 发 `AT+SKSND=<socket>,<size>`
2. 等 `+OK=<actualsize>`
3. 直接写原始二进制数据

不要按某些模组的习惯去等 `>` prompt。

### 7.3 接收不是纯文本，必须支持“文本头 + 定长二进制”

`AT+SKRCV` 的典型返回是：

1. 先回一行 `+OK=<size>`
2. 然后紧跟 `<size>` 字节原始数据

所以 MCU 侧读取逻辑必须同时支持：

- 读文本行
- 再按长度读二进制 payload

### 7.4 发送被做了分片和重试

当前 `w800_socket_send()` 已经做了几件很重要的保护：

- 按 `W800_SKSND_MAX_CHUNK` 分片
- 避免过小尾包
- 中途做 `SKSTT` 健康检查
- 对发送失败做退避重试

如果目标项目数据量比较大，不建议把这些保护逻辑删掉。

## 8. 服务端组成

服务端主要是这几个文件：

- `server/main.py`
- `server/config.py`
- `server/tcp_server.py`
- `server/speaker_service.py`
- `server/speaker_model_adapter.py`

最小运行依赖在：

- `server/requirements.txt`

基础安装：

```powershell
cd server
pip install -r requirements.txt
```

启动方式：

```powershell
uvicorn main:app --host 0.0.0.0 --port 8000
```

或从仓库根目录：

```powershell
uvicorn server.main:app --host 0.0.0.0 --port 8000
```

Windows 下也可以直接用：

```powershell
python tools/start_speaker_web.py
```

## 9. 服务端对外提供的两类接口

### 9.1 HTTP 数字识别接口

接口：

- `POST /asr/recognize`

当前 MCU 侧 `cloud_http_client.c` 发出的关键请求头是：

- `Content-Type: audio/wav`
- `X-Device-Id`
- `X-Sample-Rate`
- `X-Format: wav`

请求体直接是 WAV 二进制。

当前 `src/mvp/cloud_http_client.c` 默认拼出的请求路径来自：

- `CLOUD_PATH`

默认就是：

- `/asr/recognize`

### 9.2 声纹原始 TCP 接口

监听端口默认是：

- `18080`

这个 TCP 服务不是 HTTP，而是自定义文本头 + PCM payload 协议。

`server/tcp_server.py` 当前支持两种帧头：

- `SVI`
  - 声纹识别
- `SVR`
  - 声纹注册

## 10. 声纹 TCP 协议说明

### 10.1 识别帧 `SVI`

两种格式：

无期望说话人：

```text
SVI,1,<session>,<slot>,16000,16,1,<samples>,<payload_bytes>\n
```

带期望说话人：

```text
SVI,1,<session>,<slot>,<expected_speaker_id>,16000,16,1,<samples>,<payload_bytes>\n
```

后面紧跟 `payload_bytes` 字节 PCM16 单声道数据。

返回格式：

```text
IDENTIFY,<session>,<slot>,<status>,<speaker_id>,<score>\n
```

### 10.2 注册帧 `SVR`

格式：

```text
SVR,1,<session>,<slot>,<speaker_id>,<utter_idx>,<utter_total>,16000,16,1,<samples>,<payload_bytes>\n
```

后面同样紧跟 PCM16 payload。

返回格式：

```text
ENROLL,<session>,<slot>,<speaker_id>,<accepted>,<required>,<score>\n
```

### 10.3 音频格式要求

服务端当前明确要求：

- 16000 Hz
- 16-bit
- 单声道

如果格式不一致，`server/tcp_server.py` 会直接拒绝。

## 11. 服务端环境变量

服务端配置集中在：

- `server/config.py`

移植时重点看这些变量：

- `SPEAKER_BACKEND`
- `SPEAKER_MODEL_ID`
- `SPEAKER_TCP_HOST`
- `SPEAKER_TCP_PORT`
- `SPEAKER_IDENTIFY_THRESHOLD`
- `SPEAKER_DATA_ROOT`

## 12. ModelScope 说明

当前服务端声纹后端默认尝试 `modelscope`。

但仓库里的基础依赖并没有把它写成强制项，所以实际运行分两种情况：

1. 安装了 `modelscope`
   - 走真实声纹模型
2. 没安装
   - 自动退化到 `fallback` 后端

所以交付时要明确告诉对方：

- 演示传输链路，只要服务能跑起来，`fallback` 也可以
- 真正要声纹识别效果，必须额外补齐 `modelscope` 依赖

## 13. 推荐的移植边界

### 13.1 只要 W800 通用传输能力

只迁移：

- `w800_at.*`
- `w800_wifi.*`
- `w800_socket.*`
- `config.h`
- `log.*`
- `uart_ringbuf.*`

这样目标项目就能自己决定：

- 传 HTTP
- 传 TCP
- 还是传别的自定义协议

### 13.2 要直接复用当前服务端协议

再加：

- `cloud_http_client.*`
- `http_parser_lite.*`
- `cloud_speaker_demo_client.*` 或自己按协议组包

这是更适合快速集成的路径。

## 14. 建议的集成顺序

建议严格按下面顺序：

1. 先让 `w800_init()` 跑通
2. 先让 `w800_basic_check()` 回 `AT`
3. 再让 `w800_join_ap()` 成功
4. 再验证 `w800_socket_open_tcp()`
5. 再验证普通 TCP 收发
6. 然后再接 HTTP 上传
7. 最后再接声纹 TCP 协议

不要一开始就把服务端业务和底层 AT 一起调，否则定位问题会很慢。

## 15. 常见问题

### 15.1 新工程里编不过

优先检查：

- `hal_data.h` 是否还能提供 `g_uart6`
- UART6 回调是否还挂到了 `mvp_uart6_callback`
- `g_ioport` 是否可用

### 15.2 W800 能回应 `AT`，但 TCP 一发就失败

优先检查：

- `SKCT` 后是否真的做了 `SKSTT` 轮询
- 远端服务端是否监听
- 防火墙是否放行

### 15.3 HTTP 通了，但声纹失败

优先检查：

- `CLOUD_PORT` 和 `SPEAKER_SERVER_PORT` 是否搞混
- `8000` 是 HTTP
- `18080` 是声纹原始 TCP

### 15.4 服务端能启动，但声纹效果不稳定

优先检查：

- 当前是不是 fallback backend
- `modelscope` 是否安装成功
- 阈值是不是太激进

## 16. 交付建议

如果你现在就是要把这部分发给别人，建议你最终交付包里只保留：

1. `src/mvp/` 里的 W800 与服务端协议相关文件
2. `server/` 目录
3. `tools/start_speaker_web.py`
4. 本文档

并另外口头或文档说明两件事：

1. MCU 侧真正通用可复用的是 `w800_at/w800_wifi/w800_socket`
2. 服务端分两个端口：HTTP `8000`，声纹 TCP `18080`

