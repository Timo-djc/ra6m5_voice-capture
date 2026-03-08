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
        uint32_t conn_deadline = w800_now_ms() + 5000U;
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
                     (unsigned long)(w800_now_ms() - (conn_deadline - 5000U)));
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
    bool partial_window_logged = false;
    int rc;

    if ((socket < 0) || (NULL == data) || (0U == size))
    {
        return MVP_ERR_ARG;
    }

    deadline = w800_now_ms() + HTTP_SEND_TIMEOUT_MS;

    while (sent < size)
    {
        size_t remaining = size - sent;
        uint32_t request_size = (remaining > (size_t) W800_SKSND_MAX_CHUNK)
                                ? W800_SKSND_MAX_CHUNK
                                : (uint32_t) remaining;
        /* Drain stale async data before first SKSND attempt of each chunk. */
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

            /* Log W800 error detail (read_until_ok_line copies +ERR text). */
            LOGW("SKSND W800: %s (sent=%lu/%lu)",
                 line, (unsigned long) sent, (unsigned long) size);

            /* Check if TCP connection is still alive. */
            {
                w800_socket_status_t sst;
                if (MVP_OK == w800_socket_get_status(socket, &sst))
                {
                    if (sst.status != W800_SOCKET_STATUS_CONNECTED)
                    {
                        LOGE("SKSND: socket disconnected (status=%d), aborting", sst.status);
                        return MVP_ERR_SOCKET;
                    }
                }
            }

            if ((int32_t) (w800_now_ms() - deadline) >= 0)
            {
                LOGE("SKSND deadline expired, sent=%lu/%lu",
                     (unsigned long) sent, (unsigned long) size);
                return MVP_ERR_TIMEOUT;
            }

            w800_uart_drain_rx();
            R_BSP_SoftwareDelay(200U, BSP_DELAY_UNITS_MILLISECONDS);
        }

        rc = parse_ok_value(line, &actual_size);
        if (MVP_OK != rc)
        {
            return rc;
        }

        if (0U == actual_size)
        {
            if ((int32_t) (w800_now_ms() - deadline) >= 0)
            {
                LOGE("SKSND window stays 0, sent=%lu total=%lu",
                     (unsigned long) sent,
                     (unsigned long) size);
                return MVP_ERR_TIMEOUT;
            }

            R_BSP_SoftwareDelay(50U, BSP_DELAY_UNITS_MILLISECONDS);
            continue;
        }

        if (actual_size > request_size)
        {
            LOGE("SKSND actual_size=%lu > request=%lu",
                 (unsigned long) actual_size,
                 (unsigned long) request_size);
            return MVP_ERR_SIZE_MISMATCH;
        }

        if ((actual_size < request_size) && !partial_window_logged)
        {
            LOGW("SKSND partial window: request=%lu actual=%lu",
                 (unsigned long) request_size,
                 (unsigned long) actual_size);
            partial_window_logged = true;
        }

        rc = w800_write_raw(data + sent, actual_size, HTTP_SEND_TIMEOUT_MS);
        if (MVP_OK != rc)
        {
            return rc;
        }

        sent += actual_size;

        /* W800 TCP buffer is ~512 B; wait for TCP ACK to free it.
         * Scale delay: ~200 ms for a full 512-byte chunk on WiFi. */
        if (sent < size)
        {
            uint32_t flush_ms = (actual_size >= 256U) ? 200U : 80U;
            R_BSP_SoftwareDelay(flush_ms, BSP_DELAY_UNITS_MILLISECONDS);
        }
    }

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
                st->socket_id = (int) strtol(token, NULL, 10);
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
