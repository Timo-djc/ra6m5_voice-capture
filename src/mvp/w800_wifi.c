#include "w800_wifi.h"

#include "config.h"
#include "log.h"
#include "w800_at.h"

#include <stdio.h>
#include <string.h>

#define W800_PROBE_TIMEOUT_MS      (700U)
#define W800_PROBE_SETTLE_MS       (40U)

static int run_simple_cmd(const char * cmd, uint32_t timeout_ms)
{
    uint32_t attempts;
    int rc = MVP_ERR_AT;

    for (attempts = 0U; attempts <= RETRY_COUNT; attempts++)
    {
        rc = w800_send_cmd(cmd, "+OK", timeout_ms);
        if (MVP_OK == rc)
        {
            return MVP_OK;
        }
    }

    return rc;
}

static int probe_basic_check_at_current_baud(void)
{
    int rc = w800_send_cmd("AT+\r\n", "+OK", W800_PROBE_TIMEOUT_MS);
    if (MVP_OK == rc)
    {
        return MVP_OK;
    }

    rc = w800_send_cmd("AT\r\n", "+OK", W800_PROBE_TIMEOUT_MS);
    if (MVP_OK == rc)
    {
        LOGW("AT+ probe unsupported/unstable, fallback to AT");
        return MVP_OK;
    }

    return rc;
}

int w800_basic_check(void)
{
    /* Common W800 AT baud candidates: project default first, then fallbacks. */
    static const uint32_t k_baud_probe_list[] = {
        W800_UART_BAUD,
        1500000U,
        921600U,
        460800U,
        230400U,
        115200U
    };

    size_t i;
    int rc = MVP_ERR_TIMEOUT;

    for (i = 0U; i < (sizeof(k_baud_probe_list) / sizeof(k_baud_probe_list[0])); i++)
    {
        size_t j;
        uint32_t baud = k_baud_probe_list[i];
        bool duplicate = false;

        for (j = 0U; j < i; j++)
        {
            if (k_baud_probe_list[j] == baud)
            {
                duplicate = true;
                break;
            }
        }

        if (duplicate)
        {
            continue;
        }

        rc = w800_set_baud(baud);
        if (MVP_OK != rc)
        {
            continue;
        }

        w800_uart_drain_rx();
        R_BSP_SoftwareDelay(W800_PROBE_SETTLE_MS, BSP_DELAY_UNITS_MILLISECONDS);

        rc = probe_basic_check_at_current_baud();
        if (MVP_OK == rc)
        {
            if (baud != W800_UART_BAUD)
            {
                LOGW("W800 AT matched at baud=%lu (configured=%lu)",
                     (unsigned long) baud,
                     (unsigned long) W800_UART_BAUD);
            }
            return MVP_OK;
        }
    }

    return rc;
}

int w800_set_echo(bool on)
{
    int rc;
    char cmd[24];

    (void) snprintf(cmd, sizeof(cmd), "AT+E=%u\r\n", on ? 1U : 0U);
    rc = w800_send_cmd(cmd, "+OK", W800_CMD_TIMEOUT_MS);
    if (MVP_OK == rc)
    {
        return MVP_OK;
    }

    /* TODO: Some firmware may only support toggle-style AT+E. */
    rc = w800_send_cmd("AT+E\r\n", "+OK", W800_CMD_TIMEOUT_MS);
    if (MVP_OK == rc)
    {
        LOGW("AT+E explicit mode unsupported, fallback to toggle");
        return MVP_OK;
    }

    return rc;
}

int w800_reset(void)
{
    return run_simple_cmd("AT+Z\r\n", W800_CMD_TIMEOUT_MS);
}

int w800_set_mode_sta(void)
{
    return run_simple_cmd("AT+WPRT=0\r\n", W800_CMD_TIMEOUT_MS);
}

int w800_query_mode(char * out, size_t out_size)
{
    return w800_cmd_collect("AT+WPRT=?\r\n", out, out_size, W800_CMD_TIMEOUT_MS);
}

int w800_set_ssid(const char * ssid)
{
    char cmd[160];

    if (NULL == ssid)
    {
        return MVP_ERR_ARG;
    }

    (void) snprintf(cmd, sizeof(cmd), "AT+SSID=\"%s\"\r\n", ssid);
    return run_simple_cmd(cmd, W800_CMD_TIMEOUT_MS);
}

int w800_query_ssid(char * out, size_t out_size)
{
    return w800_cmd_collect("AT+SSID=?\r\n", out, out_size, W800_CMD_TIMEOUT_MS);
}

int w800_set_key_ascii(const char * password)
{
    char cmd[192];

    if (NULL == password)
    {
        return MVP_ERR_ARG;
    }

    (void) snprintf(cmd, sizeof(cmd), "AT+KEY=1,0,\"%s\"\r\n", password);
    return run_simple_cmd(cmd, W800_CMD_TIMEOUT_MS);
}

int w800_query_key(char * out, size_t out_size)
{
    return w800_cmd_collect("AT+KEY=?\r\n", out, out_size, W800_CMD_TIMEOUT_MS);
}

int w800_set_dhcp(void)
{
    return run_simple_cmd("AT+NIP=0\r\n", W800_CMD_TIMEOUT_MS);
}

int w800_save_config(void)
{
    return run_simple_cmd("AT+PMTF\r\n", W800_CMD_TIMEOUT_MS);
}

int w800_join_ap(void)
{
    return run_simple_cmd("AT+WJOIN\r\n", W800_JOIN_TIMEOUT_MS);
}

int w800_leave_ap(void)
{
    return run_simple_cmd("AT+WLEAV\r\n", W800_CMD_TIMEOUT_MS);
}

int w800_get_link_status(char * out, size_t out_size)
{
    return w800_cmd_collect("AT+LKSTT\r\n", out, out_size, W800_CMD_TIMEOUT_MS);
}

int w800_scan_ap(char * out, size_t out_size)
{
    return w800_cmd_collect("AT+WSCAN\r\n", out, out_size, 8000U);
}
