#ifndef CLOUD_ASR_CLIENT_H_
#define CLOUD_ASR_CLIENT_H_

#include <stdbool.h>
#include <stdint.h>

void cloud_asr_client_init(void);
void cloud_asr_client_poll(void);
void cloud_asr_client_on_pcm_slot(uint8_t slot_idx, const int16_t * pcm, uint32_t samples);
void cloud_asr_client_suspend(void);
void cloud_asr_client_restart(void);
bool cloud_asr_client_is_suspended(void);

#endif
