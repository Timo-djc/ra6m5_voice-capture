#include "log.h"

#include "../audio_capture.h"

#include <stdio.h>
#include <string.h>

void mvp_log_vprintf(const char * level, const char * fmt, va_list args)
{
    char body[300];
    char line[340];
    int body_len;
    int line_len;

    if ((NULL == level) || (NULL == fmt))
    {
        return;
    }

    body_len = vsnprintf(body, sizeof(body), fmt, args);
    if (body_len < 0)
    {
        return;
    }

    line_len = snprintf(line, sizeof(line), "[%s] %s", level, body);
    if (line_len < 0)
    {
        return;
    }

    if ((line_len >= 2) && (line[line_len - 1] != '\n'))
    {
        if (line_len < (int) sizeof(line) - 2)
        {
            line[line_len++] = '\r';
            line[line_len++] = '\n';
            line[line_len] = '\0';
        }
    }

    audio_debug_write_bytes_blocking((const uint8_t *) line, (uint32_t) strlen(line));
}

void mvp_log_printf(const char * level, const char * fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    mvp_log_vprintf(level, fmt, args);
    va_end(args);
}
