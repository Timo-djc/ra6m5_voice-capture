#ifndef DIGIT_RECOGNIZER_H_
#define DIGIT_RECOGNIZER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/* 
 * Returns true if initialization succeeded. 
 */
bool digit_recognizer_init(void);

/*
 * Runs inference on the 120x40 float features.
 * out_confidence: returns the softmax probability [0.0 - 1.0] of the best prediction.
 * Returns the predicted digit class (0-9), or -1 if error.
 */
int digit_recognizer_run(float* input_features, float* out_confidence);

#ifdef __cplusplus
}
#endif

#endif // DIGIT_RECOGNIZER_H_
