#ifndef MVP_CLOUD_SPEAKER_DEMO_CLIENT_H_
#define MVP_CLOUD_SPEAKER_DEMO_CLIENT_H_

#include "test_audio_data.h"

#include <stddef.h>

int speaker_demo_client_init(void);
int speaker_demo_client_send_clip(const test_audio_clip_t * clip, char * out_line, size_t out_line_size);

#endif
