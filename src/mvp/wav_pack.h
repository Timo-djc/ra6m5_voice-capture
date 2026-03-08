#ifndef MVP_WAV_PACK_H_
#define MVP_WAV_PACK_H_

#include <stddef.h>
#include <stdint.h>

int wav_pack_from_pcm16_mono(uint8_t * out,
                             size_t out_size,
                             const int16_t * pcm,
                             size_t samples,
                             uint32_t sample_rate,
                             size_t * wav_size);

#endif
