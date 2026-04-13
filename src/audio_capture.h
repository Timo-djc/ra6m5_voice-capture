#ifndef AUDIO_CAPTURE_H_
#define AUDIO_CAPTURE_H_

#include "hal_data.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct st_audio_stats
{
    uint32_t sample_count;
    uint32_t rms;
    uint16_t peak;
    uint32_t clipping_count;
    uint32_t overflow_count;
    uint32_t restart_count;
    uint32_t error_count;
    uint32_t callback_interval_us;
    uint32_t estimated_sample_rate_hz;
    uint32_t callbacks_in_window;
    uint32_t max_callback_gap_us;
    uint32_t samples_in_window;
    uint8_t read_submit_ok;
    int32_t read_submit_err_code;
    uint32_t no_callback_timeout_count;
    uint8_t sample_rate_ok;
    uint8_t callback_gap_ok;
    uint8_t error_growth_ok;
    uint8_t audio_sanity_ok;
    uint32_t ws_edge_observed;
    uint32_t bck_edge_observed;
    uint32_t pin_read_error_count;
    uint32_t ssi_state;
} audio_stats_t;

#define AUDIO_SPEAKER_ID_MAX_LEN    (32U)

typedef struct st_audio_speaker_result
{
    uint8_t known;
    float score;
    char speaker_id[AUDIO_SPEAKER_ID_MAX_LEN + 1U];
} audio_speaker_result_t;

typedef struct st_audio_speaker_enroll_progress
{
    uint8_t accepted;
    uint8_t required;
    float score;
    char speaker_id[AUDIO_SPEAKER_ID_MAX_LEN + 1U];
} audio_speaker_enroll_progress_t;

void audio_capture_init(void);
void audio_capture_start(void);
void audio_capture_process(void);
void audio_debug_only_init(void);
void audio_capture_run_inference_if_ready(void);
void audio_capture_ptt_press(void);
void audio_capture_ptt_release(void);
bool audio_capture_is_ptt_active(void);
bool audio_capture_consume_digit_result(int * out_digit, float * out_confidence);
bool audio_capture_consume_speaker_result(audio_speaker_result_t * out_result);
bool audio_capture_consume_enroll_progress(audio_speaker_enroll_progress_t * out_progress);
bool audio_capture_consume_slot_pcm16(int16_t * dst, uint32_t max_samples, uint32_t * out_samples, uint8_t * out_slot_index);
void audio_capture_publish_digit_result(int digit, float conf);
void audio_capture_publish_speaker_result(uint8_t known, const char * speaker_id, float score);
void audio_capture_publish_enroll_progress(const char * speaker_id, uint8_t accepted, uint8_t required, float score);
const audio_stats_t * audio_capture_stats_get(void);

bool audio_debug_write_bytes(const uint8_t * data, uint32_t length);
void audio_debug_write_bytes_blocking(const uint8_t * data, uint32_t length);
uint32_t audio_debug_read_bytes(uint8_t * out, uint32_t max_bytes);
bool audio_debug_tx_idle(void);
void audio_debug_set_muted(bool muted);
bool audio_debug_is_muted(void);
void audio_debug_write_line_force(const char * str);

void i2s0_callback(i2s_callback_args_t * p_args);
void uart5_callback(uart_callback_args_t * p_args);
void uart6_callback(uart_callback_args_t * p_args);
void uart7_callback(uart_callback_args_t * p_args);

#endif
