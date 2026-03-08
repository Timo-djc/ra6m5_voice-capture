#ifndef W800_AT_H_
#define W800_AT_H_

#include "hal_data.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum e_w800_bridge_state
{
    W800_BRIDGE_STATE_NORMAL_APP = 0,
    W800_BRIDGE_STATE_FLASH_BRIDGE,
    W800_BRIDGE_STATE_BRIDGE_CMD,
    W800_BRIDGE_STATE_RUN_BOOT_WAIT
} w800_bridge_state_t;

bool w800_at_init(void);
void w800_at_poll(void);
void w800_at_reset_rx(void);

bool w800_at_set_baud(uint32_t baud);
uint32_t w800_at_get_baud(void);

bool w800_at_send(const uint8_t * data, uint32_t len);
bool w800_at_send_str(const char * str);

bool w800_at_read_line(char * out, uint32_t out_size);
bool w800_at_read_bytes(uint8_t * out, uint32_t max_bytes, uint32_t * out_read);

bool w800_at_tx_idle(void);
bool w800_at_last_tx_ok(void);

void w800_at_set_boot_level(bsp_io_level_t level);
void w800_at_set_wakeup_level(bsp_io_level_t level);
void w800_at_set_reset_level(bsp_io_level_t level);
void w800_at_hard_reset(void);
void w800_enter_flash_mode(void);
void w800_enter_run_mode(void);

void w800_bridge_poll(void);
bool w800_bridge_is_active(void);
w800_bridge_state_t w800_bridge_get_state(void);

void uart5_callback(uart_callback_args_t * p_args);
void uart6_callback(uart_callback_args_t * p_args);

#endif
