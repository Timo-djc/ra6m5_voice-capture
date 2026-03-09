#include "w800_socket.h"

#include "config.h"
#include "log.h"
#include "w800_at.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool line_is_err(const char * line)
{
    return (NULL != strstr(line, "+ERR")) ||
           (NULL != strstr(line, "ERROR")) ||
           (NULL != strstr(line, "FAIL"));
}

static int parse_ok_value(const char * line, uint32_t * out_value)
{
    const char * p;
    uint32_t v = 0U;
    int has_digit = 0;

    if ((NULL == line) || (NULL == out_value))
    {
        return MVP_ERR_ARG;
    }

    p = strstr(line, "+OK=");
    if (NULL == p)
    {
        return MVP_ERR_AT;
    }

    p += 4;

    while ((*p == ' ') || (*p == '\t'))
    {
        p++;
    }

    while ((*p >= '0') && (*p <= '9'))
    {
        has_digit = 1;
        v = (uint32_t) (v * 10U + (uint32_t) (*p - '0'));
        p++;
    }

    if (!has_digit)
    {
        return MVP_ERR_AT;
    }

    *out_value = v;
    return MVP_OK;
}

static int read_until_ok_line(char * out_line, size_t out_size, uint32_t timeout_ms)
{
    int rc;
    char line[W800_LINE_BUF_SIZE];
    uint32_t deadline = w800_now_ms() + timeout_ms;

    if ((NULL == out_line) || (out_size < 2U))
    {
        return MVP_ERR_ARG;
    }

    out_line[0] = '\0';

    while ((int32_t) (w800_now_ms() - deadline) < 0)
    {
        uint32_t remain = (uint32_t) (deadline - w800_now_ms());
        rc = w800_read_response(line, sizeof(line), remain);
        if (MVP_OK != rc)
        {
            if (MVP_ERR_TIMEOUT == rc)
            {
                break;
            }
            return rc;
        }

        if (line_is_err(line))
        {
            strncpy(out_line, line, out_size - 1U);
            out_line[out_size - 1U] = '\0';
            return MVP_ERR_AT;
        }

        if (NULL != strstr(line, "+OK"))
        {
            strncpy(out_line, line, out_size - 1U);
            out_line[out_size - 1U] = '\0';
            return MVP_OK;
        }
    }

    return MVP_ERR_TIMEOUT;
}

static uint32_t choose_sksnd_request_size(size_t remaining)
{
    size_t request_size = remaining;
    size_t min_tail = W800_SKSND_MIN_TAIL;
    size_t tail;

    if (request_size > W800_SKSND_MAX_CHUNK)
    {
        request_size = W800_SKSND_MAX_CHUNK;
    }

    if (min_tail > W800_SKSND_MAX_CHUNK)
    {
        min_tail = W800_SKSND_MAX_CHUNK;
    }

    if (0U == min_tail)
    {
        min_tail = 1U;
    }

    tail = remaining - request_size;
    if ((tail > 0U) && (tail < min_tail))
    {
        size_t adjusted = remaining - min_tail;
        if (adjusted > W800_SKSND_MAX_CHUNK)
        {
            adjusted = W800_SKSND_MAX_CHUNK;
        }

        if ((adjusted > 0U) && (adjusted < remaining))
        {
            request_size = adjusted;
        }
    }

    return (uint32_t) request_size;
}

static int parse_sksnd_ok_value(const char * line, uint32_t request_size, uint32_t * out_value)
{
    int rc;

    if ((NULL == line) || (NULL == out_value))
    {
        return MVP_ERR_ARG;
    }

    rc = parse_ok_value(line, out_value);
    if (MVP_OK == rc)
    {
        return MVP_OK;
    }

    /* Some W800 firmware builds occasionally reply SKSND with plain "+OK"
     * (no "=<actualsize>").  Fall back to the requested size so the send
     * stream can continue. */
    if (0 == strncmp(line, "+OK", 3))
    {
        *out_value = request_size;
        LOGW("SKSND reply without size, assume request=%lu", (unsigned long) request_size);
        return MVP_OK;
    }

    return rc;
}

int w800_socket_open_tcp(const char * host, uint16_t remote_port, uint16_t local_port, int * out_socket)
{
    char cmd[192];
    char line[W800_LINE_BUF_SIZE];
    char err_line[W800_LINE_BUF_SIZE];
    char hex_line[3U * 24U + 1U];
    uint32_t socket_id;
    int rc;
    static uint16_t s_ephemeral_port = 0U;
    uint16_t use_port;

    if ((NULL == host) || (NULL == out_socket))
    {
        return MVP_ERR_ARG;
    }

    /* Rotate local port to avoid TIME_WAIT conflicts on repeated runs. */
    if (0U == local_port)
    {
        if (0U == s_ephemeral_port)
        {
            s_ephemeral_port = (uint16_t)(10000U + (w800_now_ms() % 5000U));
        }
        else
        {
            s_ephemeral_port++;
            if (s_ephemeral_port > 60000U)
            {
                s_ephemeral_port = 10000U;
            }
        }
        use_port = s_ephemeral_port;
    }
    else
    {
        use_port = local_port;
    }

    (void) snprintf(cmd,
                    sizeof(cmd),
                    "AT+SKCT=0,0,%s,%u,%u\r\n",
                    host,
                    (unsigned int) remote_port,
                    (unsigned int) use_port);

    /* MVP synchronous flow: clear stale text responses before opening socket. */
    w800_uart_drain_rx();

    rc = w800_write_raw((const uint8_t *) cmd, strlen(cmd), 5000U);
    if (MVP_OK != rc)
    {
        LOGE("SKCT write failed rc=%d cmd=%s", rc, cmd);
        return rc;
    }

    rc = read_until_ok_line(line, sizeof(line), 5000U);
    if (MVP_OK != rc)
    {
        if (MVP_OK == w800_read_response(err_line, sizeof(err_line), 50U))
        {
            LOGE("SKCT failed rc=%d resp=%s cmd=%s", rc, err_line, cmd);
        }
        else
        {
            LOGE("SKCT failed rc=%d cmd=%s", rc, cmd);
        }
        return rc;
    }

    rc = parse_ok_value(line, &socket_id);
    if (MVP_OK != rc)
    {
        size_t i;
        size_t n = strlen(line);
        size_t dump = (n < 24U) ? n : 24U;
        size_t pos = 0U;
        for (i = 0U; i < dump; i++)
        {
            pos += (size_t) snprintf(hex_line + pos, sizeof(hex_line) - pos, "%02X ", (unsigned char) line[i]);
            if (pos >= (sizeof(hex_line) - 1U))
            {
                break;
            }
        }
        hex_line[(pos < sizeof(hex_line)) ? pos : (sizeof(hex_line) - 1U)] = '\0';
        LOGE("SKCT parse failed rc=%d line='%s' hex=%s", rc, line, hex_line);
        return rc;
    }

    *out_socket = (int) socket_id;
    LOGI("SKCT ok: socket=%u local_port=%u", (unsigned) socket_id, (unsigned) use_port);

    /* W800 SKCT may return +OK before TCP handshake completes.
     * Poll SKSTT until the socket reaches CONNECTED state. */
    {
        uint32_t conn_start = w800_now_ms();
        uint32_t conn_deadline = conn_start + 5000U;
        while ((int32_t)(w800_now_ms() - conn_deadline) < 0)
        {
            w800_socket_status_t cst;
            R_BSP_SoftwareDelay(100U, BSP_DELAY_UNITS_MILLISECONDS);
            rc = w800_socket_get_status((int) socket_id, &cst);
            if (MVP_OK != rc)
            {
                LOGE("SKSTT post-SKCT failed rc=%d", rc);
                return rc;
            }
            if (cst.status == W800_SOCKET_STATUS_CONNECTED)
            {
                LOGI("TCP connected (took ~%lu ms)",
                     (unsigned long)(w800_now_ms() - conn_start));
                break;
            }
            if (cst.status == W800_SOCKET_STATUS_DISCONNECTED)
            {
                LOGE("TCP connect failed (status=0 after SKCT)");
                return MVP_ERR_SOCKET;
            }
        }
    }

    return MVP_OK;
}

int w800_socket_open(const char * host, uint16_t remote_port, uint16_t local_port, int * out_socket)
{
    return w800_socket_open_tcp(host, remote_port, local_port, out_socket);
}

int w800_socket_send(int socket, const uint8_t * data, size_t size)
{
    char cmd[80];
    char line[W800_LINE_BUF_SIZE];
    uint32_t actual_size;
    uint32_t deadline;
    size_t sent = 0U;
    size_t next_health_check = W800_SKSND_HEALTH_CHECK_INTERVAL_BYTES;
    int rc;

    if ((socket < 0) || (NULL == data) || (0U == size))
    {
        return MVP_ERR_ARG;
    }

    deadline = w800_now_ms() + HTTP_SEND_TIMEOUT_MS;

    while (sent < size)
    {
        size_t remaining = size - sent;
        uint32_t request_size = choose_sksnd_request_size(remaining);
        uint32_t backoff_ms = W800_SKSND_BACKOFF_MS;
        uint32_t retry_count = 0U;

        w800_uart_drain_rx();

        for (;;)
        {
            (void) snprintf(cmd,
                            sizeof(cmd),
                            "AT+SKSND=%d,%lu\r\n",
                            socket,
                            (unsigned long) request_size);

            rc = w800_write_raw((const uint8_t *) cmd, strlen(cmd), W800_CMD_TIMEOUT_MS);
            if (MVP_OK != rc)
            {
                return rc;
            }

            rc = read_until_ok_line(line, sizeof(line), W800_CMD_TIMEOUT_MS);
            if (MVP_OK == rc)
            {
                break;
            }

            retry_count++;
            LOGW("SKSND ERR: %s (sent=%lu/%lu retry=%lu backoff=%lums)",
                 line,
                 (unsigned long) sent,
                 (unsigned long) size,
                 (unsigned long) retry_count,
                 (unsigned long) backoff_ms);

            /* On first ERR=-4, query SKSTT to diagnose socket state.
             * Whether SKSTT succeeds or fails, log the result and bail
             * out immediately – a dead socket won't recover. */
            if ((1U == retry_count) && (NULL != strstr(line, "-4")))
            {
                w800_socket_status_t diag;
                int skstt_rc;

                LOGW("SKSND got -4, querying SKSTT socket=%d ...", socket);
                w800_uart_drain_rx();
                skstt_rc = w800_socket_get_status(socket, &diag);

                if (MVP_OK == skstt_rc)
                {
                    LOGW("SKSTT: rc=OK socket=%d status=%d rx=%lu",
                         diag.socket_id, diag.status,
                         (unsigned long) diag.rx_data_len);

                    if (diag.status != W800_SOCKET_STATUS_CONNECTED)
                    {
                        LOGE("TCP dead (status=%d), no point retrying",
                             diag.status);
                        return MVP_ERR_SOCKET;
                    }
                }
                else
                {
                    LOGW("SKSTT: rc=%d (query failed), socket gone", skstt_rc);
                    return MVP_ERR_SOCKET;
                }
            }

            if (retry_count >= W800_SKSND_MAX_RETRY)
            {
                LOGE("SKSND max retries (%u) reached, sent=%lu/%lu",
                     (unsigned) W800_SKSND_MAX_RETRY,
                     (unsigned long) sent,
                     (unsigned long) size);
                return MVP_ERR_AT;
            }

            if ((int32_t) (w800_now_ms() - deadline) >= 0)
            {
                LOGE("SKSND deadline expired, sent=%lu/%lu",
                     (unsigned long) sent, (unsigned long) size);
                return MVP_ERR_TIMEOUT;
            }

            w800_uart_drain_rx();
            R_BSP_SoftwareDelay(backoff_ms, BSP_DELAY_UNITS_MILLISECONDS);
            if (backoff_ms < 1600U)
            {
                backoff_ms *= 2U;
            }
        }

        rc = parse_sksnd_ok_value(line, request_size, &actual_size);
        if (MVP_OK != rc)
        {
            LOGE("SKSND parse failed rc=%d line='%s' req=%lu sent=%lu/%lu",
                 rc, line, (unsigned long) request_size,
                 (unsigned long) sent, (unsigned long) size);
            return rc;
        }

        if (0U == actual_size)
        {
            if ((int32_t) (w800_now_ms() - deadline) >= 0)
            {
                LOGE("SKSND window stays 0, sent=%lu total=%lu",
                     (unsigned long) sent, (unsigned long) size);
                return MVP_ERR_TIMEOUT;
            }

            R_BSP_SoftwareDelay(50U, BSP_DELAY_UNITS_MILLISECONDS);
            continue;
        }

        if (actual_size > (uint32_t) remaining)
        {
            actual_size = (uint32_t) remaining;
        }

        /* Stream actual_size raw bytes.  Chunk the UART writes to keep
         * individual DMA transfers manageable. */
        {
            size_t tx_off = 0U;

            while (tx_off < actual_size)
            {
                size_t chunk = (size_t) actual_size - tx_off;

                if (chunk > W800_SKSND_MAX_CHUNK)
                {
                    chunk = W800_SKSND_MAX_CHUNK;
                }

                rc = w800_write_raw(data + sent + tx_off, chunk, HTTP_SEND_TIMEOUT_MS);
                if (MVP_OK != rc)
                {
                    return rc;
                }

                tx_off += chunk;
            }
        }

        sent += actual_size;

        if (sent < size)
        {
            if (W800_SKSND_INTER_CHUNK_DELAY_MS > 0U)
            {
                R_BSP_SoftwareDelay(W800_SKSND_INTER_CHUNK_DELAY_MS,
                                    BSP_DELAY_UNITS_MILLISECONDS);
            }

            if ((W800_SKSND_HEALTH_CHECK_INTERVAL_BYTES > 0U) &&
                (sent >= next_health_check))
            {
                w800_socket_status_t hc;
                int hc_rc = w800_socket_get_status(socket, &hc);
                if (MVP_OK == hc_rc)
                {
                    LOGI("mid-SKSND SKSTT: status=%d rx=%lu (sent=%lu/%lu)",
                         hc.status, (unsigned long) hc.rx_data_len,
                         (unsigned long) sent, (unsigned long) size);

                    if (hc.status != W800_SOCKET_STATUS_CONNECTED)
                    {
                        LOGE("TCP dropped during send (status=%d)", hc.status);
                        return MVP_ERR_SOCKET;
                    }
                }
                else
                {
                    LOGW("mid-SKSND SKSTT failed rc=%d (sent=%lu/%lu)",
                         hc_rc, (unsigned long) sent, (unsigned long) size);
                    return MVP_ERR_SOCKET;
                }

                next_health_check += W800_SKSND_HEALTH_CHECK_INTERVAL_BYTES;
            }
        }
    }

    LOGI("SKSND stream done: sent=%lu/%lu", (unsigned long) sent, (unsigned long) size);
    return MVP_OK;
}

/* ---------------------------------------------------------------------------
 * w800_socket_send_two – scatter-send TWO buffers as one logical byte stream.
 *
 * This avoids the "header SKSND done → gap → body SKSND" pattern that the
 * W800 rejects with ERR=-4.  Instead, a single SKSND transaction declares
 * the combined length and the raw byte stream spans both buffers seamlessly.
 * ----------------------------------------------------------------------- */
int w800_socket_send_two(int socket,
                         const uint8_t * data1, size_t len1,
                         const uint8_t * data2, size_t len2)
{
    char cmd[80];
    char line[W800_LINE_BUF_SIZE];
    uint32_t actual_size;
    uint32_t deadline;
    size_t total = len1 + len2;
    size_t sent = 0U;
    size_t next_health_check = W800_SKSND_HEALTH_CHECK_INTERVAL_BYTES;
    int rc;

    if (socket < 0)
    {
        return MVP_ERR_ARG;
    }

    if ((len1 > 0U) && (NULL == data1))
    {
        return MVP_ERR_ARG;
    }

    if ((len2 > 0U) && (NULL == data2))
    {
        return MVP_ERR_ARG;
    }

    if (0U == total)
    {
        return MVP_ERR_ARG;
    }

    deadline = w800_now_ms() + HTTP_SEND_TIMEOUT_MS;

    while (sent < total)
    {
        size_t remaining = total - sent;
        uint32_t request_size = choose_sksnd_request_size(remaining);
        uint32_t backoff_ms = W800_SKSND_BACKOFF_MS;
        uint32_t retry_count = 0U;

        w800_uart_drain_rx();

        for (;;)
        {
            (void) snprintf(cmd,
                            sizeof(cmd),
                            "AT+SKSND=%d,%lu\r\n",
                            socket,
                            (unsigned long) request_size);

            rc = w800_write_raw((const uint8_t *) cmd, strlen(cmd), W800_CMD_TIMEOUT_MS);
            if (MVP_OK != rc)
            {
                return rc;
            }

            rc = read_until_ok_line(line, sizeof(line), W800_CMD_TIMEOUT_MS);
            if (MVP_OK == rc)
            {
                break;
            }

            retry_count++;
            LOGW("SKSND ERR: %s (sent=%lu/%lu retry=%lu backoff=%lums)",
                 line,
                 (unsigned long) sent,
                 (unsigned long) total,
                 (unsigned long) retry_count,
                 (unsigned long) backoff_ms);

            /* On first ERR=-4, diagnose via SKSTT and bail out fast. */
            if ((1U == retry_count) && (NULL != strstr(line, "-4")))
            {
                w800_socket_status_t diag;
                int skstt_rc;

                LOGW("SKSND got -4, querying SKSTT socket=%d ...", socket);
                w800_uart_drain_rx();
                skstt_rc = w800_socket_get_status(socket, &diag);

                if (MVP_OK == skstt_rc)
                {
                    LOGW("SKSTT: rc=OK socket=%d status=%d rx=%lu",
                         diag.socket_id, diag.status,
                         (unsigned long) diag.rx_data_len);

                    if (diag.status != W800_SOCKET_STATUS_CONNECTED)
                    {
                        LOGE("TCP dead (status=%d), no point retrying",
                             diag.status);
                        return MVP_ERR_SOCKET;
                    }
                }
                else
                {
                    LOGW("SKSTT: rc=%d (query failed), socket gone", skstt_rc);
                    return MVP_ERR_SOCKET;
                }
            }

            if (retry_count >= W800_SKSND_MAX_RETRY)
            {
                LOGE("SKSND max retries (%u) reached, sent=%lu/%lu",
                     (unsigned) W800_SKSND_MAX_RETRY,
                     (unsigned long) sent,
                     (unsigned long) total);
                return MVP_ERR_AT;
            }

            if ((int32_t) (w800_now_ms() - deadline) >= 0)
            {
                LOGE("SKSND deadline, sent=%lu/%lu",
                     (unsigned long) sent, (unsigned long) total);
                return MVP_ERR_TIMEOUT;
            }

            w800_uart_drain_rx();
            R_BSP_SoftwareDelay(backoff_ms, BSP_DELAY_UNITS_MILLISECONDS);
            if (backoff_ms < 1600U)
            {
                backoff_ms *= 2U;
            }
        }

        rc = parse_sksnd_ok_value(line, request_size, &actual_size);
        if (MVP_OK != rc)
        {
            LOGE("SKSND parse failed rc=%d line='%s' req=%lu sent=%lu/%lu",
                 rc, line, (unsigned long) request_size,
                 (unsigned long) sent, (unsigned long) total);
            return rc;
        }

        if (0U == actual_size)
        {
            if ((int32_t) (w800_now_ms() - deadline) >= 0)
            {
                return MVP_ERR_TIMEOUT;
            }

            R_BSP_SoftwareDelay(50U, BSP_DELAY_UNITS_MILLISECONDS);
            continue;
        }

        if (actual_size > (uint32_t) remaining)
        {
            actual_size = (uint32_t) remaining;
        }

        /* Stream actual_size raw bytes across the two-buffer scatter list.
         * Global offset 'pos' maps into [data1 | data2]. */
        {
            size_t to_write = actual_size;
            size_t pos = sent;

            while (to_write > 0U)
            {
                const uint8_t * src;
                size_t avail;
                size_t chunk;

                if (pos < len1)
                {
                    src   = data1 + pos;
                    avail = len1 - pos;
                }
                else
                {
                    src   = data2 + (pos - len1);
                    avail = len2 - (pos - len1);
                }

                chunk = (to_write < avail) ? to_write : avail;
                if (chunk > W800_SKSND_MAX_CHUNK)
                {
                    chunk = W800_SKSND_MAX_CHUNK;
                }

                rc = w800_write_raw(src, chunk, HTTP_SEND_TIMEOUT_MS);
                if (MVP_OK != rc)
                {
                    return rc;
                }

                pos      += chunk;
                to_write -= chunk;
            }
        }

        sent += actual_size;

        if (sent < total)
        {
            if (W800_SKSND_INTER_CHUNK_DELAY_MS > 0U)
            {
                R_BSP_SoftwareDelay(W800_SKSND_INTER_CHUNK_DELAY_MS,
                                    BSP_DELAY_UNITS_MILLISECONDS);
            }

            if ((W800_SKSND_HEALTH_CHECK_INTERVAL_BYTES > 0U) &&
                (sent >= next_health_check))
            {
                w800_socket_status_t hc;
                int hc_rc = w800_socket_get_status(socket, &hc);
                if (MVP_OK == hc_rc)
                {
                    LOGI("mid-SKSND SKSTT: status=%d rx=%lu (sent=%lu/%lu)",
                         hc.status, (unsigned long) hc.rx_data_len,
                         (unsigned long) sent, (unsigned long) total);

                    if (hc.status != W800_SOCKET_STATUS_CONNECTED)
                    {
                        LOGE("TCP dropped during send (status=%d)", hc.status);
                        return MVP_ERR_SOCKET;
                    }
                }
                else
                {
                    LOGW("mid-SKSND SKSTT failed rc=%d (sent=%lu/%lu)",
                         hc_rc, (unsigned long) sent, (unsigned long) total);
                    return MVP_ERR_SOCKET;
                }

                next_health_check += W800_SKSND_HEALTH_CHECK_INTERVAL_BYTES;
            }
        }
    }

    LOGI("SKSND stream done: sent=%lu/%lu", (unsigned long) sent, (unsigned long) total);
    return MVP_OK;
}

int w800_socket_recv(int socket, uint8_t * buf, size_t maxsize, size_t * out_size)
{
    char cmd[80];
    char line[W800_LINE_BUF_SIZE];
    uint32_t rx_size;
    int rc;

    if ((socket < 0) || (NULL == buf) || (0U == maxsize) || (NULL == out_size))
    {
        return MVP_ERR_ARG;
    }

    *out_size = 0U;

    (void) snprintf(cmd,
                    sizeof(cmd),
                    "AT+SKRCV=%d,%lu\r\n",
                    socket,
                    (unsigned long) maxsize);

    rc = w800_write_raw((const uint8_t *) cmd, strlen(cmd), W800_CMD_TIMEOUT_MS);
    if (MVP_OK != rc)
    {
        return rc;
    }

    rc = read_until_ok_line(line, sizeof(line), W800_CMD_TIMEOUT_MS);
    if (MVP_OK != rc)
    {
        return rc;
    }

    rc = parse_ok_value(line, &rx_size);
    if (MVP_OK != rc)
    {
        return rc;
    }

    if (rx_size > maxsize)
    {
        return MVP_ERR_OVERFLOW;
    }

    if (0U == rx_size)
    {
        return MVP_OK;
    }

    rc = w800_read_bytes_exact(buf, rx_size, HTTP_TIMEOUT_MS);
    if (MVP_OK != rc)
    {
        return rc;
    }

    *out_size = rx_size;
    return MVP_OK;
}

int w800_socket_get_status(int socket, w800_socket_status_t * st)
{
    char cmd[48];
    char line[W800_LINE_BUF_SIZE];
    char payload[W800_LINE_BUF_SIZE];
    char * token;
    char * next;
    int field = 0;
    int rc;

    if ((socket < 0) || (NULL == st))
    {
        return MVP_ERR_ARG;
    }

    memset(st, 0, sizeof(*st));
    st->socket_id = socket;

    (void) snprintf(cmd, sizeof(cmd), "AT+SKSTT=%d\r\n", socket);

    rc = w800_write_raw((const uint8_t *) cmd, strlen(cmd), W800_CMD_TIMEOUT_MS);
    if (MVP_OK != rc)
    {
        return rc;
    }

    rc = read_until_ok_line(line, sizeof(line), W800_CMD_TIMEOUT_MS);
    if (MVP_OK != rc)
    {
        return rc;
    }

    if (0 != strncmp(line, "+OK=", 4))
    {
        return MVP_ERR_AT;
    }

    strncpy(payload, line + 4, sizeof(payload) - 1U);
    payload[sizeof(payload) - 1U] = '\0';

    token = payload;
    while (NULL != token)
    {
        next = strchr(token, ',');
        if (NULL != next)
        {
            *next = '\0';
            next++;
        }

        switch (field)
        {
            case 0:
                /* First field of SKSTT reply is firmware-specific and is not a stable
                 * socket-id across versions. Keep queried socket id unchanged. */
                break;
            case 1:
                st->status = (int) strtol(token, NULL, 10);
                break;
            case 2:
                strncpy(st->remote_host, token, sizeof(st->remote_host) - 1U);
                st->remote_host[sizeof(st->remote_host) - 1U] = '\0';
                break;
            case 3:
                st->remote_port = (uint16_t) strtoul(token, NULL, 10);
                break;
            case 4:
                st->local_port = (uint16_t) strtoul(token, NULL, 10);
                break;
            case 5:
                st->rx_data_len = (uint32_t) strtoul(token, NULL, 10);
                break;
            default:
                break;
        }

        field++;
        token = next;
    }

    return MVP_OK;
}

int w800_socket_close(int socket)
{
    char cmd[48];

    if (socket < 0)
    {
        return MVP_ERR_ARG;
    }

    (void) snprintf(cmd, sizeof(cmd), "AT+SKCLS=%d\r\n", socket);
    return w800_send_cmd(cmd, "+OK", W800_CMD_TIMEOUT_MS);
}

int w800_socket_set_report_mode(int mode)
{
    char cmd[40];

    if ((mode < 0) || (mode > 1))
    {
        return MVP_ERR_ARG;
    }

    (void) snprintf(cmd, sizeof(cmd), "AT+SKRPTM=%d\r\n", mode);
    return w800_send_cmd(cmd, "+OK", W800_CMD_TIMEOUT_MS);
}

int w800_socket_is_open(int socket)
{
    w800_socket_status_t st;
    int rc = w800_socket_get_status(socket, &st);

    if (MVP_OK != rc)
    {
        return 0;
    }

    return (st.status == W800_SOCKET_STATUS_CONNECTED) ? 1 : 0;
}
