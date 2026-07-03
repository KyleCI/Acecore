#pragma once

/*
  ESP32 SPI SLAVE - Integer Parser for Arduino MPPT Master
  Header file: data structures, constants, and function declarations
*/

#include <Arduino.h>
#include "driver/spi_slave.h"
#include <ArduinoJson.h>

// ─── Pin Definitions ────────────────────────────────────────────────────────
#define PIN_MOSI 23
#define PIN_MISO 19
#define PIN_SCK  18
#define PIN_SS    5

// ─── SPI Config ─────────────────────────────────────────────────────────────
#define SPI_FRAME_SIZE       256
#define INT_TO_FLOAT_DIVISOR 1000.0f

// Per-frame serial diagnostics (formatted table + raw dump + JSON block).
// These run inside the background SPI task so they no longer block loop(),
// but they are very chatty. Set to 0 (e.g. -D MPPT_VERBOSE=0) to silence them
// in production without removing the display helpers.
#ifndef MPPT_VERBOSE
#define MPPT_VERBOSE 1
#endif

// ─── Buffers ─────────────────────────────────────────────────────────────────
extern uint8_t rxBuf[SPI_FRAME_SIZE];
extern uint8_t txBuf[SPI_FRAME_SIZE];

// ─── Statistics ──────────────────────────────────────────────────────────────
extern uint32_t      frameCount;
extern uint32_t      parseErrors;
extern unsigned long lastDisplayTime;
extern unsigned long lastJsonOutputTime;

extern String compactJson;

// ─── Data Structures ─────────────────────────────────────────────────────────

// Raw integer data received over SPI (values × 1000)
struct MPPTIntegerData {
  unsigned long timestamp;
  int32_t solarVoltage_mV;   // Volts   × 1000
  int32_t current_mA;        // Amps    × 1000
  int32_t power_mW;          // Watts   × 1000
  int32_t temperature_mC;    // Celsius × 1000
  int     pwmDuty;
  int     mppFound;
  int32_t mppVoltage_mV;     // Volts   × 1000
  int32_t mppCurrent_mA;     // Amps    × 1000
  int32_t mppPower_mW;       // Watts   × 1000
  int32_t batteryVoltage_mV; // Volts   × 1000
};

// Converted float data ready for display / JSON output
struct MPPTData {
  unsigned long timestamp;
  float solarVoltage;
  float current;
  float power;
  float temperature;
  int   pwmDuty;
  bool  mppFound;
  float mppVoltage;
  float mppCurrent;
  float mppPower;
  float batteryVoltage;
};

// ─── Function Declarations ───────────────────────────────────────────────────
bool handleSPI(JsonObject mppt);
void buildAck();
bool parseIntegerCSV(const char* data, MPPTIntegerData& mpptInt);
void convertToFloat(const MPPTIntegerData& mpptInt, MPPTData& mppt);
void displayJSONOutput(const MPPTData& mppt);
void displayFormattedTable(const MPPTData& mppt);
void displayRawData(const char* data, uint16_t len);
void displayStatistics();
