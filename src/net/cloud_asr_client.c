#include "cloud_asr_client.h"

#include "../audio_capture.h"
#include "cloud_asr_cfg.h"
#include "w800_at.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if ASR_MODE_CLOUD

#define SLOT_MAX_SAMPLES    ((16000U * 15U) / 10U)
#define PCM_CHUNK_BYTES     (512U)
#define COMMAND_BUF_BYTES   (96U)
#define HEADER_BUF_BYTES    (96U)

typedef enum e_cloud_state
{
    CLOUD_STATE_PROBE_15M_SEND = 0,
    CLOUD_STATE_PROBE_15M_WAIT,
    CLOUD_STATE_PROBE_115200_SEND,
    CLOUD_STATE_PROBE_115200_WAIT,
    CLOUD_STATE_SET_15M_SEND,
    CLOUD_STATE_SET_15M_WAIT,
    CLOUD_STATE_VERIFY_15M_SEND,
    CLOUD_STATE_VERIFY_15M_WAIT,
    CLOUD_STATE_NET_Z_SEND,
    CLOUD_STATE_NET_Z_WAIT,
    CLOUD_STATE_NET_MODE_SEND,
    CLOUD_STATE_NET_MODE_WAIT,
    CLOUD_STATE_NET_DHCP_SEND,
    CLOUD_STATE_NET_DHCP_WAIT,
    CLOUD_STATE_NET_SSID_SEND,
    CLOUD_STATE_NET_SSID_WAIT,
    CLOUD_STATE_NET_KEY_SEND,
    CLOUD_STATE_NET_KEY_WAIT,
    CLOUD_STATE_NET_JOIN_SEND,
    CLOUD_STATE_NET_JOIN_WAIT,
    CLOUD_STATE_NET_SOCKET_SEND,
    CLOUD_STATE_NET_SOCKET_WAIT,
    CLOUD_STATE_NET_RPT_SEND,
    CLOUD_STATE_NET_RPT_WAIT,
    CLOUD_STATE_READY,
    CLOUD_STATE_SEND_LEN_SEND,
    CLOUD_STATE_SEND_LEN_WAIT,
    CLOUD_STATE_SEND_HEADER,
    CLOUD_STATE_SEND_PCM,
    CLOUD_STATE_WAIT_RESULT,
    CLOUD_STATE_RECOVER_WAIT
} cloud_state_t;

typedef enum e_cmd_status
{
    CMD_STATUS_IDLE = 0,
    CMD_STATUS_PENDING,
    CMD_STATUS_OK,
    CMD_STATUS_ERROR,
    CMD_STATUS_TIMEOUT
} cmd_status_t;

static cloud_state_t s_state = CLOUD_STATE_PROBE_15M_SEND;
static cmd_status_t s_cmd_status = CMD_STATUS_IDLE;

static uint32_t s_core_hz = 0U;
static uint32_t s_cmd_deadline_ms = 0U;
static uint32_t s_recover_deadline_ms = 0U;
static uint32_t s_result_deadline_ms = 0U;

static uint32_t s_session_id = 1U;

static int16_t s_slot_pcm[SLOT_MAX_SAMPLES];
static uint32_t s_slot_samples = 0U;
static uint8_t s_slot_index = 0U;
static bool s_slot_pending = false;

static uint32_t s_pcm_bytes_total = 0U;
static uint32_t s_pcm_bytes_sent = 0U;
static uint32_t s_header_len = 0U;
static char s_send_len_cmd[COMMAND_BUF_BYTES];
static char s_header[HEADER_BUF_BYTES];
static bool s_suspended = false;

extern void uart_write_line_ext(const char * str);

static void cloud_log(const char * fmt, ...)
{
    char buf[200];
    va_list args;

    va_start(args, fmt);
    (void) vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    uart_write_line_ext(buf);
}

static uint32_t cloud_now_ms(void)
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

static void cmd_start_timeout(uint32_t timeout_ms)
{
    s_cmd_status = CMD_STATUS_PENDING;
    s_cmd_deadline_ms = cloud_now_ms() + timeout_ms;
}

static void cmd_update_timeout(void)
{
    if ((CMD_STATUS_PENDING == s_cmd_status) && ((int32_t) (cloud_now_ms() - s_cmd_deadline_ms) >= 0))
    {
        s_cmd_status = CMD_STATUS_TIMEOUT;
    }
}

static cmd_status_t cmd_consume_status(void)
{
    cmd_status_t status = s_cmd_status;

    if (CMD_STATUS_PENDING != status)
    {
        s_cmd_status = CMD_STATUS_IDLE;
    }

    return status;
}

static bool send_cmd(const char * cmd, uint32_t timeout_ms)
{
    if (!w800_at_tx_idle())
    {
        return false;
    }

    if (!w800_at_send_str(cmd))
    {
        return false;
    }

    cmd_start_timeout(timeout_ms);
    return true;
}

static bool line_has(const char * line, const char * token)
{
    return (NULL != strstr(line, token));
}

static bool parse_result_line(const char * line)
{
    const char * p = strstr(line, "RESULT,");
    unsigned long session = 0UL;
    unsigned long slot = 0UL;
    int digit = -1;
    float conf = 0.0f;

    if (NULL == p)
    {
        return false;
    }

    if (5 == sscanf(p, "RESULT,%lu,%lu,%d,%f", &session, &slot, &digit, &conf))
    {
        (void) session;
        (void) slot;
        audio_capture_publish_digit_result(digit, conf);
        s_slot_pending = false;
        s_state = CLOUD_STATE_READY;
        return true;
    }

    return false;
}

static bool parse_error_line(const char * line)
{
    const char * p = strstr(line, "ERROR,");

    if (NULL == p)
    {
        return false;
    }

    audio_capture_publish_digit_result(-1, 0.0f);
    s_slot_pending = false;
    s_state = CLOUD_STATE_READY;
    return true;
}

static void handle_line(const char * line)
{
    if ((NULL == line) || ('\0' == line[0]))
    {
        return;
    }

    if (parse_result_line(line) || parse_error_line(line))
    {
        return;
    }

    if (line_has(line, "CLOSED") || line_has(line, "DISCONNECT"))
    {
        s_slot_pending = false;
        s_state = CLOUD_STATE_RECOVER_WAIT;
        s_recover_deadline_ms = cloud_now_ms() + CLOUD_POLL_RETRY_MS;
        return;
    }

    if (CMD_STATUS_PENDING == s_cmd_status)
    {
        if (line_has(line, "OK") || line_has(line, "SEND OK") || line_has(line, ">"))
        {
            s_cmd_status = CMD_STATUS_OK;
            return;
        }

        if (line_has(line, "ERROR") || line_has(line, "FAIL"))
        {
            s_cmd_status = CMD_STATUS_ERROR;
        }
    }
}

static void process_incoming_lines(void)
{
    char line[W800_LINE_BUFFER_SIZE];

    while (w800_at_read_line(line, sizeof(line)))
    {
        handle_line(line);
    }
}

static void cloud_enter_recover(void)
{
    s_slot_pending = false;
    s_cmd_status = CMD_STATUS_IDLE;
    s_state = CLOUD_STATE_RECOVER_WAIT;
    s_recover_deadline_ms = cloud_now_ms() + CLOUD_POLL_RETRY_MS;
}

static void start_slot_send(uint8_t slot_idx, uint32_t samples)
{
    uint32_t pcm_bytes = samples * sizeof(int16_t);

    s_slot_index = slot_idx;
    s_slot_samples = samples;
    s_slot_pending = true;

    s_header_len = (uint32_t) snprintf(s_header,
                                       sizeof(s_header),
                                       "ASR,1,%lu,%u,16000,16,1,%lu,%lu\n",
                                       (unsigned long) s_session_id,
                                       (unsigned int) s_slot_index,
                                       (unsigned long) s_slot_samples,
                                       (unsigned long) pcm_bytes);

    if ((0U == s_header_len) || (s_header_len >= sizeof(s_header)))
    {
        audio_capture_publish_digit_result(-1, 0.0f);
        s_slot_pending = false;
        s_state = CLOUD_STATE_READY;
        return;
    }

    s_pcm_bytes_total = pcm_bytes;
    s_pcm_bytes_sent = 0U;

    (void) snprintf(s_send_len_cmd,
                    sizeof(s_send_len_cmd),
                    "AT+SKSND=%u,%lu\r\n",
                    (unsigned int) W800_SOCKET_ID,
                    (unsigned long) (pcm_bytes + s_header_len));

    s_state = CLOUD_STATE_SEND_LEN_SEND;
}

void cloud_asr_client_init(void)
{
    s_core_hz = 0U;
    s_cmd_status = CMD_STATUS_IDLE;
    s_session_id = 1U;
    s_slot_pending = false;
    s_suspended = false;

    if (!w800_at_init())
    {
        cloud_log("CLOUD: uart6 init failed\r\n");
        cloud_enter_recover();
        return;
    }

    w800_at_hard_reset();
    w800_at_reset_rx();

    s_state = CLOUD_STATE_PROBE_15M_SEND;
    cloud_log("CLOUD: init, target baud=%lu\r\n", (unsigned long) W800_UART_TARGET_BAUD);
}

void cloud_asr_client_on_pcm_slot(uint8_t slot_idx, const int16_t * pcm, uint32_t samples)
{
    if ((NULL == pcm) || (0U == samples) || (samples > SLOT_MAX_SAMPLES) || s_slot_pending)
    {
        return;
    }

    memcpy(s_slot_pcm, pcm, samples * sizeof(int16_t));
    start_slot_send(slot_idx, samples);
}

void cloud_asr_client_poll(void)
{
    cmd_status_t cmd_status;

    if (s_suspended)
    {
        return;
    }

    w800_at_poll();
    process_incoming_lines();
    cmd_update_timeout();

    if ((CLOUD_STATE_WAIT_RESULT == s_state) && ((int32_t) (cloud_now_ms() - s_result_deadline_ms) >= 0))
    {
        audio_capture_publish_digit_result(-1, 0.0f);
        s_slot_pending = false;
        s_state = CLOUD_STATE_READY;
    }

    switch (s_state)
    {
        case CLOUD_STATE_PROBE_15M_SEND:
            if (w800_at_set_baud(W800_UART_TARGET_BAUD) && send_cmd("AT\r\n", 300U))
            {
                s_state = CLOUD_STATE_PROBE_15M_WAIT;
            }
            break;

        case CLOUD_STATE_PROBE_15M_WAIT:
            cmd_status = cmd_consume_status();
            if (CMD_STATUS_OK == cmd_status)
            {
                s_state = CLOUD_STATE_NET_Z_SEND;
            }
            else if ((CMD_STATUS_ERROR == cmd_status) || (CMD_STATUS_TIMEOUT == cmd_status))
            {
                s_state = CLOUD_STATE_PROBE_115200_SEND;
            }
            break;

        case CLOUD_STATE_PROBE_115200_SEND:
            if (w800_at_set_baud(W800_UART_FALLBACK_BAUD) && send_cmd("AT\r\n", 400U))
            {
                s_state = CLOUD_STATE_PROBE_115200_WAIT;
            }
            break;

        case CLOUD_STATE_PROBE_115200_WAIT:
            cmd_status = cmd_consume_status();
            if (CMD_STATUS_OK == cmd_status)
            {
                s_state = CLOUD_STATE_SET_15M_SEND;
            }
            else if ((CMD_STATUS_ERROR == cmd_status) || (CMD_STATUS_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_STATE_SET_15M_SEND:
            (void) snprintf(s_send_len_cmd,
                            sizeof(s_send_len_cmd),
                            "AT+UART=%lu,0,0,0,0\r\n",
                            (unsigned long) W800_UART_TARGET_BAUD);
            if (send_cmd(s_send_len_cmd, 800U))
            {
                s_state = CLOUD_STATE_SET_15M_WAIT;
            }
            break;

        case CLOUD_STATE_SET_15M_WAIT:
            cmd_status = cmd_consume_status();
            if (CMD_STATUS_OK == cmd_status)
            {
                s_state = CLOUD_STATE_VERIFY_15M_SEND;
            }
            else if ((CMD_STATUS_ERROR == cmd_status) || (CMD_STATUS_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_STATE_VERIFY_15M_SEND:
            if (w800_at_set_baud(W800_UART_TARGET_BAUD) && send_cmd("AT\r\n", 400U))
            {
                s_state = CLOUD_STATE_VERIFY_15M_WAIT;
            }
            break;

        case CLOUD_STATE_VERIFY_15M_WAIT:
            cmd_status = cmd_consume_status();
            if (CMD_STATUS_OK == cmd_status)
            {
                s_state = CLOUD_STATE_NET_Z_SEND;
            }
            else if ((CMD_STATUS_ERROR == cmd_status) || (CMD_STATUS_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_STATE_NET_Z_SEND:
            if (send_cmd("AT+Z\r\n", W800_AT_TIMEOUT_MS))
            {
                s_state = CLOUD_STATE_NET_Z_WAIT;
            }
            break;

        case CLOUD_STATE_NET_Z_WAIT:
            cmd_status = cmd_consume_status();
            if (CMD_STATUS_OK == cmd_status)
            {
                s_state = CLOUD_STATE_NET_MODE_SEND;
            }
            else if ((CMD_STATUS_ERROR == cmd_status) || (CMD_STATUS_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_STATE_NET_MODE_SEND:
            if (send_cmd("AT+WPRT=0\r\n", W800_AT_TIMEOUT_MS))
            {
                s_state = CLOUD_STATE_NET_MODE_WAIT;
            }
            break;

        case CLOUD_STATE_NET_MODE_WAIT:
            cmd_status = cmd_consume_status();
            if (CMD_STATUS_OK == cmd_status)
            {
                s_state = CLOUD_STATE_NET_DHCP_SEND;
            }
            else if ((CMD_STATUS_ERROR == cmd_status) || (CMD_STATUS_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_STATE_NET_DHCP_SEND:
            if (send_cmd("AT+NIP=0\r\n", W800_AT_TIMEOUT_MS))
            {
                s_state = CLOUD_STATE_NET_DHCP_WAIT;
            }
            break;

        case CLOUD_STATE_NET_DHCP_WAIT:
            cmd_status = cmd_consume_status();
            if (CMD_STATUS_OK == cmd_status)
            {
                s_state = CLOUD_STATE_NET_SSID_SEND;
            }
            else if ((CMD_STATUS_ERROR == cmd_status) || (CMD_STATUS_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_STATE_NET_SSID_SEND:
            (void) snprintf(s_send_len_cmd, sizeof(s_send_len_cmd), "AT+SSID=%s\r\n", CLOUD_WIFI_SSID);
            if (send_cmd(s_send_len_cmd, W800_AT_TIMEOUT_MS))
            {
                s_state = CLOUD_STATE_NET_SSID_WAIT;
            }
            break;

        case CLOUD_STATE_NET_SSID_WAIT:
            cmd_status = cmd_consume_status();
            if (CMD_STATUS_OK == cmd_status)
            {
                s_state = CLOUD_STATE_NET_KEY_SEND;
            }
            else if ((CMD_STATUS_ERROR == cmd_status) || (CMD_STATUS_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_STATE_NET_KEY_SEND:
            (void) snprintf(s_send_len_cmd, sizeof(s_send_len_cmd), "AT+KEY=1,0,%s\r\n", CLOUD_WIFI_PASSWORD);
            if (send_cmd(s_send_len_cmd, W800_AT_TIMEOUT_MS))
            {
                s_state = CLOUD_STATE_NET_KEY_WAIT;
            }
            break;

        case CLOUD_STATE_NET_KEY_WAIT:
            cmd_status = cmd_consume_status();
            if (CMD_STATUS_OK == cmd_status)
            {
                s_state = CLOUD_STATE_NET_JOIN_SEND;
            }
            else if ((CMD_STATUS_ERROR == cmd_status) || (CMD_STATUS_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_STATE_NET_JOIN_SEND:
            if (send_cmd("AT+WJOIN\r\n", 8000U))
            {
                s_state = CLOUD_STATE_NET_JOIN_WAIT;
            }
            break;

        case CLOUD_STATE_NET_JOIN_WAIT:
            cmd_status = cmd_consume_status();
            if (CMD_STATUS_OK == cmd_status)
            {
                s_state = CLOUD_STATE_NET_SOCKET_SEND;
            }
            else if ((CMD_STATUS_ERROR == cmd_status) || (CMD_STATUS_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_STATE_NET_SOCKET_SEND:
            (void) snprintf(s_send_len_cmd,
                            sizeof(s_send_len_cmd),
                            "AT+SKCT=%u,0,%s,%u,%u\r\n",
                            (unsigned int) W800_SOCKET_ID,
                            CLOUD_SERVER_IP,
                            (unsigned int) CLOUD_SERVER_PORT,
                            (unsigned int) CLOUD_SERVER_LOCAL_PORT);
            if (send_cmd(s_send_len_cmd, 4000U))
            {
                s_state = CLOUD_STATE_NET_SOCKET_WAIT;
            }
            break;

        case CLOUD_STATE_NET_SOCKET_WAIT:
            cmd_status = cmd_consume_status();
            if (CMD_STATUS_OK == cmd_status)
            {
                s_state = CLOUD_STATE_NET_RPT_SEND;
            }
            else if ((CMD_STATUS_ERROR == cmd_status) || (CMD_STATUS_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_STATE_NET_RPT_SEND:
            if (send_cmd("AT+SKRPTM=1\r\n", W800_AT_TIMEOUT_MS))
            {
                s_state = CLOUD_STATE_NET_RPT_WAIT;
            }
            break;

        case CLOUD_STATE_NET_RPT_WAIT:
            cmd_status = cmd_consume_status();
            if (CMD_STATUS_OK == cmd_status)
            {
                s_state = CLOUD_STATE_READY;
                cloud_log("CLOUD: link ready\r\n");
            }
            else if ((CMD_STATUS_ERROR == cmd_status) || (CMD_STATUS_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_STATE_READY:
            if (!s_slot_pending)
            {
                uint32_t samples = 0U;
                uint8_t slot_idx = 0U;

                if (audio_capture_consume_slot_pcm16(s_slot_pcm,
                                                     SLOT_MAX_SAMPLES,
                                                     &samples,
                                                     &slot_idx))
                {
                    start_slot_send(slot_idx, samples);
                }
            }
            break;

        case CLOUD_STATE_SEND_LEN_SEND:
            if (send_cmd(s_send_len_cmd, W800_AT_TIMEOUT_MS))
            {
                s_state = CLOUD_STATE_SEND_LEN_WAIT;
            }
            break;

        case CLOUD_STATE_SEND_LEN_WAIT:
            cmd_status = cmd_consume_status();
            if (CMD_STATUS_OK == cmd_status)
            {
                s_state = CLOUD_STATE_SEND_HEADER;
            }
            else if ((CMD_STATUS_ERROR == cmd_status) || (CMD_STATUS_TIMEOUT == cmd_status))
            {
                audio_capture_publish_digit_result(-1, 0.0f);
                cloud_enter_recover();
            }
            break;

        case CLOUD_STATE_SEND_HEADER:
            if (w800_at_tx_idle() && w800_at_send((const uint8_t *) s_header, s_header_len))
            {
                s_state = CLOUD_STATE_SEND_PCM;
            }
            break;

        case CLOUD_STATE_SEND_PCM:
            if (w800_at_tx_idle())
            {
                uint32_t remain = s_pcm_bytes_total - s_pcm_bytes_sent;
                uint32_t chunk = (remain > PCM_CHUNK_BYTES) ? PCM_CHUNK_BYTES : remain;

                if (chunk > 0U)
                {
                    const uint8_t * src = ((const uint8_t *) s_slot_pcm) + s_pcm_bytes_sent;
                    if (w800_at_send(src, chunk))
                    {
                        s_pcm_bytes_sent += chunk;
                    }
                }

                if (s_pcm_bytes_sent >= s_pcm_bytes_total)
                {
                    s_result_deadline_ms = cloud_now_ms() + CLOUD_RESULT_TIMEOUT_MS;
                    s_state = CLOUD_STATE_WAIT_RESULT;
                    s_session_id++;
                }
            }
            break;

        case CLOUD_STATE_WAIT_RESULT:
            if (!s_slot_pending)
            {
                s_state = CLOUD_STATE_READY;
            }
            break;

        case CLOUD_STATE_RECOVER_WAIT:
            if ((int32_t) (cloud_now_ms() - s_recover_deadline_ms) >= 0)
            {
                w800_at_reset_rx();
                s_state = CLOUD_STATE_PROBE_15M_SEND;
            }
            break;

        default:
            cloud_enter_recover();
            break;
    }
}

void cloud_asr_client_suspend(void)
{
    s_suspended = true;
    s_slot_pending = false;
    s_cmd_status = CMD_STATUS_IDLE;
    w800_at_reset_rx();
}

void cloud_asr_client_restart(void)
{
    cloud_asr_client_init();
}

bool cloud_asr_client_is_suspended(void)
{
    return s_suspended;
}

#else

void cloud_asr_client_init(void)
{
}

void cloud_asr_client_poll(void)
{
}

void cloud_asr_client_on_pcm_slot(uint8_t slot_idx, const int16_t * pcm, uint32_t samples)
{
    (void) slot_idx;
    (void) pcm;
    (void) samples;
}

void cloud_asr_client_suspend(void)
{
}

void cloud_asr_client_restart(void)
{
    cloud_asr_client_init();
}

bool cloud_asr_client_is_suspended(void)
{
    return false;
}

#endif
