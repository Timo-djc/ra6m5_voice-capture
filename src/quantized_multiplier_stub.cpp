#include <cstdint>

namespace tflite {

// Keep these definitions aligned with TFLM kernels/internal/common.cpp
// single-rounding path, and provide them unconditionally for this build.
int32_t MultiplyByQuantizedMultiplier(int32_t x, int32_t quantized_multiplier, int shift)
{
  const int64_t total_shift = 31 - shift;
  const int64_t round = static_cast<int64_t>(1) << (total_shift - 1);
  int64_t result =
      static_cast<int64_t>(x) * static_cast<int64_t>(quantized_multiplier) + round;
  result = result >> total_shift;
  return static_cast<int32_t>(result);
}

int32_t MultiplyByQuantizedMultiplier(int64_t x, int32_t quantized_multiplier, int shift)
{
  const int32_t reduced_multiplier =
      (quantized_multiplier < 0x7FFF0000)
          ? ((quantized_multiplier + (1 << 15)) >> 16)
          : 0x7FFF;
  const int64_t total_shift = 15 - shift;
  const int64_t round = static_cast<int64_t>(1) << (total_shift - 1);
  int64_t result = x * static_cast<int64_t>(reduced_multiplier) + round;
  result = result >> total_shift;
  return static_cast<int32_t>(result);
}

}  // namespace tflite
