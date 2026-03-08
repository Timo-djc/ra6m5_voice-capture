#include "w800_at.h"

#include "../audio_capture.h"
#include "cloud_asr_cfg.h"

#include <stdio.h>
#include <string.h>

#define W800_BRIDGE_CHUNK_BYTES        (512U)
#define W800_BRIDGE_CMD_BUFFER_SIZE    (48U)
#define W800_BRIDGE_ESCAPE_GUARD_MS    (1000U)
#define W800_BRIDGE_FLASH_READY_COUNT  (8U)
#define W800_BRIDGE_ESC_BURST_COUNT    (12U)
#define W800_BRIDGE_FLASH_RETRY_MS     (1500U)
#define W800_BRIDGE_ESC_KICK_MS        (200U)
#define W800_BRIDGE_FLASH_AUTO_RUN_IDLE_MS        (4000U)
#define W800_BRIDGE_FLASH_AUTO_RUN_MIN_TX_BYTES   (262144U)
#define W800_BRIDGE_RUN_WAIT_TIMEOUT_MS           (15000U)

typedef struct st_w800_uart_channel
{
    const uart_instance_t * instance;
    const char * mcu_name;
    const char * device_name;
    volatile uint8_t * rx_ring;
    volatile bool opened;
    volatile bool tx_busy;
    volatile bool tx_ok;
    volatile uint32_t rx_head;
    volatile uint32_t rx_tail;
    uint32_t current_baud;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_events;
    uint32_t tx_complete_events;
    uint32_t error_events;
} w800_uart_channel_t;

static volatile uint8_t s_at_rx_ring[W800_RX_RING_SIZE];
static volatile uint8_t s_flash_rx_ring[W800_RX_RING_SIZE];

static w800_uart_channel_t s_at_uart =
{
    .instance = W800_AT_UART_INSTANCE,
    .mcu_name = W800_AT_UART_NAME,
    .device_name = W800_AT_DEVICE_UART_NAME,
    .rx_ring = s_at_rx_ring,
    .opened = false,
    .tx_busy = false,
    .tx_ok = true,
    .rx_head = 0U,
    .rx_tail = 0U,
    .current_baud = 0U,
    .rx_bytes = 0U,
    .tx_bytes = 0U,
    .rx_events = 0U,
    .tx_complete_events = 0U,
    .error_events = 0U
};

static w800_uart_channel_t s_flash_uart =
{
    .instance = W800_FLASH_UART_INSTANCE,
    .mcu_name = W800_FLASH_UART_NAME,
    .device_name = W800_FLASH_DEVICE_UART_NAME,
    .rx_ring = s_flash_rx_ring,
    .opened = false,
    .tx_busy = false,
    .tx_ok = true,
    .rx_head = 0U,
    .rx_tail = 0U,
    .current_baud = 0U,
    .rx_bytes = 0U,
    .tx_bytes = 0U,
    .rx_events = 0U,
    .tx_complete_events = 0U,
    .error_events = 0U
};

static uint32_t s_bridge_core_hz = 0U;

static w800_bridge_state_t s_bridge_state = W800_BRIDGE_STATE_NORMAL_APP;
static w800_bridge_state_t s_bridge_cmd_source_state = W800_BRIDGE_STATE_NORMAL_APP;
static bool s_bridge_flash_ready = false;
static bool s_bridge_run_ready = false;
static bool s_bridge_auto_exit_pending = false;

static uint8_t s_bridge_host_pending[W800_BRIDGE_CHUNK_BYTES];
static uint32_t s_bridge_host_pending_len = 0U;
static uint8_t s_bridge_dev_pending[W800_BRIDGE_CHUNK_BYTES];
static uint32_t s_bridge_dev_pending_len = 0U;

static uint8_t s_bridge_escape_count = 0U;
static uint32_t s_bridge_escape_deadline_ms = 0U;
static uint32_t s_bridge_last_host_rx_ms = 0U;
static uint32_t s_bridge_c_count = 0U;
static uint32_t s_bridge_user_task_match = 0U;
static uint32_t s_bridge_flash_retry_deadline_ms = 0U;
static uint32_t s_bridge_flash_esc_deadline_ms = 0U;
static bsp_io_level_t s_bridge_flash_boot_level = BSP_IO_LEVEL_LOW;
static bool s_bridge_flash_alt_polarity_tried = false;
static uint32_t s_bridge_flash_attempts = 0U;
static uint32_t s_bridge_escape_bursts = 0U;
static uint32_t s_bridge_flash_host_tx_bytes = 0U;
static uint32_t s_bridge_flash_last_host_tx_ms = 0U;
static bool s_bridge_flash_seen_eot = false;
static uint32_t s_bridge_run_wait_deadline_ms = 0U;

static char s_bridge_cmd_buffer[W800_BRIDGE_CMD_BUFFER_SIZE];
static uint32_t s_bridge_cmd_length = 0U;

static bsp_io_level_t inactive_level(bsp_io_level_t active_level)
{
    return (BSP_IO_LEVEL_HIGH == active_level) ? BSP_IO_LEVEL_LOW : BSP_IO_LEVEL_HIGH;
}

static uint32_t ring_next(uint32_t idx)
{
    return (idx + 1U) % W800_RX_RING_SIZE;
}

static void channel_ring_push(w800_uart_channel_t * channel, uint8_t ch)
{
    uint32_t next = ring_next(channel->rx_head);

    if (next == channel->rx_tail)
    {
        channel->rx_tail = ring_next(channel->rx_tail);
    }

    channel->rx_ring[channel->rx_head] = ch;
    channel->rx_head = next;
}

static bool channel_ring_pop(w800_uart_channel_t * channel, uint8_t * out)
{
    if (channel->rx_tail == channel->rx_head)
    {
        return false;
    }

    *out = channel->rx_ring[channel->rx_tail];
    channel->rx_tail = ring_next(channel->rx_tail);
    return true;
}

static uint32_t bridge_now_ms(void)
{
    if (0U == s_bridge_core_hz)
    {
        s_bridge_core_hz = R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_ICLK);
        if (0U == s_bridge_core_hz)
        {
            s_bridge_core_hz = 1U;
        }
    }

    return (uint32_t) (((uint64_t) DWT->CYCCNT * 1000ULL) / s_bridge_core_hz);
}

static void bridge_write_force(const char * str)
{
    audio_debug_write_line_force(str);
}

static void channel_reset_rx(w800_uart_channel_t * channel)
{
    __disable_irq();
    channel->rx_head = 0U;
    channel->rx_tail = 0U;
    __enable_irq();
}

static bool channel_open(w800_uart_channel_t * channel)
{
    fsp_err_t err;

    if (channel->opened)
    {
        return true;
    }

    err = channel->instance->p_api->open(channel->instance->p_ctrl, channel->instance->p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        return false;
    }

    channel->opened = true;
    return true;
}

static bool channel_set_baud(w800_uart_channel_t * channel, uint32_t baud)
{
    baud_setting_t setting = {0};

    if (!channel->opened)
    {
        return false;
    }

    if (FSP_SUCCESS != R_SCI_UART_BaudCalculate(baud, false, 5000U, &setting))
    {
        return false;
    }

    if (FSP_SUCCESS != channel->instance->p_api->baudSet(channel->instance->p_ctrl, &setting))
    {
        return false;
    }

    channel->current_baud = baud;
    return true;
}

static bool channel_send(w800_uart_channel_t * channel, const uint8_t * data, uint32_t len)
{
    if ((!channel->opened) || (NULL == data) || (0U == len))
    {
        return false;
    }

    if (channel->tx_busy)
    {
        return false;
    }

    channel->tx_busy = true;
    channel->tx_ok = false;

    if (FSP_SUCCESS != channel->instance->p_api->write(channel->instance->p_ctrl, data, len))
    {
        channel->tx_busy = false;
        channel->tx_ok = false;
        return false;
    }

    channel->tx_bytes += len;
    return true;
}

static bool channel_read_bytes(w800_uart_channel_t * channel, uint8_t * out, uint32_t max_bytes, uint32_t * out_read)
{
    uint32_t read_count = 0U;

    if ((NULL == out) || (0U == max_bytes))
    {
        return false;
    }

    __disable_irq();
    while ((read_count < max_bytes) && (channel->rx_tail != channel->rx_head))
    {
        out[read_count++] = channel->rx_ring[channel->rx_tail];
        channel->rx_tail = ring_next(channel->rx_tail);
    }
    __enable_irq();

    if (NULL != out_read)
    {
        *out_read = read_count;
    }

    return (read_count > 0U);
}

static bool channel_read_line(w800_uart_channel_t * channel, char * out, uint32_t out_size)
{
    uint32_t idx;
    uint32_t len = 0U;
    bool found_newline = false;

    if ((NULL == out) || (out_size < 2U))
    {
        return false;
    }

    __disable_irq();

    idx = channel->rx_tail;
    while (idx != channel->rx_head)
    {
        if ('\n' == (char) channel->rx_ring[idx])
        {
            found_newline = true;
            break;
        }

        idx = ring_next(idx);
    }

    if (!found_newline)
    {
        __enable_irq();
        return false;
    }

    while (channel->rx_tail != channel->rx_head)
    {
        uint8_t ch = 0U;

        if (!channel_ring_pop(channel, &ch))
        {
            break;
        }

        if ('\n' == (char) ch)
        {
            break;
        }

        if ('\r' == (char) ch)
        {
            continue;
        }

        if (len < (out_size - 1U))
        {
            out[len++] = (char) ch;
        }
    }

    __enable_irq();

    out[len] = '\0';
    return true;
}

static bool channel_tx_idle(const w800_uart_channel_t * channel)
{
    return !channel->tx_busy;
}

static void channel_handle_callback(w800_uart_channel_t * channel, uart_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        return;
    }

    if (UART_EVENT_RX_CHAR == p_args->event)
    {
        channel->rx_events++;
        channel->rx_bytes++;
        channel_ring_push(channel, (uint8_t) p_args->data);
        return;
    }

    if (UART_EVENT_TX_COMPLETE == p_args->event)
    {
        channel->tx_complete_events++;
        channel->tx_busy = false;
        channel->tx_ok = true;
        return;
    }

    if ((UART_EVENT_ERR_PARITY == p_args->event) ||
        (UART_EVENT_ERR_FRAMING == p_args->event) ||
        (UART_EVENT_ERR_OVERFLOW == p_args->event) ||
        (UART_EVENT_BREAK_DETECT == p_args->event))
    {
        channel->error_events++;
        channel->tx_busy = false;
        channel->tx_ok = false;
    }
}

static void bridge_reset_trackers(void)
{
    s_bridge_flash_ready = false;
    s_bridge_run_ready = false;
    s_bridge_auto_exit_pending = false;
    s_bridge_host_pending_len = 0U;
    s_bridge_dev_pending_len = 0U;
    s_bridge_escape_count = 0U;
    s_bridge_escape_deadline_ms = 0U;
    s_bridge_last_host_rx_ms = bridge_now_ms();
    s_bridge_c_count = 0U;
    s_bridge_user_task_match = 0U;
    s_bridge_flash_retry_deadline_ms = 0U;
    s_bridge_flash_esc_deadline_ms = 0U;
    s_bridge_flash_boot_level = W800_BOOT_FLASH_LEVEL;
    s_bridge_flash_alt_polarity_tried = false;
    s_bridge_flash_attempts = 0U;
    s_bridge_escape_bursts = 0U;
    s_bridge_flash_host_tx_bytes = 0U;
    s_bridge_flash_last_host_tx_ms = s_bridge_last_host_rx_ms;
    s_bridge_flash_seen_eot = false;
    s_bridge_run_wait_deadline_ms = 0U;
    s_bridge_cmd_length = 0U;
    s_bridge_cmd_buffer[0] = '\0';
}

static void bridge_reset_pulse(void)
{
    w800_at_set_wakeup_level(W800_WAKE_ACTIVE_LEVEL);
    w800_at_set_reset_level(W800_RESET_ACTIVE_LEVEL);
    R_BSP_SoftwareDelay(20U, BSP_DELAY_UNITS_MILLISECONDS);
    w800_at_set_reset_level(inactive_level(W800_RESET_ACTIVE_LEVEL));
    R_BSP_SoftwareDelay(150U, BSP_DELAY_UNITS_MILLISECONDS);
}

static void bridge_wait_uart_idle(w800_uart_channel_t * channel)
{
    uint32_t timeout = 4000000U;

    while ((!channel_tx_idle(channel)) && (timeout > 0U))
    {
        timeout--;
        __NOP();
    }
}

static void bridge_send_bootloader_escape(void)
{
    uint8_t esc = 0x1BU;
    uint32_t i;

    s_bridge_escape_bursts++;

    for (i = 0U; i < W800_BRIDGE_ESC_BURST_COUNT; i++)
    {
        bridge_wait_uart_idle(&s_flash_uart);
        (void) channel_send(&s_flash_uart, &esc, 1U);
        bridge_wait_uart_idle(&s_flash_uart);
        R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);
    }
}

static void bridge_arm_flash_retry_timers(void)
{
    uint32_t now = bridge_now_ms();
    s_bridge_flash_retry_deadline_ms = now + W800_BRIDGE_FLASH_RETRY_MS;
    s_bridge_flash_esc_deadline_ms = now + W800_BRIDGE_ESC_KICK_MS;
}

static void bridge_start_flash_attempt(bsp_io_level_t boot_level, const char * reason)
{
    s_bridge_flash_attempts++;
    s_bridge_flash_boot_level = boot_level;
    w800_at_set_boot_level(boot_level);
    bridge_send_bootloader_escape();
    bridge_reset_pulse();
    channel_reset_rx(&s_flash_uart);
    s_bridge_c_count = 0U;
    s_bridge_flash_ready = false;
    bridge_send_bootloader_escape();
    bridge_arm_flash_retry_timers();

    if (NULL != reason)
    {
        bridge_write_force(reason);
    }
}

static void bridge_enqueue_host_byte(uint8_t ch)
{
    if (s_bridge_host_pending_len < sizeof(s_bridge_host_pending))
    {
        s_bridge_host_pending[s_bridge_host_pending_len++] = ch;
    }

    if (W800_BRIDGE_STATE_FLASH_BRIDGE == s_bridge_state)
    {
        s_bridge_flash_host_tx_bytes++;
        s_bridge_flash_last_host_tx_ms = bridge_now_ms();
        if (0x04U == ch)
        {
            s_bridge_flash_seen_eot = true;
        }
    }
}

static void bridge_flush_escape_to_host_queue(void)
{
    uint8_t i;

    for (i = 0U; i < s_bridge_escape_count; i++)
    {
        bridge_enqueue_host_byte((uint8_t) '+');
    }

    s_bridge_escape_count = 0U;
    s_bridge_escape_deadline_ms = 0U;
}

static void bridge_clear_escape_state(void)
{
    s_bridge_escape_count = 0U;
    s_bridge_escape_deadline_ms = 0U;
}

static void bridge_print_prompt(void)
{
    bridge_write_force("\r\nW800 bridge cmd> ");
}

static const char * bridge_state_name(w800_bridge_state_t state)
{
    switch (state)
    {
        case W800_BRIDGE_STATE_NORMAL_APP:
            return "NORMAL_APP";

        case W800_BRIDGE_STATE_FLASH_BRIDGE:
            return "FLASH_BRIDGE";

        case W800_BRIDGE_STATE_BRIDGE_CMD:
            return "BRIDGE_CMD";

        case W800_BRIDGE_STATE_RUN_BOOT_WAIT:
            return "RUN_BOOT_WAIT";

        default:
            return "UNKNOWN";
    }
}

static const char * bridge_level_name(bsp_io_level_t level)
{
    return (BSP_IO_LEVEL_HIGH == level) ? "HIGH" : "LOW";
}

static bsp_io_level_t bridge_read_pin_level(bsp_io_port_pin_t pin)
{
    bsp_io_level_t level = BSP_IO_LEVEL_LOW;

    if (FSP_SUCCESS != g_ioport.p_api->pinRead(g_ioport.p_ctrl, pin, &level))
    {
        return BSP_IO_LEVEL_LOW;
    }

    return level;
}

static void bridge_report_pin_levels(void)
{
    char line[128];

    (void) snprintf(line,
                    sizeof(line),
                    "W800: BOOT=%s RESET=%s WAKE=%s active_flash=%s\r\n",
                    bridge_level_name(bridge_read_pin_level(W800_BOOT_PIN)),
                    bridge_level_name(bridge_read_pin_level(W800_RESET_PIN)),
                    bridge_level_name(bridge_read_pin_level(W800_WAKE_PIN)),
                    bridge_level_name(s_bridge_flash_boot_level));
    bridge_write_force(line);
}

static void bridge_report_uart_status(const char * label, const w800_uart_channel_t * channel)
{
    char line[224];

    (void) snprintf(line,
                    sizeof(line),
                    "W800: %s %s->%s baud=%lu tx_bytes=%lu rx_bytes=%lu rx_events=%lu tx_done=%lu err=%lu\r\n",
                    label,
                    channel->mcu_name,
                    channel->device_name,
                    (unsigned long) channel->current_baud,
                    (unsigned long) channel->tx_bytes,
                    (unsigned long) channel->rx_bytes,
                    (unsigned long) channel->rx_events,
                    (unsigned long) channel->tx_complete_events,
                    (unsigned long) channel->error_events);
    bridge_write_force(line);
}

static void bridge_report_status(void)
{
    char line[224];

    (void) snprintf(line,
                    sizeof(line),
                    "\r\nW800: state=%s flash_ready=%u run_ready=%u muted=%u flash_attempts=%lu esc_bursts=%lu host_uart=%s\r\n",
                    bridge_state_name(s_bridge_state),
                    s_bridge_flash_ready ? 1U : 0U,
                    s_bridge_run_ready ? 1U : 0U,
                    audio_debug_is_muted() ? 1U : 0U,
                    (unsigned long) s_bridge_flash_attempts,
                    (unsigned long) s_bridge_escape_bursts,
                    W800_HOST_UART_NAME);
    bridge_write_force(line);

    (void) snprintf(line,
                    sizeof(line),
                    "W800: flash_c_count=%lu alt=%u cmd_source=%s tx_to_flash=%lu eot=%u\r\n",
                    (unsigned long) s_bridge_c_count,
                    s_bridge_flash_alt_polarity_tried ? 1U : 0U,
                    bridge_state_name(s_bridge_cmd_source_state),
                    (unsigned long) s_bridge_flash_host_tx_bytes,
                    s_bridge_flash_seen_eot ? 1U : 0U);
    bridge_write_force(line);

#if !W800_DOWNLOAD_BRIDGE_VIA_MCU
    bridge_write_force("W800: download path=external USB-UART <-> W800 UART0 (MCU bridge disabled)\r\n");
#endif

    bridge_report_uart_status("flash_uart", &s_flash_uart);
    bridge_report_uart_status("at_uart", &s_at_uart);
    bridge_report_pin_levels();
}

static void bridge_enter_command_mode(void)
{
    s_bridge_cmd_source_state = s_bridge_state;
    s_bridge_state = W800_BRIDGE_STATE_BRIDGE_CMD;
    s_bridge_host_pending_len = 0U;
    s_bridge_dev_pending_len = 0U;
    bridge_clear_escape_state();
    s_bridge_cmd_length = 0U;
    s_bridge_cmd_buffer[0] = '\0';
    bridge_write_force("\r\nW800: bridge command mode. Commands: run, status, pins, esc, reset, retry, exit\r\n");
    bridge_print_prompt();
}

static w800_uart_channel_t * bridge_active_uart(void)
{
    if (W800_BRIDGE_STATE_FLASH_BRIDGE == s_bridge_state)
    {
        return &s_flash_uart;
    }

    if (W800_BRIDGE_STATE_RUN_BOOT_WAIT == s_bridge_state)
    {
        return &s_at_uart;
    }

    return NULL;
}

static void bridge_enter_flash_mode_with_level(bsp_io_level_t boot_level)
{
    bridge_wait_uart_idle(&s_flash_uart);
    (void) channel_set_baud(&s_flash_uart, W800_UART_FALLBACK_BAUD);
    channel_reset_rx(&s_flash_uart);
    bridge_reset_trackers();
    s_bridge_state = W800_BRIDGE_STATE_FLASH_BRIDGE;
    s_bridge_cmd_source_state = W800_BRIDGE_STATE_FLASH_BRIDGE;
    audio_debug_set_muted(true);
    bridge_start_flash_attempt(boot_level,
                               "\r\nW800: flash bridge active on SCI7<->SCI5. Expect repeated 'C' on Xshell.\r\n");
}

static void bridge_resume_normal_app(void)
{
    bridge_reset_trackers();
    s_bridge_state = W800_BRIDGE_STATE_NORMAL_APP;
    s_bridge_cmd_source_state = W800_BRIDGE_STATE_NORMAL_APP;
    audio_debug_set_muted(false);
    bridge_write_force("\r\nW800: bridge closed, returning to app mode.\r\n");
}

static void bridge_observe_flash_byte(uint8_t ch)
{
    if ('C' == (char) ch)
    {
        if (s_bridge_c_count < 0xFFFFU)
        {
            s_bridge_c_count++;
        }

        if (s_bridge_c_count >= W800_BRIDGE_FLASH_READY_COUNT)
        {
            s_bridge_flash_ready = true;
        }

        return;
    }

    if ((ch != '\r') && (ch != '\n') && (ch != ' '))
    {
        s_bridge_c_count = 0U;
    }
}

static void bridge_observe_run_byte(uint8_t ch)
{
    static const char target[] = "user task";

    if (ch == (uint8_t) target[s_bridge_user_task_match])
    {
        s_bridge_user_task_match++;
        if ('\0' == target[s_bridge_user_task_match])
        {
            s_bridge_run_ready = true;
            s_bridge_auto_exit_pending = true;
            s_bridge_user_task_match = 0U;
        }

        return;
    }

    s_bridge_user_task_match = (ch == (uint8_t) target[0]) ? 1U : 0U;
}

static void bridge_observe_device_byte(uint8_t ch)
{
    if (W800_BRIDGE_STATE_FLASH_BRIDGE == s_bridge_state)
    {
        bridge_observe_flash_byte(ch);
    }
    else if (W800_BRIDGE_STATE_RUN_BOOT_WAIT == s_bridge_state)
    {
        bridge_observe_run_byte(ch);
    }
}

static void bridge_fill_device_pending(void)
{
    uint32_t read_count = 0U;
    uint32_t i;
    w800_uart_channel_t * channel = bridge_active_uart();

    if ((NULL == channel) || (0U != s_bridge_dev_pending_len))
    {
        return;
    }

    if (!channel_read_bytes(channel, s_bridge_dev_pending, sizeof(s_bridge_dev_pending), &read_count))
    {
        return;
    }

    s_bridge_dev_pending_len = read_count;
    for (i = 0U; i < read_count; i++)
    {
        bridge_observe_device_byte(s_bridge_dev_pending[i]);
    }
}

static void bridge_pump_host_to_active_uart(void)
{
    w800_uart_channel_t * channel = bridge_active_uart();

    if ((NULL == channel) || (0U == s_bridge_host_pending_len) || !channel_tx_idle(channel))
    {
        return;
    }

    if (channel_send(channel, s_bridge_host_pending, s_bridge_host_pending_len))
    {
        s_bridge_host_pending_len = 0U;
    }
}

static void bridge_pump_device_to_host(void)
{
    if ((0U == s_bridge_dev_pending_len) || !audio_debug_tx_idle())
    {
        return;
    }

    if (audio_debug_write_bytes(s_bridge_dev_pending, s_bridge_dev_pending_len))
    {
        s_bridge_dev_pending_len = 0U;
    }
}

static void bridge_host_stream_poll(bool forward_to_flash)
{
    uint8_t ch = 0U;
    uint32_t now = bridge_now_ms();

    if ((0U == s_bridge_escape_count) && (0U == s_bridge_host_pending_len))
    {
        s_bridge_last_host_rx_ms = now;
    }

    while ((forward_to_flash ? (s_bridge_host_pending_len < sizeof(s_bridge_host_pending)) : true) &&
           (1U == audio_debug_read_bytes(&ch, 1U)))
    {
        now = bridge_now_ms();

        if (0U == s_bridge_escape_count)
        {
            if ((ch == (uint8_t) '+') &&
                ((uint32_t) (now - s_bridge_last_host_rx_ms) >= W800_BRIDGE_ESCAPE_GUARD_MS))
            {
                s_bridge_escape_count = 1U;
                s_bridge_escape_deadline_ms = now + W800_BRIDGE_ESCAPE_GUARD_MS;
            }
            else if (forward_to_flash)
            {
                bridge_enqueue_host_byte(ch);
            }

            s_bridge_last_host_rx_ms = now;
            continue;
        }

        if ((ch == (uint8_t) '+') && (s_bridge_escape_count < 3U))
        {
            s_bridge_escape_count++;
            s_bridge_escape_deadline_ms = now + W800_BRIDGE_ESCAPE_GUARD_MS;
            s_bridge_last_host_rx_ms = now;

            if (3U == s_bridge_escape_count)
            {
                bridge_clear_escape_state();
                bridge_enter_command_mode();
                return;
            }

            continue;
        }

        if (forward_to_flash)
        {
            bridge_flush_escape_to_host_queue();
            bridge_enqueue_host_byte(ch);
        }
        else
        {
            bridge_clear_escape_state();
        }

        s_bridge_last_host_rx_ms = now;
    }

    now = bridge_now_ms();
    if ((s_bridge_escape_count > 0U) &&
        ((int32_t) (now - s_bridge_escape_deadline_ms) >= 0))
    {
        if (3U == s_bridge_escape_count)
        {
            bridge_clear_escape_state();
            bridge_enter_command_mode();
        }
        else if (forward_to_flash)
        {
            bridge_flush_escape_to_host_queue();
        }
        else
        {
            bridge_clear_escape_state();
        }
    }
}

static void bridge_process_transparent_mode(void)
{
    uint32_t now;
    w800_uart_channel_t * active_uart = bridge_active_uart();

    bridge_host_stream_poll((NULL != active_uart));

    if (NULL != active_uart)
    {
        bridge_pump_host_to_active_uart();
    }

    bridge_fill_device_pending();
    bridge_pump_device_to_host();

    now = bridge_now_ms();
    if ((W800_BRIDGE_STATE_FLASH_BRIDGE == s_bridge_state) && !s_bridge_flash_ready)
    {
        if ((int32_t) (now - s_bridge_flash_esc_deadline_ms) >= 0)
        {
            bridge_send_bootloader_escape();
            s_bridge_flash_esc_deadline_ms = now + W800_BRIDGE_ESC_KICK_MS;
        }

        if ((int32_t) (now - s_bridge_flash_retry_deadline_ms) >= 0)
        {
            if (!s_bridge_flash_alt_polarity_tried)
            {
                s_bridge_flash_alt_polarity_tried = true;
                bridge_start_flash_attempt(inactive_level(W800_BOOT_FLASH_LEVEL),
                                           "\r\nW800: no 'C' yet, retrying with alternate BOOT polarity.\r\n");
            }
            else
            {
                s_bridge_flash_retry_deadline_ms = now + W800_BRIDGE_FLASH_RETRY_MS;
                bridge_write_force("\r\nW800: still no 'C'. Check SCI5<->W800 UART0 wiring and BOOT/RESET mapping.\r\n");
                bridge_report_status();
            }
        }
    }

    if ((W800_BRIDGE_STATE_FLASH_BRIDGE == s_bridge_state) &&
        s_bridge_flash_ready &&
        (0U == s_bridge_host_pending_len) &&
        (0U == s_bridge_dev_pending_len) &&
        audio_debug_tx_idle() &&
        channel_tx_idle(&s_flash_uart))
    {
        bool enough_host_data = (s_bridge_flash_host_tx_bytes >= W800_BRIDGE_FLASH_AUTO_RUN_MIN_TX_BYTES);
        bool host_idle = ((uint32_t) (now - s_bridge_flash_last_host_tx_ms) >= W800_BRIDGE_FLASH_AUTO_RUN_IDLE_MS);

        if (enough_host_data && host_idle)
        {
            bridge_write_force("\r\nW800: flash transfer idle detected. Releasing BOOT and rebooting module...\r\n");
            w800_enter_run_mode();
            bridge_write_force("\r\nW800: switched to run boot mode, waiting for 'user task'.\r\n");
            return;
        }
    }

    if ((W800_BRIDGE_STATE_RUN_BOOT_WAIT == s_bridge_state) &&
        !s_bridge_run_ready &&
        (0U != s_bridge_run_wait_deadline_ms) &&
        ((int32_t) (now - s_bridge_run_wait_deadline_ms) >= 0))
    {
        bridge_write_force("\r\nW800: no 'user task' within timeout. Exiting bridge and resuming app mode.\r\n");
        bridge_resume_normal_app();
        return;
    }

    if (s_bridge_auto_exit_pending &&
        (0U == s_bridge_dev_pending_len) &&
        (0U == s_bridge_host_pending_len) &&
        audio_debug_tx_idle() &&
        (NULL != active_uart) &&
        channel_tx_idle(active_uart))
    {
        bridge_resume_normal_app();
    }
}

static void bridge_process_run_wait_external(void)
{
    uint32_t now = bridge_now_ms();

    bridge_fill_device_pending();
    bridge_pump_device_to_host();

    if (!s_bridge_run_ready &&
        (0U != s_bridge_run_wait_deadline_ms) &&
        ((int32_t) (now - s_bridge_run_wait_deadline_ms) >= 0))
    {
        bridge_write_force("\r\nW800: no 'user task' within timeout on SCI6. Returning to app mode.\r\n");
        bridge_resume_normal_app();
        return;
    }

    if (s_bridge_auto_exit_pending &&
        (0U == s_bridge_dev_pending_len) &&
        audio_debug_tx_idle() &&
        channel_tx_idle(&s_at_uart))
    {
        bridge_write_force("\r\nW800: detected 'user task' on SCI6.\r\n");
        bridge_resume_normal_app();
    }
}

static void bridge_handle_normal_command(const char * cmd)
{
    if ((NULL == cmd) || ('\0' == cmd[0]))
    {
        return;
    }

    if (0 == strcmp(cmd, "w800 flash"))
    {
        w800_enter_flash_mode();
        return;
    }

#if W800_DOWNLOAD_BRIDGE_VIA_MCU
    if (0 == strcmp(cmd, "w800 flash_hi"))
    {
        bridge_enter_flash_mode_with_level(BSP_IO_LEVEL_HIGH);
        return;
    }

    if (0 == strcmp(cmd, "w800 flash_lo"))
    {
        bridge_enter_flash_mode_with_level(BSP_IO_LEVEL_LOW);
        return;
    }
#else
    if (0 == strcmp(cmd, "w800 flash_hi"))
    {
        w800_at_set_boot_level(BSP_IO_LEVEL_HIGH);
        bridge_reset_pulse();
        channel_reset_rx(&s_at_uart);
        bridge_write_force("\r\nW800: forced BOOT=HIGH then RESET.\r\n");
        bridge_write_force("W800: use external USB-UART on W800 UART0 for YMODEM.\r\n");
        return;
    }

    if (0 == strcmp(cmd, "w800 flash_lo"))
    {
        w800_at_set_boot_level(BSP_IO_LEVEL_LOW);
        bridge_reset_pulse();
        channel_reset_rx(&s_at_uart);
        bridge_write_force("\r\nW800: forced BOOT=LOW then RESET.\r\n");
        bridge_write_force("W800: use external USB-UART on W800 UART0 for YMODEM.\r\n");
        return;
    }
#endif

    if ((0 == strcmp(cmd, "status")) || (0 == strcmp(cmd, "w800 status")))
    {
        bridge_report_status();
        return;
    }

    if ((0 == strcmp(cmd, "w800 run")) || (0 == strcmp(cmd, "run")))
    {
        w800_enter_run_mode();
        bridge_write_force("\r\nW800: switched to run boot mode, waiting for 'user task'.\r\n");
        return;
    }

    if ((0 == strcmp(cmd, "pins")) || (0 == strcmp(cmd, "w800 pins")))
    {
        bridge_report_pin_levels();
        return;
    }

    if ((0 == strcmp(cmd, "reset")) || (0 == strcmp(cmd, "w800 reset")))
    {
        bridge_reset_pulse();
        channel_reset_rx(&s_at_uart);
        bridge_write_force("\r\nW800: RESET pulse sent.\r\n");
        bridge_report_pin_levels();
        return;
    }

    if ((0 == strcmp(cmd, "help")) || (0 == strcmp(cmd, "w800 help")))
    {
#if W800_DOWNLOAD_BRIDGE_VIA_MCU
        bridge_write_force("\r\nW800: commands: w800 flash, w800 flash_hi, w800 flash_lo, run, status, pins, reset, exit\r\n");
#else
        bridge_write_force("\r\nW800: commands: w800 flash, w800 flash_hi, w800 flash_lo, run, status, pins, reset\r\n");
        bridge_write_force("W800: note: UART0 flashing is external USB-UART, MCU only controls BOOT/RESET and SCI6 observe.\r\n");
#endif
        return;
    }

    if ((0 == strcmp(cmd, "w800 exit")) || (0 == strcmp(cmd, "exit")))
    {
        bridge_resume_normal_app();
        return;
    }

    bridge_write_force("\r\nW800: unknown command, type 'help'.\r\n");
}

static void bridge_handle_cmd_mode_command(const char * cmd)
{
    if ((NULL == cmd) || ('\0' == cmd[0]))
    {
        bridge_print_prompt();
        return;
    }

    if (0 == strcmp(cmd, "run"))
    {
        w800_enter_run_mode();
        bridge_write_force("\r\nW800: switched to run boot mode, waiting for 'user task'.\r\n");
        return;
    }

    if (0 == strcmp(cmd, "status"))
    {
        bridge_report_status();
        bridge_print_prompt();
        return;
    }

    if (0 == strcmp(cmd, "pins"))
    {
        bridge_report_pin_levels();
        bridge_print_prompt();
        return;
    }

    if (0 == strcmp(cmd, "esc"))
    {
#if W800_DOWNLOAD_BRIDGE_VIA_MCU
        bridge_send_bootloader_escape();
        bridge_write_force("\r\nW800: sent ESC burst on SCI5 -> W800 UART0.\r\n");
#else
        bridge_write_force("\r\nW800: esc unavailable, MCU flash bridge is disabled.\r\n");
#endif
        bridge_print_prompt();
        return;
    }

    if (0 == strcmp(cmd, "reset"))
    {
        bridge_reset_pulse();
        channel_reset_rx(&s_flash_uart);
        channel_reset_rx(&s_at_uart);
        bridge_write_force("\r\nW800: pulsed RESET, RX buffers cleared.\r\n");
        bridge_report_pin_levels();
        bridge_print_prompt();
        return;
    }

    if (0 == strcmp(cmd, "retry"))
    {
#if W800_DOWNLOAD_BRIDGE_VIA_MCU
        channel_reset_rx(&s_flash_uart);
        s_bridge_state = W800_BRIDGE_STATE_FLASH_BRIDGE;
        s_bridge_cmd_source_state = W800_BRIDGE_STATE_FLASH_BRIDGE;
        audio_debug_set_muted(true);
        bridge_start_flash_attempt(s_bridge_flash_boot_level,
                                   "\r\nW800: retried flash handshake on SCI5 -> W800 UART0.\r\n");
#else
        bridge_write_force("\r\nW800: retry unavailable, MCU flash bridge is disabled.\r\n");
        bridge_print_prompt();
#endif
        return;
    }

    if (0 == strcmp(cmd, "exit"))
    {
        bridge_resume_normal_app();
        return;
    }

    bridge_write_force("\r\nW800: unknown command. Use run, status, pins, esc, reset, retry or exit.\r\n");
    bridge_print_prompt();
}

static void bridge_command_poll(void)
{
    uint8_t temp[W800_BRIDGE_CHUNK_BYTES];
    uint32_t count = audio_debug_read_bytes(temp, sizeof(temp));
    uint32_t i;

    for (i = 0U; i < count; i++)
    {
        uint8_t ch = temp[i];

        if ((ch == '\r') || (ch == '\n'))
        {
            if (s_bridge_cmd_length > 0U)
            {
                s_bridge_cmd_buffer[s_bridge_cmd_length] = '\0';
                if (W800_BRIDGE_STATE_NORMAL_APP == s_bridge_state)
                {
                    bridge_handle_normal_command(s_bridge_cmd_buffer);
                }
                else
                {
                    bridge_handle_cmd_mode_command(s_bridge_cmd_buffer);
                }

                s_bridge_cmd_length = 0U;
                s_bridge_cmd_buffer[0] = '\0';
            }

            continue;
        }

        if ((ch == '\b') || (ch == 0x7FU))
        {
            if (s_bridge_cmd_length > 0U)
            {
                s_bridge_cmd_length--;
                s_bridge_cmd_buffer[s_bridge_cmd_length] = '\0';
            }

            continue;
        }

        if ((ch < 0x20U) || (ch > 0x7EU))
        {
            continue;
        }

        if (s_bridge_cmd_length < (sizeof(s_bridge_cmd_buffer) - 1U))
        {
            s_bridge_cmd_buffer[s_bridge_cmd_length++] = (char) ch;
            s_bridge_cmd_buffer[s_bridge_cmd_length] = '\0';
        }
    }
}

bool w800_at_init(void)
{
    if (!channel_open(&s_at_uart))
    {
        return false;
    }

    /* W800_DOWNLOAD_BRIDGE_VIA_MCU=0: UART5(SCI5) 不再作为烧写桥接使用。
     * W800 UART0 直接连接外部 USB-UART 适配器烧写固件。 */
#if W800_DOWNLOAD_BRIDGE_VIA_MCU
    if (!channel_open(&s_flash_uart))
    {
        return false;
    }
#endif

    s_bridge_state = W800_BRIDGE_STATE_NORMAL_APP;
    s_bridge_cmd_source_state = W800_BRIDGE_STATE_NORMAL_APP;
    bridge_reset_trackers();
    audio_debug_set_muted(false);
    w800_at_set_boot_level(W800_BOOT_RUN_LEVEL);
    w800_at_set_wakeup_level(W800_WAKE_ACTIVE_LEVEL);
    w800_at_set_reset_level(inactive_level(W800_RESET_ACTIVE_LEVEL));

    if (!channel_set_baud(&s_at_uart, W800_UART_TARGET_BAUD))
    {
        if (!channel_set_baud(&s_at_uart, W800_UART_FALLBACK_BAUD))
        {
            return false;
        }
    }

#if W800_DOWNLOAD_BRIDGE_VIA_MCU
    (void) channel_set_baud(&s_flash_uart, W800_UART_FALLBACK_BAUD);
    channel_reset_rx(&s_flash_uart);
#endif
    channel_reset_rx(&s_at_uart);
    return true;
}

void w800_at_poll(void)
{
    /* Reserved for future driver-level tasks. */
}

void w800_at_reset_rx(void)
{
    channel_reset_rx(&s_at_uart);
}

bool w800_at_set_baud(uint32_t baud)
{
    return channel_set_baud(&s_at_uart, baud);
}

uint32_t w800_at_get_baud(void)
{
    return s_at_uart.current_baud;
}

bool w800_at_send(const uint8_t * data, uint32_t len)
{
    return channel_send(&s_at_uart, data, len);
}

bool w800_at_send_str(const char * str)
{
    if (NULL == str)
    {
        return false;
    }

    return w800_at_send((const uint8_t *) str, (uint32_t) strlen(str));
}

bool w800_at_read_line(char * out, uint32_t out_size)
{
    return channel_read_line(&s_at_uart, out, out_size);
}

bool w800_at_read_bytes(uint8_t * out, uint32_t max_bytes, uint32_t * out_read)
{
    return channel_read_bytes(&s_at_uart, out, max_bytes, out_read);
}

bool w800_at_tx_idle(void)
{
    return channel_tx_idle(&s_at_uart);
}

bool w800_at_last_tx_ok(void)
{
    return s_at_uart.tx_ok;
}

void w800_at_set_boot_level(bsp_io_level_t level)
{
    (void) g_ioport.p_api->pinWrite(g_ioport.p_ctrl, W800_BOOT_PIN, level);
}

void w800_at_set_wakeup_level(bsp_io_level_t level)
{
    (void) g_ioport.p_api->pinWrite(g_ioport.p_ctrl, W800_WAKE_PIN, level);
}

void w800_at_set_reset_level(bsp_io_level_t level)
{
    (void) g_ioport.p_api->pinWrite(g_ioport.p_ctrl, W800_RESET_PIN, level);
}

void w800_at_hard_reset(void)
{
    bridge_wait_uart_idle(&s_at_uart);
    w800_at_set_boot_level(W800_BOOT_RUN_LEVEL);
    bridge_reset_pulse();
    channel_reset_rx(&s_at_uart);
}

void w800_enter_flash_mode(void)
{
#if W800_DOWNLOAD_BRIDGE_VIA_MCU
    bridge_enter_flash_mode_with_level(W800_BOOT_FLASH_LEVEL);
#else
    bridge_wait_uart_idle(&s_at_uart);
    (void) channel_set_baud(&s_at_uart, W800_UART_FALLBACK_BAUD);
    w800_at_set_boot_level(W800_BOOT_FLASH_LEVEL);
    bridge_reset_pulse();
    channel_reset_rx(&s_at_uart);
    bridge_reset_trackers();
    s_bridge_state = W800_BRIDGE_STATE_NORMAL_APP;
    s_bridge_cmd_source_state = W800_BRIDGE_STATE_NORMAL_APP;
    audio_debug_set_muted(false);
    bridge_write_force("\r\nW800: set to FLASH boot and reset done.\r\n");
    bridge_write_force("W800: now use external USB-UART on W800 UART0 and start YMODEM in terminal.\r\n");
#endif
}

void w800_enter_run_mode(void)
{
#if W800_DOWNLOAD_BRIDGE_VIA_MCU
    bridge_wait_uart_idle(&s_flash_uart);
#endif
    bridge_wait_uart_idle(&s_at_uart);
    (void) channel_set_baud(&s_at_uart, W800_UART_FALLBACK_BAUD);
    w800_at_set_boot_level(W800_BOOT_RUN_LEVEL);
    bridge_reset_pulse();
    channel_reset_rx(&s_at_uart);
#if W800_DOWNLOAD_BRIDGE_VIA_MCU
    channel_reset_rx(&s_flash_uart);
#endif
    bridge_reset_trackers();
    s_bridge_state = W800_BRIDGE_STATE_RUN_BOOT_WAIT;
    s_bridge_cmd_source_state = W800_BRIDGE_STATE_RUN_BOOT_WAIT;
    s_bridge_run_wait_deadline_ms = bridge_now_ms() + W800_BRIDGE_RUN_WAIT_TIMEOUT_MS;
    audio_debug_set_muted(false);
}

void w800_bridge_poll(void)
{
    if (W800_BRIDGE_STATE_NORMAL_APP == s_bridge_state)
    {
        bridge_command_poll();
        return;
    }

#if W800_DOWNLOAD_BRIDGE_VIA_MCU
    if (W800_BRIDGE_STATE_BRIDGE_CMD == s_bridge_state)
    {
        bridge_command_poll();
        return;
    }

    bridge_process_transparent_mode();
#else
    bridge_command_poll();

    if (W800_BRIDGE_STATE_RUN_BOOT_WAIT == s_bridge_state)
    {
        bridge_process_run_wait_external();
    }
#endif /* W800_DOWNLOAD_BRIDGE_VIA_MCU */
}

bool w800_bridge_is_active(void)
{
#if W800_DOWNLOAD_BRIDGE_VIA_MCU
    return (W800_BRIDGE_STATE_NORMAL_APP != s_bridge_state);
#else
    return (W800_BRIDGE_STATE_RUN_BOOT_WAIT == s_bridge_state);
    return false; /* MCU 桥接已禁用，使用 USB-UART 适配器直连 W800 */
#endif
}

w800_bridge_state_t w800_bridge_get_state(void)
{
    return s_bridge_state;
}

void uart5_callback(uart_callback_args_t * p_args)
{
    /* W800_DOWNLOAD_BRIDGE_VIA_MCU=0: UART5 桥接已禁用，W800 UART0 直接接 USB-UART */
#if W800_DOWNLOAD_BRIDGE_VIA_MCU
    channel_handle_callback(&s_flash_uart, p_args);
#else
    (void) p_args;
#endif
}

void uart6_callback(uart_callback_args_t * p_args)
{
    channel_handle_callback(&s_at_uart, p_args);
}
