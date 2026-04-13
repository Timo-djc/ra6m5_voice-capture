#include "cloud_speaker_demo_client.h"

#include "config.h"
#include "log.h"
#include "w800_at.h"
#include "w800_socket.h"

#include <stdio.h>
#include <string.h>

typedef struct st_wav_view
{
    const uint8_t * pcm_data;
    size_t pcm_bytes;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
} wav_view_t;

static uint16_t read_le16(const uint8_t * p)
{
    return (uint16_t) (((uint16_t) p[1] << 8) | p[0]);
}

static uint32_t read_le32(const uint8_t * p)
{
    return ((uint32_t) p[3] << 24) |
           ((uint32_t) p[2] << 16) |
           ((uint32_t) p[1] << 8) |
           (uint32_t) p[0];
}

static int wav_parse(const uint8_t * wav, size_t wav_size, wav_view_t * out)
{
    size_t pos = 12U;

    if ((NULL == wav) || (NULL == out) || (wav_size < 44U))
    {
        return MVP_ERR_ARG;
    }

    if ((0 != memcmp(wav + 0U, "RIFF", 4U)) || (0 != memcmp(wav + 8U, "WAVE", 4U)))
    {
        return MVP_ERR_ARG;
    }

    memset(out, 0, sizeof(*out));

    while ((pos + 8U) <= wav_size)
    {
        uint32_t chunk_size = read_le32(wav + pos + 4U);
        const uint8_t * chunk = wav + pos + 8U;

        if ((pos + 8U + chunk_size) > wav_size)
        {
            return MVP_ERR_SIZE_MISMATCH;
        }

        if (0 == memcmp(wav + pos, "fmt ", 4U))
        {
            if (chunk_size < 16U)
            {
                return MVP_ERR_SIZE_MISMATCH;
            }
            if (1U != read_le16(chunk + 0U))
            {
                return MVP_ERR_ARG;
            }
            out->channels = read_le16(chunk + 2U);
            out->sample_rate = read_le32(chunk + 4U);
            out->bits_per_sample = read_le16(chunk + 14U);
        }
        else if (0 == memcmp(wav + pos, "data", 4U))
        {
            out->pcm_data = chunk;
            out->pcm_bytes = chunk_size;
        }

        pos += 8U + chunk_size;
        if (chunk_size & 1U)
        {
            pos++;
        }
    }

    if ((NULL == out->pcm_data) || (0U == out->pcm_bytes))
    {
        return MVP_ERR_ARG;
    }

    if ((out->sample_rate != SAMPLE_RATE) || (out->channels != AUDIO_CHANNELS) || (out->bits_per_sample != 16U))
    {
        return MVP_ERR_ARG;
    }

    return MVP_OK;
}

static int recv_line_with_timeout(int socket, char * out_line, size_t out_line_size, uint32_t timeout_ms)
{
    uint8_t buf[256];
    size_t used = 0U;
    uint32_t deadline = w800_now_ms() + timeout_ms;
    uint32_t skrcv_err4_count = 0U;

    if ((NULL == out_line) || (out_line_size < 2U))
    {
        return MVP_ERR_ARG;
    }

    out_line[0] = '\0';

    while ((int32_t) (w800_now_ms() - deadline) < 0)
    {
        size_t got = 0U;
        int rc = w800_read_bytes_any(buf, sizeof(buf), &got, 2U);
        if ((MVP_OK == rc) && (got > 0U))
        {
            size_t i;
            for (i = 0U; i < got; i++)
            {
                if ((buf[i] == '\0') || (buf[i] == '\r') || (buf[i] == '\n'))
                {
                    if (0U == used)
                    {
                        continue;
                    }

                    out_line[used] = '\0';
                    return MVP_OK;
                }

                if ((used + 1U) >= out_line_size)
                {
                    return MVP_ERR_OVERFLOW;
                }

                out_line[used++] = (char) buf[i];
            }

            out_line[used] = '\0';
        }

        got = 0U;
        rc = w800_socket_recv(socket, buf, sizeof(buf), &got);
        if (MVP_OK == rc)
        {
            if (got > 0U)
            {
                size_t i;
                for (i = 0U; i < got; i++)
                {
                    if ((buf[i] == '\0') || (buf[i] == '\r') || (buf[i] == '\n'))
                    {
                        if (0U == used)
                        {
                            continue;
                        }

                        if (used < out_line_size)
                        {
                            out_line[used] = '\0';
                            return MVP_OK;
                        }
                        return MVP_ERR_OVERFLOW;
                    }

                    if ((used + 1U) >= out_line_size)
                    {
                        return MVP_ERR_OVERFLOW;
                    }

                    out_line[used++] = (char) buf[i];
                }
                out_line[used] = '\0';
            }

            R_BSP_SoftwareDelay(20U, BSP_DELAY_UNITS_MILLISECONDS);
            continue;
        }

        if (MVP_ERR_AT == rc)
        {
            skrcv_err4_count++;
            if ((1U == skrcv_err4_count) || ((skrcv_err4_count % 10U) == 0U))
            {
                LOGW("speaker recv: SKRCV not ready yet rc=%d try=%lu",
                     rc, (unsigned long) skrcv_err4_count);
            }

            R_BSP_SoftwareDelay(30U, BSP_DELAY_UNITS_MILLISECONDS);
            continue;
        }

        if (MVP_ERR_TIMEOUT == rc)
        {
            R_BSP_SoftwareDelay(20U, BSP_DELAY_UNITS_MILLISECONDS);
            continue;
        }

        return rc;
    }

    return MVP_ERR_TIMEOUT;
}

int speaker_demo_client_init(void)
{
    return w800_socket_set_report_mode(0);
}

int speaker_demo_client_send_clip(const test_audio_clip_t * clip, char * out_line, size_t out_line_size)
{
    wav_view_t wav;
    char header[160];
    int sock = -1;
    int rc;
    uint32_t samples;
    size_t header_len;

    if ((NULL == clip) || (NULL == out_line) || (out_line_size < 2U))
    {
        return MVP_ERR_ARG;
    }

    rc = wav_parse(clip->wav_data, clip->wav_size, &wav);
    if (MVP_OK != rc)
    {
        return rc;
    }

    samples = (uint32_t) (wav.pcm_bytes / 2U);

    if (TEST_AUDIO_ROLE_ENROLL == clip->role)
    {
        header_len = (size_t) snprintf(header,
                                       sizeof(header),
                                       "SVR,1,%lu,%u,%s,%u,%u,%lu,16,1,%lu,%lu\n",
                                       (unsigned long) (clip->utter_idx + 1U),
                                       (unsigned int) clip->utter_idx,
                                       clip->speaker_id,
                                       (unsigned int) clip->utter_idx,
                                       (unsigned int) clip->utter_total,
                                       (unsigned long) wav.sample_rate,
                                       (unsigned long) samples,
                                       (unsigned long) wav.pcm_bytes);
    }
    else
    {
        if ((NULL != clip->expected_speaker_id) && ('\0' != clip->expected_speaker_id[0]))
        {
            header_len = (size_t) snprintf(header,
                                           sizeof(header),
                                           "SVI,1,%lu,%u,%s,%lu,16,1,%lu,%lu\n",
                                           (unsigned long) (clip->utter_idx + 1U),
                                           (unsigned int) clip->utter_idx,
                                           clip->expected_speaker_id,
                                           (unsigned long) wav.sample_rate,
                                           (unsigned long) samples,
                                           (unsigned long) wav.pcm_bytes);
        }
        else
        {
            header_len = (size_t) snprintf(header,
                                           sizeof(header),
                                           "SVI,1,%lu,%u,%lu,16,1,%lu,%lu\n",
                                           (unsigned long) (clip->utter_idx + 1U),
                                           (unsigned int) clip->utter_idx,
                                           (unsigned long) wav.sample_rate,
                                           (unsigned long) samples,
                                           (unsigned long) wav.pcm_bytes);
        }
    }

    if ((0U == header_len) || (header_len >= sizeof(header)))
    {
        return MVP_ERR_OVERFLOW;
    }

    rc = w800_socket_open_tcp(SPEAKER_SERVER_IP, (uint16_t) SPEAKER_SERVER_PORT, (uint16_t) SPEAKER_SERVER_LOCAL_PORT, &sock);
    if (MVP_OK != rc)
    {
        return rc;
    }

    rc = w800_socket_send_two(sock, (const uint8_t *) header, header_len, wav.pcm_data, wav.pcm_bytes);
    if (MVP_OK == rc)
    {
        /* Match the hardened HTTP path: let W800 leave raw send mode and
         * clear trailing CR/LF before querying SKSTT/SKRCV. */
        R_BSP_SoftwareDelay(W800_HTTP_POST_SEND_GUARD_MS, BSP_DELAY_UNITS_MILLISECONDS);
        w800_uart_drain_rx();
        rc = recv_line_with_timeout(sock, out_line, out_line_size, SPEAKER_RESPONSE_TIMEOUT_MS);
    }

    if (sock >= 0)
    {
        (void) w800_socket_close(sock);
    }

    return rc;
}

