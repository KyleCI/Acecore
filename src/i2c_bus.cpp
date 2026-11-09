#include "i2c_bus.h"

#if POWERCELL_HW_V011
#include "config.h"
#include <Arduino.h>

static SemaphoreHandle_t bus = nullptr;

void i2c_bus_init() {
  if (!bus) bus = xSemaphoreCreateMutex();
}

bool i2c_bus_lock(uint32_t timeout_ms) {
  if (!bus) return false;
  return xSemaphoreTake(bus, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void i2c_bus_unlock() {
  if (bus) xSemaphoreGive(bus);
}
#else
void i2c_bus_init() {}
bool i2c_bus_lock(uint32_t) { return true; }
void i2c_bus_unlock() {}
#endif
