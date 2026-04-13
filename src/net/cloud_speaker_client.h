#ifndef CLOUD_SPEAKER_CLIENT_H_
#define CLOUD_SPEAKER_CLIENT_H_

#include <stdbool.h>
#include <stdint.h>

void cloud_speaker_client_init(void);
void cloud_speaker_client_poll(void);
void cloud_speaker_client_suspend(void);
void cloud_speaker_client_restart(void);
bool cloud_speaker_client_is_suspended(void);
bool cloud_speaker_client_start_identify(void);
bool cloud_speaker_client_start_enroll(const char * speaker_id, uint8_t utter_idx, uint8_t utter_total);
bool cloud_speaker_client_is_busy(void);

#endif
