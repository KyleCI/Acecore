// Compile-time check that each product image picks the right BMS driver.
// Built from test/host/Makefile: 2.5.35 product images are JBD on every board (DSP and STM,
// RX17/TX16 9600); JK-PB stays selectable on both (DSP_BMS_JKPB=1 on DSP, no flag on STM).
#include "config.h"
#include "pins.h"

#if defined(EXPECT_DSP_JKPB)
static_assert(POWERCELL_DSP011 == 1, "DSP image");
static_assert(DSP_BMS_JKPB == 1 && POWERCELL_BMS_JKPB == 1, "DSP uses JK-PB");
static_assert(PIN_BMS_RX == 17 && PIN_BMS_TX == 16, "DSP JK-PB pins RX 17 / TX 16 (Dubem working firmware)");
static_assert(PIN_BMS_BAUD == 115200, "DSP JK-PB baud is 115200");
static_assert(DSP_BMS_UART_SWAP == 0, "DSP_BMS_UART_SWAP stays 0");
static_assert(PIN_LED_RED == 13 && PIN_LED_ORANGE == 12 && PIN_LED_GREEN == 14,
              "DSP status lights: red GPIO13, orange GPIO12, green GPIO14");
#elif defined(EXPECT_DSP_JBD)
static_assert(POWERCELL_DSP011 == 1, "optional DSP JBD build");
static_assert(DSP_BMS_JKPB == 0 && POWERCELL_BMS_JKPB == 0, "JBD selected");
static_assert(POWERCELL_BMS_JBD == 1, "2.5.35 JBD flag");
static_assert(PIN_BMS_RX == 17 && PIN_BMS_TX == 16, "JBD pins RX 17 / TX 16 (same connector as JK-PB)");
static_assert(PIN_BMS_BAUD == 9600, "DSP JBD baud is 9600");
static_assert(DSP_BMS_UART_SWAP == 0, "DSP_BMS_UART_SWAP stays 0");
static_assert(PIN_LED_RED == 13 && PIN_LED_ORANGE == 12 && PIN_LED_GREEN == 14,
              "DSP status lights: red GPIO13, orange GPIO12, green GPIO14");
#elif defined(EXPECT_STM_JKPB)
static_assert(POWERCELL_DSP011 == 0, "STM image");
static_assert(DSP_BMS_JKPB == 0 && POWERCELL_BMS_JKPB == 1, "STM uses JK-PB and ignores DSP_BMS_JKPB");
static_assert(PIN_BMS_RX == 17 && PIN_BMS_TX == 16, "STM JK-PB pins stay RX 17 / TX 16");
static_assert(PIN_BMS_BAUD == 115200, "STM JK-PB baud is 115200");
static_assert(PIN_LED_RED == 13 && PIN_LED_YELLOW == 12, "STM status lights unchanged");
#elif defined(EXPECT_STM_JBD)
static_assert(POWERCELL_DSP011 == 0 && POWERCELL_BMS_JBD == 1, "STM JBD image (2.5.35)");
static_assert(DSP_BMS_JKPB == 0 && POWERCELL_BMS_JKPB == 0, "STM selects JBD");
static_assert(PIN_BMS_RX == 17 && PIN_BMS_TX == 16, "STM JBD pins RX 17 / TX 16");
static_assert(PIN_BMS_BAUD == 9600, "STM JBD baud is 9600");
#else
#error "define EXPECT_DSP_JKPB, EXPECT_DSP_JBD, EXPECT_STM_JKPB or EXPECT_STM_JBD"
#endif

int main() { return 0; }
