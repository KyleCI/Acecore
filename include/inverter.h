#pragma once
#include <housekeeping.h>
#include "stm32_text_validity.h"

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

// Thread-safe read of current inverter state.
// A missed mutex leaves this snapshot zero-filled. That is not a valid AC sample.
InverterSnapshot getInverterStatus();

// Hardened load-candidate view. Mutex failure is invalid, not a zero volt/amp reading.
loadac::TextCandidate getStm32LoadCandidate();