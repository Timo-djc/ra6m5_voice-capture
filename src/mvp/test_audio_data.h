#ifndef MVP_TEST_AUDIO_DATA_H_
#define MVP_TEST_AUDIO_DATA_H_

#include <stddef.h>
#include <stdint.h>

typedef enum e_test_audio_role
{
    TEST_AUDIO_ROLE_ENROLL = 0,
    TEST_AUDIO_ROLE_IDENTIFY = 1
} test_audio_role_t;

typedef struct st_test_audio_clip
{
    const char * name;
    const char * speaker_id;
    const char * expected_speaker_id;
    const uint8_t * wav_data;
    size_t wav_size;
    uint8_t role;
    uint8_t utter_idx;
    uint8_t utter_total;
} test_audio_clip_t;

size_t test_audio_clip_count(void);
const test_audio_clip_t * test_audio_get_clip(size_t index);

#endif
