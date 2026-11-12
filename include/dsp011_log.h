#pragma once
#include "config.h"

#if POWERCELL_DSP011
// SD card on GPIO5, SPI 18/19/23. Serial bytes are copied into the spool.
// BBTAIL/BBEXPORT print that log. PZEM watt-hours are stored with the DS3231
// clock when NTP has not anchored the time.
void hwLogStart();
void hwLogExport(bool full);
#endif
