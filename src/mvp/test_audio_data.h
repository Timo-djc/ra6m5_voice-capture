#ifndef MVP_TEST_AUDIO_DATA_H_
#define MVP_TEST_AUDIO_DATA_H_

#include <stddef.h>
#include <stdint.h>

const uint8_t * test_audio_get_data(void);
size_t test_audio_get_size(void);
const char * test_audio_get_name(void);

#endif
