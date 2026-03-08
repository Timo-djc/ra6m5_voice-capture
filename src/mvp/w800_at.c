#include "w800_at.h"

#include "config.h"
#include "log.h"
#include "uart_ringbuf.h"

#include <stdio.h>
#include <string.h>

/* W800 hardware reset pin: NRST = P508, active-low */
#ifndef W800_RESET_PIN
#define W800_RESET_PIN           BSP_IO_PORT_05_PIN_08
#endif
#ifndef W800_RESET_ACTIVE_LEVEL
#define W800_RESET_ACTIVE_LEVEL  BSP_IO_LEVEL_LOW
#endif

static uart_ringbuf_t s_rx_rb;
static volatile uint8_t s_rx_storage[W800_UART_RX_BUF_SIZE];
static volatile bool s_uart_opened = false;
static volatile bool s_tx_busy = false;
static volatile bool s_tx_ok = true;
static uint32_t s_core_hz = 0U;

static uint32_t mvp_now_ms(void)
{
    if (0U == s_core_hz)
    {
        s_core_hz = R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_ICLK);
        if (0U == s_core_hz)
        {
            s_core_hz = 1U;
        }
    }

    return (uint32_t) (((uint64_t) DWT->CYCCNT * 1000ULL) / s_core_hz);
}

uint32_t w800_now_ms(void)
{
    return mvp_now_ms();
}

static bool line_has(const char * line, const char * token)
{
    if ((NULL == line) || (NULL == token))
    {
        return false;
    }

    return (NULL != strstr(line, token));
}

static bool line_is_ok(const char * line)
{
    return line_has(line, "+OK") || (0 == strcmp(line, "OK"));
}

static bool line_is_err(const char * line)
{
    return line_has(line, "+ERR") || line_has(line, "ERROR") || line_has(line, "FAIL");
}

static int uart_open_if_needed(void)
{
    fsp_err_t err;

    if (s_uart_opened)
    {
        return MVP_OK;
    }

    err = g_uart6.p_api->open(g_uart6.p_ctrl, g_uart6.p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        return MVP_ERR_UART;
    }

    s_uart_opened = true;
    return MVP_OK;
}

int w800_set_baud(uint32_t baud)
{
    baud_setting_t setting = {0};
    int rc;

    rc = uart_open_if_needed();
    if (MVP_OK != rc)
    {
        return rc;
    }

    if (FSP_SUCCESS != R_SCI_UART_BaudCalculate(baud, false, 5000U, &setting))
    {
        return MVP_ERR_UART;
    }

    if (FSP_SUCCESS != g_uart6.p_api->baudSet(g_uart6.p_ctrl, &setting))
    {
        return MVP_ERR_UART;
    }

    return MVP_OK;
}

int w800_init(void)
{
    int rc;

    uart_ringbuf_init(&s_rx_rb, s_rx_storage, W800_UART_RX_BUF_SIZE);

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    rc = uart_open_if_needed();
    if (MVP_OK != rc)
    {
        return rc;
    }

    rc = w800_set_baud(W800_UART_BAUD);
    if (MVP_OK != rc)
    {
        return rc;
    }

    w800_uart_drain_rx();
    s_tx_busy = false;
    s_tx_ok = true;

    return MVP_OK;
}

bool w800_tx_idle(void)
{
    return (!s_tx_busy);
}

void w800_uart_rx_byte(uint8_t byte)
{
    (void) uart_ringbuf_push(&s_rx_rb, byte);
}

void w800_uart_drain_rx(void)
{
    __disable_irq();
    uart_ringbuf_clear(&s_rx_rb);
    __enable_irq();
}

int w800_write_raw(const uint8_t * data, size_t size, uint32_t timeout_ms)
{
    uint32_t deadline;
    int rc;

    if ((NULL == data) || (0U == size))
    {
        return MVP_ERR_ARG;
    }

    rc = uart_open_if_needed();
    if (MVP_OK != rc)
    {
        return rc;
    }

    deadline = mvp_now_ms() + timeout_ms;
    while (s_tx_busy)
    {
        if ((int32_t) (mvp_now_ms() - deadline) >= 0)
        {
            return MVP_ERR_TIMEOUT;
        }
        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
    }

    s_tx_busy = true;
    s_tx_ok = false;

    if (FSP_SUCCESS != g_uart6.p_api->write(g_uart6.p_ctrl, data, size))
    {
        s_tx_busy = false;
        s_tx_ok = false;
        return MVP_ERR_UART;
    }

    while (s_tx_busy)
    {
        if ((int32_t) (mvp_now_ms() - deadline) >= 0)
        {
            return MVP_ERR_TIMEOUT;
        }
        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
    }

    return s_tx_ok ? MVP_OK : MVP_ERR_UART;
}

int w800_read_bytes_exact(uint8_t * out, size_t size, uint32_t timeout_ms)
{
    size_t read_count = 0U;
    uint8_t ch;
    uint32_t deadline;

    if ((NULL == out) || (0U == size))
    {
        return MVP_ERR_ARG;
    }

    deadline = mvp_now_ms() + timeout_ms;
    while (read_count < size)
    {
        __disable_irq();
        if (uart_ringbuf_pop(&s_rx_rb, &ch))
        {
            __enable_irq();
            out[read_count++] = ch;
            continue;
        }
        __enable_irq();

        if ((int32_t) (mvp_now_ms() - deadline) >= 0)
        {
            return MVP_ERR_TIMEOUT;
        }
        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
    }

    return MVP_OK;
}

int w800_read_bytes_any(uint8_t * out, size_t max_size, size_t * out_size, uint32_t timeout_ms)
{
    size_t read_count = 0U;
    uint8_t ch;
    uint32_t deadline;

    if ((NULL == out) || (0U == max_size) || (NULL == out_size))
    {
        return MVP_ERR_ARG;
    }

    deadline = mvp_now_ms() + timeout_ms;

    while (true)
    {
        __disable_irq();
        if (uart_ringbuf_pop(&s_rx_rb, &ch))
        {
            __enable_irq();
            out[read_count++] = ch;
            break;
        }
        __enable_irq();

        if ((int32_t) (mvp_now_ms() - deadline) >= 0)
        {
            return MVP_ERR_TIMEOUT;
        }
        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
    }

    while (read_count < max_size)
    {
        __disable_irq();
        if (uart_ringbuf_pop(&s_rx_rb, &ch))
        {
            __enable_irq();
            out[read_count++] = ch;
        }
        else
        {
            __enable_irq();
            break;
        }
    }

    *out_size = read_count;
    return MVP_OK;
}

int w800_read_response(char * out, size_t out_size, uint32_t timeout_ms)
{
    size_t idx = 0U;
    uint8_t ch;
    uint32_t deadline;

    if ((NULL == out) || (out_size < 2U))
    {
        return MVP_ERR_ARG;
    }

    out[0] = '\0';
    deadline = mvp_now_ms() + timeout_ms;

    while (true)
    {
        __disable_irq();
        if (uart_ringbuf_pop(&s_rx_rb, &ch))
        {
            __enable_irq();

            if ('\r' == (char) ch)
            {
                continue;
            }

            if ('\n' == (char) ch)
            {
                if (idx == 0U)
                {
                    continue;
                }

                out[idx] = '\0';
                return MVP_OK;
            }

            if (idx < (out_size - 1U))
            {
                out[idx++] = (char) ch;
            }

            continue;
        }
        __enable_irq();

        if ((int32_t) (mvp_now_ms() - deadline) >= 0)
        {
            return MVP_ERR_TIMEOUT;
        }

        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
    }
}

int w800_wait_ok(uint32_t timeout_ms)
{
    char line[W800_LINE_BUF_SIZE];
    uint32_t deadline = mvp_now_ms() + timeout_ms;
    int rc;

    while ((int32_t) (mvp_now_ms() - deadline) < 0)
    {
        uint32_t remain = (uint32_t) (deadline - mvp_now_ms());
        rc = w800_read_response(line, sizeof(line), remain);
        if (MVP_OK != rc)
        {
            if (MVP_ERR_TIMEOUT == rc)
            {
                break;
            }
            return rc;
        }

        if (line_is_ok(line))
        {
            return MVP_OK;
        }

        if (line_is_err(line))
        {
            return MVP_ERR_AT;
        }
    }

    return MVP_ERR_TIMEOUT;
}

int w800_send_cmd_multi(const char * cmd,
                        const char * expect1,
                        const char * expect2,
                        uint32_t timeout_ms)
{
    char line[W800_LINE_BUF_SIZE];
    uint32_t deadline;
    int rc;

    if (NULL == cmd)
    {
        return MVP_ERR_ARG;
    }

    rc = w800_write_raw((const uint8_t *) cmd, strlen(cmd), timeout_ms);
    if (MVP_OK != rc)
    {
        return rc;
    }

    deadline = mvp_now_ms() + timeout_ms;

    while ((int32_t) (mvp_now_ms() - deadline) < 0)
    {
        uint32_t remain = (uint32_t) (deadline - mvp_now_ms());
        rc = w800_read_response(line, sizeof(line), remain);
        if (MVP_OK != rc)
        {
            if (MVP_ERR_TIMEOUT == rc)
            {
                break;
            }
            return rc;
        }

        if ((NULL != expect1) && line_has(line, expect1))
        {
            return MVP_OK;
        }

        if ((NULL != expect2) && line_has(line, expect2))
        {
            return MVP_OK;
        }

        if (line_is_err(line))
        {
            return MVP_ERR_AT;
        }

        if (((NULL == expect1) && (NULL == expect2)) && line_is_ok(line))
        {
            return MVP_OK;
        }
    }

    return MVP_ERR_TIMEOUT;
}

int w800_send_cmd(const char * cmd, const char * expect, uint32_t timeout_ms)
{
    return w800_send_cmd_multi(cmd, expect, NULL, timeout_ms);
}

int w800_wait_prompt_or_ok(char prompt, char * out, size_t out_size, uint32_t timeout_ms)
{
    size_t idx = 0U;
    uint8_t ch;
    uint32_t deadline;

    if ((NULL == out) || (out_size < 2U))
    {
        return MVP_ERR_ARG;
    }

    out[0] = '\0';
    deadline = mvp_now_ms() + timeout_ms;

    while ((int32_t) (mvp_now_ms() - deadline) < 0)
    {
        __disable_irq();
        if (uart_ringbuf_pop(&s_rx_rb, &ch))
        {
            __enable_irq();

            if (ch == (uint8_t) prompt)
            {
                out[0] = (char) prompt;
                out[1] = '\0';
                return MVP_OK;
            }

            if ('\r' == (char) ch)
            {
                continue;
            }

            if ('\n' == (char) ch)
            {
                if (idx == 0U)
                {
                    continue;
                }

                out[idx] = '\0';
                if (line_is_ok(out))
                {
                    return MVP_OK;
                }
                if (line_is_err(out))
                {
                    return MVP_ERR_AT;
                }

                idx = 0U;
                continue;
            }

            if (idx < (out_size - 1U))
            {
                out[idx++] = (char) ch;
            }

            continue;
        }
        __enable_irq();

        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
    }

    return MVP_ERR_TIMEOUT;
}

int w800_cmd_collect(const char * cmd, char * out, size_t out_size, uint32_t timeout_ms)
{
    char line[W800_LINE_BUF_SIZE];
    uint32_t deadline;
    size_t used = 0U;
    int rc;

    if ((NULL == cmd) || (NULL == out) || (out_size < 2U))
    {
        return MVP_ERR_ARG;
    }

    out[0] = '\0';

    rc = w800_write_raw((const uint8_t *) cmd, strlen(cmd), timeout_ms);
    if (MVP_OK != rc)
    {
        return rc;
    }

    deadline = mvp_now_ms() + timeout_ms;

    while ((int32_t) (mvp_now_ms() - deadline) < 0)
    {
        uint32_t remain = (uint32_t) (deadline - mvp_now_ms());
        rc = w800_read_response(line, sizeof(line), remain);
        if (MVP_OK != rc)
        {
            if (MVP_ERR_TIMEOUT == rc)
            {
                break;
            }
            return rc;
        }

        if (used + strlen(line) + 2U < out_size)
        {
            memcpy(out + used, line, strlen(line));
            used += strlen(line);
            out[used++] = '\n';
            out[used] = '\0';
        }

        if (line_is_err(line))
        {
            return MVP_ERR_AT;
        }

        if (line_is_ok(line))
        {
            return MVP_OK;
        }
    }

    return MVP_ERR_TIMEOUT;
}

int w800_wait_for_user_task(uint32_t timeout_ms)
{
    char line[W800_LINE_BUF_SIZE];
    uint32_t deadline = mvp_now_ms() + timeout_ms;
    bool saw_any = false;
    int rc;

    while ((int32_t) (mvp_now_ms() - deadline) < 0)
    {
        uint32_t remain = (uint32_t) (deadline - mvp_now_ms());
        rc = w800_read_response(line, sizeof(line), remain);
        if (MVP_OK != rc)
        {
            if (MVP_ERR_TIMEOUT == rc)
            {
                break;
            }
            return rc;
        }

        if (!saw_any)
        {
            LOGI("W800 first byte: %s", line);
            saw_any = true;
        }

        /* Accept any known W800 boot banner token */
        if (line_has(line, "user task") ||
            line_has(line, "Secboot")   ||
            line_has(line, "W800"))
        {
            return MVP_OK;
        }
    }

    if (!saw_any)
    {
        LOGW("W800: no data received during banner wait (%lu ms)", (unsigned long) timeout_ms);
    }

    return MVP_ERR_TIMEOUT;
}

void uart6_callback(uart_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        return;
    }

    if (UART_EVENT_RX_CHAR == p_args->event)
    {
        w800_uart_rx_byte((uint8_t) p_args->data);
        return;
    }

    if (UART_EVENT_TX_COMPLETE == p_args->event)
    {
        s_tx_busy = false;
        s_tx_ok = true;
        return;
    }

    if ((UART_EVENT_ERR_PARITY == p_args->event) ||
        (UART_EVENT_ERR_FRAMING == p_args->event) ||
        (UART_EVENT_ERR_OVERFLOW == p_args->event) ||
        (UART_EVENT_BREAK_DETECT == p_args->event))
    {
        s_tx_busy = false;
        s_tx_ok = false;
        LOGE("UART6 error event=%d", (int) p_args->event);
    }
}

void w800_hw_reset(void)
{
    /* Assert RST (active-low) for 20 ms, then release. Allow 150 ms for W800 to start. */
    w800_uart_drain_rx();
    s_tx_busy = false;
    s_tx_ok   = true;

    (void) g_ioport.p_api->pinWrite(g_ioport.p_ctrl, W800_RESET_PIN, W800_RESET_ACTIVE_LEVEL);
    R_BSP_SoftwareDelay(20U, BSP_DELAY_UNITS_MILLISECONDS);
    (void) g_ioport.p_api->pinWrite(g_ioport.p_ctrl, W800_RESET_PIN,
                                    (BSP_IO_LEVEL_LOW == W800_RESET_ACTIVE_LEVEL)
                                    ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW);
    R_BSP_SoftwareDelay(150U, BSP_DELAY_UNITS_MILLISECONDS);
    w800_uart_drain_rx();
}

void uart5_callback(uart_callback_args_t * p_args)
{
    /* MVP path does not use UART5 flash bridge. Keep symbol for FSP linkage. */
    (void) p_args;
}
