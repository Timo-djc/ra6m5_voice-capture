#include "audio_capture.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "fmt_float.h"
#include "net/cloud_asr_cfg.h"

/* ------ VAD & TFLM Model Integration Includes ------ */
#if ASR_MODE_LOCAL
#include "audio_features.h"
#include "digit_recognizer.h"
#endif
#include "drivers/drv_touch.h"

#define AUDIO_SAMPLE_RATE_HZ      (16000U)
#define AUDIO_FRAME_HOP_HZ        (100U)
#define FRAME_FRAMES              (AUDIO_SAMPLE_RATE_HZ / AUDIO_FRAME_HOP_HZ)
#define FRAME_CHANNELS            (2U)
#define BUFFER_COUNT              (2U)
#define RX_SAMPLES_PER_BUFFER     (FRAME_FRAMES * FRAME_CHANNELS)
#define RX_BYTES_PER_BUFFER       (RX_SAMPLES_PER_BUFFER * 4U)
#define LOG_WINDOW_US             (1000000U)
#define SAMPLE_RATE_TOLERANCE_PCT (2U)
#define CALLBACK_MIN_RATIO_NUM    (8U)
#define CALLBACK_MIN_RATIO_DEN    (10U)
#define CALLBACK_GAP_LIMIT_MULT   (3U)
#define AUDIO_SANITY_MIN_RMS_DELTA (300U)
#define EXPECTED_CALLBACK_INTERVAL_US ((FRAME_FRAMES * 1000000U) / AUDIO_SAMPLE_RATE_HZ)
#define EXPECTED_CALLBACKS_PER_SECOND (AUDIO_SAMPLE_RATE_HZ / FRAME_FRAMES)
#define AUDIO_BIT_CLOCK_HZ        (AUDIO_SAMPLE_RATE_HZ * FRAME_CHANNELS * 32U)
#define I2S_BCK_PIN               (BSP_IO_PORT_01_PIN_12)
#define I2S_WS_PIN                (BSP_IO_PORT_01_PIN_13)
#define PIN_SAMPLE_DECIMATION     (64U)
#define KEY_MIN_HOLD_MS           (250U)
#define ENABLE_VAD_TRIGGER        (0U)
#define HPF_ALPHA                 (0.995f)
#define DIGITAL_GAIN              (2)
#define UART7_RX_RING_SIZE        (8192U)

/* ------ Ring Buffer for trigger history ------ */
#define HISTORY_SAMPLES         (AUDIO_SAMPLE_RATE_HZ * 15 / 10) /* 1.5 seconds */
#define INFERENCE_SAMPLES       (AUDIO_SAMPLE_RATE_HZ * 15 / 10) /* model expects 1.2s */
#define TRIM_TOP_DB             (30.0f)
#define TRIM_PAD_SAMPLES        ((AUDIO_SAMPLE_RATE_HZ * 3U) / 100U) /* 30ms */
#define TRIM_EDGE_IGNORE_SAMPLES ((AUDIO_SAMPLE_RATE_HZ * 5U) / 100U) /* ignore first/last 50ms for trim peak search */
#define TRIM_MIN_ACTIVE_SAMPLES  (AUDIO_SAMPLE_RATE_HZ / 10U)         /* at least 100ms active region */
#if ASR_MODE_LOCAL
static float s_audio_history[HISTORY_SAMPLES];
static uint32_t s_history_idx = 0;
static bool s_history_full = false;
static uint32_t s_history_count = 0U;
#endif

#if ASR_MODE_CLOUD
static int16_t s_slot_pcm_ring[HISTORY_SAMPLES];
static uint32_t s_slot_ring_idx = 0U;
static bool s_slot_ring_full = false;
static uint32_t s_slot_ring_count = 0U;
static volatile bool s_slot_ready = false;
static volatile uint8_t s_slot_ready_index = 0U;
static uint8_t s_slot_sequence = 0U;
#endif

/* VAD settings */
#define VAD_ENERGY_THRESHOLD 40.0f
#if (ASR_MODE_LOCAL && (ENABLE_VAD_TRIGGER != 0U))
static uint32_t s_speech_frames = 0;
static uint32_t s_silence_frames = 0;
static bool s_in_speech = false;
static volatile bool s_run_inference_flag = false;
#endif
static volatile bool s_manual_trigger_flag = false;
static bool s_key_pressed = false;
static uint32_t s_key_press_time_us = 0U;
static volatile uint8_t s_key_press_log_pending = 0U;
static volatile uint8_t s_key_release_log_pending = 0U;
static volatile uint32_t s_key_last_hold_ms = 0U;
static float s_hpf_prev_x = 0.0f;
static float s_hpf_prev_y = 0.0f;
static volatile bool s_digit_result_ready = false;
static int s_digit_result_value = -1;
static float s_digit_result_confidence = 0.0f;
static volatile bool s_speaker_result_ready = false;
static uint8_t s_speaker_result_known = 0U;
static float s_speaker_result_score = 0.0f;
static char s_speaker_result_id[AUDIO_SPEAKER_ID_MAX_LEN + 1U];
static volatile bool s_speaker_enroll_ready = false;
static uint8_t s_speaker_enroll_accepted = 0U;
static uint8_t s_speaker_enroll_required = 0U;
static float s_speaker_enroll_score = 0.0f;
static char s_speaker_enroll_id[AUDIO_SPEAKER_ID_MAX_LEN + 1U];

#if ASR_MODE_LOCAL
/* TFLM features buffer */
static float s_model_features[6000];

/* Full inference input buffer (1.5s x 16kHz) */
static float s_model_input_audio[INFERENCE_SAMPLES];
#endif

/* =========================================================================
 * IMA ADPCM Encoder Implementation
 * ========================================================================= */
static const int16_t ima_step_size_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

typedef struct {
    int32_t prev_sample;
    int32_t prev_index;
} adpcm_state_t;

static adpcm_state_t s_adpcm_state = {0, 0};

/* 编码单个 16-bit 样本为 4-bit IMA ADPCM nibble */
static uint8_t __attribute__((unused)) adpcm_encode_sample(int16_t sample, adpcm_state_t *state)
{
    int32_t diff = sample - state->prev_sample;
    int32_t step = ima_step_size_table[state->prev_index];
    int32_t vpdiff = step >> 3;
    uint8_t nibble = 0;

    if (diff < 0) {
        nibble = 8;
        diff = -diff;
    }

    if (diff >= step) {
        nibble |= 4;
        diff -= step;
        vpdiff += step;
    }
    step >>= 1;

    if (diff >= step) {
        nibble |= 2;
        diff -= step;
        vpdiff += step;
    }
    step >>= 1;

    if (diff >= step) {
        nibble |= 1;
        vpdiff += step;
    }

    if (nibble & 8) {
        state->prev_sample -= vpdiff;
    } else {
        state->prev_sample += vpdiff;
    }

    if (state->prev_sample > 32767) {
        state->prev_sample = 32767;
    } else if (state->prev_sample < -32768) {
        state->prev_sample = -32768;
    }

    state->prev_index += ima_index_table[nibble];
    if (state->prev_index < 0) {
        state->prev_index = 0;
    } else if (state->prev_index > 88) {
        state->prev_index = 88;
    }

    return nibble;
}

/* =========================================================================
 * COBS Encoder Implementation
 * ========================================================================= */
static uint32_t __attribute__((unused)) cobs_encode(const uint8_t *src, uint32_t length, uint8_t *dst)
{
    uint32_t read_index = 0;
    uint32_t write_index = 1;
    uint32_t code_index = 0;
    uint8_t code = 1;

    while (read_index < length) {
        if (src[read_index] == 0) {
            dst[code_index] = code;
            code = 1;
            code_index = write_index++;
        } else {
            dst[write_index++] = src[read_index];
            code++;
            if (code == 0xFF) {
                dst[code_index] = code;
                code = 1;
                code_index = write_index++;
            }
        }
        read_index++;
    }
    dst[code_index] = code;
    return write_index;
}

static int32_t s_rx_buffer[BUFFER_COUNT][RX_SAMPLES_PER_BUFFER];
static int32_t s_tx_buffer[BUFFER_COUNT][RX_SAMPLES_PER_BUFFER];

static volatile uint8_t s_active_buffer = 0U;
static volatile uint32_t s_ready_mask = 0U;
static volatile bool s_restart_pending = false;
static volatile bool s_uart_tx_done = true;
static bool s_capture_started = false;
static bool s_uart_opened = false;
static bool s_i2s_opened = false;
static bool s_timer_opened = false;
static volatile uint8_t s_uart7_rx_ring[UART7_RX_RING_SIZE];
static volatile uint32_t s_uart7_rx_head = 0U;
static volatile uint32_t s_uart7_rx_tail = 0U;
static volatile bool s_uart_log_muted = false;

static uint32_t s_core_hz = 0U;
static uint32_t s_last_callback_cycle = 0U;
static uint32_t s_window_start_cycle = 0U;
static uint32_t s_log_second_index = 0U;

static uint64_t s_window_energy = 0U;
static uint32_t s_window_samples = 0U;
static uint32_t s_window_clipping = 0U;
static uint16_t s_window_peak = 0U;
static volatile uint32_t s_window_callback_count = 0U;
static volatile uint32_t s_window_max_callback_gap_us = 0U;
static volatile uint32_t s_window_error_count = 0U;
static uint32_t s_window_ws_edges = 0U;
static uint32_t s_window_bck_edges = 0U;
static uint32_t s_window_pin_read_errors = 0U;
static volatile uint8_t s_read_submit_ok = 1U;
static volatile int32_t s_read_submit_err_code = 0;

static uint32_t s_prev_overflow_count = 0U;
static uint32_t s_prev_restart_count = 0U;
static uint32_t s_prev_error_count = 0U;
static uint32_t s_rms_min_observed = 0xFFFFFFFFUL;
static uint32_t s_rms_max_observed = 0U;
static bool s_pin_prev_valid = false;
static uint8_t s_prev_bck_level = 0U;
static uint8_t s_prev_ws_level = 0U;
static uint32_t s_pin_sample_divider = 0U;
static bool s_hw_link_hint_printed = false;

static audio_stats_t s_stats;
static void uart_write_line(char const * fmt, ...);

static uint32_t uart7_ring_next(uint32_t idx)
{
    return (idx + 1U) % UART7_RX_RING_SIZE;
}

static void uart7_ring_push(uint8_t ch)
{
    uint32_t next = uart7_ring_next(s_uart7_rx_head);

    if (next == s_uart7_rx_tail)
    {
        s_uart7_rx_tail = uart7_ring_next(s_uart7_rx_tail);
    }

    s_uart7_rx_ring[s_uart7_rx_head] = ch;
    s_uart7_rx_head = next;
}

static bool uart7_write_bytes_internal(const uint8_t * data, uint32_t length, bool wait_complete)
{
    fsp_err_t err;

    if ((!s_uart_opened) || (NULL == data) || (0U == length))
    {
        return false;
    }

    if (!s_uart_tx_done)
    {
        return false;
    }

    s_uart_tx_done = false;
    err = g_uart7.p_api->write(g_uart7.p_ctrl, data, length);
    if (FSP_SUCCESS != err)
    {
        s_uart_tx_done = true;
        return false;
    }

    if (wait_complete)
    {
        uint32_t timeout = 4000000U;
        while ((!s_uart_tx_done) && (timeout > 0U))
        {
            timeout--;
            __NOP();
        }
    }

    return true;
}

static uint32_t isqrt_u64(uint64_t value)
{
    uint64_t op = value;
    uint64_t res = 0U;
    uint64_t one = 1ULL << 62;

    while (one > op)
    {
        one >>= 2;
    }

    while (one != 0U)
    {
        if (op >= (res + one))
        {
            op -= (res + one);
            res = (res >> 1) + one;
        }
        else
        {
            res >>= 1;
        }

        one >>= 2;
    }

    return (uint32_t) res;
}

static uint32_t cycles_to_us(uint32_t cycles)
{
    if (0U == s_core_hz)
    {
        return 0U;
    }

    return (uint32_t) (((uint64_t) cycles * 1000000ULL) / s_core_hz);
}

static void record_task_error(void)
{
    __disable_irq();
    s_stats.error_count++;
    s_window_error_count++;
    __enable_irq();
}

static void reset_window_counters(uint32_t now_cycle)
{
    s_window_start_cycle = now_cycle;
    s_window_energy = 0U;
    s_window_samples = 0U;
    s_window_clipping = 0U;
    s_window_peak = 0U;
    s_window_ws_edges = 0U;
    s_window_bck_edges = 0U;
    s_window_pin_read_errors = 0U;
    __disable_irq();
    s_window_callback_count = 0U;
    s_window_max_callback_gap_us = 0U;
    s_window_error_count = 0U;
    __enable_irq();
}

static void monitor_i2s_pin_activity(void)
{
    s_pin_sample_divider++;
    if (s_pin_sample_divider < PIN_SAMPLE_DECIMATION)
    {
        return;
    }
    s_pin_sample_divider = 0U;

    bsp_io_level_t bck_level = BSP_IO_LEVEL_LOW;
    bsp_io_level_t ws_level = BSP_IO_LEVEL_LOW;
    fsp_err_t bck_err = g_ioport.p_api->pinRead(g_ioport.p_ctrl, I2S_BCK_PIN, &bck_level);
    fsp_err_t ws_err = g_ioport.p_api->pinRead(g_ioport.p_ctrl, I2S_WS_PIN, &ws_level);
    if ((FSP_SUCCESS != bck_err) || (FSP_SUCCESS != ws_err))
    {
        s_window_pin_read_errors++;
        return;
    }

    uint8_t bck_u8 = (uint8_t) bck_level;
    uint8_t ws_u8 = (uint8_t) ws_level;
    if (s_pin_prev_valid)
    {
        if (bck_u8 != s_prev_bck_level)
        {
            s_window_bck_edges++;
        }

        if (ws_u8 != s_prev_ws_level)
        {
            s_window_ws_edges++;
        }
    }

    s_prev_bck_level = bck_u8;
    s_prev_ws_level = ws_u8;
    s_pin_prev_valid = true;
}

static void process_key_trigger_logs(void)
{
    uint8_t press_pending = 0U;
    uint8_t release_pending = 0U;
    uint32_t hold_ms = 0U;

    __disable_irq();
    press_pending = s_key_press_log_pending;
    release_pending = s_key_release_log_pending;
    hold_ms = s_key_last_hold_ms;
    s_key_press_log_pending = 0U;
    s_key_release_log_pending = 0U;
    __enable_irq();

    if (press_pending != 0U)
    {
        uart_write_line("KEY: press\r\n");
    }

    if (release_pending != 0U)
    {
        uart_write_line("KEY: release hold_ms=%lu\r\n", (unsigned long) hold_ms);
    }
}

static void start_key_recording_session(void)
{
#if ASR_MODE_LOCAL
    s_history_idx = 0U;
    s_history_full = false;
    s_history_count = 0U;
#endif
#if ASR_MODE_CLOUD
    s_slot_ring_idx = 0U;
    s_slot_ring_full = false;
    s_slot_ring_count = 0U;
    s_slot_ready = false;
#endif
}

void audio_capture_ptt_press(void)
{
    uint32_t now_us = cycles_to_us(DWT->CYCCNT);

    if (s_key_pressed)
    {
        return;
    }

    s_key_pressed = true;
    s_key_press_time_us = now_us;
    start_key_recording_session();
    s_key_press_log_pending = 1U;
}

void audio_capture_ptt_release(void)
{
    uint32_t now_us;
    uint32_t hold_ms;

    if (!s_key_pressed)
    {
        return;
    }

    now_us = cycles_to_us(DWT->CYCCNT);
    hold_ms = (now_us - s_key_press_time_us) / 1000U;
    s_key_pressed = false;
    s_key_last_hold_ms = hold_ms;
    s_key_release_log_pending = 1U;

    if (hold_ms >= KEY_MIN_HOLD_MS)
    {
#if ASR_MODE_LOCAL
        s_manual_trigger_flag = true;
#endif
#if ASR_MODE_CLOUD
        __disable_irq();
        if (s_slot_ring_count > 0U)
        {
            s_slot_ready = true;
            s_slot_ready_index = s_slot_sequence;
            s_slot_sequence = (uint8_t) ((s_slot_sequence + 1U) & 0x03U);
        }
        __enable_irq();
#endif
    }
}

bool audio_capture_is_ptt_active(void)
{
    return s_key_pressed;
}

bool audio_capture_consume_digit_result(int * out_digit, float * out_confidence)
{
    bool ready;

    __disable_irq();
    ready = s_digit_result_ready;
    if (ready)
    {
        s_digit_result_ready = false;
    }
    __enable_irq();

    if (!ready)
    {
        return false;
    }

    if (out_digit != NULL)
    {
        *out_digit = s_digit_result_value;
    }

    if (out_confidence != NULL)
    {
        *out_confidence = s_digit_result_confidence;
    }

    return true;
}

bool audio_capture_consume_speaker_result(audio_speaker_result_t * out_result)
{
    bool ready;

    if (NULL == out_result)
    {
        return false;
    }

    __disable_irq();
    ready = s_speaker_result_ready;
    if (ready)
    {
        s_speaker_result_ready = false;
        out_result->known = s_speaker_result_known;
        out_result->score = s_speaker_result_score;
        memcpy(out_result->speaker_id, s_speaker_result_id, sizeof(out_result->speaker_id));
    }
    __enable_irq();

    return ready;
}

bool audio_capture_consume_enroll_progress(audio_speaker_enroll_progress_t * out_progress)
{
    bool ready;

    if (NULL == out_progress)
    {
        return false;
    }

    __disable_irq();
    ready = s_speaker_enroll_ready;
    if (ready)
    {
        s_speaker_enroll_ready = false;
        out_progress->accepted = s_speaker_enroll_accepted;
        out_progress->required = s_speaker_enroll_required;
        out_progress->score = s_speaker_enroll_score;
        memcpy(out_progress->speaker_id, s_speaker_enroll_id, sizeof(out_progress->speaker_id));
    }
    __enable_irq();

    return ready;
}

bool audio_capture_consume_slot_pcm16(int16_t * dst,
                                      uint32_t max_samples,
                                      uint32_t * out_samples,
                                      uint8_t * out_slot_index)
{
#if ASR_MODE_CLOUD
    uint32_t i;
    uint32_t count;
    uint32_t oldest;

    if ((NULL == dst) || (NULL == out_samples) || (NULL == out_slot_index))
    {
        return false;
    }

    __disable_irq();
    if (!s_slot_ready)
    {
        __enable_irq();
        return false;
    }

    count = s_slot_ring_count;
    if ((0U == count) || (count > max_samples))
    {
        __enable_irq();
        return false;
    }

    oldest = s_slot_ring_full ? s_slot_ring_idx : 0U;
    for (i = 0U; i < count; i++)
    {
        dst[i] = s_slot_pcm_ring[(oldest + i) % HISTORY_SAMPLES];
    }

    *out_samples = count;
    *out_slot_index = s_slot_ready_index;
    s_slot_ready = false;
    __enable_irq();
    return true;
#else
    (void) dst;
    (void) max_samples;
    (void) out_samples;
    (void) out_slot_index;
    return false;
#endif
}

void audio_capture_publish_digit_result(int digit, float conf)
{
    __disable_irq();
    s_digit_result_value = digit;
    s_digit_result_confidence = conf;
    s_digit_result_ready = true;
    __enable_irq();
}

void audio_capture_publish_speaker_result(uint8_t known, const char * speaker_id, float score)
{
    __disable_irq();
    s_speaker_result_known = known;
    s_speaker_result_score = score;
    memset(s_speaker_result_id, 0, sizeof(s_speaker_result_id));
    if (NULL != speaker_id)
    {
        strncpy(s_speaker_result_id, speaker_id, sizeof(s_speaker_result_id) - 1U);
    }
    s_speaker_result_ready = true;
    __enable_irq();
}

void audio_capture_publish_enroll_progress(const char * speaker_id, uint8_t accepted, uint8_t required, float score)
{
    __disable_irq();
    s_speaker_enroll_accepted = accepted;
    s_speaker_enroll_required = required;
    s_speaker_enroll_score = score;
    memset(s_speaker_enroll_id, 0, sizeof(s_speaker_enroll_id));
    if (NULL != speaker_id)
    {
        strncpy(s_speaker_enroll_id, speaker_id, sizeof(s_speaker_enroll_id) - 1U);
    }
    s_speaker_enroll_ready = true;
    __enable_irq();
}

static void uart_write_line(char const * fmt, ...)
{
    if ((!s_uart_opened) || s_uart_log_muted)
    {
        return;
    }

    char buffer[320];
    va_list args;
    va_start(args, fmt);
    int length = vsnprintf(buffer, sizeof(buffer) - 1, fmt, args); // Leave space for \0
    va_end(args);

    if (length <= 0)
    {
        return;
    }

    if (length >= (int) sizeof(buffer) - 1)
    {
        length = (int) sizeof(buffer) - 2;
    }
    
    /* Python 脚本现在使用 0x00 作为通用帧界限。文本日志结尾也带上一个 \0。 */
    buffer[length] = '\0';
    length += 1;

    (void) uart7_write_bytes_internal((uint8_t const *) buffer, (uint32_t) length, true);
}

/* Expose for C++ stubs (e.g. ErrorReporter) */
void uart_write_line_ext(const char* str)
{
    uart_write_line("%s", str);
}

bool audio_debug_write_bytes(const uint8_t * data, uint32_t length)
{
    return uart7_write_bytes_internal(data, length, false);
}

void audio_debug_write_bytes_blocking(const uint8_t * data, uint32_t length)
{
    (void) uart7_write_bytes_internal(data, length, true);
}

uint32_t audio_debug_read_bytes(uint8_t * out, uint32_t max_bytes)
{
    uint32_t read_count = 0U;

    if ((NULL == out) || (0U == max_bytes))
    {
        return 0U;
    }

    __disable_irq();
    while ((read_count < max_bytes) && (s_uart7_rx_tail != s_uart7_rx_head))
    {
        out[read_count++] = s_uart7_rx_ring[s_uart7_rx_tail];
        s_uart7_rx_tail = uart7_ring_next(s_uart7_rx_tail);
    }
    __enable_irq();

    return read_count;
}

bool audio_debug_tx_idle(void)
{
    return s_uart_tx_done;
}

void audio_debug_set_muted(bool muted)
{
    s_uart_log_muted = muted;
}

bool audio_debug_is_muted(void)
{
    return s_uart_log_muted;
}

void audio_debug_write_line_force(const char * str)
{
    if (NULL == str)
    {
        return;
    }

    audio_debug_write_bytes_blocking((const uint8_t *) str, (uint32_t) strlen(str));
}

static fsp_err_t queue_next_transfer(uint8_t buffer_index)
{
    /* SLAVE mode: Only read from the microphone to avoid TX underflow errors. */
    fsp_err_t err = g_i2s0.p_api->read(g_i2s0.p_ctrl,
                                       s_rx_buffer[buffer_index],
                                       RX_BYTES_PER_BUFFER);
    s_read_submit_ok = (FSP_SUCCESS == err) ? 1U : 0U;
    s_read_submit_err_code = (int32_t) err;

    /* AUCKE=1 is now set once after open(), before the first read().
     * No need to re-patch SSIFCR/SSICR here; r_ssi_start() handles REN/RIE.
     * r_ssi_stop_sub (error path) preserves AUCKE=1 via our patched config. */
    
    return err;
}

static fsp_err_t start_audio_clock_timer(void)
{
    if (s_timer_opened)
    {
        return FSP_SUCCESS;
    }

    /* Configure BCK Timer (GPT1 - g_timer0) */
    fsp_err_t err = g_timer0.p_api->open(g_timer0.p_ctrl, g_timer0.p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        record_task_error();
        return err;
    }

    timer_info_t timer_info;
    err = g_timer0.p_api->infoGet(g_timer0.p_ctrl, &timer_info);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    uint32_t bck_period = timer_info.clock_frequency / AUDIO_BIT_CLOCK_HZ;
    if (bck_period < 2U) bck_period = 2U;

    err = g_timer0.p_api->periodSet(g_timer0.p_ctrl, bck_period);
    if (FSP_SUCCESS != err) return err;

    err = g_timer0.p_api->dutyCycleSet(g_timer0.p_ctrl, bck_period / 2U, GPT_IO_PIN_GTIOCA);
    if (FSP_SUCCESS != err) return err;

    /* Configure WS Timer (GPT0 - g_timer1) */
    err = g_timer1.p_api->open(g_timer1.p_ctrl, g_timer1.p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        record_task_error();
        return err;
    }

    /* I2S 32-bit per channel = 64 bits per frame. WS period must be exactly 64 * BCK period. */
    uint32_t ws_period = bck_period * 64U;
    
    err = g_timer1.p_api->periodSet(g_timer1.p_ctrl, ws_period);
    if (FSP_SUCCESS != err) return err;

    err = g_timer1.p_api->dutyCycleSet(g_timer1.p_ctrl, ws_period / 2U, GPT_IO_PIN_GTIOCA);
    if (FSP_SUCCESS != err) return err;

    /* Start both timers synchronously via GPT hardware GTSTR register.
     * This guarantees BCK and WS start exactly at the same CPU cycle, 
     * providing perfect phase alignment for I2S. */
    volatile R_GPT0_Type * gpt0_hw = (volatile R_GPT0_Type *)R_GPT0;
    volatile R_GPT0_Type * gpt1_hw = (volatile R_GPT0_Type *)((uint32_t)R_GPT0 + 0x100U);
    
    /* Set GTCNT to 0 for both just to be absolutely sure */
    gpt0_hw->GTCNT = 0;
    gpt1_hw->GTCNT = 0;

    /* Write 1 to the corresponding channel bits in Software Start register */
    uint32_t start_mask = (1UL << g_timer0.p_cfg->channel) | (1UL << g_timer1.p_cfg->channel);
    gpt0_hw->GTSTR = start_mask;

    s_timer_opened = true;
    return FSP_SUCCESS;
}

static void process_one_buffer(uint8_t buffer_index)
{
    uint64_t frame_energy = 0U;
    uint32_t frame_clipping = 0U;
    uint16_t frame_peak = 0U;

    /* ADPCM 压缩 + COBS 编码发送 (全量 16000Hz 数据)。
     * 160 帧 PCM (320 bytes) 压缩为 80 bytes 的 ADPCM nibbles。
     * 数据包结构: [Sync(0x00)] [PacketType(0xAA)] [ADPCM State(int16=2b, int8=1b)] [Payload(80b)] [COBS Terminator(0x00)]
     */
    uint8_t raw_pkt[84]; // 1 type + 2 prev_sample + 1 prev_index + 80 payload 
    raw_pkt[0] = 0xAA; // Packet Type Identifier for ADPCM audio
    raw_pkt[1] = (uint8_t)(s_adpcm_state.prev_sample & 0xFF);
    raw_pkt[2] = (uint8_t)((s_adpcm_state.prev_sample >> 8) & 0xFF);
    raw_pkt[3] = (uint8_t)s_adpcm_state.prev_index;
    
    for (uint32_t frame = 0; frame < FRAME_FRAMES; frame++)
    {
        int32_t left_24 = s_rx_buffer[buffer_index][frame * 2U];
        /* 24-bit data from INMP441 is physically located in bits 23..0. 
         * To preserve the two's complement sign at bit 23, we MUST left-shift by 8 
         * to force it into bit 31, and THEN arithmetic right-shift by 16 
         * to extract the most significant 16 bits with proper sign extension. */
        int16_t raw_16 = (int16_t) ((left_24 << 8) >> 16);
        
        /* DIAGNOSTIC FLAG: Dump the very first few raw I2S 32-bit registers to see the bit alignment physically */
        static uint32_t s_hex_dump_cnt = 0;
        if (s_hex_dump_cnt < 20 && buffer_index == 0) {
            uart_write_line("DBG_RAW: left_24=0x%08lX, raw_16=%d\r\n", (unsigned long)left_24, (int)raw_16);
            s_hex_dump_cnt++;
        }
        
        /* Bypass HPF + DC + digital gain to match training-time raw waveform distribution. */
        int16_t mono_16 = raw_16;

        int32_t abs_value = (mono_16 < 0) ? -mono_16 : mono_16;
        if (abs_value > frame_peak)
        {
            frame_peak = (uint16_t) abs_value;
        }

        if ((mono_16 >= 32760) || (mono_16 <= -32760))
        {
            frame_clipping++;
        }

        frame_energy += (uint64_t) ((int32_t) mono_16 * (int32_t) mono_16);
        

        /* Save samples for inference (raw waveform path). */
        if (s_key_pressed)
        {
#if ASR_MODE_LOCAL
            s_audio_history[s_history_idx] = (float) mono_16;
            s_history_idx++;
            if (s_history_idx >= HISTORY_SAMPLES)
            {
                s_history_idx = 0U;
                s_history_full = true;
            }

            if (s_history_count < HISTORY_SAMPLES)
            {
                s_history_count++;
            }
#endif
#if ASR_MODE_CLOUD
            s_slot_pcm_ring[s_slot_ring_idx] = mono_16;
            s_slot_ring_idx++;
            if (s_slot_ring_idx >= HISTORY_SAMPLES)
            {
                s_slot_ring_idx = 0U;
                s_slot_ring_full = true;
            }

            if (s_slot_ring_count < HISTORY_SAMPLES)
            {
                s_slot_ring_count++;
            }
#endif
        }

        /* Encode 2 samples into 1 byte (ADPCM nibbles) */
    } // End of frame loop

    /* We previously had ADPCM and COBS transmission here.
     * We comment it out for the AI digit recognition mode so the UART
     * doesn't overflow with binary data, allowing text logs to print cleanly.
     *
    uint8_t raw_pkt[84]; ...
    uint8_t cobs_pkt[90]; ...
    g_uart7.p_api->write(...)
    */


    s_window_energy += frame_energy;
    s_window_samples += FRAME_FRAMES;
    s_window_clipping += frame_clipping;
    if (frame_peak > s_window_peak)
    {
        s_window_peak = frame_peak;
    }

    s_stats.sample_count += FRAME_FRAMES;

    /* Optional VAD path; manual key trigger is the default path. */
#if (ASR_MODE_LOCAL && (ENABLE_VAD_TRIGGER != 0U))
    {
        float frame_rms = sqrtf((float) frame_energy / FRAME_FRAMES);
        if (frame_rms > VAD_ENERGY_THRESHOLD)
        {
            s_speech_frames++;
            s_silence_frames = 0;
            if (s_speech_frames > 5)
            {
                if (!s_in_speech)
                {
                    uart_write_line("VAD: --- SPEECH START DETECTED! (rms=%s) ---\r\n", ff(frame_rms, 1));
                }
                s_in_speech = true;
            }
        }
        else
        {
            if (s_in_speech)
            {
                s_silence_frames++;
                if (s_silence_frames > 30)
                {
                    uart_write_line("VAD: --- SPEECH END, FLAG INFERENCE ---\r\n");
                    s_in_speech = false;
                    if (s_history_full)
                    {
                        s_run_inference_flag = true;
                    }
                }
            }
            else
            {
                s_speech_frames = 0;
            }
        }
    }
#endif
}

static void restart_capture_if_needed(void)
{
    if (!s_restart_pending)
    {
        return;
    }

    __disable_irq();
    s_restart_pending = false;
    s_ready_mask = 0U;
    __enable_irq();

    if (FSP_SUCCESS != start_audio_clock_timer())
    {
        s_restart_pending = true;
        return;
    }

    (void) g_i2s0.p_api->close(g_i2s0.p_ctrl);

    /* 等待SSI硬件完全进入Idle状态（SSISR.IIRQ=1）后才能重新open/start。
     * r_ssi_start()在IIRQ!=1时会返回FSP_ERR_IN_USE，导致TEN/REN不被写入，
     * BCK/WS时钟永远无法产生。 */
    volatile R_SSI0_Type * ssi_wait = R_SSI0;
    uint32_t idle_timeout = 200000U;
    while ((0U == ssi_wait->SSISR_b.IIRQ) && (idle_timeout > 0U))
    {
        idle_timeout--;
        __NOP();
    }

    /* Override FSP config: we only need RX in this Slave setup.
     * Force TX transfer to NULL to prevent DTC conflicts or unexpected underflow checks.
     * CRITICAL: audio_clock must NOT be EXTERNAL in SLAVE mode, otherwise the hardware
     * might look at the AUDIO_CLK pin instead of the actual SCK (P112) pin!
     *
     * Using STATIC storage: FSP stores p_cfg pointer in the control block and 
     * dereferences it later in ISR context (r_ssi_stop_sub, r_ssi_rx_unload_fifo, etc.).
     * Local variables would become dangling pointers after this function returns.
     *
     * SLAVE → MASTER trick: We set operating_mode = I2S_MODE_MASTER (=1) so that FSP's
     *   ssifcr = operating_mode << AUCKE_BIT  →  AUCKE = 1
     *   ssicr  = operating_mode << MST_BIT    →  MST = 1
     * Then we immediately clear MST bit after open(), keeping AUCKE=1.
     * This is the cleanest way to work around the FSP AUCKE=0 bug for slave mode
     * without patching every SSIFCR write in r_ssi_stop_sub / r_ssi_start. */
    static ssi_extended_cfg_t s_restart_ext_cfg;
    if (g_i2s0_cfg.p_extend != NULL)
    {
        s_restart_ext_cfg = *(ssi_extended_cfg_t const *) g_i2s0_cfg.p_extend;
    }
    s_restart_ext_cfg.audio_clock = SSI_AUDIO_CLOCK_INTERNAL;

    static i2s_cfg_t s_restart_i2s_cfg;
    s_restart_i2s_cfg = g_i2s0_cfg;
    s_restart_i2s_cfg.operating_mode = I2S_MODE_MASTER;  /* Trick: makes AUCKE=1 in FSP */
    s_restart_i2s_cfg.p_transfer_tx = NULL;
    s_restart_i2s_cfg.p_callback = i2s0_callback;
    s_restart_i2s_cfg.p_extend = &s_restart_ext_cfg;

    fsp_err_t err = g_i2s0.p_api->open(g_i2s0.p_ctrl, &s_restart_i2s_cfg);
    if (FSP_SUCCESS != err)
    {
        record_task_error();
        s_restart_pending = true;
        return;
    }

    /* Clear MST bit: we are actually slave. AUCKE stays 1 (FSP set it in SSIFCR).
     * Now r_ssi_stop_sub will also produce AUCKE=1 when it recomputes from operating_mode. */
    R_SSI0->SSICR &= ~(1UL << 14);  /* MST bit = 0 → slave */

    s_i2s_opened = true;
    s_active_buffer = 0U;
    s_last_callback_cycle = 0U;
    reset_window_counters(DWT->CYCCNT);

    err = queue_next_transfer(s_active_buffer);
    if (FSP_SUCCESS == err)
    {
        s_capture_started = true;
        s_stats.restart_count++;
    }
    else
    {
        record_task_error();
        s_restart_pending = true;
    }
}

const audio_stats_t * audio_capture_stats_get(void)
{
    return &s_stats;
}

void audio_capture_init(void)
{
    /* CRITICAL: Hardware reset stabilization delay.
     * When the MCU undergoes a warm pin reset 'RST', the INMP441 microphone 
     * loses its clock abruptly. Waiting 150ms gives the acoustic sensor IC time
     * to naturally time out and be ready for a clean clock start. */
    R_BSP_SoftwareDelay(150, BSP_DELAY_UNITS_MILLISECONDS);

    memset((void *) &s_stats, 0, sizeof(s_stats));
    s_rms_min_observed = 0xFFFFFFFFUL;
    s_rms_max_observed = 0U;
    s_prev_overflow_count = 0U;
    s_prev_restart_count = 0U;
    s_prev_error_count = 0U;
    s_log_second_index = 0U;
    s_pin_prev_valid = false;
    s_prev_bck_level = 0U;
    s_prev_ws_level = 0U;
    s_pin_sample_divider = 0U;
    s_hw_link_hint_printed = false;
    memset(s_tx_buffer, 0, sizeof(s_tx_buffer));
    s_manual_trigger_flag = false;
    s_key_pressed = false;
    s_key_press_time_us = 0U;
    s_key_press_log_pending = 0U;
    s_key_release_log_pending = 0U;
    s_key_last_hold_ms = 0U;
    s_digit_result_ready = false;
    s_digit_result_value = -1;
    s_digit_result_confidence = 0.0f;
    s_speaker_result_ready = false;
    s_speaker_result_known = 0U;
    s_speaker_result_score = 0.0f;
    memset(s_speaker_result_id, 0, sizeof(s_speaker_result_id));
    s_speaker_enroll_ready = false;
    s_speaker_enroll_accepted = 0U;
    s_speaker_enroll_required = 0U;
    s_speaker_enroll_score = 0.0f;
    memset(s_speaker_enroll_id, 0, sizeof(s_speaker_enroll_id));
    s_hpf_prev_x = 0.0f;
    s_hpf_prev_y = 0.0f;
#if ASR_MODE_LOCAL
    s_history_idx = 0U;
    s_history_full = false;
    s_history_count = 0U;
#endif
#if ASR_MODE_CLOUD
    s_slot_ring_idx = 0U;
    s_slot_ring_full = false;
    s_slot_ring_count = 0U;
    s_slot_ready = false;
    s_slot_ready_index = 0U;
    s_slot_sequence = 0U;
#endif

    s_core_hz = R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_ICLK);

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    reset_window_counters(DWT->CYCCNT);

    if (FSP_SUCCESS == g_uart7.p_api->open(g_uart7.p_ctrl, g_uart7.p_cfg))
    {
        s_uart_opened = true;
    }

#if ASR_MODE_LOCAL
    /* Initialize the local TFLM digit recognition model. */
    if (digit_recognizer_init())
    {
        uart_write_line("INFO: TFLM model initialized successfully.\r\n");
    }
    else
    {
        uart_write_line("ERR: TFLM model initialization FAILED!\r\n");
    }
#else
    uart_write_line("INFO: CLOUD mode enabled, local TFLM disabled.\r\n");
#endif

    /* Start GPT timer for I2S master clock (INMP441 is slave, needs our clock) */
    if (FSP_SUCCESS != start_audio_clock_timer())
    {
        if (s_uart_opened)
        {
            uart_write_line("WARN: GPT audio clock start failed.\r\n");
        }
    }
    else if (s_uart_opened)
    {
        timer_info_t tinfo;
        if (FSP_SUCCESS == g_timer0.p_api->infoGet(g_timer0.p_ctrl, &tinfo))
        {
            uart_write_line("DIAG: GPT ch=%u clk=%luHz (for INMP441 slave mic)\r\n",
                            (unsigned)g_timer0.p_cfg->channel,
                            (unsigned long)tinfo.clock_frequency);
        }
        timer_status_t tstat;
        if (FSP_SUCCESS == g_timer0.p_api->statusGet(g_timer0.p_ctrl, &tstat))
        {
            uart_write_line("DIAG: GPT state=%u count=%lu\r\n",
                            (unsigned)tstat.state, (unsigned long)tstat.counter);
        }

        /* Dump GPT hardware registers to verify PWM output configuration for GPT1 */
        uint32_t ch0 = g_timer0.p_cfg->channel;
        volatile R_GPT0_Type * gpt_reg0 = (volatile R_GPT0_Type *)
            ((uint32_t)R_GPT0 + ch0 * ((uint32_t)R_GPT1 - (uint32_t)R_GPT0));
        uart_write_line("DIAG: GPT1 GTIOR=0x%08lX GTCR=0x%08lX GTPR=0x%08lX\r\n",
                        (unsigned long)gpt_reg0->GTIOR, (unsigned long)gpt_reg0->GTCR, (unsigned long)gpt_reg0->GTPR);

        /* Diagnostic for GPT0 (WS clock) */
        timer_info_t tinfo1;
        if (FSP_SUCCESS == g_timer1.p_api->infoGet(g_timer1.p_ctrl, &tinfo1))
        {
            uart_write_line("DIAG: GPT0 ch=%u clk=%luHz (WS generator)\r\n",
                            (unsigned)g_timer1.p_cfg->channel,
                            (unsigned long)tinfo1.clock_frequency);
        }
        timer_status_t tstat1;
        if (FSP_SUCCESS == g_timer1.p_api->statusGet(g_timer1.p_ctrl, &tstat1))
        {
            uart_write_line("DIAG: GPT0 state=%u count=%lu\r\n",
                            (unsigned)tstat1.state, (unsigned long)tstat1.counter);
        }

        uint32_t ch1 = g_timer1.p_cfg->channel;
        volatile R_GPT0_Type * gpt_reg1 = (volatile R_GPT0_Type *)
            ((uint32_t)R_GPT0 + ch1 * ((uint32_t)R_GPT1 - (uint32_t)R_GPT0));
        uart_write_line("DIAG: GPT0 GTIOR=0x%08lX GTCR=0x%08lX GTPR=0x%08lX\r\n",
                        (unsigned long)gpt_reg1->GTIOR, (unsigned long)gpt_reg1->GTCR, (unsigned long)gpt_reg1->GTPR);
    }

    bool generated_cfg_ok = (I2S_PCM_WIDTH_24_BITS == g_i2s0_cfg.pcm_width) &&
                            (I2S_WORD_LENGTH_32_BITS == g_i2s0_cfg.word_length) &&
                            (NULL != g_i2s0_cfg.p_transfer_rx) &&
                            (I2S_MODE_SLAVE == g_i2s0_cfg.operating_mode);
    if ((!generated_cfg_ok) && s_uart_opened)
    {
        uart_write_line("WARN: I2S cfg mismatch (expect SLAVE + word32 for INMP441).\r\n");
    }

    /* Wait 50ms for the GPT-generated BCK/WS to stabilize and the INMP441 
     * microphone to start outputting valid I2S data before configuring SSI. */
    R_BSP_SoftwareDelay(50, BSP_DELAY_UNITS_MILLISECONDS);

    /* Open SSI with optimized config for RX-only slave mode.
     * - p_transfer_tx = NULL: no TX DTC (avoid accidental underflow check).
     * - operating_mode = MASTER (trick): FSP computes AUCKE = operating_mode << 31.
     *   Setting MASTER(=1) gives AUCKE=1 automatically. We then clear MST bit in SSICR
     *   to revert to actual slave mode. This ensures AUCKE stays 1 through ALL FSP
     *   code paths (open, start, stop_sub, restart) without manual register patching.
     *
     * CRITICAL: STATIC storage. FSP stores &cfg pointer in p_instance_ctrl->p_cfg and
     * dereferences it in ISR context (ssi_rxi_isr → r_ssi_rx_unload_fifo → p_cfg->p_transfer_rx,
     * ssi_int_isr → r_ssi_stop_sub → p_cfg->operating_mode, p_cfg->p_transfer_rx/tx).
     * A local (stack) variable becomes a dangling pointer after this function returns,
     * causing HardFault when the first DTC transfer completes ~10ms later. */
    static ssi_extended_cfg_t s_init_ext_cfg;
    if (g_i2s0_cfg.p_extend != NULL)
    {
        s_init_ext_cfg = *(ssi_extended_cfg_t const *) g_i2s0_cfg.p_extend;
    }
    s_init_ext_cfg.audio_clock = SSI_AUDIO_CLOCK_INTERNAL;

    static i2s_cfg_t s_init_i2s_cfg;
    s_init_i2s_cfg = g_i2s0_cfg;
    s_init_i2s_cfg.operating_mode = I2S_MODE_MASTER;  /* Trick: makes AUCKE=1 */
    s_init_i2s_cfg.p_transfer_tx = NULL;
    s_init_i2s_cfg.p_extend = &s_init_ext_cfg;

    fsp_err_t open_err = g_i2s0.p_api->open(g_i2s0.p_ctrl, &s_init_i2s_cfg);
    if (FSP_SUCCESS == open_err)
    {
        s_i2s_opened = true;

        /* Clear MST bit to revert to actual slave mode. AUCKE remains 1 in SSIFCR
         * because FSP already wrote it based on operating_mode=MASTER. */
        R_SSI0->SSICR &= ~(1UL << 14);  /* MST = 0 → slave */
    }
    else
    {
        record_task_error();
        if (s_uart_opened)
        {
            uart_write_line("ERR: SSI open fail=%ld\r\n", (long)open_err);
        }
    }

    /* Diagnostic: dump SSI registers after open */
    if (s_uart_opened && s_i2s_opened)
    {
        volatile R_SSI0_Type * ssi = R_SSI0;
        uart_write_line("DIAG: SSICR=0x%08lX SSIFCR=0x%08lX\r\n",
                        (unsigned long)ssi->SSICR, (unsigned long)ssi->SSIFCR);
        uart_write_line("DIAG: SSISR=0x%08lX SSISCR=0x%08lX\r\n",
                        (unsigned long)ssi->SSISR, (unsigned long)ssi->SSISCR);
        uart_write_line("DIAG: SSIFSR=0x%08lX SSIOFR=0x%08lX\r\n",
                        (unsigned long)ssi->SSIFSR, (unsigned long)ssi->SSIOFR);
        uart_write_line("DIAG: p_transfer_tx=%s p_transfer_rx=%s\r\n",
                        g_i2s0_cfg.p_transfer_tx ? "SET" : "NULL",
                        g_i2s0_cfg.p_transfer_rx ? "SET" : "NULL");
        uart_write_line("DIAG: txi_irq=%d rxi_irq=%d int_irq=%d\r\n",
                        (int)g_i2s0_cfg.txi_irq, (int)g_i2s0_cfg.rxi_irq,
                        (int)g_i2s0_cfg.int_irq);

        /* 底层时钟路由诊断：MSTPCRC (SSI模块停止控制) 和 PFS (引脚功能选择) */
        uart_write_line("DIAG_CLK: MSTPCRC=0x%08lX (bit8=SSI0 stop: %lu)\r\n",
                        (unsigned long)R_MSTP->MSTPCRC,
                        (unsigned long)((R_MSTP->MSTPCRC >> 8) & 1U));

        /* GPT0 和 GPT1 内部路由诊断 */
        volatile R_GPT0_Type * gpt0 = (volatile R_GPT0_Type *)R_GPT0;
        volatile R_GPT0_Type * gpt1 = (volatile R_GPT0_Type *)((uint32_t)R_GPT0 + 0x100U);
        uart_write_line("DIAG_CLK: GPT0_GTCNT=0x%08lX GPT1_GTCNT=0x%08lX\r\n",
                        (unsigned long)gpt0->GTCNT,
                        (unsigned long)gpt1->GTCNT);

        /* 引脚PFS寄存器诊断 (P109=GPT1，P512=GPT0，P112/P113=SSI0) */
        uint32_t pfs_p109 = R_PFS->PORT[1].PIN[9].PmnPFS;
        uint32_t pfs_p112 = R_PFS->PORT[1].PIN[12].PmnPFS;
        uint32_t pfs_p113 = R_PFS->PORT[1].PIN[13].PmnPFS;
        uint32_t pfs_p512 = R_PFS->PORT[5].PIN[12].PmnPFS;
        uart_write_line("DIAG_CLK: PFS_P109=0x%08lX PFS_P112=0x%08lX\r\n",
                        (unsigned long)pfs_p109, (unsigned long)pfs_p112);
        uart_write_line("DIAG_CLK: PFS_P512=0x%08lX PFS_P113=0x%08lX\r\n",
                        (unsigned long)pfs_p512, (unsigned long)pfs_p113);
    }

    if (s_uart_opened)
    {
        uart_write_line("A_i2s_capture init: fs=%luHz, frame=%lu, bytes=%lu\r\n",
                        (unsigned long) AUDIO_SAMPLE_RATE_HZ,
                        (unsigned long) FRAME_FRAMES,
                        (unsigned long) RX_BYTES_PER_BUFFER);
        uart_write_line("STAT_HEADER,t_sec,samples,fs_est,cb_count,cb_gap_max_us,rms,peak,clip,ovf,rst,err,read_ok,read_err,no_cb_to,sample_ok,cb_ok,error_ok,audio_ok,ws_edge,bck_edge,pin_err,ssi_state\r\n");
    }
}

void audio_capture_start(void)
{
    if (!s_i2s_opened)
    {
        return;
    }

    s_active_buffer = 0U;
    s_ready_mask = 0U;
    s_restart_pending = false;
    s_last_callback_cycle = 0U;
    reset_window_counters(DWT->CYCCNT);

    /* 等待SSI在open()后完全进入Idle状态（IIRQ=1）。
     * r_ssi_start()要求IIRQ==1才能写入TEN/REN。若不等待，
     * writeRead()返回FSP_ERR_IN_USE，BCK/WS时钟不会产生。 */
    volatile R_SSI0_Type * ssi_chk = R_SSI0;
    uint32_t idle_wait = 200000U;
    while ((0U == ssi_chk->SSISR_b.IIRQ) && (idle_wait > 0U))
    {
        idle_wait--;
        __NOP();
    }

    fsp_err_t err = queue_next_transfer(s_active_buffer);
    if (FSP_SUCCESS == err)
    {
        s_capture_started = true;
    }
    else
    {
        record_task_error();
    }

    /* Diagnostic: SSI registers after read() in master mode (INMP441 slave mic) */
    if (s_uart_opened)
    {
        volatile R_SSI0_Type * ssi = R_SSI0;
        uart_write_line("DIAG_START: writeRead err=%ld started=%u mode=%s\r\n",
                        (long)err, (unsigned)s_capture_started,
                        (I2S_MODE_MASTER == g_i2s0_cfg.operating_mode) ? "MASTER" : "SLAVE");
        uart_write_line("DIAG_START: SSICR=0x%08lX SSISR=0x%08lX SSIFCR=0x%08lX\r\n",
                        (unsigned long)ssi->SSICR,
                        (unsigned long)ssi->SSISR,
                        (unsigned long)ssi->SSIFCR);

        /* 关键诊断: 延迟50ms后再次读取SSI状态，检查是否TX下溢导致SSI停止 */
        R_BSP_SoftwareDelay(50, BSP_DELAY_UNITS_MILLISECONDS);
        uint32_t ssicr_after  = ssi->SSICR;
        uint32_t ssisr_after  = ssi->SSISR;
        uint32_t ssifsr_after = ssi->SSIFSR;
        uart_write_line("DIAG_50ms: SSICR=0x%08lX SSISR=0x%08lX SSIFSR=0x%08lX\r\n",
                        (unsigned long)ssicr_after,
                        (unsigned long)ssisr_after,
                        (unsigned long)ssifsr_after);
        uart_write_line("DIAG_50ms: TEN=%lu REN=%lu TUIRQ=%lu ROIRQ=%lu IIRQ=%lu TDC=%lu RDC=%lu\r\n",
                        (unsigned long)((ssicr_after >> 1) & 1U),
                        (unsigned long)(ssicr_after & 1U),
                        (unsigned long)((ssisr_after >> 29) & 1U),
                        (unsigned long)((ssisr_after >> 26) & 1U),
                        (unsigned long)((ssisr_after >> 25) & 1U),
                        (unsigned long)((ssifsr_after >> 24) & 0xFU),
                        (unsigned long)((ssifsr_after >> 8) & 0xFU));
        uart_write_line("INFO: Generating clock for INMP441 slave microphone...\r\n");
    }
}

void audio_debug_only_init(void)
{
    s_uart7_rx_head = 0U;
    s_uart7_rx_tail = 0U;
    s_uart_tx_done = true;
    s_uart_log_muted = false;

    if (!s_uart_opened)
    {
        if (FSP_SUCCESS == g_uart7.p_api->open(g_uart7.p_ctrl, g_uart7.p_cfg))
        {
            s_uart_opened = true;
        }
    }
}

void audio_capture_process(void)
{
    if (!s_capture_started)
    {
        restart_capture_if_needed();
        return;
    }

    restart_capture_if_needed();
    monitor_i2s_pin_activity();
    process_key_trigger_logs();

    while (true)
    {
        __disable_irq();
        uint32_t ready_mask = s_ready_mask;
        if (0U == ready_mask)
        {
            __enable_irq();
            break;
        }

        uint8_t buffer_index = (ready_mask & 0x1U) ? 0U : 1U;
        s_ready_mask &= ~(1UL << buffer_index);
        __enable_irq();

        process_one_buffer(buffer_index);
    }

    if (0U == s_core_hz)
    {
        return;
    }

    uint32_t now_cycle = DWT->CYCCNT;
    uint32_t elapsed_us = cycles_to_us(now_cycle - s_window_start_cycle);
    if (elapsed_us < LOG_WINDOW_US)
    {
        return;
    }

    uint64_t window_energy = s_window_energy;
    uint32_t window_samples = s_window_samples;
    uint32_t window_clipping = s_window_clipping;
    uint16_t window_peak = s_window_peak;

    uint32_t window_callbacks = 0U;
    uint32_t window_max_gap_us = 0U;
    uint32_t window_errors = 0U;
    uint32_t window_ws_edges = s_window_ws_edges;
    uint32_t window_bck_edges = s_window_bck_edges;
    uint32_t window_pin_read_errors = s_window_pin_read_errors;
    uint32_t overflow_total = 0U;
    uint32_t restart_total = 0U;
    uint32_t error_total = 0U;
    uint8_t read_submit_ok = 0U;
    int32_t read_submit_err_code = 0;
    __disable_irq();
    window_callbacks = s_window_callback_count;
    window_max_gap_us = s_window_max_callback_gap_us;
    window_errors = s_window_error_count;
    overflow_total = s_stats.overflow_count;
    restart_total = s_stats.restart_count;
    error_total = s_stats.error_count;
    read_submit_ok = s_read_submit_ok;
    read_submit_err_code = s_read_submit_err_code;
    s_window_callback_count = 0U;
    s_window_max_callback_gap_us = 0U;
    s_window_error_count = 0U;
    __enable_irq();

    s_window_start_cycle = now_cycle;
    s_window_energy = 0U;
    s_window_samples = 0U;
    s_window_clipping = 0U;
    s_window_peak = 0U;
    s_window_ws_edges = 0U;
    s_window_bck_edges = 0U;
    s_window_pin_read_errors = 0U;

    uint32_t window_rms = 0U;
    if (window_samples > 0U)
    {
        window_rms = isqrt_u64(window_energy / window_samples);
    }

    s_stats.rms = window_rms;
    s_stats.peak = window_peak;
    s_stats.clipping_count += window_clipping;
    s_stats.callbacks_in_window = window_callbacks;
    s_stats.max_callback_gap_us = window_max_gap_us;
    s_stats.samples_in_window = window_samples;
    s_stats.read_submit_ok = read_submit_ok;
    s_stats.read_submit_err_code = read_submit_err_code;
    s_stats.estimated_sample_rate_hz = (uint32_t) (((uint64_t) window_samples * 1000000ULL) / elapsed_us);

    if (window_rms < s_rms_min_observed)
    {
        s_rms_min_observed = window_rms;
    }

    if (window_rms > s_rms_max_observed)
    {
        s_rms_max_observed = window_rms;
    }

    uint32_t sample_low = AUDIO_SAMPLE_RATE_HZ - ((AUDIO_SAMPLE_RATE_HZ * SAMPLE_RATE_TOLERANCE_PCT) / 100U);
    uint32_t sample_high = AUDIO_SAMPLE_RATE_HZ + ((AUDIO_SAMPLE_RATE_HZ * SAMPLE_RATE_TOLERANCE_PCT) / 100U);
    bool sample_rate_ok = (s_stats.estimated_sample_rate_hz >= sample_low) &&
                          (s_stats.estimated_sample_rate_hz <= sample_high);

    uint32_t expected_callbacks = (elapsed_us + (EXPECTED_CALLBACK_INTERVAL_US / 2U)) / EXPECTED_CALLBACK_INTERVAL_US;
    if (0U == expected_callbacks)
    {
        expected_callbacks = EXPECTED_CALLBACKS_PER_SECOND;
    }

    uint32_t callback_min = (expected_callbacks * CALLBACK_MIN_RATIO_NUM) / CALLBACK_MIN_RATIO_DEN;
    if (0U == callback_min)
    {
        callback_min = 1U;
    }

    uint32_t callback_gap_limit_us = EXPECTED_CALLBACK_INTERVAL_US * CALLBACK_GAP_LIMIT_MULT;
    bool callback_gap_ok = (window_callbacks >= callback_min) &&
                           ((0U == window_max_gap_us) || (window_max_gap_us <= callback_gap_limit_us));

    if (0U == window_callbacks)
    {
        __disable_irq();
        s_stats.error_count++;
        s_stats.no_callback_timeout_count++;
        error_total = s_stats.error_count;
        __enable_irq();
        window_errors++;
        s_restart_pending = true;

        if ((!s_hw_link_hint_printed) && (0U == window_ws_edges) && (0U == window_bck_edges))
        {
            s_hw_link_hint_printed = true;
            uart_write_line("HINT: no WS/BCK edges. Check INMP441 wiring: SCK->P112 WS->P113 SD->P114 L/R->GND CHIPEN->VDD.\r\n");
        }

    }

    uint32_t ovf_delta = overflow_total - s_prev_overflow_count;
    uint32_t rst_delta = restart_total - s_prev_restart_count;
    uint32_t err_delta = error_total - s_prev_error_count;
    s_prev_overflow_count = overflow_total;
    s_prev_restart_count = restart_total;
    s_prev_error_count = error_total;

    bool error_growth_ok = (0U == ovf_delta) && (0U == rst_delta) && (0U == err_delta) && (0U == window_errors);
    bool audio_sanity_ok = (0U == window_clipping) &&
                           (s_rms_max_observed > s_rms_min_observed) &&
                           ((s_rms_max_observed - s_rms_min_observed) >= AUDIO_SANITY_MIN_RMS_DELTA) &&
                           ((uint64_t) s_rms_max_observed >= ((uint64_t) s_rms_min_observed * 2ULL));
    i2s_status_t i2s_status = {0};
    if (FSP_SUCCESS == g_i2s0.p_api->statusGet(g_i2s0.p_ctrl, &i2s_status))
    {
        s_stats.ssi_state = (uint32_t) i2s_status.state;
    }
    else
    {
        s_stats.ssi_state = 0xFFFFFFFFUL;
    }

    s_stats.ws_edge_observed = window_ws_edges;
    s_stats.bck_edge_observed = window_bck_edges;
    s_stats.pin_read_error_count = window_pin_read_errors;

    s_stats.sample_rate_ok = sample_rate_ok ? 1U : 0U;
    s_stats.callback_gap_ok = callback_gap_ok ? 1U : 0U;
    s_stats.error_growth_ok = error_growth_ok ? 1U : 0U;
    s_stats.audio_sanity_ok = audio_sanity_ok ? 1U : 0U;

    s_log_second_index++;
    uart_write_line("STAT,%lu,%lu,%lu,%lu,%lu,%lu,%u,%lu,%lu,%lu,%lu,%u,%ld,%lu,%u,%u,%u,%u,%lu,%lu,%lu,%lu\r\n",
                    (unsigned long) s_log_second_index,
                    (unsigned long) window_samples,
                    (unsigned long) s_stats.estimated_sample_rate_hz,
                    (unsigned long) window_callbacks,
                    (unsigned long) window_max_gap_us,
                    (unsigned long) window_rms,
                    (unsigned int) window_peak,
                    (unsigned long) window_clipping,
                    (unsigned long) ovf_delta,
                    (unsigned long) rst_delta,
                    (unsigned long) err_delta,
                    (unsigned int) s_stats.read_submit_ok,
                    (long) s_stats.read_submit_err_code,
                    (unsigned long) s_stats.no_callback_timeout_count,
                    (unsigned int) s_stats.sample_rate_ok,
                    (unsigned int) s_stats.callback_gap_ok,
                    (unsigned int) s_stats.error_growth_ok,
                    (unsigned int) s_stats.audio_sanity_ok,
                    (unsigned long) s_stats.ws_edge_observed,
                    (unsigned long) s_stats.bck_edge_observed,
                    (unsigned long) s_stats.pin_read_error_count,
                    (unsigned long) s_stats.ssi_state);
}

void audio_capture_run_inference_if_ready(void)
{
#if ASR_MODE_LOCAL
    bool has_trigger = false;
    __disable_irq();
#if (ENABLE_VAD_TRIGGER != 0U)
    has_trigger = s_manual_trigger_flag || s_run_inference_flag;
#else
    has_trigger = s_manual_trigger_flag;
#endif
    __enable_irq();

    if (!has_trigger) {
        return;
    }

    /* Clear flags so we don't run multiple times for one trigger. */
    __disable_irq();
    s_manual_trigger_flag = false;
#if (ENABLE_VAD_TRIGGER != 0U)
    s_run_inference_flag = false;
#endif
    __enable_irq();
    s_digit_result_value = -1;
    s_digit_result_confidence = 0.0f;

    uint32_t captured_samples = s_history_count;
    if (captured_samples == 0U)
    {
        uart_write_line("WARN: no recorded samples for trigger.\r\n");
        __disable_irq();
        s_digit_result_value = -1;
        s_digit_result_confidence = 0.0f;
        s_digit_result_ready = true;
        __enable_irq();
        return;
    }

    /* Flatten ring buffer in-place into s_audio_history[] starting at index 0.
     * This avoids a separate 96KB static buffer: we rotate the ring buffer slots
     * directly, re-using the same memory.
     * After this, s_audio_history[0..captured_samples-1] is chronological. */
    if (s_history_full)
    {
        uint32_t oldest = s_history_idx;
        /* Rotate buffer in-place using 3-reversal algorithm */
        /* Reverse [0..oldest-1] */
        uint32_t lo = 0U, hi = (oldest > 0U) ? (oldest - 1U) : 0U;
        while (lo < hi) { float t = s_audio_history[lo]; s_audio_history[lo] = s_audio_history[hi]; s_audio_history[hi] = t; lo++; hi--; }
        /* Reverse [oldest..HISTORY_SAMPLES-1] */
        lo = oldest; hi = HISTORY_SAMPLES - 1U;
        while (lo < hi) { float t = s_audio_history[lo]; s_audio_history[lo] = s_audio_history[hi]; s_audio_history[hi] = t; lo++; hi--; }
        /* Reverse entire array */
        lo = 0U; hi = HISTORY_SAMPLES - 1U;
        while (lo < hi) { float t = s_audio_history[lo]; s_audio_history[lo] = s_audio_history[hi]; s_audio_history[hi] = t; lo++; hi--; }
        captured_samples = HISTORY_SAMPLES;
    }
    /* If not full, s_audio_history[0..captured_samples-1] is already chronological. */

    /* Use s_audio_history directly as the flat buffer from here on. */
#define s_flat_audio_buffer s_audio_history

    memset(s_model_input_audio, 0, sizeof(s_model_input_audio));

    /* trim_and_fit(top_db=40, pad=30ms) + center crop/pad to 1.2s */
    uint32_t trim_start = 0U;
    uint32_t trim_end = captured_samples;
    float peak = 0.0f;
    for (uint32_t i = 0U; i < captured_samples; i++)
    {
        float abs_val = fabsf(s_flat_audio_buffer[i]);
        if (abs_val > peak)
        {
            peak = abs_val;
        }
    }

    uint32_t scan_start = 0U;
    uint32_t scan_end = captured_samples;
    if (captured_samples > (2U * TRIM_EDGE_IGNORE_SAMPLES))
    {
        scan_start = TRIM_EDGE_IGNORE_SAMPLES;
        scan_end = captured_samples - TRIM_EDGE_IGNORE_SAMPLES;
    }

    if (peak >= 1e-8f)
    {
        float threshold = peak * powf(10.0f, -TRIM_TOP_DB / 20.0f);
        bool found_active = false;
        uint32_t first_active = 0U;
        uint32_t last_active = 0U;

        for (uint32_t i = scan_start; i < scan_end; i++)
        {
            if (fabsf(s_flat_audio_buffer[i]) >= threshold)
            {
                if (!found_active)
                {
                    first_active = i;
                    found_active = true;
                }
                last_active = i;
            }
        }

        if (found_active)
        {
            uint32_t active_len = (last_active >= first_active) ? (last_active - first_active + 1U) : 0U;
            if (active_len >= TRIM_MIN_ACTIVE_SAMPLES)
            {
                trim_start = (first_active > TRIM_PAD_SAMPLES) ? (first_active - TRIM_PAD_SAMPLES) : 0U;
                uint32_t right = last_active + 1U + TRIM_PAD_SAMPLES;
                trim_end = (right < captured_samples) ? right : captured_samples;
            }
        }
    }

    uint32_t trimmed_len = (trim_end > trim_start) ? (trim_end - trim_start) : 0U;
    if (trimmed_len > INFERENCE_SAMPLES)
    {
        uint32_t crop_start = trim_start + ((trimmed_len - INFERENCE_SAMPLES) / 2U);
        memcpy(s_model_input_audio,
               &s_flat_audio_buffer[crop_start],
               INFERENCE_SAMPLES * sizeof(float));
    }
    else if (trimmed_len > 0U)
    {
        uint32_t left_pad = (INFERENCE_SAMPLES - trimmed_len) / 2U;
        memcpy(&s_model_input_audio[left_pad],
               &s_flat_audio_buffer[trim_start],
               trimmed_len * sizeof(float));
    }

    /* normalize_audio: divide by peak if non-silent */
    peak = 0.0f;
    for (uint32_t i = 0U; i < INFERENCE_SAMPLES; i++)
    {
        float abs_val = fabsf(s_model_input_audio[i]);
        if (abs_val > peak)
        {
            peak = abs_val;
        }
    }
    if (peak >= 1e-8f)
    {
        for (uint32_t i = 0U; i < INFERENCE_SAMPLES; i++)
        {
            s_model_input_audio[i] /= peak;
        }
    }

    /* Extract Feature and Run Inference */
    uart_write_line("\r\nINFO: Manual trigger -> running model...\r\n");

    /* Pause SSI capture during inference to prevent overflow errors.
     * Inference can take 10-30s on Cortex-M33 @ 200MHz with a 370KB CNN model.
     * Without pausing, SSI RX FIFO overflows → ssi_int_isr error path fires
     * while main loop is blocked, potentially causing restart loops. */
    if (s_i2s_opened)
    {
        (void) g_i2s0.p_api->stop(g_i2s0.p_ctrl);
    }
    s_capture_started = false;

    uint32_t t0 = DWT->CYCCNT;

    uart_write_line("TRACE: Before extract_log_mel_features\r\n");
    if (extract_log_mel_features(s_model_input_audio, s_model_features)) {
        uint32_t t1 = DWT->CYCCNT;
        uart_write_line("TRACE: After extract_log_mel_features (%lu ms)\r\n",
                        (unsigned long)((uint64_t)(t1 - t0) * 1000ULL / s_core_hz));

        float conf = 0.0f;
        uart_write_line("TRACE: Before digit_recognizer_run (this may take 10-30s)...\r\n");
        int digit = digit_recognizer_run(s_model_features, &conf);
        uint32_t t2 = DWT->CYCCNT;
        uart_write_line("TRACE: After digit_recognizer_run (%lu ms)\r\n",
                        (unsigned long)((uint64_t)(t2 - t1) * 1000ULL / s_core_hz));
        
        if (digit >= 0) {
            uart_write_line(">>> Recognized Digit: %d (Conf: %s) <<<\r\n", digit, ff(conf, 3));
        } else {
            uart_write_line("ERR: Model execution failed.\r\n");
        }

        __disable_irq();
        s_digit_result_value = digit;
        s_digit_result_confidence = conf;
        s_digit_result_ready = true;
        __enable_irq();
    } else {
        uart_write_line("ERR: Feature extraction failed.\r\n");

        __disable_irq();
        s_digit_result_value = -1;
        s_digit_result_confidence = 0.0f;
        s_digit_result_ready = true;
        __enable_irq();
    }

    /* Resume SSI capture after inference. */
    s_restart_pending = true;

#undef s_flat_audio_buffer
#else
    /* In cloud mode the slot is consumed by cloud_asr_client_poll(). */
#endif
}

void key_irq6_isr(void)
{
    R_BSP_IrqStatusClear(VECTOR_NUMBER_ICU_IRQ6);
    FT6336_OnInterrupt();
}

void i2s0_callback(i2s_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        return;
    }

    uint32_t now = DWT->CYCCNT;
    if ((0U != s_last_callback_cycle) && (0U != s_core_hz))
    {
        uint32_t delta_cycles = now - s_last_callback_cycle;
        s_stats.callback_interval_us = cycles_to_us(delta_cycles);
        if (s_stats.callback_interval_us > 0U)
        {
            s_stats.estimated_sample_rate_hz = (FRAME_FRAMES * 1000000U) / s_stats.callback_interval_us;
            if (s_stats.callback_interval_us > s_window_max_callback_gap_us)
            {
                s_window_max_callback_gap_us = s_stats.callback_interval_us;
            }
        }
    }
    s_last_callback_cycle = now;

    if (I2S_EVENT_RX_FULL == p_args->event)
    {
        s_window_callback_count++;
        uint8_t completed_buffer = s_active_buffer;
        uint32_t completed_mask = (1UL << completed_buffer);

        if (0U != (s_ready_mask & completed_mask))
        {
            s_stats.overflow_count++;
            s_stats.error_count++;
            s_window_error_count++;
        }
        s_ready_mask |= completed_mask;

        s_active_buffer ^= 1U;
        fsp_err_t err = queue_next_transfer(s_active_buffer);
        if ((FSP_SUCCESS != err) && (FSP_ERR_IN_USE != err))
        {
            s_stats.overflow_count++;
            s_stats.error_count++;
            s_window_error_count++;
            s_restart_pending = true;
        }
    }
    else if ((I2S_EVENT_TX_EMPTY == p_args->event) || (I2S_EVENT_IDLE == p_args->event))
    {
        /* In SLAVE mode, TX empty and IDLE events are normal and should not cause a restart */
    }
    else
    {
        /* Ignore other minor events, don't restart unless explicitly needed */
    }
}

void uart7_callback(uart_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        return;
    }

    if ((UART_EVENT_TX_COMPLETE == p_args->event) ||
        (UART_EVENT_ERR_PARITY == p_args->event) ||
        (UART_EVENT_ERR_FRAMING == p_args->event) ||
        (UART_EVENT_ERR_OVERFLOW == p_args->event) ||
        (UART_EVENT_BREAK_DETECT == p_args->event))
    {
        s_uart_tx_done = true;
    }
    
    if (UART_EVENT_RX_CHAR == p_args->event)
    {
        uart7_ring_push((uint8_t) p_args->data);
        if (p_args->data == 'n' || p_args->data == 'N')
        {
            extern volatile bool g_ui_switch_requested;
            g_ui_switch_requested = true;
        }
    }
}
