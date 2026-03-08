#ifndef AUDIO_FEATURES_H_
#define AUDIO_FEATURES_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#define AUDIO_FEAT_SAMPLE_RATE 16000
#define AUDIO_FEAT_WIN_LEN     400
#define AUDIO_FEAT_HOP_LEN     160
#define AUDIO_FEAT_N_FFT       1024
#define AUDIO_FEAT_N_MELS      40
#define AUDIO_FEAT_FRAMES      150
#define AUDIO_FEAT_PRE_EMPH    0.97f

/*
 * extract_log_mel_features
 * input_audio: 1.5s of PCM float samples, normalized to [-1.0, 1.0] (24000 samples)
 * out_features: Buffer for output log-mel features [40][150] = 6000 floats
 */
bool extract_log_mel_features(const float* input_audio, float* out_features);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_FEATURES_H_
