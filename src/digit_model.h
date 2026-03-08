#ifndef DIGIT_MODEL_H_
#define DIGIT_MODEL_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * digit_model.h
 *
 * 作用：
 * - 对外声明 TFLite 模型字节数组，供 MCU 固件加载使用。
 * - 模型数据应放在只读存储区（Flash），不要放在 RAM。
 *
 * 集成说明：
 * - 在初始化 TFLite Micro 模型的源码中包含本头文件。
 * - 将 g_digit_model_data / g_digit_model_data_len 传给 GetModel()。
 */
extern const unsigned char g_digit_model_data[];
extern const unsigned int g_digit_model_data_len;

#ifdef __cplusplus
}
#endif

#endif  // DIGIT_MODEL_H_
