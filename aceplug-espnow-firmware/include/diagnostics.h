#pragma once
#include <Arduino.h>
#include <esp_system.h>
#include <stdarg.h>

inline void diagnostic(const char *level, const char *component,
                       const char *code, const char *format, ...) {
  char message[256];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  Serial.printf("[%lu ms][%s][%s][%s] %s\n",
                (unsigned long)millis(), level, component, code, message);
}

inline void logBoot(const char *role, const char *version) {
  esp_reset_reason_t reason = esp_reset_reason();
  diagnostic("INFO", "BOOT", "START", "role=%s firmware=%s reset_reason=%d free_heap=%lu",
             role, version, int(reason), (unsigned long)ESP.getFreeHeap());
  if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT)
    diagnostic("ERROR", "BOOT", "WATCHDOG_RESET", "Previous execution stalled; inspect messages before restart");
  if (reason == ESP_RST_BROWNOUT)
    diagnostic("ERROR", "BOOT", "BROWNOUT_RESET", "Supply voltage dropped; check power supply and wiring");
}
