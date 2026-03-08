#ifndef MVP_HTTP_PARSER_LITE_H_
#define MVP_HTTP_PARSER_LITE_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct st_http_lite_response
{
    int status_code;
    int content_length;
    const uint8_t * body;
    size_t body_len;
    size_t header_len;
    bool complete;
} http_lite_response_t;

int http_lite_parse(const uint8_t * data, size_t size, http_lite_response_t * out);
int http_lite_extract_json_result(const uint8_t * body,
                                  size_t body_len,
                                  int * out_code,
                                  char * out_text,
                                  size_t text_size,
                                  float * out_confidence);

#endif
