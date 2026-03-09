#include "cloud_http_client.h"

#include "config.h"
#include "http_parser_lite.h"
#include "log.h"
#include "w800_at.h"
#include "w800_socket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MicroLib strtod returns 0 for all inputs.  This manual parser handles
 * simple decimal numbers like "0.95" or "-1.5" using integer arithmetic. */
static float mvp_strtof(const char * s)
{
    int sign = 1;
    int64_t integer_part = 0;
    int64_t frac_part = 0;
    int64_t frac_div = 1;
    int has_digit = 0;

    if (NULL == s) { return 0.0f; }
    while ((*s == ' ') || (*s == '\t')) { s++; }
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }

    while ((*s >= '0') && (*s <= '9'))
    {
        has_digit = 1;
        integer_part = integer_part * 10 + (*s - '0');
        s++;
    }

    if (*s == '.')
    {
        s++;
        while ((*s >= '0') && (*s <= '9'))
        {
            has_digit = 1;
            frac_part = frac_part * 10 + (*s - '0');
            frac_div *= 10;
            s++;
        }
    }

    if (!has_digit) { return 0.0f; }
    return (float) sign * ((float) integer_part + (float) frac_part / (float) frac_div);
}

static int s_last_code = -1;
static float s_last_confidence = 0.0f;
static char s_last_text[CLOUD_TEXT_MAX_LEN];
static uint8_t s_http_rx_buf[CLOUD_RX_BUFFER_SIZE];

static int buffer_contains_token(const uint8_t * data, size_t len, const char * token)
{
    size_t i;
    size_t n;

    if ((NULL == data) || (NULL == token))
    {
        return 0;
    }

    n = strlen(token);
    if ((0U == n) || (len < n))
    {
        return 0;
    }

    for (i = 0U; i <= (len - n); i++)
    {
        if (0 == memcmp(data + i, token, n))
        {
            return 1;
        }
    }

    return 0;
}

static int buffer_has_http_payload_hint(const uint8_t * data, size_t len)
{
    return buffer_contains_token(data, len, "HTTP/") ||
           buffer_contains_token(data, len, "{");
}

static int buffer_is_line_noise_only(const uint8_t * data, size_t len)
{
    size_t i;

    if ((NULL == data) || (0U == len))
    {
        return 0;
    }

    for (i = 0U; i < len; i++)
    {
        uint8_t c = data[i];
        if ((c != '\r') && (c != '\n') && (c != '\0'))
        {
            return 0;
        }
    }

    return 1;
}

static const uint8_t * find_token_bytes(const uint8_t * data, size_t len, const char * token)
{
    size_t i;
    size_t n;

    if ((NULL == data) || (NULL == token))
    {
        return NULL;
    }

    n = strlen(token);
    if ((0U == n) || (len < n))
    {
        return NULL;
    }

    for (i = 0U; i <= (len - n); i++)
    {
        if (0 == memcmp(data + i, token, n))
        {
            return data + i;
        }
    }

    return NULL;
}

static int parse_int_after_colon(const uint8_t * p, size_t len, int * out_value)
{
    size_t i = 0U;
    int sign = 1;
    int value = 0;
    int has_digit = 0;

    if ((NULL == p) || (NULL == out_value) || (0U == len))
    {
        return MVP_ERR_ARG;
    }

    while ((i < len) && (p[i] != ':'))
    {
        i++;
    }
    if (i >= len)
    {
        return MVP_ERR_JSON;
    }
    i++;

    while ((i < len) &&
           ((p[i] == ' ') || (p[i] == '\t') || (p[i] == '\r') || (p[i] == '\n')))
    {
        i++;
    }

    if ((i < len) && (p[i] == '-'))
    {
        sign = -1;
        i++;
    }

    while ((i < len) && (p[i] >= '0') && (p[i] <= '9'))
    {
        has_digit = 1;
        value = value * 10 + (int) (p[i] - '0');
        i++;
    }

    if (!has_digit)
    {
        return MVP_ERR_JSON;
    }

    *out_value = sign * value;
    return MVP_OK;
}

static int parse_float_after_colon(const uint8_t * p, size_t len, float * out_value)
{
    char numbuf[32];
    size_t i = 0U;
    size_t n = 0U;

    if ((NULL == p) || (NULL == out_value) || (0U == len))
    {
        return MVP_ERR_ARG;
    }

    while ((i < len) && (p[i] != ':'))
    {
        i++;
    }
    if (i >= len)
    {
        return MVP_ERR_JSON;
    }
    i++;

    while ((i < len) &&
           ((p[i] == ' ') || (p[i] == '\t') || (p[i] == '\r') || (p[i] == '\n')))
    {
        i++;
    }

    while ((i < len) && (n + 1U < sizeof(numbuf)))
    {
        uint8_t c = p[i];
        if (((c >= '0') && (c <= '9')) ||
            (c == '+') || (c == '-') || (c == '.') ||
            (c == 'e') || (c == 'E'))
        {
            numbuf[n++] = (char) c;
            i++;
        }
        else
        {
            break;
        }
    }

    if (0U == n)
    {
        return MVP_ERR_JSON;
    }

    numbuf[n] = '\0';
    *out_value = mvp_strtof(numbuf);
    return MVP_OK;
}

static int extract_last_float_token(const uint8_t * data, size_t len, float * out_value)
{
    size_t i = 0U;
    int found = 0;
    float last = 0.0f;

    if ((NULL == data) || (NULL == out_value) || (0U == len))
    {
        return MVP_ERR_ARG;
    }

    while (i < len)
    {
        if ((data[i] >= '0' && data[i] <= '9') || (data[i] == '-') || (data[i] == '+') || (data[i] == '.'))
        {
            char numbuf[32];
            size_t n = 0U;
            size_t j = i;
            int has_digit = 0;
            int has_dot_or_exp = 0;

            while ((j < len) && (n + 1U < sizeof(numbuf)))
            {
                uint8_t c = data[j];
                if (((c >= '0') && (c <= '9')) ||
                    (c == '+') || (c == '-') || (c == '.') ||
                    (c == 'e') || (c == 'E'))
                {
                    if ((c >= '0') && (c <= '9'))
                    {
                        has_digit = 1;
                    }
                    if ((c == '.') || (c == 'e') || (c == 'E'))
                    {
                        has_dot_or_exp = 1;
                    }
                    numbuf[n++] = (char) c;
                    j++;
                }
                else
                {
                    break;
                }
            }

            if (has_digit)
            {
                numbuf[n] = '\0';
                last = mvp_strtof(numbuf);
                /* Prefer decimal-like number (e.g. 0.95) over integers. */
                if (has_dot_or_exp || !found)
                {
                    found = 1;
                    *out_value = last;
                }
            }

            if (j > i)
            {
                i = j;
                continue;
            }
        }

        i++;
    }

    return found ? MVP_OK : MVP_ERR_JSON;
}

static int parse_text_after_colon(const uint8_t * p, size_t len, char * out_text, size_t out_size)
{
    size_t i = 0U;
    size_t start;
    size_t end;
    size_t copy_len;

    if ((NULL == p) || (NULL == out_text) || (out_size < 2U))
    {
        return MVP_ERR_ARG;
    }

    out_text[0] = '\0';

    while ((i < len) && (p[i] != ':'))
    {
        i++;
    }
    if (i >= len)
    {
        return MVP_ERR_JSON;
    }
    i++;

    while ((i < len) &&
           ((p[i] == ' ') || (p[i] == '\t') || (p[i] == '\r') || (p[i] == '\n')))
    {
        i++;
    }

    if ((i >= len) || (p[i] != '"'))
    {
        return MVP_ERR_JSON;
    }
    i++;
    start = i;
    end = start;

    while (end < len)
    {
        if (p[end] == '"')
        {
            break;
        }
        end++;
    }

    copy_len = end - start;
    if (copy_len >= (out_size - 1U))
    {
        copy_len = out_size - 1U;
    }

    memcpy(out_text, p + start, copy_len);
    out_text[copy_len] = '\0';
    return MVP_OK;
}

static int extract_json_best_effort(const uint8_t * body,
                                    size_t body_len,
                                    int * out_code,
                                    char * out_text,
                                    size_t text_size,
                                    float * out_confidence)
{
    const uint8_t * p_code;
    const uint8_t * p_text;
    const uint8_t * p_conf;
    int rc_code;

    if ((NULL == body) || (0U == body_len) ||
        (NULL == out_code) || (NULL == out_text) || (NULL == out_confidence))
    {
        return MVP_ERR_ARG;
    }

    p_code = find_token_bytes(body, body_len, "\"code\"");
    if (NULL == p_code)
    {
        return MVP_ERR_JSON;
    }

    rc_code = parse_int_after_colon(p_code, body_len - (size_t) (p_code - body), out_code);
    if (MVP_OK != rc_code)
    {
        return rc_code;
    }

    out_text[0] = '\0';
    p_text = find_token_bytes(body, body_len, "\"text\"");
    if (NULL != p_text)
    {
        (void) parse_text_after_colon(p_text, body_len - (size_t) (p_text - body), out_text, text_size);
    }

    *out_confidence = 0.0f;
    p_conf = find_token_bytes(body, body_len, "\"confidence\"");
    if (NULL != p_conf)
    {
        (void) parse_float_after_colon(p_conf, body_len - (size_t) (p_conf - body), out_confidence);
    }

    return MVP_OK;
}

static void patch_confidence_from_body(const uint8_t * body, size_t body_len, float * io_confidence)
{
    const uint8_t * p_conf;
    float parsed = 0.0f;

    if ((NULL == body) || (NULL == io_confidence) || (0U == body_len))
    {
        return;
    }

    p_conf = find_token_bytes(body, body_len, "\"confidence\"");
    if (NULL == p_conf)
    {
        return;
    }

    if (MVP_OK == parse_float_after_colon(p_conf, body_len - (size_t) (p_conf - body), &parsed))
    {
        *io_confidence = parsed;
        return;
    }

    /* If key exists but payload is partially corrupted, try last numeric token. */
    if (MVP_OK == extract_last_float_token(body, body_len, &parsed))
    {
        *io_confidence = parsed;
    }
}

static int extract_json_code_only(const uint8_t * body, size_t body_len, int * out_code)
{
    static const char k_code_token[] = "\"code\"";
    size_t i;
    size_t token_len = sizeof(k_code_token) - 1U;

    if ((NULL == body) || (NULL == out_code) || (body_len < token_len))
    {
        return MVP_ERR_ARG;
    }

    for (i = 0U; i + token_len <= body_len; i++)
    {
        if (0 == memcmp(body + i, k_code_token, token_len))
        {
            size_t p = i + token_len;
            int sign = 1;
            int value = 0;
            int has_digit = 0;

            while ((p < body_len) && (body[p] != ':'))
            {
                p++;
            }
            if (p >= body_len)
            {
                return MVP_ERR_JSON;
            }
            p++;

            while ((p < body_len) &&
                   ((body[p] == ' ') || (body[p] == '\t') || (body[p] == '\r') || (body[p] == '\n')))
            {
                p++;
            }

            if ((p < body_len) && (body[p] == '-'))
            {
                sign = -1;
                p++;
            }

            while ((p < body_len) && (body[p] >= '0') && (body[p] <= '9'))
            {
                has_digit = 1;
                value = value * 10 + (int) (body[p] - '0');
                p++;
            }

            if (!has_digit)
            {
                return MVP_ERR_JSON;
            }

            *out_code = sign * value;
            return MVP_OK;
        }
    }

    return MVP_ERR_JSON;
}

static void log_http_preview(const uint8_t * data, size_t len)
{
    char preview[161];
    size_t i;
    size_t n = (len < 160U) ? len : 160U;

    if (NULL == data)
    {
        return;
    }

    for (i = 0U; i < n; i++)
    {
        uint8_t c = data[i];
        preview[i] = ((c >= 32U) && (c <= 126U)) ? (char) c : '.';
    }
    preview[n] = '\0';

    LOGI("HTTP rx preview (%lu bytes): %s", (unsigned long) len, preview);
}

static int select_target_host(char * out, size_t out_size)
{
    if ((NULL == out) || (out_size < 2U))
    {
        return MVP_ERR_ARG;
    }

    if ('\0' != CLOUD_HOST_IP[0])
    {
        strncpy(out, CLOUD_HOST_IP, out_size - 1U);
        out[out_size - 1U] = '\0';
        return MVP_OK;
    }

    if ('\0' != CLOUD_HOST_NAME[0])
    {
        strncpy(out, CLOUD_HOST_NAME, out_size - 1U);
        out[out_size - 1U] = '\0';
        return MVP_OK;
    }

    return MVP_ERR_ARG;
}

int cloud_client_init(void)
{
    s_last_code = -1;
    s_last_confidence = 0.0f;
    s_last_text[0] = '\0';

    return w800_socket_set_report_mode(0);
}

int cloud_client_upload_wav_once(const uint8_t * wav, size_t wav_size)
{
    char host[80];
    char header[512];
    size_t header_len;
    int sock = -1;
    int recv_sock = -1;
    int rc;
    size_t rx_total = 0U;
    uint32_t err4_no_data_count = 0U;
    uint32_t no_data_start_ms = 0U;
    uint32_t first_rx_ms = 0U;
    uint32_t last_rx_ms = 0U;
    uint32_t deadline;
    http_lite_response_t http;

    if ((NULL == wav) || (0U == wav_size))
    {
        return MVP_ERR_ARG;
    }

    rc = select_target_host(host, sizeof(host));
    if (MVP_OK != rc)
    {
        return rc;
    }

    LOGI("HTTP target: %s:%u%s", host, (unsigned int) CLOUD_PORT, CLOUD_PATH);

    rc = w800_socket_open_tcp(host, (uint16_t) CLOUD_PORT, (uint16_t) CLOUD_LOCAL_PORT, &sock);
    if (MVP_OK != rc)
    {
        LOGE("socket open failed rc=%d", rc);
        return rc;
    }

    {
        int header_chars = snprintf(header,
                                    sizeof(header),
                                    "POST %s HTTP/1.1\r\n"
                                    "Host: %s:%u\r\n"
                                    "Content-Type: audio/wav\r\n"
                                    "Content-Length: %lu\r\n"
                                    "X-Device-Id: %s\r\n"
                                    "X-Sample-Rate: %u\r\n"
                                    "X-Format: wav\r\n"
                                    "Connection: keep-alive\r\n"
                                    "\r\n",
                                    CLOUD_PATH,
                                    host,
                                    (unsigned int) CLOUD_PORT,
                                    (unsigned long) wav_size,
                                    DEVICE_ID,
                                    (unsigned int) SAMPLE_RATE);

        if ((header_chars <= 0) || ((size_t) header_chars >= sizeof(header)))
        {
            rc = MVP_ERR_OVERFLOW;
            goto cleanup;
        }

        header_len = (size_t) header_chars;
    }

    /* Verify socket is alive before first data send.  Some W800 firmware
     * builds tear down the socket asynchronously right after SKCT when
     * the TCP peer resets or the WiFi TX path encounters an error. */
    {
        w800_socket_status_t pre;

        R_BSP_SoftwareDelay(300U, BSP_DELAY_UNITS_MILLISECONDS);

        rc = w800_socket_get_status(sock, &pre);
        if (MVP_OK != rc)
        {
            LOGE("pre-send SKSTT failed rc=%d, socket already gone", rc);
            goto cleanup;
        }

        LOGI("pre-send SKSTT: socket=%d status=%d rx=%lu",
             pre.socket_id, pre.status, (unsigned long) pre.rx_data_len);

        if (pre.status != W800_SOCKET_STATUS_CONNECTED)
        {
            LOGE("pre-send: socket not connected (status=%d)", pre.status);
            rc = MVP_ERR_SOCKET;
            goto cleanup;
        }

        recv_sock = sock;
        if ((pre.socket_id >= 0) && (pre.socket_id != sock))
        {
            /* Observed on some firmware: first SKSTT field may be 0 even when queried socket is 1.
             * Do not switch recv socket based on this field. */
            LOGW("SKSTT first field differs: query=%d resp_first=%d status=%d (keep recv socket=%d)",
                 sock, pre.socket_id, pre.status, recv_sock);
        }
    }

    /* Send header + body as one continuous SKSND byte stream. */
    rc = w800_socket_send_two(sock,
                              (const uint8_t *) header, header_len,
                              wav, wav_size);
    if (MVP_OK != rc)
    {
        LOGE("socket send failed rc=%d", rc);
        goto cleanup;
    }

    /* Let W800 transition from raw-data mode back to AT command mode. */
    R_BSP_SoftwareDelay(W800_HTTP_POST_SEND_GUARD_MS, BSP_DELAY_UNITS_MILLISECONDS);

    /* Drain stale UART bytes (trailing \r\n from last SKSND response) before
     * entering the recv loop.  AT-mode SKRCV is the only way to read socket
     * data, so draining the UART cannot lose HTTP payload. */
    w800_uart_drain_rx();

    /* Poll SKSTT until the W800 reports rx_data_len > 0 and the value
     * stabilises (no growth for 200 ms), meaning the full HTTP response has
     * likely landed in the W800 socket buffer. */
    {
        uint32_t poll_deadline = w800_now_ms() + HTTP_TIMEOUT_MS;
        uint32_t prev_rx = 0U;
        uint32_t stable_since = 0U;
        while ((int32_t)(w800_now_ms() - poll_deadline) < 0)
        {
            w800_socket_status_t poll_st;
            int poll_rc = w800_socket_get_status(recv_sock, &poll_st);
            if (MVP_OK == poll_rc)
            {
                if (poll_st.status != W800_SOCKET_STATUS_CONNECTED)
                {
                    LOGW("SKSTT poll: socket closed (status=%d) rx=%lu",
                         poll_st.status, (unsigned long) poll_st.rx_data_len);
                    break;
                }
                if (poll_st.rx_data_len > 0U)
                {
                    if (poll_st.rx_data_len != prev_rx)
                    {
                        prev_rx = poll_st.rx_data_len;
                        stable_since = w800_now_ms();
                    }
                    else if ((int32_t)(w800_now_ms() - stable_since) >= 200)
                    {
                        LOGI("SKSTT poll: rx=%lu stable, proceed to SKRCV",
                             (unsigned long) poll_st.rx_data_len);
                        break;
                    }
                }
            }
            else
            {
                LOGW("SKSTT poll failed rc=%d, retry", poll_rc);
            }
            R_BSP_SoftwareDelay(50U, BSP_DELAY_UNITS_MILLISECONDS);
        }
    }

    deadline = w800_now_ms() + HTTP_TIMEOUT_MS;

    while ((int32_t) (w800_now_ms() - deadline) < 0)
    {
        size_t room = CLOUD_RX_BUFFER_SIZE - rx_total;
        size_t got = 0U;
        size_t want = (room > 1024U) ? 1024U : room;
        uint32_t now_ms = w800_now_ms();

        /* Fast path: avoid an extra SKRCV if buffered bytes are already complete. */
        if (rx_total > 0U)
        {
            rc = http_lite_parse(s_http_rx_buf, rx_total, &http);
            if ((MVP_OK == rc) && http.complete)
            {
                break;
            }
        }

        /* After first response bytes arrive, poll with smaller SKRCV size to reduce
         * extra blocking and make tail completion/fallback faster. */
        if ((rx_total > 0U) && (want > 256U))
        {
            want = 256U;
        }

        if (0U == want)
        {
            rc = MVP_ERR_OVERFLOW;
            goto cleanup;
        }

        if (recv_sock < 0)
        {
            recv_sock = sock;
        }

        rc = w800_socket_recv(recv_sock, s_http_rx_buf + rx_total, want, &got);
        if (MVP_OK == rc)
        {
            err4_no_data_count = 0U;
            no_data_start_ms = 0U;
            if (got > 0U)
            {
                if (0U == first_rx_ms)
                {
                    first_rx_ms = now_ms;
                }
                last_rx_ms = now_ms;

                rx_total += got;
                rc = http_lite_parse(s_http_rx_buf, rx_total, &http);
                if ((MVP_OK == rc) && http.complete)
                {
                    break;
                }

                if ((MVP_OK == rc) &&
                    (http.content_length > 0) &&
                    (http.body_len < (size_t) http.content_length))
                {
                    size_t missing = (size_t) http.content_length - http.body_len;
                    if (missing <= 4U)
                    {
                        LOGW("response phase near-complete body=%lu/%d, stop recv and parse fallback",
                             (unsigned long) http.body_len, http.content_length);
                        break;
                    }
                }

                continue;
            }

            /* No new data from SKRCV but socket is alive.  If we already
             * accumulated response bytes and nothing new arrived for 500 ms,
             * stop polling and fall through to the parse-fallback path. */
            if ((rx_total > 0U) && (last_rx_ms > 0U) &&
                ((int32_t)(w800_now_ms() - last_rx_ms) >= 500))
            {
                LOGW("response phase no new data for %lu ms after %lu bytes, stop recv",
                     (unsigned long)(w800_now_ms() - last_rx_ms),
                     (unsigned long) rx_total);
                break;
            }
            R_BSP_SoftwareDelay(20U, BSP_DELAY_UNITS_MILLISECONDS);
            continue;
        }

        if (MVP_ERR_AT == rc)
        {
            uint32_t fresh_ms = w800_now_ms();
            uint32_t ref_ms = (last_rx_ms > 0U) ? last_rx_ms : first_rx_ms;
            if ((rx_total > 0U) && (ref_ms > 0U) && ((int32_t)(fresh_ms - ref_ms) >= 300))
            {
                LOGW("response phase tail timeout after last data (%lu ms, rx=%lu), stop recv and parse fallback",
                     (unsigned long) (fresh_ms - ref_ms),
                     (unsigned long) rx_total);
                break;
            }

            /* Socket may be closed by peer; if we already have bytes, try parse. */
            if (rx_total > 0U)
            {
                size_t recovered = 0U;
                for (;;)
                {
                    size_t tail_room = CLOUD_RX_BUFFER_SIZE - rx_total;
                    size_t tail_got = 0U;
                    size_t tail_want = (tail_room > 32U) ? 32U : tail_room;
                    int tail_rc;

                    if (0U == tail_want)
                    {
                        break;
                    }

                    tail_rc = w800_read_bytes_any(s_http_rx_buf + rx_total,
                                                  tail_want,
                                                  &tail_got,
                                                  5U);
                    if ((MVP_OK != tail_rc) || (0U == tail_got))
                    {
                        break;
                    }

                    rx_total += tail_got;
                    recovered += tail_got;
                }

                if (buffer_has_http_payload_hint(s_http_rx_buf, rx_total))
                {
                    /* If recovered bytes are pure CR/LF/NUL and we already had HTTP bytes,
                     * treat them as AT-line noise instead of payload growth. */
                    if ((recovered > 0U) &&
                        buffer_has_http_payload_hint(s_http_rx_buf, rx_total - recovered) &&
                        buffer_is_line_noise_only(s_http_rx_buf + (rx_total - recovered), recovered))
                    {
                        rx_total -= recovered;
                        LOGW("response phase SKRCV rc=-4 tail line-noise (+%lu), ignored",
                             (unsigned long) recovered);
                        recovered = 0U;
                    }

                    rc = http_lite_parse(s_http_rx_buf, rx_total, &http);
                    if ((MVP_OK == rc) && http.complete)
                    {
                        LOGW("response phase SKRCV got rc=-4 after %lu bytes (tail +%lu), response complete",
                             (unsigned long) (rx_total - recovered),
                             (unsigned long) recovered);
                        break;
                    }

                    /* If parser sees only a tiny tail missing, try one direct UART read to complete it. */
                    if ((MVP_OK == rc) &&
                        (http.content_length > 0) &&
                        (http.body_len < (size_t) http.content_length))
                    {
                        size_t missing = (size_t) http.content_length - http.body_len;
                        size_t fill_room = CLOUD_RX_BUFFER_SIZE - rx_total;
                        size_t fill_want = (missing < fill_room) ? missing : fill_room;
                        size_t fill_got = 0U;

                        if ((fill_want > 0U) && (fill_want <= 64U) &&
                            (MVP_OK == w800_read_bytes_any(s_http_rx_buf + rx_total,
                                                           fill_want,
                                                           &fill_got,
                                                           20U)) &&
                            (fill_got > 0U))
                        {
                            rx_total += fill_got;
                            rc = http_lite_parse(s_http_rx_buf, rx_total, &http);
                            if ((MVP_OK == rc) && http.complete)
                            {
                                LOGW("response phase completed by direct tail fill (+%lu), total=%lu",
                                     (unsigned long) fill_got,
                                     (unsigned long) rx_total);
                                break;
                            }
                        }
                    }

                    if (0U == recovered)
                    {
                        LOGW("response phase SKRCV rc=-4 with no meaningful tail, stop recv and parse fallback");
                        break;
                    }

                    LOGW("response phase SKRCV got rc=-4 after %lu bytes (tail +%lu), response partial",
                         (unsigned long) (rx_total - recovered),
                         (unsigned long) recovered);
                }

                if (buffer_is_line_noise_only(s_http_rx_buf, rx_total) && (rx_total <= 64U))
                {
                    /* Pure CR/LF noise from AT parser; safe to drop. */
                    LOGW("response phase SKRCV rc=-4 with line-noise bytes (%lu), cleared",
                         (unsigned long) rx_total);
                    rx_total = 0U;
                }
                else
                {
                    /* Keep partial/non-noise bytes. It may be a fragmented HTTP prefix. */
                    LOGW("response phase SKRCV rc=-4 with partial bytes (%lu), keep accumulating",
                         (unsigned long) rx_total);
                }
            }
            else
            {
                /* Some W800 firmware paths leak payload bytes to UART even when
                 * SKRCV reports ERR. Try harvesting a short burst directly. */
                size_t uart_room = CLOUD_RX_BUFFER_SIZE - rx_total;
                size_t uart_got = 0U;
                size_t uart_want = (uart_room > 64U) ? 64U : uart_room;
                if ((uart_want > 0U) &&
                    (MVP_OK == w800_read_bytes_any(s_http_rx_buf + rx_total,
                                                   uart_want,
                                                   &uart_got,
                                                   5U)) &&
                    (uart_got > 0U))
                {
                    rx_total += uart_got;
                    if (buffer_has_http_payload_hint(s_http_rx_buf, rx_total))
                    {
                        rc = http_lite_parse(s_http_rx_buf, rx_total, &http);
                        if ((MVP_OK == rc) && http.complete)
                        {
                            LOGW("response phase recovered %lu bytes directly from UART (socket=%d), response complete",
                                 (unsigned long) uart_got, recv_sock);
                            break;
                        }

                        LOGW("response phase recovered %lu bytes directly from UART (socket=%d), response partial",
                             (unsigned long) uart_got, recv_sock);
                        continue;
                    }

                    if (buffer_is_line_noise_only(s_http_rx_buf, rx_total) && (rx_total <= 64U))
                    {
                        LOGW("response phase UART tail line-noise (%lu), cleared",
                             (unsigned long) rx_total);
                        rx_total = 0U;
                    }
                    else
                    {
                        LOGW("response phase UART partial bytes (%lu), keep accumulating",
                             (unsigned long) rx_total);
                    }
                }
            }

            err4_no_data_count++;
            if (0U == no_data_start_ms)
            {
                no_data_start_ms = w800_now_ms();
            }

            /* Treat ERR=-4 as "no data yet" and keep polling until HTTP deadline.
             * Every N retries, probe SKSTT to avoid waiting forever on a dead socket. */
            if ((1U == err4_no_data_count) ||
                ((err4_no_data_count % (W800_SKRCV_ERR4_NO_DATA_MAX * 4U)) == 0U))
            {
                w800_socket_status_t st;
                int skstt_rc = w800_socket_get_status(recv_sock, &st);

                if (MVP_OK == skstt_rc)
                {
                    uint32_t elapsed = w800_now_ms() - no_data_start_ms;
                    if (st.status != W800_SOCKET_STATUS_CONNECTED)
                    {
                        LOGE("response wait: SKRCV rc=-4 x%lu, socket status=%d rx=%lu elapsed=%lu ms",
                             (unsigned long) err4_no_data_count,
                             st.status,
                             (unsigned long) st.rx_data_len,
                             (unsigned long) elapsed);
                        break;
                    }

                    LOGW("response phase SKRCV rc=-4 (no data yet), socket alive rx=%lu elapsed=%lu ms (try=%lu)",
                         (unsigned long) st.rx_data_len,
                         (unsigned long) elapsed,
                         (unsigned long) err4_no_data_count);
                }
                else
                {
                    LOGW("response phase SKRCV rc=-4 (no data yet), SKSTT rc=%d (try=%lu)",
                         skstt_rc,
                         (unsigned long) err4_no_data_count);
                }
            }

            R_BSP_SoftwareDelay(30U, BSP_DELAY_UNITS_MILLISECONDS);
            continue;
        }

        if (MVP_ERR_TIMEOUT == rc)
        {
            if ((rx_total > 0U) && (first_rx_ms > 0U) && ((now_ms - first_rx_ms) >= 300U))
            {
                LOGW("response phase timeout after first bytes (%lu ms), stop recv and parse fallback",
                     (unsigned long) (now_ms - first_rx_ms));
                break;
            }

            R_BSP_SoftwareDelay(20U, BSP_DELAY_UNITS_MILLISECONDS);
            continue;
        }

        LOGE("response phase SKRCV failed rc=%d (rx_total=%lu)",
             rc, (unsigned long) rx_total);
        goto cleanup;
    }

    if (0U == rx_total)
    {
        LOGE("HTTP response empty after upload");
        rc = MVP_ERR_HTTP;
        goto cleanup;
    }

    rc = http_lite_parse(s_http_rx_buf, rx_total, &http);
    if (MVP_OK != rc)
    {
        const uint8_t * json_begin = NULL;
        const uint8_t * json_end = NULL;
        size_t i;

        for (i = 0U; i < rx_total; i++)
        {
            if ('{' == (char) s_http_rx_buf[i])
            {
                json_begin = s_http_rx_buf + i;
                break;
            }
        }

        for (i = rx_total; i > 0U; i--)
        {
            if ('}' == (char) s_http_rx_buf[i - 1U])
            {
                json_end = s_http_rx_buf + (i - 1U);
                break;
            }
        }

        if (NULL != json_begin)
        {
            const uint8_t * json_stop = (NULL != json_end) ? (json_end + 1U)
                                                           : (s_http_rx_buf + rx_total);

            http.status_code = 200;
            http.content_length = -1;
            http.body = json_begin;
            http.body_len = (size_t) (json_stop - json_begin);
            http.header_len = (size_t) (json_begin - s_http_rx_buf);
            http.complete = (NULL != json_end);
            LOGW("HTTP parse fallback engaged body_len=%lu complete=%d has_http200=%d",
                 (unsigned long) http.body_len,
                 http.complete ? 1 : 0,
                 (buffer_contains_token(s_http_rx_buf, rx_total, "200 OK") ||
                  buffer_contains_token(s_http_rx_buf, rx_total, "HTTP/1.1 200") ||
                  buffer_contains_token(s_http_rx_buf, rx_total, "HTTP/1.0 200")) ? 1 : 0);
        }
        else
        {
            LOGE("HTTP parse failed rc=%d rx_total=%lu", rc, (unsigned long) rx_total);
            log_http_preview(s_http_rx_buf, rx_total);
            goto cleanup;
        }
    }

    if (200 != http.status_code)
    {
        LOGE("HTTP status=%d body_len=%lu complete=%d",
             http.status_code, (unsigned long) http.body_len, http.complete ? 1 : 0);
        log_http_preview(s_http_rx_buf, rx_total);
        rc = MVP_ERR_HTTP;
        goto cleanup;
    }

    rc = http_lite_extract_json_result(http.body,
                                       http.body_len,
                                       &s_last_code,
                                       s_last_text,
                                       sizeof(s_last_text),
                                       &s_last_confidence);
    if (MVP_OK != rc)
    {
        if (MVP_OK == extract_json_best_effort(http.body,
                                               http.body_len,
                                               &s_last_code,
                                               s_last_text,
                                               sizeof(s_last_text),
                                               &s_last_confidence))
        {
            int _cp = (int)(s_last_confidence * 1000.0f + 0.5f);
            LOGW("HTTP JSON best-effort parse: code=%d text=%s confidence=%d.%03d body_len=%lu complete=%d",
                 s_last_code, s_last_text, _cp / 1000, _cp % 1000,
                 (unsigned long) http.body_len, http.complete ? 1 : 0);
            rc = MVP_OK;
        }
        else
        {
            int code_only = 0;
            if (MVP_OK == extract_json_code_only(http.body, http.body_len, &code_only))
            {
                s_last_code = code_only;
                s_last_text[0] = '\0';
                s_last_confidence = 0.0f;
                LOGW("HTTP JSON fallback: code=%d (text/confidence unavailable)", s_last_code);
                rc = MVP_OK;
            }
            else
            {
                LOGE("HTTP JSON parse failed rc=%d body_len=%lu complete=%d",
                     rc, (unsigned long) http.body_len, http.complete ? 1 : 0);
                log_http_preview(http.body, http.body_len);
                goto cleanup;
            }
        }
    }
    else
    {
        /* http_lite_extract_json_result may keep MVP_OK even when confidence
         * field parse fails; patch from raw body when available. */
        patch_confidence_from_body(http.body, http.body_len, &s_last_confidence);
    }

    rc = MVP_OK;

cleanup:
    if ((recv_sock >= 0) && (recv_sock != sock))
    {
        (void) w800_socket_close(recv_sock);
    }

    if (sock >= 0)
    {
        (void) w800_socket_close(sock);
    }

    return rc;
}

int cloud_client_get_last_result(char * text, size_t text_size, float * confidence)
{
    if ((NULL == text) || (text_size < 2U) || (NULL == confidence))
    {
        return MVP_ERR_ARG;
    }

    strncpy(text, s_last_text, text_size - 1U);
    text[text_size - 1U] = '\0';
    *confidence = s_last_confidence;

    return s_last_code;
}
