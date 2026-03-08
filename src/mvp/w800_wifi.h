#ifndef MVP_W800_WIFI_H_
#define MVP_W800_WIFI_H_

#include <stdbool.h>
#include <stddef.h>

int w800_basic_check(void);
int w800_set_echo(bool on);
int w800_reset(void);
int w800_set_mode_sta(void);
int w800_query_mode(char * out, size_t out_size);
int w800_set_ssid(const char * ssid);
int w800_query_ssid(char * out, size_t out_size);
int w800_set_key_ascii(const char * password);
int w800_query_key(char * out, size_t out_size);
int w800_set_dhcp(void);
int w800_save_config(void);
int w800_join_ap(void);
int w800_leave_ap(void);
int w800_get_link_status(char * out, size_t out_size);
int w800_scan_ap(char * out, size_t out_size);

#endif
