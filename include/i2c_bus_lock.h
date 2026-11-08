#pragma once
#include <stdint.h>

// Serializes the original v0.1 I2C bus (GPIO 21/22) between the ADS1115 and
// an optional DS1307. Callers must not hold the lock across long waits.
bool i2c_bus_try_lock(uint32_t timeout_ms);
void i2c_bus_lock();
void i2c_bus_unlock();
