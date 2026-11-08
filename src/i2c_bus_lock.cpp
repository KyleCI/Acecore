#include "i2c_bus_lock.h"

#include <Arduino.h>

namespace {
SemaphoreHandle_t bus = nullptr;

bool ensure() {
  if (bus) return true;
  bus = xSemaphoreCreateMutex();
  return bus != nullptr;
}
}  // namespace

bool i2c_bus_try_lock(uint32_t timeout_ms) {
  if (!ensure()) return false;
  return xSemaphoreTake(bus, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void i2c_bus_lock() {
  if (!ensure()) return;
  xSemaphoreTake(bus, portMAX_DELAY);
}

void i2c_bus_unlock() {
  if (bus) xSemaphoreGive(bus);
}
