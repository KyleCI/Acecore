// 2.5.36 socket power keys, shared by the ESP-NOW and LoRa socket paths.
#include "config.h"
#include "sockets.h"
#include "socket_power.h"
#include "../stm32_link.h"

// DSP: stm32link::status() is the PZEM-004T v3 sample on the AC output (fresh within the PZEM
// window). STM: the STM32 status frame vout. Both are the voltage the sockets are plugged into.
SocketVoltage socketsCollectorVoltage() {
  SocketVoltage v;
  pc::Stm32Status s; uint32_t age = 0;
  if (stm32link::status(s, &age) && age < STM32_STALE_MS && s.has_vout) { v.fresh = true; v.volts = s.vout; }
  return v;
}

void socketPowerToJson(JsonObject o, bool socketFresh, float socketAmps, const SocketVoltage &v) {
  pc::sockpower::Result r = pc::sockpower::compute(socketFresh, socketAmps, v.fresh, v.volts);
  if (r.hasVoltage) o["voltage"] = r.voltage; else o["voltage"] = nullptr;
  o["voltage_source"] = pc::sockpower::kVoltageSource;
  if (r.hasPower) {
    o["apparent_power_va"] = r.apparentVa;
    o["current_usage_watts"] = r.apparentVa;  // VA, not real watts: the app/backend key name
  } else {
    o["apparent_power_va"] = nullptr;
    o["current_usage_watts"] = nullptr;
  }
  o["power_quantity"] = pc::sockpower::kQuantity;
}
