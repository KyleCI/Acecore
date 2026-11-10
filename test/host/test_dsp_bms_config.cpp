// Compile-time check that each product image picks the right BMS driver.
// Built three ways from test/host/Makefile (DSP 5 kW JK-PB, DSP 3.5 kW JBD, STM JK-PB).
#include "config.h"
#include "pins.h"

#if defined(EXPECT_DSP_JKPB)
static_assert(POWERCELL_DSP011 == 1, "5 kW DSP image");
static_assert(DSP_BMS_JKPB == 1 && POWERCELL_BMS_JKPB == 1, "5 kW DSP uses JK-PB");
static_assert(PIN_BMS_RX == 16 && PIN_BMS_TX == 17, "DSP BMS pins stay RX 16 / TX 17");
static_assert(PIN_BMS_BAUD == 115200, "DSP JK-PB baud is 115200");
static_assert(DSP_BMS_UART_SWAP == 0, "DSP_BMS_UART_SWAP stays 0");
#elif defined(EXPECT_DSP_JBD)
static_assert(POWERCELL_DSP011 == 1, "3.5 kW DSP image");
static_assert(DSP_BMS_JKPB == 0 && POWERCELL_BMS_JKPB == 0, "3.5 kW DSP uses JBD");
static_assert(PIN_BMS_RX == 16 && PIN_BMS_TX == 17, "DSP BMS pins stay RX 16 / TX 17");
static_assert(PIN_BMS_BAUD == 9600, "DSP JBD baud is 9600");
static_assert(DSP_BMS_UART_SWAP == 0, "DSP_BMS_UART_SWAP stays 0");
#elif defined(EXPECT_STM_JKPB)
static_assert(POWERCELL_DSP011 == 0, "STM image");
static_assert(DSP_BMS_JKPB == 0 && POWERCELL_BMS_JKPB == 1, "STM uses JK-PB and ignores DSP_BMS_JKPB");
static_assert(PIN_BMS_RX == 17 && PIN_BMS_TX == 16, "STM JK-PB pins stay RX 17 / TX 16");
static_assert(PIN_BMS_BAUD == 115200, "STM JK-PB baud is 115200");
#else
#error "define EXPECT_DSP_JKPB, EXPECT_DSP_JBD, or EXPECT_STM_JKPB"
#endif

int main() { return 0; }
