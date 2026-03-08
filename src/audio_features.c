#include "audio_features.h"
#include "net/cloud_asr_cfg.h"

#if ASR_MODE_LOCAL
#include "mel_filterbank.h"
#include "arm_math.h"
#include "fmt_float.h"
#include <math.h>
#include <stdio.h>

extern void uart_write_line_ext(const char* str);

static arm_rfft_fast_instance_f32 s_rfft_instance;

#define AUDIO_INPUT_SAMPLES ((AUDIO_FEAT_SAMPLE_RATE * 15) / 10)
#define AUDIO_VALID_FRAMES  (((AUDIO_INPUT_SAMPLES - AUDIO_FEAT_WIN_LEN) / AUDIO_FEAT_HOP_LEN) + 1)

/* Symmetric Hanning window matching scipy.signal.windows.hann(N):
 *   w[n] = 0.5 * (1 - cos(2*PI*n / (N-1)))  for n = 0..N-1
 * scipy.signal.stft(window="hann") uses this SYMMETRIC window (denominator N-1).
 * Previously we used a PERIODIC window (denominator N) which caused mismatch. */
static float s_hanning_window[AUDIO_FEAT_WIN_LEN];
static bool s_features_initialized = false;

static void init_hanning_window(void)
{
    const float denom = (float)(AUDIO_FEAT_WIN_LEN - 1); /* N-1 = 399 for symmetric */
    for (int i = 0; i < AUDIO_FEAT_WIN_LEN; i++)
    {
        float phase = (2.0f * PI * (float)i) / denom;
        s_hanning_window[i] = 0.5f - 0.5f * arm_cos_f32(phase);
    }
}



bool extract_log_mel_features(const float* input_audio, float* out_features)
{
    if (!s_features_initialized)
    {
        arm_status status = arm_rfft_fast_init_f32(&s_rfft_instance, AUDIO_FEAT_N_FFT);
        if (status != ARM_MATH_SUCCESS) return false;
        init_hanning_window();
        s_features_initialized = true;
    }

    static float fft_in_out[AUDIO_FEAT_N_FFT];
    
    float sum_logmel = 0.0f;
    float sum_sq_logmel = 0.0f;
    int count = AUDIO_FEAT_FRAMES * AUDIO_FEAT_N_MELS;

    for (int frame = 0; frame < AUDIO_FEAT_FRAMES; frame++)
    {
        bool has_valid_audio = (frame < AUDIO_VALID_FRAMES);

        if (has_valid_audio)
        {
            int start_idx = frame * AUDIO_FEAT_HOP_LEN;

            // Windowing, pre-emphasis, and zero-padding
            for (int i = 0; i < AUDIO_FEAT_N_FFT; i++)
            {
                if (i < AUDIO_FEAT_WIN_LEN)
                {
                    // Apply pre-emphasis dynamically: y[i] = x[i] - coef * x[i-1]
                    float cur_val = input_audio[start_idx + i];
                    float prev_val = (start_idx + i > 0) ? input_audio[start_idx + i - 1] : 0.0f;
                    float emp_val = cur_val - AUDIO_FEAT_PRE_EMPH * prev_val;
                    
                    fft_in_out[i] = emp_val * s_hanning_window[i];
                }
                else
                {
                    fft_in_out[i] = 0.0f;
                }
            }

            // Real FFT (1024 points) -> Output format is complex
            arm_rfft_fast_f32(&s_rfft_instance, fft_in_out, fft_in_out, 0);

            // Power spectrum (squared magnitude)
            static float power_spec[MEL_BINS]; // 513 bins
            power_spec[0] = (fft_in_out[0] * fft_in_out[0]);
            power_spec[512] = (fft_in_out[1] * fft_in_out[1]); // Nyquist is at fft_in_out[1]

            for (int i = 1; i < MEL_BINS - 1; i++)
            {
                float real = fft_in_out[2 * i];
                float imag = fft_in_out[2 * i + 1];
                power_spec[i] = (real * real + imag * imag);
            }

            // Mel Filterbank application
            for (int m = 0; m < AUDIO_FEAT_N_MELS; m++)
            {
                float mel_val = 0.0f;
                for (int k = 0; k < MEL_BINS; k++)
                {
                    mel_val += power_spec[k] * g_mel_filterbank[m][k];
                }

                if (mel_val < 1e-10f) mel_val = 1e-10f;
                mel_val = log10f(mel_val);

                out_features[m * AUDIO_FEAT_FRAMES + frame] = mel_val;

                sum_logmel += mel_val;
                sum_sq_logmel += mel_val * mel_val;
            }
        }
        else
        {
            // Pad tail frames with zero
            for (int m = 0; m < AUDIO_FEAT_N_MELS; m++)
            {
                out_features[m * AUDIO_FEAT_FRAMES + frame] = 0.0f;
                // sum_logmel += 0.0f; (no-op)
                // sum_sq_logmel += 0.0f; (no-op)
            }
        }
    }
    
    // Global Normalization (Z-score)
    float mean = sum_logmel / (float)count;
    float variance = (sum_sq_logmel / (float)count) - (mean * mean);
    float std = 0.0f;
    if (variance > 0.0f) std = sqrtf(variance);
    std += 1e-6f;
    
    for (int i = 0; i < count; i++)
    {
        out_features[i] = (out_features[i] - mean) / std;
    }
    
    /* Diagnostic: print audio input and feature stats */
    {
        char dbuf[120];
        /* Input audio stats (first 10 samples and overall) */
        float amin = input_audio[0], amax = input_audio[0];
        float asum = 0.0f;
        int nonzero = 0;
        for (int i = 0; i < AUDIO_INPUT_SAMPLES; i++)
        {
            if (input_audio[i] < amin) amin = input_audio[i];
            if (input_audio[i] > amax) amax = input_audio[i];
            asum += input_audio[i];
            if (fabsf(input_audio[i]) > 1e-10f) nonzero++;
        }
        snprintf(dbuf, sizeof(dbuf),
                 "FEAT_DIAG: audio min=%s max=%s mean=%s nonzero=%d/%d\r\n",
                 ff(amin, 4), ff(amax, 4), ff(asum / AUDIO_INPUT_SAMPLES, 4), nonzero, AUDIO_INPUT_SAMPLES);
        uart_write_line_ext(dbuf);

        /* Log-mel stats before Z-score */
        snprintf(dbuf, sizeof(dbuf),
                 "FEAT_DIAG: logmel_mean=%s logmel_std=%s (before Z-score)\r\n",
                 ff(mean, 4), ff(std, 4));
        uart_write_line_ext(dbuf);

        /* Feature stats after Z-score */
        float fmin = out_features[0], fmax = out_features[0];
        for (int i = 1; i < count; i++)
        {
            if (out_features[i] < fmin) fmin = out_features[i];
            if (out_features[i] > fmax) fmax = out_features[i];
        }
        snprintf(dbuf, sizeof(dbuf),
                 "FEAT_DIAG: feat min=%s max=%s (after Z-score, count=%d)\r\n",
                 ff(fmin, 4), ff(fmax, 4), count);
        uart_write_line_ext(dbuf);
    }
    
    return true;
}

#else

bool extract_log_mel_features(const float* input_audio, float* out_features)
{
    (void) input_audio;
    (void) out_features;
    return false;
}

#endif
