#pragma once
#include "config.h"
#include <stdint.h>

#if POWERCELL_HW_V011
// DS3231 on I2C SDA21/SCL22 at 100 kHz, address 0x68.
// A coherent read is unanchored: OSF, a bad calendar, a NACK, or a short read
// returns false and does not invent a time. A write is accepted only after
// the calendar registers read back.
bool hwRtcBegin(int64_t* unix_utc);
bool hwRtcWrite(int64_t unix_utc);
#endif
