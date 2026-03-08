#include "wav_pack.h"

#include "config.h"

#include <string.h>

static void put_le16(uint8_t * p, uint16_t v)
{
    p[0] = (uint8_t) (v & 0xFFU);
    p[1] = (uint8_t) ((v >> 8) & 0xFFU);
}

static void put_le32(uint8_t * p, uint32_t v)
{
    p[0] = (uint8_t) (v & 0xFFU);
    p[1] = (uint8_t) ((v >> 8) & 0xFFU);
    p[2] = (uint8_t) ((v >> 16) & 0xFFU);
    p[3] = (uint8_t) ((v >> 24) & 0xFFU);
}

int wav_pack_from_pcm16_mono(uint8_t * out,
                             size_t out_size,
                             const int16_t * pcm,
                             size_t samples,
                             uint32_t sample_rate,
                             size_t * wav_size)
{
    uint32_t data_bytes;
    uint32_t riff_size;
    uint32_t byte_rate;
    uint32_t i;

    if ((NULL == out) || (NULL == pcm) || (NULL == wav_size))
    {
        return MVP_ERR_ARG;
    }

    data_bytes = (uint32_t) (samples * sizeof(int16_t));
    riff_size = 36U + data_bytes;
    byte_rate = sample_rate * 2U;

    if (out_size < (size_t) (44U + data_bytes))
    {
        return MVP_ERR_OVERFLOW;
    }

    memcpy(out + 0, "RIFF", 4U);
    put_le32(out + 4, riff_size);
    memcpy(out + 8, "WAVE", 4U);

    memcpy(out + 12, "fmt ", 4U);
    put_le32(out + 16, 16U);
    put_le16(out + 20, 1U);
    put_le16(out + 22, 1U);
    put_le32(out + 24, sample_rate);
    put_le32(out + 28, byte_rate);
    put_le16(out + 32, 2U);
    put_le16(out + 34, 16U);

    memcpy(out + 36, "data", 4U);
    put_le32(out + 40, data_bytes);

    for (i = 0U; i < samples; i++)
    {
        put_le16(out + 44U + (i * 2U), (uint16_t) pcm[i]);
    }

    *wav_size = (size_t) (44U + data_bytes);
    return MVP_OK;
}
