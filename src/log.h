#pragma once
#include <Arduino.h>
#include <stdarg.h>

// One-line structured log: [ms][LEVEL][COMPONENT][CODE] message
inline void logf(const char *level, const char *component, const char *code, const char *fmt, ...) {
  char msg[224];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  Serial.printf("[%lu][%s][%s][%s] %s\n", (unsigned long)millis(), level, component, code, msg);
}
#define LOGI(c, code, ...) logf("INFO", c, code, __VA_ARGS__)
#define LOGW(c, code, ...) logf("WARN", c, code, __VA_ARGS__)
#define LOGE(c, code, ...) logf("ERROR", c, code, __VA_ARGS__)
