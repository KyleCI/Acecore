#pragma once
#include <housekeeping.h>

// Public snapshot struct — safe copy of current inverter state
struct InverterSnapshot {
  String sys;       // iNVERTER STATE(IDLE/ INVERTER/ CHARGING)
  String sw;      // Switch State(Inverter / Grid)
  float  batVoltage;    //Battery Voltage(Ignore, bmsData.packVoltage is more accurate)
  float  gridVoltage;   //Inverter Input Voltage
  float  tempC;
  String chargeMode;      // Float, Bulk, Absorption
  float  vout;
  float  iout;
};

// Call once from setup() — starts UART + background parsing task
void initInverterParser(int rxPin, int txPin, long baud);

// Thread-safe read of current inverter state
InverterSnapshot getInverterStatus();