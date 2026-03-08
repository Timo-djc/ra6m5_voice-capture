// Stub for base ErrorReporter::Report which is not defined in TFLM Micro pack
// but referenced by flatbuffer_conversions.cpp and similar files

#include "tensorflow/lite/core/api/error_reporter.h"
#include <cstdarg>
#include <cstdio>

extern "C" void uart_write_line_ext(const char* str);

namespace tflite {

// Just a simple fallback implementation that prints to stdout
int ErrorReporter::Report(const char* format, ...) {
  char buffer[128];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer) - 1, format, args);
  buffer[sizeof(buffer) - 1] = '\0';
  va_end(args);
  
  uart_write_line_ext(buffer);
  return 0;
}

}  // namespace tflite
