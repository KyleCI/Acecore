#pragma once
// RF modulation only. No GPIO pins and no backend URLs.
// The collector (include/pins.h) and the AcePlug image both use this profile.
// AcePlug v0.1 pins live in smart-socket-firmware (RELAY IO15). Not here.

#ifndef POWERCELL_LORA_FREQUENCY_HZ
#define POWERCELL_LORA_FREQUENCY_HZ 433000000L
#endif
#ifndef POWERCELL_LORA_SYNC_WORD
#define POWERCELL_LORA_SYNC_WORD 0xF1
#endif
#ifndef POWERCELL_LORA_SF
#define POWERCELL_LORA_SF 9
#endif
#ifndef POWERCELL_LORA_BW_HZ
#define POWERCELL_LORA_BW_HZ 125000L
#endif
#ifndef POWERCELL_LORA_CR
#define POWERCELL_LORA_CR 5  // 4/5
#endif
#ifndef POWERCELL_LORA_PREAMBLE
#define POWERCELL_LORA_PREAMBLE 12
#endif
#ifndef POWERCELL_LORA_TX_POWER_DBM
#define POWERCELL_LORA_TX_POWER_DBM 20
#endif
#ifndef POWERCELL_LORA_SPI_HZ
#define POWERCELL_LORA_SPI_HZ 2000000L
#endif
