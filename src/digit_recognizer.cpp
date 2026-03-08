#include "digit_recognizer.h"
#include "digit_model.h"
#include "net/cloud_asr_cfg.h"

#if ASR_MODE_LOCAL
// TFLite Micro includes (Assumes CMSIS-NN / TF Lite pack is provided by Keil or added manually)
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/micro/tflite_bridge/micro_error_reporter.h"
#include <math.h>
#include <cstdio>
#include "fmt_float.h"

// Model settings
// 192KB arena: Model (370KB flash) has Conv2D+MaxPool+FC layers whose intermediate
// int8 feature maps can easily exceed 128KB. RA6M5 has 512KB SRAM — 192KB arena
// leaves ~320KB for audio buffers, stack, and other statics.
#define TENSOR_ARENA_SIZE (192 * 1024)
alignas(16) static uint8_t s_tensor_arena[TENSOR_ARENA_SIZE];

static const tflite::Model* s_model = nullptr;
static tflite::MicroInterpreter* s_interpreter = nullptr;
static TfLiteTensor* s_input = nullptr;
static TfLiteTensor* s_output = nullptr;

extern "C" void uart_write_line_ext(const char* str);

static tflite::MicroMutableOpResolver<8> s_op_resolver;

bool digit_recognizer_init(void)
{
    uart_write_line_ext("TFLM: Starting init...\r\n");

    uart_write_line_ext("TFLM: GetModel...\r\n");
    s_model = tflite::GetModel(g_digit_model_data);
    if (s_model->version() != TFLITE_SCHEMA_VERSION) {
        uart_write_line_ext("TFLM ERR: Schema version mismatch!\r\n");
        return false;
    }

    uart_write_line_ext("TFLM: Allocating op_resolver...\r\n");
    if (s_op_resolver.AddAdd() != kTfLiteOk) return false;
    if (s_op_resolver.AddConv2D() != kTfLiteOk) return false;
    if (s_op_resolver.AddMaxPool2D() != kTfLiteOk) return false;
    if (s_op_resolver.AddMul() != kTfLiteOk) return false;
    if (s_op_resolver.AddReshape() != kTfLiteOk) return false;
    if (s_op_resolver.AddFullyConnected() != kTfLiteOk) return false;
    if (s_op_resolver.AddSoftmax() != kTfLiteOk) return false;

    uart_write_line_ext("TFLM: Creating MicroInterpreter...\r\n");
    // Do NOT use a function-local static MicroInterpreter object!
    // ARM Compiler C++ will silently inject __cxa_guard_acquire and __aeabi_atexit 
    // which depend on Semihosting and RTOS mutexes, causing a HardFault!
    // Instead, allocate a global aligned buffer and use placement new.
    alignas(16) static uint8_t s_interpreter_buffer[sizeof(tflite::MicroInterpreter)];
    s_interpreter = new(s_interpreter_buffer) tflite::MicroInterpreter(
        s_model, s_op_resolver, s_tensor_arena, TENSOR_ARENA_SIZE);

    uart_write_line_ext("TFLM: AllocateTensors()...\r\n");
    TfLiteStatus alloc_status = s_interpreter->AllocateTensors();
    if (alloc_status != kTfLiteOk) {
        char buf[80];
        snprintf(buf, sizeof(buf), "TFLM ERR: AllocateTensors failed (status=%d, arena=%d)\r\n",
                 (int)alloc_status, TENSOR_ARENA_SIZE);
        uart_write_line_ext(buf);
        return false;
    }
    {
        /* Print arena usage so we know how tight memory is. */
        size_t used = s_interpreter->arena_used_bytes();
        char buf[80];
        snprintf(buf, sizeof(buf), "TFLM: arena used=%u / %u bytes\r\n",
                 (unsigned)used, (unsigned)TENSOR_ARENA_SIZE);
        uart_write_line_ext(buf);
    }

    uart_write_line_ext("TFLM: Getting IO pointers...\r\n");
    s_input = s_interpreter->input(0);
    s_output = s_interpreter->output(0);
    
    uart_write_line_ext("TFLM: Init complete.\r\n");
    return true;
}

int digit_recognizer_run(float* input_features, float* out_confidence)
{
    if (!s_interpreter) {
        uart_write_line_ext("TFLM ERR: s_interpreter is NULL! Did init fail?\r\n");
        return -1;
    }

    // The TFLite int8 model input expects int8.
    // We must quantize our float features into the input tensor.
    // Features array is [40][150] (mel x frame), flattened to 6000 length.
    float input_scale = s_input->params.scale;
    int32_t input_zero_point = s_input->params.zero_point;
    
    /* Print model tensor parameters for verification against deployment doc */
    {
        char buf[120];
        snprintf(buf, sizeof(buf),
                 "QUANT: input scale=%s zero_point=%d  dims=[%d,%d,%d,%d]\r\n",
                 ff(input_scale, 10), (int)input_zero_point,
                 (int)s_input->dims->data[0], (int)s_input->dims->data[1],
                 (int)s_input->dims->data[2], (int)s_input->dims->data[3]);
        uart_write_line_ext(buf);
    }

    int8_t* quant_input = s_input->data.int8;
    
    /* Diagnostic: check input feature stats */
    float feat_min = input_features[0], feat_max = input_features[0];
    for (int i = 1; i < 6000; i++)
    {
        if (input_features[i] < feat_min) feat_min = input_features[i];
        if (input_features[i] > feat_max) feat_max = input_features[i];
    }
    
    for (int i = 0; i < 6000; i++)
    {
        // Align with Python-side np.round quantization behavior.
        float val = nearbyintf(input_features[i] / input_scale + input_zero_point);
        if (val < -128.0f) val = -128.0f;
        if (val > 127.0f) val = 127.0f;
        quant_input[i] = (int8_t)val;
    }
    
    /* Diagnostic: print first few quantized values and saturation stats */
    {
        char buf[120];
        int sat_lo = 0, sat_hi = 0;
        for (int i = 0; i < 6000; i++)
        {
            if (quant_input[i] == -128) sat_lo++;
            if (quant_input[i] == 127) sat_hi++;
        }
        snprintf(buf, sizeof(buf),
                 "QUANT: feat_range=[%s,%s] q[0..4]=%d,%d,%d,%d,%d sat_lo=%d sat_hi=%d\r\n",
                 ff(feat_min, 3), ff(feat_max, 3),
                 (int)quant_input[0], (int)quant_input[1], (int)quant_input[2],
                 (int)quant_input[3], (int)quant_input[4],
                 sat_lo, sat_hi);
        uart_write_line_ext(buf);
    }

    // Run inference
    uart_write_line_ext("TFLM: Invoking model...\r\n");
    if (s_interpreter->Invoke() != kTfLiteOk) {
        uart_write_line_ext("TFLM ERR: Invoke() returned error!\r\n");
        return -1;
    }
    uart_write_line_ext("TFLM: Invoke() OK!\r\n");

    // Find the max softmax output
    int8_t* quant_output = s_output->data.int8;
    float output_scale = s_output->params.scale;
    int32_t output_zero_point = s_output->params.zero_point;
    
    /* Diagnostic: print ALL 10 output int8 values and their dequantized probabilities */
    {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "OUT: scale=%s zp=%d raw=[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]\r\n",
                 ff(output_scale, 8), (int)output_zero_point,
                 (int)quant_output[0], (int)quant_output[1], (int)quant_output[2],
                 (int)quant_output[3], (int)quant_output[4], (int)quant_output[5],
                 (int)quant_output[6], (int)quant_output[7], (int)quant_output[8],
                 (int)quant_output[9]);
        uart_write_line_ext(buf);
        /* Print dequantized probabilities (loop to avoid >4 ff() slots) */
        {
            int pos = snprintf(buf, sizeof(buf), "OUT: prob=[");
            for (int i = 0; i < 10; i++) {
                float p = (float)(quant_output[i] - output_zero_point) * output_scale;
                pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%s",
                                ff(p, 3), i < 9 ? "," : "");
            }
            snprintf(buf + pos, sizeof(buf) - pos, "]\r\n");
        }
        uart_write_line_ext(buf);
    }

    int best_class = -1;
    int8_t max_val = -128;
    
    for (int i = 0; i < 10; i++)
    {
        if (quant_output[i] > max_val)
        {
            max_val = quant_output[i];
            best_class = i;
        }
    }

    if (out_confidence && best_class != -1)
    {
        *out_confidence = (max_val - output_zero_point) * output_scale;
    }

    return best_class;
}

#else

bool digit_recognizer_init(void)
{
    return false;
}

int digit_recognizer_run(float* input_features, float* out_confidence)
{
    (void) input_features;
    if (out_confidence)
    {
        *out_confidence = 0.0f;
    }
    return -1;
}

#endif
