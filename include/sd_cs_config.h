#pragma once

#include "board_pins.h"

// Original builds leave the card disabled. v0.1.1 uses the stated chip select.
#ifndef POWERCELL_SD_CS
#ifdef POWERCELL_HW_V011
#define POWERCELL_SD_CS (powercell::kSdCsV011)
#else
#define POWERCELL_SD_CS (-1)
#endif
#endif

#ifndef PZEM_RX_GPIO
#define PZEM_RX_GPIO (powercell::kPzemRx)
#endif
#ifndef PZEM_TX_GPIO
#define PZEM_TX_GPIO (powercell::kPzemTx)
#endif

#ifdef POWERCELL_HW_V011
static_assert(POWERCELL_SD_CS == 33,
              "SD chip select for hardware v0.1.1 is GPIO33; do not move it onto the PZEM UART");
static_assert(PZEM_RX_GPIO == 25 && PZEM_TX_GPIO == 26,
              "PZEM hardware UART defaults are RX GPIO25 and TX GPIO26");
static_assert(PZEM_RX_GPIO != POWERCELL_SD_CS && PZEM_TX_GPIO != POWERCELL_SD_CS,
              "PZEM pins must not be the SD chip select");
static_assert(!powercell::kV011PzemIsSoftwareUart, "v0.1.1 uses UART1 for PZEM");
#else
static_assert(POWERCELL_SD_CS < 0,
              "SD chip select stays disabled on the original profile because GPIO33 is STM32 RX");
#endif
