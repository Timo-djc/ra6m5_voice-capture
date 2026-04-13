#include "cloud_speaker_client.h"

#include "../audio_capture.h"
#include "cloud_speaker_cfg.h"
#include "w800_at.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if ASR_MODE_CLOUD

#define SLOT_MAX_SAMPLES          ((16000U * 15U) / 10U)
#define PCM_CHUNK_BYTES           (512U)
#define COMMAND_BUF_BYTES         (128U)
#define HEADER_BUF_BYTES          (160U)

typedef enum e_cloud_speaker_state
{
    CLOUD_SPK_STATE_PROBE_15M_SEND = 0,
    CLOUD_SPK_STATE_PROBE_15M_WAIT,
    CLOUD_SPK_STATE_PROBE_115200_SEND,
    CLOUD_SPK_STATE_PROBE_115200_WAIT,
    CLOUD_SPK_STATE_SET_15M_SEND,
    CLOUD_SPK_STATE_SET_15M_WAIT,
    CLOUD_SPK_STATE_VERIFY_15M_SEND,
    CLOUD_SPK_STATE_VERIFY_15M_WAIT,
    CLOUD_SPK_STATE_NET_Z_SEND,
    CLOUD_SPK_STATE_NET_Z_WAIT,
    CLOUD_SPK_STATE_NET_MODE_SEND,
    CLOUD_SPK_STATE_NET_MODE_WAIT,
    CLOUD_SPK_STATE_NET_DHCP_SEND,
    CLOUD_SPK_STATE_NET_DHCP_WAIT,
    CLOUD_SPK_STATE_NET_SSID_SEND,
    CLOUD_SPK_STATE_NET_SSID_WAIT,
    CLOUD_SPK_STATE_NET_KEY_SEND,
    CLOUD_SPK_STATE_NET_KEY_WAIT,
    CLOUD_SPK_STATE_NET_JOIN_SEND,
    CLOUD_SPK_STATE_NET_JOIN_WAIT,
    CLOUD_SPK_STATE_NET_SOCKET_SEND,
    CLOUD_SPK_STATE_NET_SOCKET_WAIT,
    CLOUD_SPK_STATE_NET_RPT_SEND,
    CLOUD_SPK_STATE_NET_RPT_WAIT,
    CLOUD_SPK_STATE_READY,
    CLOUD_SPK_STATE_SEND_LEN_SEND,
    CLOUD_SPK_STATE_SEND_LEN_WAIT,
    CLOUD_SPK_STATE_SEND_HEADER,
    CLOUD_SPK_STATE_SEND_PCM,
    CLOUD_SPK_STATE_WAIT_RESULT,
    CLOUD_SPK_STATE_RECOVER_WAIT
} cloud_speaker_state_t;

typedef enum e_cmd_status
{
    SPK_CMD_IDLE = 0,
    SPK_CMD_PENDING,
    SPK_CMD_OK,
    SPK_CMD_ERROR,
    SPK_CMD_TIMEOUT
} spk_cmd_status_t;

typedef enum e_speaker_operation
{
    SPEAKER_OP_NONE = 0,
    SPEAKER_OP_IDENTIFY,
    SPEAKER_OP_ENROLL
} speaker_operation_t;

static cloud_speaker_state_t s_state = CLOUD_SPK_STATE_PROBE_15M_SEND;
static spk_cmd_status_t s_cmd_status = SPK_CMD_IDLE;
static speaker_operation_t s_pending_op = SPEAKER_OP_NONE;
static speaker_operation_t s_active_op = SPEAKER_OP_NONE;

static uint32_t s_core_hz = 0U;
static uint32_t s_cmd_deadline_ms = 0U;
static uint32_t s_recover_deadline_ms = 0U;
static uint32_t s_result_deadline_ms = 0U;
static uint32_t s_session_id = 1U;

static int16_t s_slot_pcm[SLOT_MAX_SAMPLES];
static uint32_t s_slot_samples = 0U;
static uint8_t s_slot_index = 0U;
static bool s_slot_pending = false;
static bool s_suspended = false;

static uint32_t s_pcm_bytes_total = 0U;
static uint32_t s_pcm_bytes_sent = 0U;
static uint32_t s_header_len = 0U;
static char s_send_len_cmd[COMMAND_BUF_BYTES];
static char s_header[HEADER_BUF_BYTES];
static char s_target_speaker_id[AUDIO_SPEAKER_ID_MAX_LEN + 1U];
static uint8_t s_target_utter_idx = 0U;
static uint8_t s_target_utter_total = 0U;

extern void uart_write_line_ext(const char * str);

static void speaker_log(const char * fmt, ...)
{
    char buf[200];
    va_list args;

    va_start(args, fmt);
    (void) vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    uart_write_line_ext(buf);
}

static uint32_t speaker_now_ms(void)
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
    s_cmd_status = SPK_CMD_PENDING;
    s_cmd_deadline_ms = speaker_now_ms() + timeout_ms;
}

static void cmd_update_timeout(void)
{
    if ((SPK_CMD_PENDING == s_cmd_status) && ((int32_t) (speaker_now_ms() - s_cmd_deadline_ms) >= 0))
    {
        s_cmd_status = SPK_CMD_TIMEOUT;
    }
}

static spk_cmd_status_t cmd_consume_status(void)
{
    spk_cmd_status_t status = s_cmd_status;

    if (SPK_CMD_PENDING != status)
    {
        s_cmd_status = SPK_CMD_IDLE;
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

static void cloud_enter_recover(void)
{
    s_slot_pending = false;
    s_active_op = SPEAKER_OP_NONE;
    s_cmd_status = SPK_CMD_IDLE;
    s_state = CLOUD_SPK_STATE_RECOVER_WAIT;
    s_recover_deadline_ms = speaker_now_ms() + CLOUD_POLL_RETRY_MS;
}

static bool parse_identify_line(const char * line)
{
    const char * p = strstr(line, "IDENTIFY,");
    unsigned long session = 0UL;
    unsigned long slot = 0UL;
    char status[16];
    char speaker_id[AUDIO_SPEAKER_ID_MAX_LEN + 1U];
    float score = 0.0f;

    if (NULL == p)
    {
        return false;
    }

    memset(status, 0, sizeof(status));
    memset(speaker_id, 0, sizeof(speaker_id));
    if (5 == sscanf(p, "IDENTIFY,%lu,%lu,%15[^,],%32[^,],%f", &session, &slot, status, speaker_id, &score))
    {
        (void) session;
        (void) slot;
        audio_capture_publish_speaker_result((uint8_t) (0 == strcmp(status, "KNOWN")), speaker_id, score);
        s_slot_pending = false;
        s_active_op = SPEAKER_OP_NONE;
        s_state = CLOUD_SPK_STATE_READY;
        return true;
    }

    return false;
}

static bool parse_enroll_line(const char * line)
{
    const char * p = strstr(line, "ENROLL,");
    unsigned long session = 0UL;
    unsigned long slot = 0UL;
    char speaker_id[AUDIO_SPEAKER_ID_MAX_LEN + 1U];
    unsigned int accepted = 0U;
    unsigned int required = 0U;
    float score = 0.0f;

    if (NULL == p)
    {
        return false;
    }

    memset(speaker_id, 0, sizeof(speaker_id));
    if (6 == sscanf(p, "ENROLL,%lu,%lu,%32[^,],%u,%u,%f", &session, &slot, speaker_id, &accepted, &required, &score))
    {
        (void) session;
        (void) slot;
        audio_capture_publish_enroll_progress(speaker_id, (uint8_t) accepted, (uint8_t) required, score);
        s_slot_pending = false;
        s_active_op = SPEAKER_OP_NONE;
        s_state = CLOUD_SPK_STATE_READY;
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

    if (SPEAKER_OP_IDENTIFY == s_active_op)
    {
        audio_capture_publish_speaker_result(0U, "unknown", 0.0f);
    }
    else if (SPEAKER_OP_ENROLL == s_active_op)
    {
        audio_capture_publish_enroll_progress(s_target_speaker_id, 0U, s_target_utter_total, 0.0f);
    }

    s_slot_pending = false;
    s_active_op = SPEAKER_OP_NONE;
    s_state = CLOUD_SPK_STATE_READY;
    return true;
}

static void handle_line(const char * line)
{
    if ((NULL == line) || ('\0' == line[0]))
    {
        return;
    }

    if (parse_identify_line(line) || parse_enroll_line(line) || parse_error_line(line))
    {
        return;
    }

    if (line_has(line, "CLOSED") || line_has(line, "DISCONNECT"))
    {
        cloud_enter_recover();
        return;
    }

    if (SPK_CMD_PENDING == s_cmd_status)
    {
        if (line_has(line, "+OK") || line_has(line, "OK"))
        {
            s_cmd_status = SPK_CMD_OK;
        }
        else if (line_has(line, "+ERR") || line_has(line, "ERR"))
        {
            s_cmd_status = SPK_CMD_ERROR;
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

static void start_slot_send(uint8_t slot_idx, uint32_t samples)
{
    uint32_t pcm_bytes = samples * sizeof(int16_t);

    s_slot_index = slot_idx;
    s_slot_samples = samples;
    s_slot_pending = true;
    s_active_op = s_pending_op;

    if (SPEAKER_OP_IDENTIFY == s_pending_op)
    {
        s_header_len = (uint32_t) snprintf(s_header,
                                           sizeof(s_header),
                                           "SVI,1,%lu,%u,16000,16,1,%lu,%lu\n",
                                           (unsigned long) s_session_id,
                                           (unsigned int) s_slot_index,
                                           (unsigned long) s_slot_samples,
                                           (unsigned long) pcm_bytes);
    }
    else
    {
        s_header_len = (uint32_t) snprintf(s_header,
                                           sizeof(s_header),
                                           "SVR,1,%lu,%u,%s,%u,%u,16000,16,1,%lu,%lu\n",
                                           (unsigned long) s_session_id,
                                           (unsigned int) s_slot_index,
                                           s_target_speaker_id,
                                           (unsigned int) s_target_utter_idx,
                                           (unsigned int) s_target_utter_total,
                                           (unsigned long) s_slot_samples,
                                           (unsigned long) pcm_bytes);
    }

    if ((0U == s_header_len) || (s_header_len >= sizeof(s_header)))
    {
        s_slot_pending = false;
        s_active_op = SPEAKER_OP_NONE;
        s_state = CLOUD_SPK_STATE_READY;
        return;
    }

    s_pcm_bytes_total = pcm_bytes;
    s_pcm_bytes_sent = 0U;
    (void) snprintf(s_send_len_cmd,
                    sizeof(s_send_len_cmd),
                    "AT+SKSND=%u,%lu\r\n",
                    (unsigned int) W800_SOCKET_ID,
                    (unsigned long) (pcm_bytes + s_header_len));

    s_state = CLOUD_SPK_STATE_SEND_LEN_SEND;
}

bool cloud_speaker_client_start_identify(void)
{
    if ((s_pending_op != SPEAKER_OP_NONE) || s_slot_pending)
    {
        return false;
    }

    memset(s_target_speaker_id, 0, sizeof(s_target_speaker_id));
    s_target_utter_idx = 0U;
    s_target_utter_total = 0U;
    s_pending_op = SPEAKER_OP_IDENTIFY;
    return true;
}

bool cloud_speaker_client_start_enroll(const char * speaker_id, uint8_t utter_idx, uint8_t utter_total)
{
    if ((NULL == speaker_id) || ('\0' == speaker_id[0]) || (s_pending_op != SPEAKER_OP_NONE) || s_slot_pending)
    {
        return false;
    }

    memset(s_target_speaker_id, 0, sizeof(s_target_speaker_id));
    strncpy(s_target_speaker_id, speaker_id, sizeof(s_target_speaker_id) - 1U);
    s_target_utter_idx = utter_idx;
    s_target_utter_total = utter_total;
    s_pending_op = SPEAKER_OP_ENROLL;
    return true;
}

bool cloud_speaker_client_is_busy(void)
{
    return (s_pending_op != SPEAKER_OP_NONE) || s_slot_pending || (s_active_op != SPEAKER_OP_NONE);
}

void cloud_speaker_client_init(void)
{
    s_core_hz = 0U;
    s_cmd_status = SPK_CMD_IDLE;
    s_pending_op = SPEAKER_OP_NONE;
    s_active_op = SPEAKER_OP_NONE;
    s_session_id = 1U;
    s_slot_pending = false;
    s_suspended = false;
    memset(s_target_speaker_id, 0, sizeof(s_target_speaker_id));

    if (!w800_at_init())
    {
        speaker_log("SPK: uart6 init failed\r\n");
        cloud_enter_recover();
        return;
    }

    w800_at_hard_reset();
    w800_at_reset_rx();
    s_state = CLOUD_SPK_STATE_PROBE_15M_SEND;
    speaker_log("SPK: init, target baud=%lu\r\n", (unsigned long) W800_UART_TARGET_BAUD);
}

void cloud_speaker_client_poll(void)
{
    spk_cmd_status_t cmd_status;

    if (s_suspended)
    {
        return;
    }

    w800_at_poll();
    process_incoming_lines();
    cmd_update_timeout();

    if ((CLOUD_SPK_STATE_WAIT_RESULT == s_state) && ((int32_t) (speaker_now_ms() - s_result_deadline_ms) >= 0))
    {
        parse_error_line("ERROR,0,0,TIMEOUT");
    }

    switch (s_state)
    {
        case CLOUD_SPK_STATE_PROBE_15M_SEND:
            if (w800_at_set_baud(W800_UART_TARGET_BAUD) && send_cmd("AT\r\n", 300U))
            {
                s_state = CLOUD_SPK_STATE_PROBE_15M_WAIT;
            }
            break;

        case CLOUD_SPK_STATE_PROBE_15M_WAIT:
            cmd_status = cmd_consume_status();
            if (SPK_CMD_OK == cmd_status)
            {
                s_state = CLOUD_SPK_STATE_NET_Z_SEND;
            }
            else if ((SPK_CMD_ERROR == cmd_status) || (SPK_CMD_TIMEOUT == cmd_status))
            {
                s_state = CLOUD_SPK_STATE_PROBE_115200_SEND;
            }
            break;

        case CLOUD_SPK_STATE_PROBE_115200_SEND:
            if (w800_at_set_baud(W800_UART_FALLBACK_BAUD) && send_cmd("AT\r\n", 400U))
            {
                s_state = CLOUD_SPK_STATE_PROBE_115200_WAIT;
            }
            break;

        case CLOUD_SPK_STATE_PROBE_115200_WAIT:
            cmd_status = cmd_consume_status();
            if (SPK_CMD_OK == cmd_status)
            {
                s_state = CLOUD_SPK_STATE_SET_15M_SEND;
            }
            else if ((SPK_CMD_ERROR == cmd_status) || (SPK_CMD_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_SPK_STATE_SET_15M_SEND:
            (void) snprintf(s_send_len_cmd,
                            sizeof(s_send_len_cmd),
                            "AT+UART=%lu,0,0,0,0\r\n",
                            (unsigned long) W800_UART_TARGET_BAUD);
            if (send_cmd(s_send_len_cmd, 800U))
            {
                s_state = CLOUD_SPK_STATE_SET_15M_WAIT;
            }
            break;

        case CLOUD_SPK_STATE_SET_15M_WAIT:
            cmd_status = cmd_consume_status();
            if (SPK_CMD_OK == cmd_status)
            {
                s_state = CLOUD_SPK_STATE_VERIFY_15M_SEND;
            }
            else if ((SPK_CMD_ERROR == cmd_status) || (SPK_CMD_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_SPK_STATE_VERIFY_15M_SEND:
            if (w800_at_set_baud(W800_UART_TARGET_BAUD) && send_cmd("AT\r\n", 400U))
            {
                s_state = CLOUD_SPK_STATE_VERIFY_15M_WAIT;
            }
            break;

        case CLOUD_SPK_STATE_VERIFY_15M_WAIT:
            cmd_status = cmd_consume_status();
            if (SPK_CMD_OK == cmd_status)
            {
                s_state = CLOUD_SPK_STATE_NET_Z_SEND;
            }
            else if ((SPK_CMD_ERROR == cmd_status) || (SPK_CMD_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_SPK_STATE_NET_Z_SEND:
            if (send_cmd("AT+Z\r\n", W800_AT_TIMEOUT_MS))
            {
                s_state = CLOUD_SPK_STATE_NET_Z_WAIT;
            }
            break;

        case CLOUD_SPK_STATE_NET_Z_WAIT:
            cmd_status = cmd_consume_status();
            if (SPK_CMD_OK == cmd_status)
            {
                s_state = CLOUD_SPK_STATE_NET_MODE_SEND;
            }
            else if ((SPK_CMD_ERROR == cmd_status) || (SPK_CMD_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_SPK_STATE_NET_MODE_SEND:
            if (send_cmd("AT+WPRT=0\r\n", W800_AT_TIMEOUT_MS))
            {
                s_state = CLOUD_SPK_STATE_NET_MODE_WAIT;
            }
            break;

        case CLOUD_SPK_STATE_NET_MODE_WAIT:
            cmd_status = cmd_consume_status();
            if (SPK_CMD_OK == cmd_status)
            {
                s_state = CLOUD_SPK_STATE_NET_DHCP_SEND;
            }
            else if ((SPK_CMD_ERROR == cmd_status) || (SPK_CMD_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_SPK_STATE_NET_DHCP_SEND:
            if (send_cmd("AT+NIP=0\r\n", W800_AT_TIMEOUT_MS))
            {
                s_state = CLOUD_SPK_STATE_NET_DHCP_WAIT;
            }
            break;

        case CLOUD_SPK_STATE_NET_DHCP_WAIT:
            cmd_status = cmd_consume_status();
            if (SPK_CMD_OK == cmd_status)
            {
                s_state = CLOUD_SPK_STATE_NET_SSID_SEND;
            }
            else if ((SPK_CMD_ERROR == cmd_status) || (SPK_CMD_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_SPK_STATE_NET_SSID_SEND:
            (void) snprintf(s_send_len_cmd, sizeof(s_send_len_cmd), "AT+SSID=%s\r\n", CLOUD_WIFI_SSID);
            if (send_cmd(s_send_len_cmd, W800_AT_TIMEOUT_MS))
            {
                s_state = CLOUD_SPK_STATE_NET_SSID_WAIT;
            }
            break;

        case CLOUD_SPK_STATE_NET_SSID_WAIT:
            cmd_status = cmd_consume_status();
            if (SPK_CMD_OK == cmd_status)
            {
                s_state = CLOUD_SPK_STATE_NET_KEY_SEND;
            }
            else if ((SPK_CMD_ERROR == cmd_status) || (SPK_CMD_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_SPK_STATE_NET_KEY_SEND:
            (void) snprintf(s_send_len_cmd, sizeof(s_send_len_cmd), "AT+KEY=1,0,%s\r\n", CLOUD_WIFI_PASSWORD);
            if (send_cmd(s_send_len_cmd, W800_AT_TIMEOUT_MS))
            {
                s_state = CLOUD_SPK_STATE_NET_KEY_WAIT;
            }
            break;

        case CLOUD_SPK_STATE_NET_KEY_WAIT:
            cmd_status = cmd_consume_status();
            if (SPK_CMD_OK == cmd_status)
            {
                s_state = CLOUD_SPK_STATE_NET_JOIN_SEND;
            }
            else if ((SPK_CMD_ERROR == cmd_status) || (SPK_CMD_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_SPK_STATE_NET_JOIN_SEND:
            if (send_cmd("AT+WJOIN\r\n", 8000U))
            {
                s_state = CLOUD_SPK_STATE_NET_JOIN_WAIT;
            }
            break;

        case CLOUD_SPK_STATE_NET_JOIN_WAIT:
            cmd_status = cmd_consume_status();
            if (SPK_CMD_OK == cmd_status)
            {
                s_state = CLOUD_SPK_STATE_NET_SOCKET_SEND;
            }
            else if ((SPK_CMD_ERROR == cmd_status) || (SPK_CMD_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_SPK_STATE_NET_SOCKET_SEND:
            (void) snprintf(s_send_len_cmd,
                            sizeof(s_send_len_cmd),
                            "AT+SKCT=%u,0,%s,%u,%u\r\n",
                            (unsigned int) W800_SOCKET_ID,
                            CLOUD_SPEAKER_SERVER_IP,
                            (unsigned int) CLOUD_SPEAKER_SERVER_PORT,
                            (unsigned int) CLOUD_SPEAKER_LOCAL_PORT);
            if (send_cmd(s_send_len_cmd, 4000U))
            {
                s_state = CLOUD_SPK_STATE_NET_SOCKET_WAIT;
            }
            break;

        case CLOUD_SPK_STATE_NET_SOCKET_WAIT:
            cmd_status = cmd_consume_status();
            if (SPK_CMD_OK == cmd_status)
            {
                s_state = CLOUD_SPK_STATE_NET_RPT_SEND;
            }
            else if ((SPK_CMD_ERROR == cmd_status) || (SPK_CMD_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_SPK_STATE_NET_RPT_SEND:
            if (send_cmd("AT+SKRPTM=1\r\n", W800_AT_TIMEOUT_MS))
            {
                s_state = CLOUD_SPK_STATE_NET_RPT_WAIT;
            }
            break;

        case CLOUD_SPK_STATE_NET_RPT_WAIT:
            cmd_status = cmd_consume_status();
            if (SPK_CMD_OK == cmd_status)
            {
                s_state = CLOUD_SPK_STATE_READY;
                speaker_log("SPK: link ready\r\n");
            }
            else if ((SPK_CMD_ERROR == cmd_status) || (SPK_CMD_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_SPK_STATE_READY:
            if (!s_slot_pending && (s_pending_op != SPEAKER_OP_NONE))
            {
                uint32_t samples = 0U;
                uint8_t slot_idx = 0U;

                if (audio_capture_consume_slot_pcm16(s_slot_pcm, SLOT_MAX_SAMPLES, &samples, &slot_idx))
                {
                    start_slot_send(slot_idx, samples);
                }
            }
            break;

        case CLOUD_SPK_STATE_SEND_LEN_SEND:
            if (send_cmd(s_send_len_cmd, W800_AT_TIMEOUT_MS))
            {
                s_state = CLOUD_SPK_STATE_SEND_LEN_WAIT;
            }
            break;

        case CLOUD_SPK_STATE_SEND_LEN_WAIT:
            cmd_status = cmd_consume_status();
            if (SPK_CMD_OK == cmd_status)
            {
                s_state = CLOUD_SPK_STATE_SEND_HEADER;
            }
            else if ((SPK_CMD_ERROR == cmd_status) || (SPK_CMD_TIMEOUT == cmd_status))
            {
                cloud_enter_recover();
            }
            break;

        case CLOUD_SPK_STATE_SEND_HEADER:
            if (w800_at_tx_idle() && w800_at_send((const uint8_t *) s_header, s_header_len))
            {
                s_state = CLOUD_SPK_STATE_SEND_PCM;
            }
            break;

        case CLOUD_SPK_STATE_SEND_PCM:
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
                    s_result_deadline_ms = speaker_now_ms() + CLOUD_SPEAKER_RESULT_TIMEOUT_MS;
                    s_state = CLOUD_SPK_STATE_WAIT_RESULT;
                    s_session_id++;
                    s_pending_op = SPEAKER_OP_NONE;
                }
            }
            break;

        case CLOUD_SPK_STATE_WAIT_RESULT:
            if (!s_slot_pending)
            {
                s_state = CLOUD_SPK_STATE_READY;
            }
            break;

        case CLOUD_SPK_STATE_RECOVER_WAIT:
            if ((int32_t) (speaker_now_ms() - s_recover_deadline_ms) >= 0)
            {
                w800_at_reset_rx();
                s_state = CLOUD_SPK_STATE_PROBE_15M_SEND;
            }
            break;

        default:
            cloud_enter_recover();
            break;
    }
}

void cloud_speaker_client_suspend(void)
{
    s_suspended = true;
    s_slot_pending = false;
    s_pending_op = SPEAKER_OP_NONE;
    s_active_op = SPEAKER_OP_NONE;
    s_cmd_status = SPK_CMD_IDLE;
    w800_at_reset_rx();
}

void cloud_speaker_client_restart(void)
{
    cloud_speaker_client_init();
}

bool cloud_speaker_client_is_suspended(void)
{
    return s_suspended;
}

#else

void cloud_speaker_client_init(void)
{
}

void cloud_speaker_client_poll(void)
{
}

void cloud_speaker_client_suspend(void)
{
}

void cloud_speaker_client_restart(void)
{
    cloud_speaker_client_init();
}

bool cloud_speaker_client_is_suspended(void)
{
    return false;
}

bool cloud_speaker_client_start_identify(void)
{
    return false;
}

bool cloud_speaker_client_start_enroll(const char * speaker_id, uint8_t utter_idx, uint8_t utter_total)
{
    (void) speaker_id;
    (void) utter_idx;
    (void) utter_total;
    return false;
}

bool cloud_speaker_client_is_busy(void)
{
    return false;
}

#endif
