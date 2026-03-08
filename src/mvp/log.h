#ifndef MVP_LOG_H_
#define MVP_LOG_H_

#include <stdarg.h>

void mvp_log_printf(const char * level, const char * fmt, ...);
void mvp_log_vprintf(const char * level, const char * fmt, va_list args);

#define LOGI(...) mvp_log_printf("I", __VA_ARGS__)
#define LOGW(...) mvp_log_printf("W", __VA_ARGS__)
#define LOGE(...) mvp_log_printf("E", __VA_ARGS__)

#endif
