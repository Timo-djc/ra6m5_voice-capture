#ifndef MVP_CLOUD_HTTP_CLIENT_H_
#define MVP_CLOUD_HTTP_CLIENT_H_

#include <stddef.h>
#include <stdint.h>

int cloud_client_init(void);
int cloud_client_upload_wav_once(const uint8_t * wav, size_t wav_size);
int cloud_client_get_last_result(char * text, size_t text_size, float * confidence);

#endif
