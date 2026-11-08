#pragma once
#include <stdint.h>

// Original collector hardware v0.1 as pinned on main a0932adec88a1f5ee3679becf51a084874f986f1.
// STM32 text telemetry stays on UART1 RX 33 / TX 2. JK BMS stays on UART2.
// LoRa, the relay, and the ADS1115 I2C bus stay on these pins.
// Hardware v0.1.1 (PZEM, SD chip-select on GPIO 33, light moved onto GPIO 15,
// STM32 removed) is a different board and is not this profile.

constexpr int ORIGINAL_V01_INVERTER_RX = 33;
constexpr int ORIGINAL_V01_INVERTER_TX = 2;
constexpr int ORIGINAL_V01_BMS_RX = 17;
constexpr int ORIGINAL_V01_BMS_TX = 16;
constexpr int ORIGINAL_V01_LORA_RST = 26;
constexpr int ORIGINAL_V01_LORA_CS = 4;
constexpr int ORIGINAL_V01_LORA_DIO0 = 2;
constexpr int ORIGINAL_V01_RELAY = 15;
constexpr int ORIGINAL_V01_RED = 13;
constexpr int ORIGINAL_V01_YELLOW = 12;
constexpr int ORIGINAL_V01_GREEN = 27;
constexpr int ORIGINAL_V01_AMBER = 32;
constexpr int ORIGINAL_V01_LIGHTS = 25;
constexpr int ORIGINAL_V01_I2C_SDA = 21;
constexpr int ORIGINAL_V01_I2C_SCL = 22;
constexpr int ORIGINAL_V01_MPPT_CS = 5;
constexpr int ORIGINAL_V01_SD_CS = -1;
constexpr uint32_t ORIGINAL_V01_INVERTER_BAUD = 9600;
constexpr uint32_t ORIGINAL_V01_I2C_HZ = 100000;
constexpr uint8_t ORIGINAL_V01_DS1307_ADDR = 0x68;
