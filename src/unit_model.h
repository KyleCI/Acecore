#pragma once
#include <Arduino.h>
#include "live.h"
// 2.5.4 / 2.5.13: Powercell model (PC3K | PC3_5K | PC5K | PC10K), set at the
// factory with "SET model PC3_5K" (tools/factory.py --model), NVS pc_cfg/model.
// Unset by default (JSON nulls). "SET model none" clears it. Applies at once.
namespace unitmodel {
void begin();
const pc::live::Model *get();          // nullptr when unset
bool set(const String &code);          // "none"/"" clears
}  // namespace unitmodel
