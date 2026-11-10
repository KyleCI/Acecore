#pragma once

#include <stdint.h>

// v0.1.1 shares I2C GPIO21/22 between the ADS1115 and the DS3231.
// The original profile's lock is a no-op.
void i2c_bus_init();
bool i2c_bus_lock(uint32_t timeout_ms);
void i2c_bus_unlock();
