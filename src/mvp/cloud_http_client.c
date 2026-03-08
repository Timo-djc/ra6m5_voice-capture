#include "cloud_http_client.h"

#include "config.h"
#include "http_parser_lite.h"
#include "log.h"
#include "w800_at.h"
#include "w800_socket.h"

#include <stdio.h>
#include <string.h>

static int s_last_code = -1;
static float s_last_confidence = 0.0f;
static char s_last_text[CLOUD_TEXT_MAX_LEN];
static uint8_t s_http_rx_buf[CLOUD_RX_BUFFER_SIZE];

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
    int rc;
    size_t rx_total = 0U;
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
                                    "Connection: close\r\n"
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

    rc = w800_socket_send(sock, (const uint8_t *) header, header_len);
    if (MVP_OK != rc)
    {
        LOGE("socket send header failed rc=%d", rc);
        goto cleanup;
    }

    /* W800 needs time to flush the header over TCP before accepting new SKSND
     * commands for the body.  A passive delay is safer than issuing AT commands
     * (like SKSTT) which can themselves be rejected while the module is busy. */
    R_BSP_SoftwareDelay(300U, BSP_DELAY_UNITS_MILLISECONDS);

    rc = w800_socket_send(sock, wav, wav_size);
    if (MVP_OK != rc)
    {
        LOGE("socket send body failed rc=%d", rc);
        goto cleanup;
    }

    deadline = w800_now_ms() + HTTP_TIMEOUT_MS;

    while ((int32_t) (w800_now_ms() - deadline) < 0)
    {
        w800_socket_status_t st;

        rc = w800_socket_get_status(sock, &st);
        if (MVP_OK != rc)
        {
            goto cleanup;
        }

        if (st.rx_data_len > 0U)
        {
            size_t room = CLOUD_RX_BUFFER_SIZE - rx_total;
            size_t want = (st.rx_data_len < room) ? st.rx_data_len : room;
            size_t got = 0U;

            if (0U == want)
            {
                rc = MVP_ERR_OVERFLOW;
                goto cleanup;
            }

            rc = w800_socket_recv(sock, s_http_rx_buf + rx_total, want, &got);
            if (MVP_OK != rc)
            {
                goto cleanup;
            }

            rx_total += got;

            rc = http_lite_parse(s_http_rx_buf, rx_total, &http);
            if ((MVP_OK == rc) && http.complete)
            {
                break;
            }
        }
        else
        {
            if ((st.status == W800_SOCKET_STATUS_DISCONNECTED) && (rx_total > 0U))
            {
                break;
            }

            R_BSP_SoftwareDelay(50U, BSP_DELAY_UNITS_MILLISECONDS);
        }
    }

    rc = http_lite_parse(s_http_rx_buf, rx_total, &http);
    if (MVP_OK != rc)
    {
        goto cleanup;
    }

    if (200 != http.status_code)
    {
        LOGE("HTTP status=%d", http.status_code);
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
        goto cleanup;
    }

    rc = MVP_OK;

cleanup:
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
