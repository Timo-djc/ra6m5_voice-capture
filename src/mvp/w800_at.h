#ifndef MVP_W800_AT_H_
#define MVP_W800_AT_H_

#include "hal_data.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int w800_init(void);
int w800_set_baud(uint32_t baud);
uint32_t w800_now_ms(void);

int w800_send_cmd(const char * cmd, const char * expect, uint32_t timeout_ms);
int w800_send_cmd_multi(const char * cmd,
                        const char * expect1,
                        const char * expect2,
                        uint32_t timeout_ms);
int w800_read_response(char * out, size_t out_size, uint32_t timeout_ms);
int w800_wait_ok(uint32_t timeout_ms);
int w800_wait_prompt_or_ok(char prompt, char * out, size_t out_size, uint32_t timeout_ms);

int w800_cmd_collect(const char * cmd, char * out, size_t out_size, uint32_t timeout_ms);
int w800_wait_for_user_task(uint32_t timeout_ms);

int w800_write_raw(const uint8_t * data, size_t size, uint32_t timeout_ms);
int w800_read_bytes_exact(uint8_t * out, size_t size, uint32_t timeout_ms);
int w800_read_bytes_any(uint8_t * out, size_t max_size, size_t * out_size, uint32_t timeout_ms);

void w800_uart_rx_byte(uint8_t byte);
void w800_uart_drain_rx(void);
bool w800_tx_idle(void);

void w800_hw_reset(void);

void mvp_uart5_callback(uart_callback_args_t * p_args);
void mvp_uart6_callback(uart_callback_args_t * p_args);

#endif
