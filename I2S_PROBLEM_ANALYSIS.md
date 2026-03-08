# I2S 音频采集问题分析报告

## 问题症状

从串口数据可以看出：
- `window_samples=0` - 没有采集到任何音频样本
- `window_callbacks=0` - I2S RX回调从未触发
- `no_callback_timeout_count` 持续增加 - DMA传输从未完成
- BCK/WS边沿计数为0 - 引脚上没有检测到时钟信号

## 根本原因

### SSI配置问题（主模式 + 只读模式 冲突）

**当前配置：**
- 模式：I2S_MODE_MASTER (主模式)
- 音频时钟：SSI_AUDIO_CLOCK_INTERNAL (使用GPT1输出作为MCLK)
- TXI中断：**禁用** (BSP_IRQ_DISABLED)
- p_transfer_tx：**NULL** (无TX DMA)
- 代码使用：`g_i2s0.p_api->read()` (只读模式)

**问题分析：**

根据 Renesas RA SSI 模块的工作原理：

1. **在主模式下，SSI需要同时使能REN和TEN才能产生BCK/WS时钟**
   - 即使只接收数据，也需要TX使能来驱动时钟生成器
   - 只设置REN=1而TEN=0会导致时钟生成器不工作

2. **当前寄存器状态** (从日志):
   ```
   SSICR=0x442B4201
   bit[0] REN = 1  (接收使能)
   bit[8] TEN = 0  (发送禁用)  ← 这是问题所在！
   ```

3. **GPT1时钟配置**
   - GPT1配置为输出音频主时钟 (AUDIO_CLK ≈ 1.024MHz)
   - 但由于SSI的TEN=0，SSI不会使用这个时钟来生成BCK/WS

## 解决方案

### 方案1：启用TX支持（推荐）

需要修改FSP配置以支持WriteRead模式：

#### 步骤1：在FSP Configurator中添加TX DMA

1. 打开项目的 `configuration.xml` (双击进入FSP Configuration)
2. 找到 **g_i2s0** (SSI0) 配置
3. 在 **Stacks** 标签页中：
   - 右键点击 **g_i2s0** → **Add Module** → **Transfer (DTC)**
   - 将新添加的传输实例重命名为 `g_transfer_tx`
   - 设置 **Transfer Size**: `4 Bytes`
   - 设置 **Mode**: `Block`
   - 设置 **Activation Source**: `SSI0 TXI`

4. 在 **g_i2s0** 属性中：
   - 找到 **Interrupts** 部分
   - **TXI Interrupt Priority**: 设置为 `13` （与RXI相同）

5. 保存并生成代码 (Generate Project Content)

#### 步骤2：重新编译并烧录

#### 步骤3：验证

代码会在10秒无回调后自动切换到WriteRead模式，但有了TX DMA支持，应该能正常工作。

### 方案2：改用从模式（如果有外部主设备）

如果有外部音频设备提供时钟信号：

1. 在FSP Configuration中修改：
   - **Operating Mode**: 从 `Master` 改为 `Slave`
   - **Audio Clock**: 改为 `External`
   
2. 连接硬件：
   - 外部设备的BCK → P112 (SSISCK0)
   - 外部设备的WS → P113 (SSIWS0)
   - 外部设备的DATA → P114 (SSIRXD0)

### 方案3：使用外部音频时钟输入

如果要保持主模式但使用外部MCLK：

1. 在FSP Configuration中：
   - **Audio Clock**: 改为 `External` (使用AUDIO_CLK引脚输入)
   
2. 将GPT1输出连接到AUDIO_CLK输入引脚（需查阅MCU引脚表）

## 调试步骤

### 1. 验证GPT1输出

使用示波器或逻辑分析仪测量：
- **P109 (GTIOC1A)**: 应该有约1.024MHz的方波输出
- 占空比应该约为50%

### 2. 验证SSI引脚

在采用方案1后，测量：
- **P112 (SSIBCK0)**: 位时钟，1.024MHz
- **P113 (SSIWS0)**: 字选择信号，16kHz，占空比50%
- **P114 (SSIRXD0)**: 数据输入

### 3. 检查诊断输出

重启开发板后，在串口输出的前几行应该看到：
```
DIAG: GPT ch=1 clk=100000000Hz
DIAG: GPT state=1 count=...
DIAG: GPT GTPR=0x00000061 GTCCRA=...
DIAG: SSICR=...
```

关键检查项：
- `GTPR` 应该约为 97-98 (0x61-0x62)
- 修改后，`SSICR` 的bit[8]应该变为1 (TEN=1)

## 参考资料

- RA6M5 User's Manual: Hardware - Chapter "Serial Sound Interface (SSI)"
- FSP User's Manual: r_ssi module
- 项目文档: `audio_capture.c` 中的注释（第216行附近）

## 修改历史

- 2026-02-13: 初始分析，识别主模式+只读冲突问题
