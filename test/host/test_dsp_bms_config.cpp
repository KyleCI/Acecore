// Compile-time check that each product image picks the right BMS driver.
// Built four ways from test/host/Makefile (DSP 5 kW and 3.5 kW JK-PB, optional DSP JBD, STM JK-PB).
#include "config.h"
#include "pins.h"

#if defined(EXPECT_DSP_JKPB)
static_assert(POWERCELL_DSP011 == 1, "DSP image");
static_assert(DSP_BMS_JKPB == 1 && POWERCELL_BMS_JKPB == 1, "DSP uses JK-PB");
static_assert(PIN_BMS_RX == 17 && PIN_BMS_TX == 16, "DSP JK-PB pins RX 17 / TX 16 (Dubem working firmware)");
static_assert(PIN_BMS_BAUD == 115200, "DSP JK-PB baud is 115200");
static_assert(DSP_BMS_UART_SWAP == 0, "DSP_BMS_UART_SWAP stays 0");
static_assert(PIN_LED_RED == 13 && PIN_LED_BLUE == 12 && PIN_LED_GREEN == 14,
              "DSP status lights: red GPIO13, orange GPIO12, green GPIO14");
#elif defined(EXPECT_DSP_JBD)
static_assert(POWERCELL_DSP011 == 1, "optional DSP JBD build");
static_assert(DSP_BMS_JKPB == 0 && POWERCELL_BMS_JKPB == 0, "JBD selected");
static_assert(PIN_BMS_RX == 16 && PIN_BMS_TX == 17, "JBD pins RX 16 / TX 17");
static_assert(PIN_BMS_BAUD == 9600, "DSP JBD baud is 9600");
static_assert(DSP_BMS_UART_SWAP == 0, "DSP_BMS_UART_SWAP stays 0");
static_assert(PIN_LED_RED == 13 && PIN_LED_BLUE == 12 && PIN_LED_GREEN == 14,
              "DSP status lights: red GPIO13, orange GPIO12, green GPIO14");
#elif defined(EXPECT_STM_JKPB)
static_assert(POWERCELL_DSP011 == 0, "STM image");
static_assert(DSP_BMS_JKPB == 0 && POWERCELL_BMS_JKPB == 1, "STM uses JK-PB and ignores DSP_BMS_JKPB");
static_assert(PIN_BMS_RX == 17 && PIN_BMS_TX == 16, "STM JK-PB pins stay RX 17 / TX 16");
static_assert(PIN_BMS_BAUD == 115200, "STM JK-PB baud is 115200");
static_assert(PIN_LED_RED == 13 && PIN_LED_YELLOW == 12, "STM status lights unchanged");
#else
#error "define EXPECT_DSP_JKPB, EXPECT_DSP_JBD, or EXPECT_STM_JKPB"
#endif

int main() { return 0; }
